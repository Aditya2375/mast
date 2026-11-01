/* server.c - one thread, one epoll loop, every socket non-blocking. A
 * connection is a small state machine: read the head, read the body,
 * write the response (headers, then the file range via sendfile), then
 * keep-alive or close. Slow clients are cut off by per-state timeouts, so
 * a trickling slowloris never pins a slot for longer than the limit. */
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

typedef struct {
    int fd;
    ConnState state;
    MastParser parser;
    MastRequest req;
    MastBodyReader body;
    int have_head;          /* the request line + headers are parsed */
    size_t head_consumed;
    MastResponse resp;
    size_t out_sent;        /* bytes of resp.head/mem written */
    int responding;
    int peer_done;      /* the peer sent FIN: answer, then close */
    long long file_sent;
    long long started_ms, last_ms;
    struct sockaddr_in peer;
} Conn;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

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
    mast_parser_free(&c->parser);
    mast_response_free(&c->resp);
    free(c->req.body);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static void log_access(Conn *c, const char *docroot) {
    (void)docroot;
    char ip[32] = "?";
    inet_ntop(AF_INET, &c->peer.sin_addr, ip, sizeof ip);
    long long bytes = (long long)c->resp.head.len + c->resp.mem.len + c->file_sent;
    fprintf(stdout, "%s %s %s -> %d (%lld bytes, %lld ms)\n",
            ip, c->req.method_raw, c->req.target ? c->req.target : "-",
            c->resp.status, bytes, now_ms() - c->started_ms);
    fflush(stdout);
}

/* Reset per-request state for the next pipelined request on this conn. */
static void conn_next_request(Conn *c) {
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
    c->started_ms = now_ms();
}

/* Write as much of the pending response as the socket takes. Returns 1
 * when the response is fully sent, 0 when more writes are needed, -1 when
 * the connection is dead. */
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        c->out_sent += (size_t)w;
    }
    while (c->resp.has_file && c->file_sent < c->resp.file_len) {
        off_t off = (off_t)(c->resp.file_off + c->file_sent);
        size_t want = (size_t)(c->resp.file_len - c->file_sent);
        ssize_t w = sendfile(c->fd, c->resp.file.fd, &off, want);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        c->file_sent += w;
    }
    return 1;
}

/* Feed buffered bytes through the parser; may produce a response. */
static int conn_process(Conn *c, const char *docroot) {
    for (;;) {
        if (!c->have_head) {
            size_t consumed = 0;
            MastParse r = mast_parser_feed(&c->parser, NULL, 0, &c->req, &consumed);
            if (r == MAST_PARSE_INCOMPLETE) return 0;   /* wait for bytes */
            if (r == MAST_PARSE_BAD) {
                int status = atoi(c->parser.err_status);
                mast_reply_error(&c->resp.head, status, c->parser.err_why, 0);
                c->resp.status = status;
                c->resp.close_after = 1;
                c->responding = 1;
                c->state = CONN_WRITE;
                return 1;   /* answer, then close */
            }
            /* a parsed head: drop the consumed bytes from the buffer */
            c->have_head = 1;
            c->head_consumed = consumed;
            memmove(c->parser.buf, c->parser.buf + consumed, c->parser.len - consumed);
            c->parser.len -= consumed;
            c->state = CONN_READ_BODY;
            if (c->req.expect_continue && (c->req.chunked || c->req.content_length > 0)) {
                /* answer Expect: 100-continue by just proceeding - the body
                 * follows on the wire whether or not we interim-reply */
            }
        }
        /* body */
        if (c->req.chunked || c->req.content_length > 0) {
            size_t bconsumed = 0;
            MastBodyRead br = mast_body_feed(&c->req, &c->body,
                                             c->parser.buf, c->parser.len, &bconsumed);
            memmove(c->parser.buf, c->parser.buf + bconsumed, c->parser.len - bconsumed);
            c->parser.len -= bconsumed;
            if (br == MAST_BODY_INCOMPLETE) return 0;
            if (br == MAST_BODY_BAD) {
                mast_reply_error(&c->resp.head, 413, "the body is missing or over 1 MB", 0);
                c->resp.status = 413;
                c->resp.close_after = 1;
                c->responding = 1;
                c->state = CONN_WRITE;
                return 1;
            }
        }
        /* a complete request */
        mast_respond(&c->req, docroot, &c->resp);
        c->responding = 1;
        c->state = CONN_WRITE;
        return 1;
    }
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
    while (!g_stop || nconn > 0) {
        int nev = epoll_wait(ep, evs, 128, 500);
        if (nev < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        long long now = now_ms();
        for (int i = 0; i < nev; i++) {
            int fd = evs[i].data.fd;
            if (fd == lfd) {
                if (g_stop) continue;
                for (;;) {
                    struct sockaddr_in peer;
                    socklen_t plen = sizeof peer;
                    int cfd = accept4(lfd, (struct sockaddr *)&peer, &plen, SOCK_NONBLOCK);
                    if (cfd < 0) break;   /* EAGAIN: the edge is drained */
                    if (nconn >= cfg->max_conn) { close(cfd); continue; }
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    Conn *c = calloc(1, sizeof *c);
                    if (!c) { close(cfd); continue; }
                    c->fd = cfd;
                    c->state = CONN_READ_HEAD;
                    c->peer = peer;
                    mast_parser_init(&c->parser);
                    mast_response_init(&c->resp);
                    c->started_ms = c->last_ms = now;
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
            c->last_ms = now;
            if ((evs[i].events & EPOLLERR) && c->state != CONN_WRITE) goto close_conn;
            if ((evs[i].events & (EPOLLIN | EPOLLRDHUP)) &&
                (c->state == CONN_READ_HEAD || c->state == CONN_READ_BODY)) {
                /* buffer the fresh bytes, then let conn_process drive the
                 * parser over the buffer - one parse path, no double work */
                char tmp[16384];
                for (;;) {
                    ssize_t r = recv(c->fd, tmp, sizeof tmp, 0);
                    if (r <= 0) {
                        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                        if (r < 0 && errno == EINTR) continue;
                        if (r == 0) { c->peer_done = 1; break; }   /* FIN: the request may still be whole */
                        goto close_conn;   /* a hard error mid-request */
                    }
                    size_t cap = c->state == CONN_READ_HEAD
                        ? MAST_MAX_HEADERS_BYTES
                        : (size_t)MAST_MAX_BODY + 8192;
                    if (c->parser.len + (size_t)r > cap) {
                        /* an over-cap head gets a real 431; an over-cap body
                         * is a flood - close without ceremony */
                        if (c->state == CONN_READ_BODY) goto close_conn;
                        mast_reply_error(&c->resp.head, 431,
                            "the header block grew past 32 KB", 0);
                        c->resp.status = 431;
                        c->resp.close_after = 1;
                        c->responding = 1;
                        c->state = CONN_WRITE;
                        goto write_side;
                    }
                    if (mast_parser_append(&c->parser, tmp, (size_t)r, cap) != 0) goto close_conn;
                }
                int has = conn_process(c, cfg->docroot_real);
                if (has) goto write_side;
                if (c->peer_done) goto close_conn;   /* an incomplete request from a gone peer */
            }
            if ((evs[i].events & EPOLLOUT) && c->state == CONN_WRITE) {
            write_side:
                ;
                int done = conn_flush(c);
                if (done < 0) goto close_conn;
                if (done) {
                    log_access(c, cfg->docroot_real);
                    /* FIN only ends the connection once no pipelined
                     * request is still buffered */
                    if (c->resp.close_after || (c->peer_done && c->parser.len == 0)) goto close_conn;
                    conn_next_request(c);
                    /* pipelined bytes may already sit in the buffer */
                    if (c->parser.len > 0) {
                        int has = conn_process(c, cfg->docroot_real);
                        if (has) goto write_side;
                    }
                }
            }
            continue;
        close_conn:
            for (int k = 0; k < cfg->max_conn; k++)
                if (conns[k] == c) { conns[k] = NULL; break; }
            epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
            conn_free(c);
            free(c);
            nconn--;
        }
        /* timeout sweep */
        long long t = now_ms();
        for (int k = 0; k < cfg->max_conn; k++) {
            Conn *c = conns[k];
            if (!c) continue;
            long long limit;
            if (c->state == CONN_READ_HEAD)
                limit = c->have_head ? cfg->idle_timeout_ms : cfg->header_timeout_ms;
            else if (c->state == CONN_READ_BODY) limit = cfg->body_timeout_ms;
            else limit = cfg->write_timeout_ms;
            long long base = c->state == CONN_READ_HEAD && !c->have_head ? c->started_ms : c->last_ms;
            if (t - base > limit) {
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
    free(conns);
    fprintf(stdout, "mast: stopped\n");
    return 0;
}
