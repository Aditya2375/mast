/* handle.c - request to response. Static GET/HEAD with conditionals and
 * ranges, POST /echo for request bodies, and exact error pages for every
 * way a request can be bad. HEAD sends the GET headers with no body:
 * Content-Length still says what a GET would have sent (RFC 9110 9.3.2). */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "handle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

void mast_response_init(MastResponse *r) {
    memset(r, 0, sizeof *r);
    mast_buf_init(&r->head);
    mast_buf_init(&r->mem);
    r->file.fd = -1;
}

void mast_response_free(MastResponse *r) {
    mast_buf_free(&r->head);
    mast_buf_free(&r->mem);
    if (r->file.fd >= 0) close(r->file.fd);
    r->file.fd = -1;
}

/* An error response: headers in r->head, the text page in r->mem (unless
 * the request was HEAD - the status line and Content-Length are unchanged,
 * the body stays home). extra is spliced into the head when set. */
static void err(MastResponse *r, const MastRequest *req, int status, const char *why,
                const char *extra) {
    int ka = req->keep_alive;
    mast_reply_error(&r->head,
                     req->method == MAST_METHOD_HEAD ? NULL : &r->mem,
                     status, why, extra, ka);
    r->status = status;
    r->close_after = !ka;
}

/* If-Range demands a strong validator (RFC 9110 13.1.4): the exact
 * entity-tag (a weak W/ tag never holds), or an HTTP-date exactly equal
 * to Last-Modified - one second early or late means "send the whole
 * file", and so does a date in the future. */
static int if_range_holds(const MastRequest *req, const MastFile *f) {
    if (!req->if_range) return 1;
    if (req->if_range[0] == 'W' && req->if_range[1] == '/') return 0;
    if (strcmp(req->if_range, f->etag) == 0) return 1;
    if (req->if_range[0] == '"') return 0;   /* an etag, but not this one */
    int64_t d = mast_parse_http_date(req->if_range);
    return d >= 0 && d == (int64_t)f->st.st_mtime;
}

void mast_respond(const MastRequest *req, const char *docroot_real, MastResponse *r) {
    int ka = req->keep_alive;

    /* OPTIONS: the same Allow list for "*" and for any path, so the
     * answer never depends on which form the client picked (RFC 9110
     * 9.3.7). */
    if (req->method == MAST_METHOD_OPTIONS) {
        mast_buf_puts(&r->head, "HTTP/1.1 204 No Content\r\n");
        char date[MAST_DATE_LEN];
        mast_http_date(date, (int64_t)time(NULL));
        mast_buf_printf(&r->head, "Date: %s\r\nServer: mast\r\n"
                        "Allow: GET, HEAD, POST, OPTIONS\r\n"
                        "Content-Length: 0\r\nConnection: %s\r\n\r\n",
                        date, ka ? "keep-alive" : "close");
        r->status = 204;
        r->close_after = !ka;
        return;
    }
    if (!req->path_ok) {
        err(r, req, 400, "the request target is not a safe path", NULL);
        return;
    }
    if (req->method == MAST_METHOD_OTHER) {
        err(r, req, 405, "this server speaks GET, HEAD, POST and OPTIONS",
            "Allow: GET, HEAD, POST, OPTIONS\r\n");
        return;
    }
    if (req->method == MAST_METHOD_POST) {
        if (strcmp(req->path, "/echo") != 0) {
            err(r, req, 404, "only /echo accepts a body here", NULL);
            return;
        }
        /* an echoed body is no stored representation: every If-Match
         * fails, including "*" (RFC 9110 13.1.1) */
        if (req->if_match) {
            err(r, req, 412, "If-Match failed: /echo stores nothing", NULL);
            return;
        }
        mast_reply_head(&r->head, 200, "OK",
                        req->content_type ? req->content_type : "application/octet-stream",
                        (long long)req->body_len, ka);
        mast_buf_put(&r->mem, req->body ? req->body : "", req->body_len);
        r->status = 200;
        r->close_after = !ka;
        return;
    }

    /* GET / HEAD */
    int st = mast_file_open(docroot_real, req, &r->file);
    if (st == 301) {
        char body[320];
        int n = snprintf(body, sizeof body, "301 Moved Permanently\nLocation: %s\n",
                         r->file.redirect_to);
        mast_buf_printf(&r->head, "HTTP/1.1 301 Moved Permanently\r\n");
        char date[MAST_DATE_LEN];
        mast_http_date(date, (int64_t)time(NULL));
        mast_buf_printf(&r->head, "Date: %s\r\nServer: mast\r\nLocation: %s\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\n"
                        "Connection: %s\r\n\r\n",
                        date, r->file.redirect_to, n, ka ? "keep-alive" : "close");
        if (req->method != MAST_METHOD_HEAD) mast_buf_put(&r->mem, body, (size_t)n);
        r->status = 301;
        r->close_after = !ka;
        return;
    }
    if (st == 304) {
        mast_buf_printf(&r->head, "HTTP/1.1 304 Not Modified\r\n");
        char date[MAST_DATE_LEN];
        mast_http_date(date, (int64_t)time(NULL));
        mast_buf_printf(&r->head, "Date: %s\r\nServer: mast\r\nETag: %s\r\nConnection: %s\r\n\r\n",
                        date, r->file.etag, ka ? "keep-alive" : "close");
        r->status = 304;
        r->close_after = !ka;
        return;
    }
    if (st == 412) {
        err(r, req, 412, "a precondition failed", NULL);
        return;
    }
    if (st == 404) {
        err(r, req, 404, "nothing lives at this path", NULL);
        return;
    }

    /* 200 or 206 */
    long long size = (long long)r->file.st.st_size;
    long long start = 0, len = size;
    int range = 0;
    if (if_range_holds(req, &r->file))
        range = mast_range_parse(req->range, size, &start, &len);
    if (range < 0) {
        close(r->file.fd);
        r->file.fd = -1;
        char cr[96];
        snprintf(cr, sizeof cr, "Content-Range: bytes */%lld\r\n", size);
        err(r, req, 416, "that range does not fit this file", cr);
        return;
    }
    int status = range == 1 ? 206 : 200;
    MastBuf *h = &r->head;
    mast_buf_printf(h, "HTTP/1.1 %d %s\r\n", status, mast_reason(status));
    char date[MAST_DATE_LEN];
    mast_http_date(date, (int64_t)time(NULL));
    mast_buf_printf(h, "Date: %s\r\nServer: mast\r\n", date);
    mast_buf_printf(h, "Content-Type: %s\r\n", r->file.mime);
    mast_buf_printf(h, "Content-Length: %lld\r\n", (long long)len);
    mast_buf_printf(h, "ETag: %s\r\nLast-Modified: %s\r\n", r->file.etag, r->file.last_modified);
    mast_buf_puts(h, "Accept-Ranges: bytes\r\n");
    if (range == 1)
        mast_buf_printf(h, "Content-Range: bytes %lld-%lld/%lld\r\n",
                        start, start + len - 1, size);
    mast_buf_printf(h, "Connection: %s\r\n\r\n", ka ? "keep-alive" : "close");
    r->status = status;
    r->close_after = !ka;
    if (req->method == MAST_METHOD_GET && len > 0) {
        r->has_file = 1;
        r->file_off = start;
        r->file_len = len;
    } else {
        close(r->file.fd);
        r->file.fd = -1;
    }
}
