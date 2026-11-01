/* http.h - incremental HTTP/1.1 request parser + response builder.
 * The parser is a byte-fed state machine: the server (or the wasm demo)
 * hands it whatever bytes arrived, and it says when a request is complete,
 * when more bytes are needed, or exactly why the request is bad. No
 * allocation grows without a documented cap. */
#ifndef MAST_HTTP_H
#define MAST_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* Documented limits, each with a clean 4xx (never a crash), each pinned by
 * the unit and fuzz suites. */
#define MAST_MAX_REQUEST_LINE 8192     /* method + target + version */
#define MAST_MAX_HEADERS_BYTES 32768   /* the whole header block */
#define MAST_MAX_HEADER_COUNT 64
#define MAST_MAX_TARGET 2048
#define MAST_MAX_BODY (1024 * 1024)    /* request bodies, default cap */

typedef enum {
    MAST_PARSE_INCOMPLETE = 0,  /* need more bytes */
    MAST_PARSE_OK,              /* a full request is ready */
    MAST_PARSE_BAD              /* malformed: send the stored status, close */
} MastParse;

typedef enum {
    MAST_METHOD_GET,
    MAST_METHOD_HEAD,
    MAST_METHOD_POST,
    MAST_METHOD_OPTIONS,
    MAST_METHOD_OTHER          /* syntactically fine, not served -> 405 */
} MastMethod;

typedef struct {
    char *name;   /* points into the request's header pool */
    char *value;
} MastHeader;

typedef struct {
    MastMethod method;
    char method_raw[16];
    char target_raw[MAST_MAX_TARGET];   /* raw, still percent-encoded */
    char *target;                       /* = target_raw */
    char hpool[MAST_MAX_HEADERS_BYTES]; /* header names + values */
    size_t hpool_used;
    char path[MAST_MAX_TARGET];   /* decoded, normalized, traversal-safe */
    int path_ok;             /* 0 -> reply 400/404, do not touch the fs */
    int minor;               /* HTTP/1.minor */
    MastHeader headers[MAST_MAX_HEADER_COUNT];
    int header_count;
    const char *host;        /* NULL if absent (HTTP/1.1 -> 400) */
    const char *connection;  /* raw value, for keep-alive decisions */
    long long content_length;     /* -1 when absent */
    int chunked;                  /* Transfer-Encoding: chunked */
    int te_other;                 /* a non-chunked TE token on any TE line:
                         * RFC 9110 5.3 combines the lines into one list */
    int expect_continue;          /* Expect: 100-continue */
    const char *expect;           /* first Expect value: a differing repeat is a 400 */
    int conn_close, conn_keep_alive;   /* exact Connection tokens seen */
    int keep_alive;               /* resolved: may the connection live on */
    const char *if_none_match;
    const char *if_modified_since;
    const char *if_match;              /* If-Match list, or NULL */
    const char *if_unmodified_since;   /* If-Unmodified-Since date, or NULL */
    const char *if_range;         /* raw If-Range validator, or NULL */
    const char *range;            /* raw Range header value, or NULL */
    int range_seen;               /* every Range line: any repeat ignores them all */
    const char *content_type;
    char authority[256];  /* absolute-form scheme://authority, or "" */
    /* body: filled by the body reader (chunked or length-delimited) */
    char *body;
    size_t body_len, body_cap;
} MastRequest;

typedef struct {
    char *buf;          /* receive buffer, grown up to the header cap */
    size_t len, cap;
    int state;          /* 0 = request line, 1 = headers */
    char err_status[4]; /* "400", "408", ... when MAST_PARSE_BAD */
    char err_why[96];   /* one-line reason for the error page/log */
    int err_head_method; /* the failed request line was HEAD: no body */
    int blanks;         /* empty lines skipped before this request, counted
                         * across feeds so dribbling cannot dodge the cap */
} MastParser;

void mast_parser_init(MastParser *p);
void mast_parser_free(MastParser *p);
/* Append received bytes to the parse buffer. 0 ok, -1 over cap (the head
 * cap while reading headers, the body cap + slack while reading a body). */
int mast_parser_append(MastParser *p, const char *data, size_t n, size_t cap);

/* Feed bytes and parse. Returns INCOMPLETE / OK / BAD. On OK, *out holds
 * the parsed request (self-contained: names, values and the target are
 * copied into the request) and *consumed says how many buffered bytes it
 * used (the rest is the next pipelined request). On BAD, p->err_status and
 * p->err_why are set. The parse buffer is never modified, so a fragmented
 * header block re-parses cleanly after every feed. */
MastParse mast_parser_feed(MastParser *p, const char *data, size_t n,
                           MastRequest *out, size_t *consumed);

/* Percent-decode + normalize a target into req->path: collapses // and
 * /./, rejects /.. that would escape the root, rejects NUL and control
 * bytes. Returns 1 when the path is safe to join with the docroot. */
int mast_sanitize_path(const char *target, char *out, size_t out_cap);

/* A body reader: feeds after the headers complete. Handles both
 * Content-Length and chunked framing, enforcing MAST_MAX_BODY. */
typedef enum {
    MAST_BODY_INCOMPLETE,   /* need more bytes */
    MAST_BODY_OK,           /* the body is complete */
    MAST_BODY_BAD,          /* malformed framing: 400 */
    MAST_BODY_TOO_LARGE,    /* over MAST_MAX_BODY: 413 */
    MAST_BODY_NOMEM         /* the body buffer would not grow: 503 */
} MastBodyRead;

/* Incremental body state: content-length copies bytes; chunked decodes
 * HEXSIZE CRLF DATA CRLF ... 0 CRLF trailers CRLF. */
typedef enum { MAST_CHUNK_SIZE, MAST_CHUNK_DATA, MAST_CHUNK_CRLF, MAST_CHUNK_TRAILER } MastChunkState;
typedef struct {
    MastChunkState state;
    unsigned long long chunk_left;
    char line[4096];      /* a straddling chunk-size or trailer line */
    size_t line_len;
} MastBodyReader;

MastBodyRead mast_body_feed(MastRequest *req, MastBodyReader *r,
                            const char *data, size_t n, size_t *consumed);

/* ---- response building ---- */

typedef struct {
    char *buf;
    size_t len, cap;
} MastBuf;

void mast_buf_init(MastBuf *b);
void mast_buf_free(MastBuf *b);
void mast_buf_put(MastBuf *b, const void *data, size_t n);
void mast_buf_puts(MastBuf *b, const char *s);
void mast_buf_printf(MastBuf *b, const char *fmt, ...);

/* Response head: status line + the standard headers. Caller adds body. */
void mast_reply_head(MastBuf *b, int status, const char *reason,
                     const char *content_type, long long content_length,
                     int keep_alive);
/* A small plain-text error page: status line + headers go into head, the
 * body text into body (pass body NULL for HEAD - Content-Length still
 * reflects the would-be body, per RFC 9110). extra_headers, when not NULL,
 * is spliced in before the blank line (e.g. "Allow: GET, HEAD, POST\r\n"). */
void mast_reply_error(MastBuf *head, MastBuf *body, int status, const char *why,
                      const char *extra_headers, int keep_alive);
#define MAST_DATE_LEN 40   /* IMF-fixdate is 29 bytes + NUL, with headroom */
/* RFC 7231 HTTP-date, e.g. for Date / Last-Modified. */
void mast_http_date(char out[MAST_DATE_LEN], int64_t unix_seconds);
/* Parse an HTTP-date; -1 on garbage. */
int64_t mast_parse_http_date(const char *s);

const char *mast_reason(int status);

#endif
