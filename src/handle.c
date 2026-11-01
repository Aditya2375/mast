/* handle.c - request to response. Static GET/HEAD with conditionals and
 * ranges, POST /echo for request bodies, and exact error pages for every
 * way a request can be bad. HEAD sends the GET headers with no body. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "handle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void err(MastResponse *r, int status, const char *why, int keep_alive) {
    mast_reply_error(&r->head, status, why, keep_alive);
    r->status = status;
    r->close_after = !keep_alive;
}

void mast_respond(const MastRequest *req, const char *docroot_real, MastResponse *r) {
    int ka = req->keep_alive;
    if (!req->path_ok) {
        err(r, 400, "the request target is not a safe path", ka);
        return;
    }
    if (req->method == MAST_METHOD_OTHER) {
        char allow[160];
        snprintf(allow, sizeof allow, "Allow: GET, HEAD, POST\r\n");
        mast_reply_error(&r->head, 405, "this server speaks GET, HEAD and POST", ka);
        /* splice the Allow header before the blank line */
        r->head.len -= 2;
        mast_buf_puts(&r->head, allow);
        mast_buf_puts(&r->head, "\r\n");
        r->status = 405;
        r->close_after = !ka;
        return;
    }
    if (req->method == MAST_METHOD_POST) {
        if (strcmp(req->path, "/echo") != 0) {
            err(r, 404, "only /echo accepts a body here", ka);
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
    if (st == 404) {
        err(r, 404, "nothing lives at this path", ka);
        return;
    }

    /* 200 or 206 */
    long long size = (long long)r->file.st.st_size;
    long long start = 0, len = size;
    int range = mast_range_parse(req->range, size, &start, &len);
    if (range < 0) {
        close(r->file.fd);
        r->file.fd = -1;
        char cr[96];
        snprintf(cr, sizeof cr, "bytes */%lld", size);
        mast_reply_error(&r->head, 416, "that range does not fit this file", ka);
        r->head.len -= 2;
        mast_buf_printf(&r->head, "Content-Range: %s\r\n\r\n", cr);
        r->status = 416;
        r->close_after = !ka;
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
