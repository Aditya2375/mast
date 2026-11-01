/* files.h - static-file resolution under a docroot: safe path join, MIME
 * types, ETag/Last-Modified, and Range handling. Everything the HTTP layer
 * needs to answer GET/HEAD without ever touching the filesystem outside
 * the docroot. */
#ifndef MAST_FILES_H
#define MAST_FILES_H

#include "http.h"
#include <sys/stat.h>

typedef struct {
    char docroot[4096];       /* realpath of the served root */
    int fd;                   /* an open fd for the file, or -1 */
    struct stat st;           /* valid when fd >= 0 */
    char etag[96];            /* strong: "ino-size-mtimesec-mtimens" hex */
    char last_modified[MAST_DATE_LEN];   /* HTTP-date */
    const char *mime;
    int is_dir;               /* path is a directory: redirect to /index */
    char redirect_to[MAST_MAX_TARGET + 16];   /* set when is_dir needs a slash */
} MastFile;

/* Resolve req->path under docroot. Returns an HTTP status: 200 with fd
 * open, 301 with redirect_to set (add the trailing slash), 304 not
 * modified, 412 on a failed If-Match / If-Unmodified-Since precondition,
 * 404 on miss (also for unreadable files and escaping symlinks). Any
 * answer besides 200 leaves fd == -1. */
int mast_file_open(const char *docroot, const MastRequest *req, MastFile *out);

/* Containment test, shared by every resolved-path check: resolved must
 * equal docroot or sit beneath it (boundary character included, so
 * "/var/www2" never counts as inside "/var/www"). */
int mast_path_beneath(const char *resolved, const char *docroot);

/* Single-range parsing: "bytes=A-B", "bytes=A-", "bytes=-B". Returns 1 and
 * fills the start/len out-params (clamped to size). Returns 0 when the
 * Range header should be ignored and the full body served (absent, unknown
 * unit, malformed, or multi-range - RFC 9110 permits ignoring Range).
 * Returns -1 for a syntactically valid but unsatisfiable range (416). */
int mast_range_parse(const char *header, long long size,
                     long long *start, long long *len);

const char *mast_mime(const char *path);

#endif
