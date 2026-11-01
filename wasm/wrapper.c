/* emscripten wrapper: the real parser + responder over an in-memory
 * docroot (MEMFS). The page feeds raw request bytes, reads raw response
 * bytes - the same code paths the epoll server drives over sockets,
 * including keep-alive across calls on one connection handle. */
#define _GNU_SOURCE
#include "../src/http.h"
#include "../src/handle.h"
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    MastParser parser;
    MastRequest req;
    MastBodyReader body;
    int have_head;
    MastBuf out;        /* raw response bytes, drained by the page */
    int closed;         /* the server would close after the last response */
    int requests;       /* served so far on this connection */
} DemoConn;

static void demo_write(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, data, n);
    (void)w;
    close(fd);
}

EMSCRIPTEN_KEEPALIVE
int mast_boot(void) {
    mkdir("/demo", 0755);
    static const char index_html[] =
        "<!doctype html><title>demo</title><h1>hello from mast</h1>\n";
    demo_write("/demo/index.html", index_html, sizeof index_html - 1);
    static const char hello[] = "hello, world - this file lives in the demo's in-memory filesystem\n";
    demo_write("/demo/hello.txt", hello, sizeof hello - 1);
    static const char style[] = "body { font-family: sans-serif; }\n";
    demo_write("/demo/style.css", style, sizeof style - 1);
    static const char data[] = "{\"served\": \"by mast\", \"wasm\": true}\n";
    demo_write("/demo/data.json", data, sizeof data - 1);
    static const char page[] = "<p>a page in a subdirectory</p>\n";
    mkdir("/demo/docs", 0755);
    demo_write("/demo/docs/index.html", page, sizeof page - 1);
    char *big = malloc(65536);
    if (!big) return 1;
    for (int i = 0; i < 65536; i++) big[i] = (char)('A' + (i % 26));
    demo_write("/demo/big.txt", big, 65536);
    free(big);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
DemoConn *mast_conn_new(void) {
    DemoConn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    mast_parser_init(&c->parser);
    mast_buf_init(&c->out);
    return c;
}

EMSCRIPTEN_KEEPALIVE
void mast_conn_free(DemoConn *c) {
    if (!c) return;
    mast_parser_free(&c->parser);
    mast_buf_free(&c->out);
    free(c->req.body);
    free(c);
}

static void conn_reset_request(DemoConn *c) {
    free(c->req.body);
    c->req.body = NULL;
    c->req.body_len = c->req.body_cap = 0;
    memset(&c->body, 0, sizeof c->body);
    c->have_head = 0;
}

/* Append the response (head + memory body + file range) to the out buffer. */
static void emit_response(DemoConn *c, MastResponse *r) {
    mast_buf_put(&c->out, r->head.buf, r->head.len);
    mast_buf_put(&c->out, r->mem.buf, r->mem.len);
    if (r->has_file) {
        char chunk[16384];
        long long left = r->file_len;
        long long off = r->file_off;
        while (left > 0) {
            size_t want = left > (long long)sizeof chunk ? sizeof chunk : (size_t)left;
            ssize_t got = pread(r->file.fd, chunk, want, (off_t)off);
            if (got <= 0) break;
            mast_buf_put(&c->out, chunk, (size_t)got);
            left -= got;
            off += got;
        }
    }
}

/* Feed bytes in 16 KB slices; parse and answer every complete request in
 * them (pipelining works, like the socket server, and a head followed by a
 * big body in one call flips to the body cap the moment the head parses).
 * Returns 0, or 1 if the peer vanished. */
EMSCRIPTEN_KEEPALIVE
int mast_recv(DemoConn *c, const char *data, int n) {
    if (!c || c->closed) return 1;
    size_t off = 0;
    while (off < (size_t)n) {
        size_t slice = (size_t)n - off > 16384 ? 16384 : (size_t)n - off;
        size_t cap = c->have_head ? (size_t)MAST_MAX_BODY + 8192 : MAST_MAX_HEADERS_BYTES;
        if (c->parser.len + slice > cap) {
            /* a response to HEAD never carries a body, even an error page;
             * before the head parses, the buffered prefix still says
             * whether a HEAD is on its way (blank-line skip, like bad()) */
            int to_head;
            if (c->have_head) {
                to_head = c->req.method == MAST_METHOD_HEAD;
            } else {
                size_t boff = 0;
                while (boff < c->parser.len &&
                       (c->parser.buf[boff] == '\r' || c->parser.buf[boff] == '\n'))
                    boff++;
                to_head = c->parser.len - boff >= 5 &&
                    memcmp(c->parser.buf + boff, "HEAD ", 5) == 0;
            }
            mast_reply_error(&c->out, to_head ? NULL : &c->out,
                c->have_head ? 413 : 431,
                c->have_head ? "the body grew past 1 MB"
                             : "the header block grew past 32 KB", NULL, 0);
            c->closed = 1;
            return 0;
        }
        if (mast_parser_append(&c->parser, data + off, slice, cap) != 0) {
            c->closed = 1;
            return 0;
        }
        off += slice;
        for (;;) {
            if (!c->have_head) {
                size_t consumed = 0;
                MastParse r = mast_parser_feed(&c->parser, NULL, 0, &c->req, &consumed);
                if (r == MAST_PARSE_INCOMPLETE) break;
                if (r == MAST_PARSE_BAD) {
                    int status = atoi(c->parser.err_status);
                    mast_reply_error(&c->out,
                        c->parser.err_head_method ? NULL : &c->out,
                        status, c->parser.err_why, NULL, 0);
                    c->closed = 1;
                    return 0;
                }
                c->have_head = 1;
                memmove(c->parser.buf, c->parser.buf + consumed, c->parser.len - consumed);
                c->parser.len -= consumed;
                if (c->req.expect_continue && c->req.minor >= 1 &&
                    (c->req.chunked || c->req.content_length > 0)) {
                    /* HTTP/1.0: the expectation is ignored, no interim */
                    /* same policy as the socket server: interim 100 only
                     * when the body will be accepted */
                    int oversize = !c->req.chunked && c->req.content_length > MAST_MAX_BODY;
                    int acceptable = c->req.path_ok
                        && c->req.method != MAST_METHOD_OTHER
                        && (c->req.method != MAST_METHOD_POST || strcmp(c->req.path, "/echo") == 0)
                        && !oversize;
                    if (acceptable) {
                        mast_buf_puts(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
                    } else if (oversize) {
                        mast_reply_error(&c->out,
                            c->req.method == MAST_METHOD_HEAD ? NULL : &c->out,
                            413, "the body is over 1 MB", NULL, 0);
                        c->closed = 1;
                        return 0;
                    } else {
                        /* the demo conn closes after this answer, so the
                         * head must say Connection: close (RFC 9112 9.6) */
                        c->req.keep_alive = 0;
                        MastResponse resp;
                        mast_response_init(&resp);
                        mast_respond(&c->req, "/demo", &resp);
                        c->requests++;
                        emit_response(c, &resp);
                        mast_response_free(&resp);
                        conn_reset_request(c);
                        c->closed = 1;
                        return 0;
                    }
                }
            }
            if (c->req.chunked || c->req.content_length > 0) {
                size_t bc = 0;
                MastBodyRead br = mast_body_feed(&c->req, &c->body, c->parser.buf, c->parser.len, &bc);
                memmove(c->parser.buf, c->parser.buf + bc, c->parser.len - bc);
                c->parser.len -= bc;
                if (br == MAST_BODY_INCOMPLETE) break;
                if (br == MAST_BODY_TOO_LARGE || br == MAST_BODY_BAD) {
                    mast_reply_error(&c->out,
                        c->req.method == MAST_METHOD_HEAD ? NULL : &c->out,
                        br == MAST_BODY_TOO_LARGE ? 413 : 400,
                        br == MAST_BODY_TOO_LARGE ? "the body grew past 1 MB"
                                                  : "the chunked body framing is malformed",
                        NULL, 0);
                    c->closed = 1;
                    return 0;
                }
            }
            MastResponse resp;
            mast_response_init(&resp);
            mast_respond(&c->req, "/demo", &resp);
            c->requests++;
            emit_response(c, &resp);
            int keep = !resp.close_after;
            mast_response_free(&resp);
            conn_reset_request(c);
            if (!keep) { c->closed = 1; return 0; }
            if (c->parser.len == 0) break;   /* no pipelined request buffered */
        }
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
const char *mast_out_ptr(DemoConn *c) { return c->out.buf ? c->out.buf : ""; }
EMSCRIPTEN_KEEPALIVE
int mast_out_len(DemoConn *c) { return (int)c->out.len; }
EMSCRIPTEN_KEEPALIVE
void mast_out_drain(DemoConn *c) { c->out.len = 0; }
EMSCRIPTEN_KEEPALIVE
int mast_conn_closed(DemoConn *c) { return c->closed; }
EMSCRIPTEN_KEEPALIVE
int mast_conn_requests(DemoConn *c) { return c->requests; }
