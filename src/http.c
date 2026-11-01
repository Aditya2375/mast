/* http.c - the parser and response builder. The parser never blocks and
 * never trusts a byte: every cap is checked before a buffer grows, and a
 * bad request always maps to a specific 4xx with a one-line reason. */
#define _GNU_SOURCE   /* strcasestr; also the wasm demo and native tests */
#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- MastBuf ---- */

void mast_buf_init(MastBuf *b) { b->buf = NULL; b->len = b->cap = 0; }
void mast_buf_free(MastBuf *b) { free(b->buf); b->buf = NULL; b->len = b->cap = 0; }

static void mast_buf_reserve(MastBuf *b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need) cap *= 2;
    char *nb = realloc(b->buf, cap);
    if (!nb) { fprintf(stderr, "mast: out of memory\n"); exit(70); }
    b->buf = nb;
    b->cap = cap;
}

void mast_buf_put(MastBuf *b, const void *data, size_t n) {
    mast_buf_reserve(b, n);
    if (n) memcpy(b->buf + b->len, data, n);
    b->len += n;
}

void mast_buf_puts(MastBuf *b, const char *s) { mast_buf_put(b, s, strlen(s)); }

void mast_buf_printf(MastBuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    mast_buf_reserve(b, (size_t)n + 1);
    vsnprintf(b->buf + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

/* ---- status reasons ---- */

const char *mast_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return "Status";
    }
}

/* ---- HTTP dates ---- */

void mast_http_date(char out[MAST_DATE_LEN], int64_t unix_seconds) {
    static const char *const wd[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *const mo[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t t = (time_t)unix_seconds;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, MAST_DATE_LEN, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             wd[tmv.tm_wday], tmv.tm_mday, mo[tmv.tm_mon], tmv.tm_year + 1900,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static int month_from_name(const char *s) {
    static const char *const mo[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++) if (memcmp(s, mo[i], 3) == 0) return i;
    return -1;
}

/* RFC 7231 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" (the only format
 * servers must generate; we accept exactly it back). */
int64_t mast_parse_http_date(const char *s) {
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    int mday, year, hour, min, sec;
    char wday[4], mon[4], gmt[4];
    if (sscanf(s, "%3c, %2d %3c %4d %2d:%2d:%2d %3c",
               wday, &mday, mon, &year, &hour, &min, &sec, gmt) != 8)
        return -1;
    wday[3] = mon[3] = gmt[3] = '\0';   /* %Nc does not terminate */
    int month = month_from_name(mon);
    if (month < 0 || strcmp(gmt, "GMT") != 0) return -1;
    tmv.tm_mday = mday; tmv.tm_mon = month; tmv.tm_year = year - 1900;
    tmv.tm_hour = hour; tmv.tm_min = min; tmv.tm_sec = sec;
    tmv.tm_isdst = 0;
    /* portable timegm: set TZ=UTC is not thread-safe; compute via a fixed
     * epoch offset instead */
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days = 0;
    for (int y = 1970; y < year; y++)
        days += 365 + ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    for (int m = 0; m < month; m++) {
        days += mdays[m];
        if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days++;
    }
    days += mday - 1;
    if (mday < 1 || mday > 31 || hour > 23 || min > 59 || sec > 60) return -1;
    return ((days * 24 + hour) * 60 + min) * 60 + sec;
}

/* ---- path sanitizing ---- */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mast_sanitize_path(const char *target, char *out, size_t out_cap) {
    /* split the query first */
    size_t tlen = strcspn(target, "?");
    if (tlen == 0 || target[0] != '/' || tlen + 1 > out_cap) return 0;
    /* percent-decode into a temp, rejecting NUL/control/ DEL */
    char *dec = malloc(tlen + 1);
    if (!dec) { fprintf(stderr, "mast: out of memory\n"); exit(70); }
    size_t d = 0;
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c == '%') {
            if (i + 2 >= tlen) { free(dec); return 0; }
            int hi = hexval((unsigned char)target[i + 1]);
            int lo = hexval((unsigned char)target[i + 2]);
            if (hi < 0 || lo < 0) { free(dec); return 0; }
            c = (unsigned char)(hi * 16 + lo);
            i += 2;
        }
        if (c == 0 || c < 32 || c == 127) { free(dec); return 0; }
        dec[d++] = (char)c;
    }
    dec[d] = '\0';
    int trailing_slash = d > 1 && dec[d - 1] == '/';
    /* normalize segments: drop "" (//) and ".", reject ".." escaping root */
    size_t o = 0;
    int depth = 0;
    char *save = NULL;
    for (char *seg = strtok_r(dec, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) {
            if (depth == 0) { free(dec); return 0; }   /* would escape the root */
            depth--;
            /* pop the previous segment from out */
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;   /* drop its leading slash */
            out[o] = '\0';
            continue;
        }
        size_t sl = strlen(seg);
        if (o + sl + 2 > out_cap) { free(dec); return 0; }
        out[o++] = '/';
        memcpy(out + o, seg, sl);
        o += sl;
        depth++;
    }
    /* preserve a trailing slash from the source: it marks a directory
     * request, and strtok_r swallowed the empty final segment */
    if (trailing_slash && (o == 0 || out[o - 1] != '/'))
        out[o++] = '/';
    free(dec);
    if (o == 0) out[o++] = '/';
    out[o] = '\0';
    return 1;
}

/* ---- the parser ---- */

void mast_parser_init(MastParser *p) {
    memset(p, 0, sizeof *p);
}

void mast_parser_free(MastParser *p) {
    free(p->buf);
    p->buf = NULL;
    p->len = p->cap = p->pos = 0;
}

static MastParse bad(MastParser *p, const char *status, const char *why) {
    memcpy(p->err_status, status, 4);
    snprintf(p->err_why, sizeof p->err_why, "%s", why);
    return MAST_PARSE_BAD;
}

static char *find_crlf(char *buf, size_t from, size_t len) {
    for (size_t i = from; i + 1 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n') return buf + i;
    return NULL;
}

static int method_from(const char *m, size_t n) {
    if (n == 3 && memcmp(m, "GET", 3) == 0) return MAST_METHOD_GET;
    if (n == 4 && memcmp(m, "HEAD", 4) == 0) return MAST_METHOD_HEAD;
    if (n == 4 && memcmp(m, "POST", 4) == 0) return MAST_METHOD_POST;
    return MAST_METHOD_OTHER;
}

static int header_name_ok(const char *s, size_t n) {
    static const char *tchar = "!#$%&'*+-.^_`|~";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && !strchr(tchar, c)) return 0;
    }
    return n > 0;
}

static int name_is(const char *a, size_t an, const char *b) {
    return strlen(b) == an && strncasecmp(a, b, an) == 0;
}

/* A comma-separated list header carries exact, case-insensitive tokens:
 * substring matching would read "disclose" as "close". */
static int has_token(const char *value, const char *token) {
    size_t tn = strlen(token);
    for (const char *q = value; ;) {
        while (*q == ' ' || *q == '\t' || *q == ',') q++;
        const char *e = q;
        while (*e && *e != ',') e++;
        const char *te = e;
        while (te > q && (te[-1] == ' ' || te[-1] == '\t')) te--;
        if ((size_t)(te - q) == tn && strncasecmp(q, token, tn) == 0) return 1;
        if (!*e) return 0;
        q = e + 1;
    }
}

/* Non-negative decimal integer, strict: digits only, no sign, no space. */
static int parse_decimal(const char *s, long long *out) {
    if (!isdigit((unsigned char)s[0])) return -1;
    long long v = 0;
    for (const char *q = s; *q; q++) {
        if (!isdigit((unsigned char)*q)) return -1;
        if (v > (9223372036854775807LL - 9) / 10) return -1;   /* overflow */
        v = v * 10 + (*q - '0');
    }
    *out = v;
    return 0;
}

int mast_parser_append(MastParser *p, const char *data, size_t n, size_t cap) {
    if (p->len + n > cap) return -1;
    if (p->len + n > p->cap) {
        size_t cap = p->cap ? p->cap : 4096;
        while (cap < p->len + n) cap *= 2;
        char *nb = realloc(p->buf, cap);
        if (!nb) { fprintf(stderr, "mast: out of memory\n"); exit(70); }
        p->buf = nb;
        p->cap = cap;
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;
    return 0;
}

MastParse mast_parser_feed(MastParser *p, const char *data, size_t n,
                           MastRequest *out, size_t *consumed) {
    if (n && mast_parser_append(p, data, n, MAST_MAX_HEADERS_BYTES) != 0)
        return bad(p, "431", "the header block grew past 32 KB");
    memset(out, 0, sizeof *out);
    out->content_length = -1;

    /* request line */
    char *eol = find_crlf(p->buf, 0, p->len);
    if (!eol) {
        if (p->len > MAST_MAX_REQUEST_LINE)
            return bad(p, "414", "the request line grew past 8 KB");
        return MAST_PARSE_INCOMPLETE;
    }
    if (eol - p->buf > MAST_MAX_REQUEST_LINE)
        return bad(p, "414", "the request line grew past 8 KB");
    size_t line_len = (size_t)(eol - p->buf);
    if (line_len == 0) {
        /* an empty line before a request is tolerated (RFC 7230 3.5) */
        memmove(p->buf, p->buf + 2, p->len - 2);
        p->len -= 2;
        return mast_parser_feed(p, NULL, 0, out, consumed);
    }
    char *sp1 = memchr(p->buf, ' ', line_len);
    char *sp2 = sp1 ? memchr(sp1 + 1, ' ', line_len - (size_t)(sp1 + 1 - p->buf)) : NULL;
    if (!sp1 || !sp2 || memchr(sp2 + 1, ' ', (size_t)(eol - sp2 - 1)))
        return bad(p, "400", "the request line must be METHOD SP TARGET SP VERSION");
    size_t mlen = (size_t)(sp1 - p->buf);
    if (mlen == 0 || mlen >= sizeof out->method_raw)
        return bad(p, "400", "the method is empty or over 15 bytes");
    for (size_t i = 0; i < mlen; i++)
        if (!isupper((unsigned char)p->buf[i]))
            return bad(p, "400", "methods are uppercase tokens");
    memcpy(out->method_raw, p->buf, mlen);
    out->method_raw[mlen] = '\0';
    out->method = method_from(p->buf, mlen);
    size_t tlen = (size_t)(sp2 - sp1 - 1);
    if (tlen == 0) return bad(p, "400", "the request target is empty");
    if (tlen >= MAST_MAX_TARGET) return bad(p, "414", "the request target is over 2048 bytes");
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)sp1[1 + i];
        if (c <= 32 || c == 127)   /* raw NUL and friends truncate C strings */
            return bad(p, "400", "the request target has an illegal byte");
    }
    memcpy(out->target_raw, sp1 + 1, tlen);
    out->target_raw[tlen] = '\0';
    out->target = out->target_raw;
    /* absolute-form: scheme://authority/path - keep the authority for the
     * Host consistency check, serve the path part as origin-form */
    {
        const char *auth = NULL;
        if (strncasecmp(out->target_raw, "http://", 7) == 0) auth = out->target_raw + 7;
        else if (strncasecmp(out->target_raw, "https://", 8) == 0) auth = out->target_raw + 8;
        if (auth) {
            const char *slash = strchr(auth, '/');
            size_t an = slash ? (size_t)(slash - auth) : strlen(auth);
            if (an == 0 || an >= sizeof out->authority)
                return bad(p, "400", "the absolute-form authority is missing or too long");
            memcpy(out->authority, auth, an);
            out->authority[an] = '\0';
            if (slash) {
                memmove(out->target_raw, slash, strlen(slash) + 1);
            } else {
                out->target_raw[0] = '/';
                out->target_raw[1] = '\0';
            }
        }
    }
    size_t vlen = (size_t)(eol - sp2 - 1);
    if (vlen == 8 && memcmp(sp2 + 1, "HTTP/1.1", 8) == 0) out->minor = 1;
    else if (vlen == 8 && memcmp(sp2 + 1, "HTTP/1.0", 8) == 0) out->minor = 0;
    else return bad(p, "505", "only HTTP/1.0 and HTTP/1.1 are spoken here");

    /* headers: read-only slices into the buffer, copied into the request's
     * pool, so a fragmented block re-parses identically after each feed */
    const char *cur = eol + 2;
    const char *bend = p->buf + p->len;
    for (;;) {
        const char *hend = NULL;
        for (const char *q = cur; q + 1 < bend; q++)
            if (q[0] == '\r' && q[1] == '\n') { hend = q; break; }
        if (!hend) return MAST_PARSE_INCOMPLETE;
        if (hend == cur) { cur += 2; break; }   /* the blank line: head done */
        size_t hlen = (size_t)(hend - cur);
        const char *colon = memchr(cur, ':', hlen);
        if (!colon) return bad(p, "400", "a header line has no colon");
        size_t nlen = (size_t)(colon - cur);
        if (!header_name_ok(cur, nlen))
            return bad(p, "400", "a header name has an illegal byte");
        const char *v = colon + 1;
        const char *vend = hend;
        while (v < vend && (*v == ' ' || *v == '\t')) v++;
        while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) vend--;
        size_t vlen2 = (size_t)(vend - v);
        for (size_t i = 0; i < vlen2; i++)
            if (((unsigned char)v[i] < 32 && v[i] != '\t') || (unsigned char)v[i] == 127)
                return bad(p, "400", "a header value has an illegal byte");
        if (out->header_count >= MAST_MAX_HEADER_COUNT)
            return bad(p, "431", "more than 64 headers");
        if (out->hpool_used + nlen + vlen2 + 2 > sizeof out->hpool)
            return bad(p, "431", "the header block grew past 32 KB");
        char *np = out->hpool + out->hpool_used;
        memcpy(np, cur, nlen);
        np[nlen] = '\0';
        char *vp = np + nlen + 1;
        memcpy(vp, v, vlen2);
        vp[vlen2] = '\0';
        out->hpool_used += nlen + vlen2 + 2;
        MastHeader *hd = &out->headers[out->header_count++];
        hd->name = np;
        hd->value = vp;
        if (name_is(np, nlen, "Host")) {
            if (out->host) return bad(p, "400", "two Host headers");
            out->host = vp;
        }
        else if (name_is(np, nlen, "Connection")) {
            if (has_token(vp, "close")) out->conn_close = 1;
            if (has_token(vp, "keep-alive")) out->conn_keep_alive = 1;
            out->connection = vp;
        }
        else if (name_is(np, nlen, "Content-Length")) {
            if (out->content_length != -1)
                return bad(p, "400", "two Content-Length headers");
            long long cl = 0;
            if (parse_decimal(vp, &cl) != 0)
                return bad(p, "400", "Content-Length is not a non-negative integer");
            out->content_length = cl;
        }
        else if (name_is(np, nlen, "Transfer-Encoding")) {
            if (strcasecmp(vp, "chunked") == 0) {
                if (out->chunked)
                    return bad(p, "400", "duplicate Transfer-Encoding: chunked");
                out->chunked = 1;
            } else if (has_token(vp, "chunked"))
                return bad(p, "400", "chunked Transfer-Encoding must stand alone");
            else
                return bad(p, "501", "only chunked Transfer-Encoding is understood");
        }
        else if (name_is(np, nlen, "Expect")) {
            if (strcasecmp(vp, "100-continue") != 0)
                return bad(p, "417", "only Expect: 100-continue is understood");
            out->expect_continue = 1;
        }
        else if (name_is(np, nlen, "If-None-Match")) out->if_none_match = vp;
        else if (name_is(np, nlen, "If-Modified-Since")) out->if_modified_since = vp;
        else if (name_is(np, nlen, "If-Range")) out->if_range = vp;
        else if (name_is(np, nlen, "Range")) out->range = vp;
        else if (name_is(np, nlen, "Content-Type")) out->content_type = vp;
        cur = hend + 2;
    }

    if (out->minor == 1 && !out->host)
        return bad(p, "400", "HTTP/1.1 requests need a Host header");
    if (out->authority[0] && out->host && strcasecmp(out->authority, out->host) != 0)
        return bad(p, "400", "the absolute-form authority and Host disagree");
    if (out->chunked && out->content_length != -1)
        return bad(p, "400", "Transfer-Encoding and Content-Length conflict");
    if (out->minor == 1)
        out->keep_alive = !out->conn_close;
    else
        out->keep_alive = out->conn_keep_alive;
    out->path_ok = mast_sanitize_path(out->target, out->path, sizeof out->path);
    *consumed = (size_t)(cur - p->buf);
    return MAST_PARSE_OK;
}

/* ---- body reading ----
 * An incremental reader with its own little state machine, so a chunked or
 * length-delimited body can arrive in any fragmentation. The decoded body
 * accumulates in req->body, capped at MAST_MAX_BODY. */

MastBodyRead mast_body_feed(MastRequest *req, MastBodyReader *r,
                            const char *data, size_t n, size_t *consumed) {
    *consumed = 0;
    if (!req->chunked && req->content_length <= 0) return MAST_BODY_OK;
    if (!req->body) {
        req->body = malloc(MAST_MAX_BODY + 1);
        if (!req->body) { fprintf(stderr, "mast: out of memory\n"); exit(70); }
        req->body_cap = MAST_MAX_BODY + 1;
    }
    if (!req->chunked) {
        if ((unsigned long long)req->content_length > MAST_MAX_BODY)
            return MAST_BODY_TOO_LARGE;   /* the caller maps this to 413 */
        size_t want = (size_t)req->content_length - req->body_len;
        size_t take = n < want ? n : want;
        memcpy(req->body + req->body_len, data, take);
        req->body_len += take;
        *consumed = take;
        return req->body_len == (size_t)req->content_length
            ? MAST_BODY_OK : MAST_BODY_INCOMPLETE;
    }
    /* chunked framing: HEXSIZE [;ext] CRLF DATA CRLF ... 0 CRLF [trailers] CRLF.
     * Size and trailer lines straddle feeds, so they buffer into r->line
     * (63/255-byte caps) and complete only when the CRLF lands. */
    size_t i = 0;
    for (;;) {
        if (r->state == MAST_CHUNK_SIZE || r->state == MAST_CHUNK_TRAILER) {
            size_t cap = r->state == MAST_CHUNK_SIZE ? 63 : 255;
            int got_line = 0;
            while (i < n) {
                if (r->line_len >= cap) return MAST_BODY_BAD;
                char ch = data[i++];
                r->line[r->line_len++] = ch;
                if (r->line_len >= 2 && r->line[r->line_len - 2] == '\r' && ch == '\n') {
                    got_line = 1;
                    break;
                }
            }
            if (!got_line) break;   /* need more bytes */
            r->line[r->line_len - 2] = '\0';
            if (r->state == MAST_CHUNK_SIZE) {
                /* strict: hexdigits only (no whitespace, no sign), then an
                 * optional ;extension */
                const char *q = r->line;
                unsigned long long size = 0;
                int digits = 0;
                while (isxdigit((unsigned char)*q)) {
                    if (size > ~0ULL / 16) return MAST_BODY_TOO_LARGE;
                    size = size * 16 + (unsigned long long)hexval((unsigned char)*q);
                    q++;
                    digits++;
                }
                if (digits == 0 || (*q != '\0' && *q != ';'))
                    return MAST_BODY_BAD;   /* malformed framing -> 400 */
                r->line_len = 0;
                if (size == 0) { r->state = MAST_CHUNK_TRAILER; continue; }
                if (size > MAST_MAX_BODY || req->body_len + size > MAST_MAX_BODY)
                    return MAST_BODY_TOO_LARGE;   /* over the body cap -> 413 */
                r->chunk_left = size;
                r->state = MAST_CHUNK_DATA;
            } else {
                int blank = r->line[0] == '\0';
                r->line_len = 0;
                if (blank) { *consumed = i; return MAST_BODY_OK; }
                /* a trailer line: accepted, ignored */
            }
        } else if (r->state == MAST_CHUNK_DATA) {
            size_t take = n - i < r->chunk_left ? n - i : (size_t)r->chunk_left;
            memcpy(req->body + req->body_len, data + i, take);
            req->body_len += take;
            i += take;
            r->chunk_left -= take;
            if (r->chunk_left == 0) r->state = MAST_CHUNK_CRLF;
            else break;
        } else {   /* MAST_CHUNK_CRLF: one byte at a time, so a lone '\r' is held */
            while (i < n) {
                char ch = data[i++];
                if (r->line_len == 0) {
                    if (ch != '\r') return MAST_BODY_BAD;
                    r->line[r->line_len++] = ch;
                } else {
                    if (ch != '\n') return MAST_BODY_BAD;
                    r->line_len = 0;
                    r->state = MAST_CHUNK_SIZE;
                    break;
                }
            }
            if (r->state == MAST_CHUNK_CRLF) break;   /* still waiting for the '\n' */
        }
    }
    *consumed = i;
    return MAST_BODY_INCOMPLETE;
}

/* ---- responses ---- */

void mast_reply_head(MastBuf *b, int status, const char *reason,
                     const char *content_type, long long content_length,
                     int keep_alive) {
    mast_buf_printf(b, "HTTP/1.1 %d %s\r\n", status, reason);
    char date[MAST_DATE_LEN];
    mast_http_date(date, (int64_t)time(NULL));
    mast_buf_printf(b, "Date: %s\r\n", date);
    mast_buf_puts(b, "Server: mast\r\n");
    if (content_type) mast_buf_printf(b, "Content-Type: %s\r\n", content_type);
    if (content_length >= 0)
        mast_buf_printf(b, "Content-Length: %lld\r\n", content_length);
    mast_buf_printf(b, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close");
    mast_buf_puts(b, "\r\n");
}

void mast_reply_error(MastBuf *head, MastBuf *body, int status, const char *why,
                      const char *extra_headers, int keep_alive) {
    const char *reason = mast_reason(status);
    char text[256];
    int n = snprintf(text, sizeof text, "%d %s\n%s\n", status, reason, why);
    mast_reply_head(head, status, reason, "text/plain; charset=utf-8", n, keep_alive);
    if (extra_headers) {
        head->len -= 2;                    /* lift the blank line ... */
        mast_buf_puts(head, extra_headers); /* ... splice the extra headers ... */
        mast_buf_puts(head, "\r\n");      /* ... and close the head again */
    }
    if (body) mast_buf_put(body, text, (size_t)n);
}
