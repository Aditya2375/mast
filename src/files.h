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
    char etag[64];            /* "mtime-size" hex */
    char last_modified[MAST_DATE_LEN];   /* HTTP-date */
    const char *mime;
    int is_dir;               /* path is a directory: redirect to /index */
    char redirect_to[MAST_MAX_TARGET + 16];   /* set when is_dir needs a slash */
} MastFile;

/* Resolve req->path under docroot. Returns an HTTP status: 200 with fd
 * open, 301 with redirect_to set (add the trailing slash), 304 not
 * modified, 404/403 on miss. Any answer besides 200 leaves fd == -1. */
int mast_file_open(const char *docroot, const MastRequest *req, MastFile *out);

/* Single-range parsing: "bytes=A-B", "bytes=A-", "bytes=-B". Returns 1 and
 * fills the start/len out-params (clamped to size), or 0 for none.
 * Returns -1 for unsatisfiable (416). */
int mast_range_parse(const char *header, long long size,
                     long long *start, long long *len);

const char *mast_mime(const char *path);

#endif
