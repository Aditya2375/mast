/* files.c - the static-file answer. Path safety is layered: the HTTP
 * parser already rejected NUL/control bytes and escaping "..", and here
 * realpath() must keep the resolved file inside the resolved docroot, so a
 * symlink cannot smuggle a request outside the tree. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "files.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int mast_range_parse(const char *header, long long size,
                     long long *start, long long *len) {
    if (!header) return 0;
    if (strncmp(header, "bytes=", 6) != 0) return -1;   /* a unit we do not serve */
    const char *spec = header + 6;
    if (strchr(spec, ',')) return -1;   /* multi-range: not served (honest limit) */
    const char *dash = strchr(spec, '-');
    if (!dash) return -1;
    char *endp = NULL;
    long long a = -1, b = -1;
    if (dash != spec) {
        a = strtoll(spec, &endp, 10);
        if (endp != dash || a < 0) return -1;
    }
    if (dash[1]) {
        b = strtoll(dash + 1, &endp, 10);
        if (*endp != '\0' || b < 0) return -1;
    }
    if (a == -1) {
        /* the last b bytes */
        if (b == -1 || b <= 0) return -1;
        if (b > size) b = size;
        *start = size - b;
        *len = b;
    } else {
        if (a >= size) return -1;   /* 416 */
        *start = a;
        *len = (b == -1 || b >= size ? size : b + 1) - a;
    }
    if (*len <= 0) return -1;
    return 1;
}

int mast_file_open(const char *docroot_real, const MastRequest *req, MastFile *out) {
    memset(out, 0, sizeof *out);
    out->fd = -1;
    size_t rl = strlen(docroot_real);
    if (rl == 0 || rl >= sizeof out->docroot) return 404;
    memcpy(out->docroot, docroot_real, rl + 1);

    char joined[8192];
    int jn = snprintf(joined, sizeof joined, "%s%s", docroot_real, req->path);
    if (jn < 0 || (size_t)jn >= sizeof joined) return 404;

    int fd = open(joined, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        /* a directory misses O_NOFOLLOW only for symlink loops; plain
         * directories open fine. ENOENT vs EACCES both land here. */
        return 404;   /* no 403: existence of unreadable files stays secret */
    }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 404; }

    /* containment: the opened file must resolve inside the docroot. With
     * O_NOFOLLOW the leaf is no symlink, but a parent could be. */
    char real[4096];
    if (!realpath(joined, real)) { close(fd); return 404; }
    size_t dlen = strlen(docroot_real);
    if (strncmp(real, docroot_real, dlen) != 0 ||
        (real[dlen] != '\0' && real[dlen] != '/')) { close(fd); return 404; }

    out->st = st;
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        if (req->path[strlen(req->path) - 1] != '/') {
            /* a directory without the slash: redirect to it */
            int n = snprintf(out->redirect_to, sizeof out->redirect_to, "%s/", req->path);
            if (n < 0 || (size_t)n >= sizeof out->redirect_to) return 404;
            out->is_dir = 1;
            return 301;
        }
        /* serve its index.html if present */
        int n = snprintf(joined, sizeof joined, "%s%sindex.html", docroot_real, req->path);
        if (n < 0 || (size_t)n >= sizeof joined) return 404;
        fd = open(joined, O_RDONLY | O_NOFOLLOW);
        if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); return 404; }
        if (!S_ISREG(st.st_mode)) { close(fd); return 404; }
        if (!realpath(joined, real)) { close(fd); return 404; }
        if (strncmp(real, docroot_real, dlen) != 0) { close(fd); return 404; }
    } else if (!S_ISREG(st.st_mode)) {
        close(fd);
        return 404;   /* no devices, fifos or sockets over HTTP */
    }

    out->fd = fd;
    out->st = st;
    out->mime = mast_mime(joined);
    snprintf(out->etag, sizeof out->etag, "\"%llx-%llx\"",
             (unsigned long long)st.st_mtime, (unsigned long long)st.st_size);
    mast_http_date(out->last_modified, (int64_t)st.st_mtime);

    /* conditionals (RFC 7232: If-None-Match beats If-Modified-Since) */
    if (req->if_none_match) {
        if (strcmp(req->if_none_match, out->etag) == 0 ||
            strcmp(req->if_none_match, "*") == 0) { close(fd); out->fd = -1; return 304; }
    } else if (req->if_modified_since) {
        int64_t since = mast_parse_http_date(req->if_modified_since);
        if (since >= 0 && st.st_mtime <= since) { close(fd); out->fd = -1; return 304; }
    }
    return 200;
}
