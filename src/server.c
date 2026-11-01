/* server.c - one thread, one epoll loop, every socket non-blocking. A
 * connection is a small state machine: read the head, read the body,
 * write the response (headers, then the file range via sendfile), then
 * keep-alive or close. Slow clients are cut off by per-state timeouts
 * measured from real progress, so a trickling slowloris or a client that
 * stops reading never pins a slot. A closing connection lingers a moment
 * to drain the peer's unread bytes, so the final response is never cut
 * off by an RST. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "handle.h"
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    CONN_READ_HEAD, CONN_READ_BODY, CONN_WRITE, CONN_DRAIN_CLOSE
} ConnState;

#define DRAIN_TIMEOUT_MS 5000
/* Fairness: one connection may complete at most this many responses (or
 * bytes) per loop pass; more buffered work yields its turn so a fast
 * pipelining client cannot starve the rest. */
#define TURN_MAX_RESPONSES 8
#define TURN_MAX_BYTES (512 * 1024)
/* Lingering closes share a small budget, so clients that never FIN cannot
 * pin every connection slot in the drain state. */
#define DRAIN_MAX_SHARE 4    /* of max_conn, capped at 16 */
#define SHUTDOWN_GRACE_MS 5000   /* in-flight writes get this, then exit */

static int g_drainers = 0;

/* Total bytes held in per-connection dynamic buffers (parse backlog,
 * request bodies and response heads/pages). Bounded by the -M budget: a
 * connection that
 * would push past it gets a 503 and closes instead of growing without
 * limit, and a failed allocation fails that one connection, never the
 * process. */
static size_t g_dyn_bytes = 0;

static size_t dyn_budget(const MastConfig *cfg) {
    return (size_t)cfg->mem_budget_mb * 1024 * 1024;
}


typedef struct {
    int fd;
    ConnState state;
    MastParser parser;
    MastRequest req;
    MastBodyReader body;
    int have_head;          /* the request line + headers are parsed */
    MastResponse resp;
    size_t out_sent;        /* bytes of resp.head/mem written */
    int responding;
    int peer_done;      /* the peer sent FIN: answer, then close */
    long long file_sent;
    int requests;       /* completed requests on this connection */
    long long started_ms, last_ms;   /* last_ms: last real progress */
    long long req_start_ms;    /* first byte of the current request's head */
    long long body_start_ms;   /* head parsed: the body phase begins */
    long long resp_start_ms;   /* response begun: the write phase begins */
    long long drain_deadline;
    int turn_responses;     /* completed this loop pass (fairness) */
    long long turn_bytes;   /* written this loop pass (fairness) */
    int yielded;            /* budget spent with work pending: re-run */
    size_t dyn_counted;     /* bytes this conn added to g_dyn_bytes */
    size_t parser_counted;  /* of parser.cap: the part already in g_dyn_bytes */
    int head_waiting;       /* last head parse said INCOMPLETE: skip
                             * re-parsing recv batches that hold no LF */
    struct sockaddr_in peer;
} Conn;

/* only growth through the monitored paths (parse backlog, request bodies
 * and response builds) is counted, and exactly that is handed back on close */
static void dyn_return(Conn *c) {
    g_dyn_bytes -= c->dyn_counted;
    c->dyn_counted = 0;
}

/* Charge a just-built response's buffer growth (head + mem, error pages
 * and interims included) so the budget ledger sees every byte it will
 * later hand back. */
static void dyn_charge_resp(Conn *c, size_t old_hcap, size_t old_mcap) {
    size_t grew = (c->resp.head.cap - old_hcap) + (c->resp.mem.cap - old_mcap);
    g_dyn_bytes += grew;
    c->dyn_counted += grew;
}

/* Is this error going to a HEAD? In READ_BODY the head already parsed and
 * the method answers directly; before that, the parse buffer still holds
 * the failed head's prefix - skip any leading blank lines, mirroring
 * bad() in http.c. A response to HEAD never carries a body, even from
 * these pre-parser error paths. */
static int err_to_head(const Conn *c) {
    if (c->state == CONN_READ_BODY)
        return c->req.method == MAST_METHOD_HEAD;
    size_t off = 0;
    while (off < c->parser.len &&
           (c->parser.buf[off] == '\r' || c->parser.buf[off] == '\n'))
        off++;
    return c->parser.len - off >= 5 &&
           memcmp(c->parser.buf + off, "HEAD ", 5) == 0;
}

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
    (void)sig;
    if (g_stop) _exit(1);   /* a second SIGINT/SIGTERM forces exit */
    g_stop = 1;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0;
}

static void conn_free(Conn *c) {
    dyn_return(c);
    mast_parser_free(&c->parser);
    mast_response_free(&c->resp);
    free(c->req.body);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

/* Request bytes are attacker-controlled: escape anything a terminal or a
 * log pipeline could trip on before it reaches stdout. */
static void log_escaped(const char *s) {
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        if (*q < 32 || *q == 127 || *q > 126) fprintf(stdout, "\\x%02x", *q);
        else fputc(*q, stdout);
    }
}

static void log_access(Conn *c) {
    char ip[32] = "?";
    inet_ntop(AF_INET, &c->peer.sin_addr, ip, sizeof ip);
    long long bytes = (long long)c->resp.head.len + c->resp.mem.len + c->file_sent;
    fprintf(stdout, "%s ", ip);
    log_escaped(c->req.method_raw);
    fputc(' ', stdout);
    if (c->req.target) log_escaped(c->req.target); else fputc('-', stdout);
    fprintf(stdout, " -> %d (%lld bytes, %lld ms)\n",
            c->resp.status, bytes, now_ms() - c->started_ms);
    fflush(stdout);
}

/* Reset per-request state for the next pipelined request on this conn. */
static void conn_next_request(Conn *c) {
    dyn_return(c);
    /* the parse buffer lives on across requests (pipelined bytes sit in
     * it), so its counted bytes stay in the budget */
    g_dyn_bytes += c->parser_counted;
    c->dyn_counted = c->parser_counted;
    c->head_waiting = 0;
    mast_response_free(&c->resp);
    mast_response_init(&c->resp);
    free(c->req.body);
    c->req.body = NULL;
    c->req.body_len = c->req.body_cap = 0;
    memset(&c->body, 0, sizeof c->body);
    c->have_head = 0;
    c->responding = 0;
    c->out_sent = 0;
    c->file_sent = 0;
    c->state = CONN_READ_HEAD;
    c->req_start_ms = c->body_start_ms = c->resp_start_ms = 0;
}

/* Mark the response phase: the absolute write deadline counts from here. */
static void enter_write(Conn *c) {
    c->state = CONN_WRITE;
    c->resp_start_ms = now_ms();
}

/* Write as much of the pending response as the socket takes. Returns 1
 * when the response is fully sent, 0 when more writes are needed, -1 when
 * the connection is dead. Every loop iteration must make progress or
 * return: sendfile reporting 0 means the file shrank under us, the
 * promised Content-Length can never arrive, and the only honest answer is
 * to close. */
static int conn_flush(Conn *c) {
    while (c->out_sent < c->resp.head.len + c->resp.mem.len) {
        const char *p;
        size_t n;
        if (c->out_sent < c->resp.head.len) {
            p = c->resp.head.buf + c->out_sent;
            n = c->resp.head.len - c->out_sent;
        } else {
            p = c->resp.mem.buf + (c->out_sent - c->resp.head.len);
            n = c->resp.mem.len - (c->out_sent - c->resp.head.len);
        }
        ssize_t w = send(c->fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (w == 0) return 0;
        c->out_sent += (size_t)w;
        c->turn_bytes += w;
        c->last_ms = now_ms();
    }
    while (c->resp.has_file && c->file_sent < c->resp.file_len) {
        off_t off = (off_t)(c->resp.file_off + c->file_sent);
        size_t want = (size_t)(c->resp.file_len - c->file_sent);
        ssize_t w = sendfile(c->fd, c->resp.file.fd, &off, want);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (w == 0) return -1;   /* truncated mid-transfer: cannot complete */
        c->file_sent += w;
        c->turn_bytes += w;
        c->last_ms = now_ms();
    }
    return 1;
}

/* Feed buffered bytes through the parser; may produce a response. Returns
 * 1 when a response is pending, 0 when more bytes are needed. */
static int conn_process(Conn *c, const MastConfig *cfg) {
    for (;;) {
        if (!c->have_head) {
            size_t consumed = 0;
            if (c->req_start_ms == 0) c->req_start_ms = now_ms();
            MastParse r = mast_parser_feed(&c->parser, NULL, 0, &c->req, &consumed);
            c->head_waiting = r == MAST_PARSE_INCOMPLETE;
            if (r == MAST_PARSE_INCOMPLETE) return 0;   /* wait for bytes */
            if (r == MAST_PARSE_BAD) {
                int status = atoi(c->parser.err_status);
                /* a response to HEAD never carries a body (RFC 9110
                 * 9.3.2), even when the request then failed to parse */
                size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                mast_reply_error(&c->resp.head,
                                 c->parser.err_head_method ? NULL : &c->resp.mem,
                                 status, c->parser.err_why, NULL, 0);
                dyn_charge_resp(c, oh, om);
                c->resp.status = status;
                c->resp.close_after = 1;
                c->responding = 1;
                enter_write(c);
                return 1;   /* answer, then close */
            }
            /* a parsed head: drop the consumed bytes from the buffer */
            c->have_head = 1;
            c->body_start_ms = now_ms();
            memmove(c->parser.buf, c->parser.buf + consumed, c->parser.len - consumed);
            c->parser.len -= consumed;
            c->state = CONN_READ_BODY;
            if (c->req.expect_continue && c->req.minor >= 1 &&
                (c->req.chunked || c->req.content_length > 0)) {
                /* HTTP/1.0 has no 100-continue: the expectation is ignored */
                /* RFC 9110 10.1.1: the interim 100 only when the body will
                 * be accepted; otherwise the final error right away, and
                 * the unread body must not poison a follow-up request. */
                int oversize = !c->req.chunked && c->req.content_length > MAST_MAX_BODY;
                int acceptable = c->req.path_ok
                    && c->req.method != MAST_METHOD_OTHER
                    && (c->req.method != MAST_METHOD_POST || strcmp(c->req.path, "/echo") == 0)
                    && !oversize;
                if (acceptable) {
                    /* send the interim right away: the client waits for it
                     * before it will ever send the body. Anything the
                     * socket cannot take stays buffered ahead of the final
                     * response head, which is the correct wire order. */
                    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    ssize_t w = send(c->fd, interim, sizeof interim - 1, MSG_NOSIGNAL);
                    if (w < 0) w = 0;
                    if ((size_t)w < sizeof interim - 1) {
                        size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                        mast_buf_put(&c->resp.head, interim + w, sizeof interim - 1 - (size_t)w);
                        dyn_charge_resp(c, oh, om);
                    }
                    c->last_ms = now_ms();
                } else if (oversize) {
                    size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                    mast_reply_error(&c->resp.head, &c->resp.mem, 413,
                                     "the body is over 1 MB", NULL, 0);
                    dyn_charge_resp(c, oh, om);
                    c->resp.status = 413;
                    c->resp.close_after = 1;
                    c->responding = 1;
                    enter_write(c);
                    return 1;
                } else {
                    /* we close after this answer, so the head must say
                     * so: keep_alive off before building the response */
                    c->req.keep_alive = 0;
                    size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                    mast_respond(&c->req, cfg->docroot_real, &c->resp);
                    dyn_charge_resp(c, oh, om);
                    c->resp.close_after = 1;
                    c->responding = 1;
                    enter_write(c);
                    return 1;
                }
            }
        }
        /* body */
        if (c->req.chunked || c->req.content_length > 0) {
            /* the feed grows the body buffer to an exact doubling target:
             * project that target, or a batch larger than the current cap
             * slips past the budget check (grow computes to 0) */
            size_t proj;
            if (c->req.chunked) {
                size_t want = c->req.body_len + c->parser.len;
                proj = c->req.body_cap ? c->req.body_cap : 4096;
                while (proj < want) proj *= 2;
                if (proj > MAST_MAX_BODY + 1) proj = MAST_MAX_BODY + 1;
            } else {
                proj = (size_t)c->req.content_length + 1;
            }
            size_t grow = proj > c->req.body_cap ? proj - c->req.body_cap : 0;
            size_t old_cap = c->req.body_cap;
            if (grow && g_dyn_bytes + grow > dyn_budget(cfg)) {
                size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                mast_reply_error(&c->resp.head,
                                 c->req.method == MAST_METHOD_HEAD ? NULL : &c->resp.mem,
                                 503, "the server is out of buffer room, retry shortly",
                                 "Retry-After: 1\r\n", 0);
                dyn_charge_resp(c, oh, om);
                c->resp.status = 503;
                c->resp.close_after = 1;
                c->responding = 1;
                enter_write(c);
                return 1;
            }
            size_t bconsumed = 0;
            MastBodyRead br = mast_body_feed(&c->req, &c->body,
                                             c->parser.buf, c->parser.len, &bconsumed);
            if (c->req.body_cap > old_cap) {
                g_dyn_bytes += c->req.body_cap - old_cap;
                c->dyn_counted += c->req.body_cap - old_cap;
            }
            memmove(c->parser.buf, c->parser.buf + bconsumed, c->parser.len - bconsumed);
            c->parser.len -= bconsumed;
            if (br == MAST_BODY_INCOMPLETE) return 0;
            if (br == MAST_BODY_TOO_LARGE) {
                size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                mast_reply_error(&c->resp.head,
                                 c->req.method == MAST_METHOD_HEAD ? NULL : &c->resp.mem,
                                 413, "the body grew past 1 MB", NULL, 0);
                dyn_charge_resp(c, oh, om);
                c->resp.status = 413;
                c->resp.close_after = 1;
                c->responding = 1;
                enter_write(c);
                return 1;
            }
            if (br == MAST_BODY_NOMEM) {
                size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                mast_reply_error(&c->resp.head,
                                 c->req.method == MAST_METHOD_HEAD ? NULL : &c->resp.mem,
                                 503, "the server is out of buffer room, retry shortly",
                                 "Retry-After: 1\r\n", 0);
                dyn_charge_resp(c, oh, om);
                c->resp.status = 503;
                c->resp.close_after = 1;
                c->responding = 1;
                enter_write(c);
                return 1;
            }
            if (br == MAST_BODY_BAD) {
                size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                mast_reply_error(&c->resp.head,
                                 c->req.method == MAST_METHOD_HEAD ? NULL : &c->resp.mem,
                                 400, "the chunked body framing is malformed", NULL, 0);
                dyn_charge_resp(c, oh, om);
                c->resp.status = 400;
                c->resp.close_after = 1;
                c->responding = 1;
                enter_write(c);
                return 1;
            }
        }
        /* a complete request; the cap-th response says Connection: close */
        if (c->requests + 1 >= cfg->max_requests) c->req.keep_alive = 0;
        if (c->req.method == MAST_METHOD_POST &&
            g_dyn_bytes + c->req.body_len > dyn_budget(cfg)) {
            /* the echo would copy the body into the response buffer */
            size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
            mast_reply_error(&c->resp.head, &c->resp.mem, 503,
                             "the server is out of buffer room, retry shortly",
                             "Retry-After: 1\r\n", 0);
            dyn_charge_resp(c, oh, om);
            c->resp.status = 503;
            c->resp.close_after = 1;
            c->responding = 1;
            enter_write(c);
            return 1;
        }
        size_t old_hcap = c->resp.head.cap, old_mcap = c->resp.mem.cap;
        mast_respond(&c->req, cfg->docroot_real, &c->resp);
        dyn_charge_resp(c, old_hcap, old_mcap);
        c->responding = 1;
        enter_write(c);
        return 1;
    }
}

/* Drain available bytes into the parse buffer, parsing as they land so a
 * head followed by a big body in one flight flips to the body cap in time.
 * The drain always reads to EAGAIN: a FIN arriving with the last request
 * byte must be consumed now, because edge-triggered epoll will not report
 * it twice. Once a response is pending, further pipelined bytes stay
 * buffered (and capped) until the flush completes. Returns 1 when a
 * response is pending, 0 when drained, -1 when dead. */
static int conn_read_drain(Conn *c, const MastConfig *cfg) {
    char tmp[16384];
    int pending = 0;
    for (;;) {
        /* the READ_BODY arm is defensive: mast_body_feed drains the
         * backlog into the body buffer after every batch (an unterminated
         * chunk-size line caps at 1023 bytes inside the reader), so the
         * backlog cannot reach this cap in practice */
        size_t cap = c->state == CONN_READ_HEAD
            ? MAST_MAX_HEADERS_BYTES
            : (size_t)MAST_MAX_BODY + 8192;
        if (c->parser.len >= cap) {
            /* a pipelined flood past the cap while a response is pending
             * must not kill the pending answer: stop reading and let TCP
             * backpressure hold the client until the flush catches up */
            if (pending) return 1;
            /* over a cap: answer cleanly; the lingering close then drains
             * the flood so the answer is not cut off by an RST */
            int status = c->state == CONN_READ_HEAD ? 431 : 413;
            size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
            mast_reply_error(&c->resp.head, err_to_head(c) ? NULL : &c->resp.mem, status,
                status == 431 ? "the header block grew past 32 KB"
                              : "the body grew past 1 MB", NULL, 0);
            dyn_charge_resp(c, oh, om);
            c->resp.status = status;
            c->resp.close_after = 1;
            c->responding = 1;
            c->state = CONN_WRITE;
            return 1;
        }
        size_t room = cap - c->parser.len;
        ssize_t r = recv(c->fd, tmp, room < sizeof tmp ? room : sizeof tmp, 0);
        if (r <= 0) {
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (r < 0 && errno == EINTR) continue;
            if (r == 0) c->peer_done = 1;   /* FIN: the request may still be whole */
            else return -1;   /* a hard error mid-request */
            break;
        }
        c->last_ms = now_ms();
        if (c->state == CONN_READ_HEAD && c->parser.len == 0 && c->req_start_ms == 0)
            c->req_start_ms = c->last_ms;   /* the head's absolute deadline starts here */
        /* the parse backlog counts against the -M budget: refuse the
         * growth, not the bytes (a small batch fitting the current cap
         * always lands, so a plain request is never rejected for this) */
        size_t pneed = c->parser.len + (size_t)r;
        if (pneed > c->parser.cap) {
            size_t ptarget = c->parser.cap ? c->parser.cap : 4096;
            while (ptarget < pneed) ptarget *= 2;
            size_t pgrow = ptarget - c->parser.cap;
            if (pgrow && g_dyn_bytes + pgrow > dyn_budget(cfg)) {
                if (pending) return 1;   /* let the pending answer flush first */
                size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                mast_reply_error(&c->resp.head, err_to_head(c) ? NULL : &c->resp.mem,
                                 503, "the server is out of buffer room, retry shortly",
                                 "Retry-After: 1\r\n", 0);
                dyn_charge_resp(c, oh, om);
                c->resp.status = 503;
                c->resp.close_after = 1;
                c->responding = 1;
                c->state = CONN_WRITE;
                return 1;
            }
        }
        if (mast_parser_append(&c->parser, tmp, (size_t)r, cap) != 0) return -1;
        if (c->parser.cap > c->parser_counted) {
            g_dyn_bytes += c->parser.cap - c->parser_counted;
            c->dyn_counted += c->parser.cap - c->parser_counted;
            c->parser_counted = c->parser.cap;
        }
        /* a batch with no LF cannot complete any head line: when the last
         * parse already said INCOMPLETE, re-parsing from byte 0 would only
         * re-read the same bytes (an O(n^2) CPU pin under a byte-dribble).
         * The 8 KB request-line cap is length-only, so crossing it still
         * gets its 414 by running the parser once the buffer passes it. */
        int has_lf = memchr(tmp, '\n', (size_t)r) != NULL;
        /* past 8 KB the parser must still get its shot at a 414 - but
         * only while the request line itself has no LF yet. A complete
         * request line followed by a growing header line has an LF in the
         * first 8 KB + 1 bytes, so the no-new-LF skip keeps holding and
         * the dribble stays O(1) per byte instead of re-parsing the whole
         * backlog per recv */
        int line_maybe_over = c->parser.len > MAST_MAX_REQUEST_LINE &&
            !memchr(c->parser.buf, '\n',
                    c->parser.len < MAST_MAX_REQUEST_LINE + 1
                        ? c->parser.len : MAST_MAX_REQUEST_LINE + 1);
        if (!pending &&
            (c->state != CONN_READ_HEAD || !c->head_waiting || has_lf ||
             line_maybe_over))
            pending = conn_process(c, cfg);
    }
    return pending;
}

/* The response is out and the connection ends: half-close our side so the
 * peer sees a clean FIN after the full response, then drain whatever it
 * still sends until its own FIN or a short deadline. This keeps unread
 * request bytes from turning our close into an RST that destroys the
 * response before the peer can read it. */
static int conn_linger_close(Conn *c, const MastConfig *cfg) {
    int budget = cfg->max_conn / DRAIN_MAX_SHARE;
    if (budget < 1) budget = 1;
    if (budget > 16) budget = 16;
    if (g_drainers >= budget) return 0;   /* budget spent: close at once */
    g_drainers++;
    shutdown(c->fd, SHUT_WR);
    c->state = CONN_DRAIN_CLOSE;
    c->drain_deadline = now_ms() + DRAIN_TIMEOUT_MS;
    return 1;
}

/* Service one connection: events is the epoll mask (EPOLLIN|EPOLLOUT for
 * a fairness re-run after a yield). Returns 1 when the connection must be
 * closed by the caller. A pipelining client gets a bounded budget of
 * responses and bytes per pass; spending it with work still pending sets
 * c->yielded, and the main loop re-runs the connection after the other
 * connections had their turn. */
static int conn_service(Conn *c, const MastConfig *cfg, uint32_t events) {
    c->turn_responses = 0;
    c->turn_bytes = 0;
    if ((events & EPOLLERR) && c->state != CONN_WRITE) return 1;
    if (c->state == CONN_DRAIN_CLOSE) {
        /* lingering close: discard whatever the peer still sends, but
         * bounded per call - a continuously readable peer gets the rest
         * discarded on later passes and still hits the 5s deadline */
        char junk[8192];
        ssize_t r;
        size_t drained = 0;
        while ((r = recv(c->fd, junk, sizeof junk, 0)) > 0) {
            drained += (size_t)r;
            if (drained >= 262144) break;
        }
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return 1;
        if (now_ms() >= c->drain_deadline) return 1;
        return 0;
    }
    if ((events & (EPOLLIN | EPOLLRDHUP)) &&
        (c->state == CONN_READ_HEAD || c->state == CONN_READ_BODY)) {
        int has = conn_read_drain(c, cfg);
        if (has < 0) return 1;
        if (has) goto write_side;
        if (c->peer_done) return 1;   /* an incomplete request from a gone peer */
    }
    if ((events & EPOLLOUT) && c->state == CONN_WRITE) {
    write_side:
        ;
        int done = conn_flush(c);
        if (done < 0) return 1;
        if (done) {
            log_access(c);
            c->requests++;
            c->turn_responses++;
            /* FIN only ends the connection once no pipelined request is
             * still buffered */
            if (c->resp.close_after) return conn_linger_close(c, cfg) ? 0 : 1;
            if (c->peer_done && c->parser.len == 0) return 1;
            conn_next_request(c);
            /* pipelined bytes may already sit in the buffer; a flood held
             * back by the cap (backpressure) resumes reading now */
            int has = 0;
            if (c->parser.len > 0) has = conn_process(c, cfg);
            if (!has && !c->peer_done) has = conn_read_drain(c, cfg);
            if (has) {
                if (c->turn_responses >= TURN_MAX_RESPONSES ||
                    c->turn_bytes >= TURN_MAX_BYTES) {
                    c->yielded = 1;   /* continue on a later loop pass */
                    return 0;
                }
                goto write_side;
            }
            if (c->peer_done && c->parser.len == 0) return 1;
        }
    }
    return 0;
}

int mast_serve(const MastConfig *cfg) {
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)cfg->port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 128) < 0) { perror("listen"); return 1; }
    if (set_nonblock(lfd) < 0) { perror("fcntl"); return 1; }

    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); return 1; }
    struct epoll_event lev;
    memset(&lev, 0, sizeof lev);
    lev.events = EPOLLIN | EPOLLET;
    lev.data.fd = lfd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &lev) < 0) { perror("epoll_ctl"); return 1; }

    Conn **conns = calloc((size_t)cfg->max_conn, sizeof *conns);
    if (!conns) { fprintf(stderr, "mast: out of memory\n"); return 70; }
    int nconn = 0;
    fprintf(stdout, "mast: serving %s on http://0.0.0.0:%d (%d max connections)\n",
            cfg->docroot_real, cfg->port, cfg->max_conn);
    fflush(stdout);

    struct epoll_event evs[128];
    int work_pending = 0;   /* a yielded connection is owed another pass */
    long long stop_since = 0;   /* when shutdown began, for the grace cap */
    while (!g_stop || nconn > 0) {
        int nev = epoll_wait(ep, evs, 128, g_stop ? 100 : (work_pending ? 0 : 500));
        if (nev < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < nev; i++) {
            int fd = evs[i].data.fd;
            if (fd == lfd) {
                if (g_stop) continue;
                for (;;) {
                    struct sockaddr_in peer;
                    socklen_t plen = sizeof peer;
                    int cfd = accept4(lfd, (struct sockaddr *)&peer, &plen, SOCK_NONBLOCK);
                    if (cfd < 0) {
                        if (errno == EMFILE || errno == ENFILE)
                            /* out of fds (see -c vs ulimit -n in the
                             * README): re-arm so a future close re-edges
                             * the listener instead of stalling accepts */
                            epoll_ctl(ep, EPOLL_CTL_MOD, lfd, &lev);
                        break;   /* EAGAIN: the edge is drained */
                    }
                    if (nconn >= cfg->max_conn) {
                        /* full: say so best-effort, then close - a silent
                         * RST would look like a network fault */
                        static const char body[] =
                            "503 Service Unavailable\nthe server is full - try again later\n";
                        char busy[256];
                        int bl = snprintf(busy, sizeof busy,
                            "HTTP/1.1 503 Service Unavailable\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                            sizeof body - 1, body);
                        if (bl > 0) {
                            if ((size_t)bl > sizeof busy) bl = sizeof busy;
                            (void)!send(cfd, busy, (size_t)bl, MSG_NOSIGNAL);
                        }
                        close(cfd);
                        continue;
                    }
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    Conn *c = calloc(1, sizeof *c);
                    if (!c) { close(cfd); continue; }
                    c->fd = cfd;
                    c->state = CONN_READ_HEAD;
                    c->peer = peer;
                    mast_parser_init(&c->parser);
                    mast_response_init(&c->resp);
                    c->started_ms = c->last_ms = now_ms();
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof cev);
                    cev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
                    cev.data.ptr = c;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        conn_free(c);
                        free(c);
                        continue;
                    }
                    for (int k = 0; k < cfg->max_conn; k++)
                        if (!conns[k]) { conns[k] = c; break; }
                    nconn++;
                }
                continue;
            }
            Conn *c = evs[i].data.ptr;
            if (conn_service(c, cfg, evs[i].events)) {
                for (int k = 0; k < cfg->max_conn; k++)
                    if (conns[k] == c) { conns[k] = NULL; break; }
                if (c->state == CONN_DRAIN_CLOSE) g_drainers--;
                epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                conn_free(c);
                free(c);
                nconn--;
            }
        }
        /* fairness pass: every yielded connection gets one fresh budget
         * after the others had their turn */
        work_pending = 0;
        for (int k = 0; k < cfg->max_conn; k++) {
            Conn *c = conns[k];
            if (!c || !c->yielded) continue;
            c->yielded = 0;
            if (conn_service(c, cfg, EPOLLIN | EPOLLOUT)) {
                if (c->state == CONN_DRAIN_CLOSE) g_drainers--;
                epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                conn_free(c);
                free(c);
                conns[k] = NULL;
                nconn--;
            } else if (c->yielded) {
                work_pending = 1;
            }
        }
        /* timeout sweep: every limit counts from real progress, so junk
         * events on the socket can never keep a dead connection alive.
         * During shutdown, anything not mid-response closes at once. */
        long long t = now_ms();
        if (g_stop) {
            if (!stop_since) stop_since = t;
            /* graceful shutdown lets in-flight responses finish, but only
             * within a grace cap: one non-reading client cannot hold the
             * process up to the full write timeout */
            if (t - stop_since > SHUTDOWN_GRACE_MS) break;
        }
        for (int k = 0; k < cfg->max_conn; k++) {
            Conn *c = conns[k];
            if (!c) continue;
            if (c->state == CONN_DRAIN_CLOSE) {
                if (g_stop || t >= c->drain_deadline) {
                    g_drainers--;
                    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                    conn_free(c);
                    free(c);
                    conns[k] = NULL;
                    nconn--;
                }
                continue;
            }
            if (g_stop && c->state != CONN_WRITE) {
                /* shutdown: idle and half-read connections close at once;
                 * only in-flight responses finish writing */
                epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                conn_free(c);
                free(c);
                conns[k] = NULL;
                nconn--;
                continue;
            }
            /* no connection outlives its age cap, whatever its phase */
            if (t - c->started_ms > cfg->max_age_ms) {
                epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                conn_free(c);
                free(c);
                conns[k] = NULL;
                nconn--;
                continue;
            }
            /* every phase has both a stall timeout (refreshed by real
             * progress) and an absolute deadline progress cannot move:
             * a trickler sending one byte at a time dies on schedule */
            long long limit;
            long long abs_deadline = 0;
            if (c->state == CONN_READ_HEAD) {
                if (c->requests > 0 && c->parser.len == 0) {
                    limit = cfg->idle_timeout_ms;   /* idle keep-alive */
                } else {
                    limit = cfg->header_timeout_ms;
                    if (c->req_start_ms)
                        abs_deadline = c->req_start_ms + cfg->header_timeout_ms;
                }
            } else if (c->state == CONN_READ_BODY) {
                limit = cfg->body_timeout_ms;
                abs_deadline = (c->body_start_ms ? c->body_start_ms : c->last_ms)
                    + cfg->body_timeout_ms;
            } else {
                limit = cfg->write_timeout_ms;
                abs_deadline = (c->resp_start_ms ? c->resp_start_ms : c->last_ms)
                    + (long long)cfg->write_timeout_ms * 8;
            }
            if (t - c->last_ms > limit || (abs_deadline && t > abs_deadline)) {
                /* a head or body that timed out mid-request still gets a
                 * 408 when nothing was written yet; a merely idle or a
                 * silent connection closes quietly */
                if (!g_stop && !c->responding && c->resp.head.len == 0 &&
                    (c->state == CONN_READ_BODY || c->parser.len > 0)) {
                    size_t oh = c->resp.head.cap, om = c->resp.mem.cap;
                    mast_reply_error(&c->resp.head, err_to_head(c) ? NULL : &c->resp.mem,
                                     408, "the request took too long", NULL, 0);
                    dyn_charge_resp(c, oh, om);
                    c->resp.status = 408;
                    c->resp.close_after = 1;
                    c->responding = 1;
                    c->state = CONN_WRITE;
                    c->last_ms = t;   /* the write timeout now governs */
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof cev);
                    cev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
                    cev.data.ptr = c;
                    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &cev);   /* re-edge the writable socket */
                    continue;
                }
                epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                conn_free(c);
                free(c);
                conns[k] = NULL;
                nconn--;
            }
        }
        if (g_stop && nconn == 0) break;
    }
    epoll_ctl(ep, EPOLL_CTL_DEL, lfd, NULL);
    close(lfd);
    close(ep);
    /* the grace-cap break can leave connections open; the process is
     * exiting, but close them cleanly anyway */
    for (int k = 0; k < cfg->max_conn; k++) {
        if (conns[k]) { conn_free(conns[k]); free(conns[k]); }
    }
    free(conns);
    fprintf(stdout, "mast: stopped\n");
    return 0;
}
