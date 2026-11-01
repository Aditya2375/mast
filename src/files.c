/* files.c - the static-file answer. Path safety is layered: the HTTP
 * parser already rejected NUL/control bytes and escaping "..", and here
 * realpath() must keep the resolved file inside the resolved docroot, so a
 * symlink cannot smuggle a request outside the tree. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "files.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

const char *mast_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    struct { const char *ext, *type; } t[] = {
        {".html", "text/html; charset=utf-8"},
        {".htm",  "text/html; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},
        {".js",   "text/javascript; charset=utf-8"},
        {".mjs",  "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".txt",  "text/plain; charset=utf-8"},
        {".md",   "text/markdown; charset=utf-8"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".svg",  "image/svg+xml"},
        {".ico",  "image/x-icon"},
        {".webp", "image/webp"},
        {".wasm", "application/wasm"},
        {".pdf",  "application/pdf"},
        {".xml",  "application/xml"},
        {".zip",  "application/zip"},
        {".mp3",  "audio/mpeg"},
        {".mp4",  "video/mp4"},
        {".woff", "font/woff"},
        {".woff2","font/woff2"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strcasecmp(dot, t[i].ext) == 0) return t[i].type;
    return "application/octet-stream";
}

/* Strictly one digit run, then end or the expected delimiter. Returns 0
 * and fills *out, 1 on overflow (the value saturates to LLONG_MAX so the
 * caller can clamp it to the file size, per RFC 9110 14.2: a last-byte-pos
 * beyond the length is replaced by the length), -1 on malformed. */
static int range_digits(const char *s, const char *end, long long *out) {
    if (s == end) return -1;
    long long v = 0;
    for (const char *q = s; q < end; q++) {
        if (*q < '0' || *q > '9') return -1;
        if (v > (9223372036854775807LL - 9) / 10) { *out = 9223372036854775807LL; return 1; }
        v = v * 10 + (*q - '0');
    }
    *out = v;
    return 0;
}

int mast_range_parse(const char *header, long long size,
                     long long *start, long long *len) {
    if (!header) return 0;
    if (strncasecmp(header, "bytes=", 6) != 0) return 0;   /* unknown unit: ignore */
    const char *spec = header + 6;
    if (strchr(spec, ',')) return 0;   /* multi-range: ignore, serve the full body */
    const char *dash = strchr(spec, '-');
    if (!dash || strchr(dash + 1, '-')) return 0;   /* malformed: ignore */
    long long a = -1, b = -1;
    /* an overflowing first or last pos saturates: the first then misses
     * EOF (416), the last clamps to the end of the file (206) */
    if (dash != spec && range_digits(spec, dash, &a) < 0) return 0;
    if (dash[1] && range_digits(dash + 1, spec + strlen(spec), &b) < 0) return 0;
    if (a == -1) {
        /* the last b bytes */
        if (b <= 0) return -1;   /* "bytes=-" or "bytes=-0": unsatisfiable */
        if (b > size) b = size;
        *start = size - b;
        *len = b;
    } else {
        if (b != -1 && b < a) return -1;   /* last < first: unsatisfiable */
        if (a >= size) return -1;   /* 416 */
        *start = a;
        *len = (b == -1 || b >= size ? size : b + 1) - a;
    }
    if (*len <= 0) return -1;
    return 1;
}

/* If-None-Match uses weak comparison: a comma list of entity-tags where
 * "W/" prefixes wash off, "*" matches any current representation. */
static int etag_list_matches(const char *list, const char *etag) {
    for (const char *q = list; ;) {
        while (*q == ' ' || *q == '\t' || *q == ',') q++;
        if (!*q) return 0;
        const char *e = q;
        while (*e && *e != ',') e++;
        const char *te = e;
        while (te > q && (te[-1] == ' ' || te[-1] == '\t')) te--;
        size_t n = (size_t)(te - q);
        if (n == 1 && *q == '*') return 1;
        if (n >= 2 && q[0] == 'W' && q[1] == '/') { q += 2; n -= 2; }
        if (strlen(etag) == n && memcmp(q, etag, n) == 0) return 1;
        if (!*e) return 0;
        q = e + 1;
    }
}

/* Open rel beneath dirfd, resolving in-tree symlinks but never escaping
 * the tree. First choice is openat2(RESOLVE_BENEATH): the kernel resolves
 * the whole path atomically, so there is no check-then-open window and a
 * symlink or ".." that would climb out fails with EXDEV. Kernels without
 * openat2 (or a seccomp policy that filters it) fall back to realpath +
 * the containment check + O_NOFOLLOW on the resolved leaf: still safe for
 * a docroot only the server operator can write, which is the threat model
 * of a static file server. Returns -2 when the fallback must run. */
static int open_beneath(int dfd, const char *rel) {
#ifdef SYS_openat2
    struct open_how how;
    memset(&how, 0, sizeof how);
    /* O_NONBLOCK: a FIFO or device in the docroot must never block the
     * single-threaded loop on open; the S_ISREG check refuses it after */
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    long fd = syscall(SYS_openat2, dfd, rel, &how, (size_t)sizeof how);
    if (fd >= 0) return (int)fd;
    /* ENOSYS: no openat2; EPERM: a seccomp filter; EINVAL: unknown
     * resolve flags - all mean "use the fallback", not "fail" */
    if (errno == ENOSYS || errno == EPERM || errno == EINVAL) return -2;
    return -1;
#else
    (void)dfd; (void)rel;
    return -2;
#endif
}

/* realpath + containment fallback for open_beneath. joined is the
 * docroot-joined candidate path. */
static int open_fallback(const char *joined, const char *docroot_real) {
    char real[4096];
    if (!realpath(joined, real)) return -1;
    if (!mast_path_beneath(real, docroot_real)) return -1;
    /* O_NONBLOCK as above: a FIFO can never wedge the server here */
    return open(real, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
}

int mast_path_beneath(const char *resolved, const char *docroot) {
    size_t dlen = strlen(docroot);
    if (strncmp(resolved, docroot, dlen) != 0) return 0;
    return dlen == 1 || resolved[dlen] == '\0' || resolved[dlen] == '/';
}

/* If-Match uses strong comparison over a comma list: "W/" entries never
 * match, "*" matches any current representation (RFC 9110 13.1.1). */
static int etag_list_matches_strong(const char *list, const char *etag) {
    for (const char *q = list; ;) {
        while (*q == ' ' || *q == '\t' || *q == ',') q++;
        if (!*q) return 0;
        const char *e = q;
        while (*e && *e != ',') e++;
        const char *te = e;
        while (te > q && (te[-1] == ' ' || te[-1] == '\t')) te--;
        size_t n = (size_t)(te - q);
        if (n == 1 && *q == '*') return 1;
        if (n >= 2 && q[0] == '"' && strlen(etag) == n && memcmp(q, etag, n) == 0)
            return 1;
        if (!*e) return 0;
        q = e + 1;
    }
}

int mast_file_open(const char *docroot_real, const MastRequest *req, MastFile *out) {
    memset(out, 0, sizeof *out);
    out->fd = -1;
    size_t rl = strlen(docroot_real);
    if (rl == 0 || rl >= sizeof out->docroot) return 404;
    memcpy(out->docroot, docroot_real, rl + 1);

    const char *suffix = docroot_real[rl - 1] == '/' ? req->path + 1 : req->path;
    char joined[8192];
    int jn = snprintf(joined, sizeof joined, "%s%s", docroot_real, suffix);
    if (jn < 0 || (size_t)jn >= sizeof joined) return 404;

    /* the docroot-relative form, for openat2 beneath-resolution */
    const char *rel = req->path + 1;              /* path starts with '/' */
    char relbuf[MAST_MAX_TARGET + 16];
    if (!*rel) rel = ".";

    int dfd = open(docroot_real, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return 404;
    int fd = open_beneath(dfd, rel);
    if (fd == -2) fd = open_fallback(joined, docroot_real);
    if (fd < 0) { close(dfd); return 404; }   /* no 403: unreadable stays secret */

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); close(dfd); return 404; }

    out->st = st;
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        if (req->path[strlen(req->path) - 1] != '/') {
            /* a directory without the slash: redirect to it */
            int n = snprintf(out->redirect_to, sizeof out->redirect_to, "%s/", req->path);
            if (n < 0 || (size_t)n >= sizeof out->redirect_to) { close(dfd); return 404; }
            out->is_dir = 1;
            close(dfd);
            return 301;
        }
        /* serve its index.html if present */
        int n = snprintf(joined, sizeof joined, "%s%sindex.html", docroot_real, suffix);
        if (n < 0 || (size_t)n >= sizeof joined) { close(dfd); return 404; }
        n = snprintf(relbuf, sizeof relbuf, "%sindex.html",
                     req->path[1] ? req->path + 1 : "");
        if (n < 0 || (size_t)n >= sizeof relbuf) { close(dfd); return 404; }
        fd = open_beneath(dfd, relbuf);
        if (fd == -2) fd = open_fallback(joined, docroot_real);
        close(dfd);
        if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); return 404; }
        if (!S_ISREG(st.st_mode)) { close(fd); return 404; }
    } else {
        close(dfd);
        if (!S_ISREG(st.st_mode)) {
            close(fd);
            return 404;   /* no devices, fifos or sockets over HTTP */
        }
    }

    out->fd = fd;
    out->st = st;
    out->mime = mast_mime(joined);
    snprintf(out->etag, sizeof out->etag, "\"%llx-%llx-%llx-%llx\"",
             (unsigned long long)st.st_ino, (unsigned long long)st.st_size,
             (unsigned long long)st.st_mtim.tv_sec,
             (unsigned long long)st.st_mtim.tv_nsec);
    mast_http_date(out->last_modified, (int64_t)st.st_mtime);

    /* preconditions, in RFC 9110 13.2.2 order: If-Match, then
     * If-Unmodified-Since, then the 304 pair (If-None-Match beats
     * If-Modified-Since) */
    if (req->if_match && !etag_list_matches_strong(req->if_match, out->etag)) {
        close(fd); out->fd = -1; return 412;
    }
    if (req->if_unmodified_since) {
        int64_t d = mast_parse_http_date(req->if_unmodified_since);
        if (d >= 0 && st.st_mtime > d) { close(fd); out->fd = -1; return 412; }
    }
    if (req->if_none_match) {
        if (etag_list_matches(req->if_none_match, out->etag)) {
            close(fd); out->fd = -1; return 304;
        }
    } else if (req->if_modified_since) {
        int64_t since = mast_parse_http_date(req->if_modified_since);
        if (since >= 0 && st.st_mtime <= since) { close(fd); out->fd = -1; return 304; }
    }
    return 200;
}
