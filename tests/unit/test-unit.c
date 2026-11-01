/* Unit suite for the socket-independent core: the request parser, path
 * sanitizing, body framing, ranges, dates, MIME, and mast_respond over a
 * temporary docroot. Prints "N checks passed" at the end (check-docs pins
 * the README to this count). */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../../src/http.h"
#include "../../src/handle.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks = 0, fails = 0;
#define CHECK(cond, label) do { \
    checks++; \
    if (!(cond)) { printf("FAIL %d: %s\n", checks, label); fails = 1; } } while (0)

/* Parse a whole request in one feed. */
static MastParse parse(const char *s, MastRequest *out, size_t *consumed) {
    MastParser p;
    mast_parser_init(&p);
    MastParse r = mast_parser_feed(&p, s, strlen(s), out, consumed);
    mast_parser_free(&p);
    return r;
}

int main(void) {
    /* ---- request line ---- */
    MastRequest q;
    size_t used = 0;
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_OK, "simple GET parses");
    CHECK(q.method == MAST_METHOD_GET && q.minor == 1 && q.keep_alive, "GET fields");
    CHECK(strcmp(q.target, "/") == 0 && strcmp(q.path, "/") == 0, "root target/path");
    CHECK(used == strlen("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "consumed counts the head");

    CHECK(parse("HEAD /a.txt HTTP/1.0\r\n\r\n", &q, &used) == MAST_PARSE_OK, "HTTP/1.0 HEAD");
    CHECK(q.method == MAST_METHOD_HEAD && q.minor == 0 && !q.keep_alive, "1.0 closes by default");
    CHECK(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && q.keep_alive, "1.0 keep-alive opt-in");
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && !q.keep_alive, "1.1 close opt-out");

    CHECK(parse("get / HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_BAD, "lowercase method rejected");
    CHECK(parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_BAD, "HTTP/2 rejected as 505");
    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &q, &used) == MAST_PARSE_BAD, "1.1 without Host rejected");
    CHECK(parse("OPTIONS / HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && q.method == MAST_METHOD_OTHER, "other methods parse (405 later)");
    CHECK(parse("GET / HTTP/1.1\nHost: x\n\n", &q, &used) == MAST_PARSE_INCOMPLETE,
          "bare LF never completes a request");
    CHECK(parse("\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_OK,
          "one empty line before the request is tolerated");

    /* header/target strictness */
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "duplicate Host rejected");
    CHECK(parse("GET /\x1f HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "raw control byte in the target rejected");
    CHECK(parse("GET /\x7f HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "raw DEL in the target rejected");
    CHECK(parse("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "Content-Length with a sign rejected");
    CHECK(parse("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5x\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "Content-Length with trailing junk rejected");
    CHECK(parse("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
                &q, &used) == MAST_PARSE_BAD, "duplicate Transfer-Encoding rejected");
    CHECK(parse("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, chunked\r\n\r\n",
                &q, &used) == MAST_PARSE_BAD, "listed chunked rejected");
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: disclose\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && q.keep_alive, "Connection tokens are exact: disclose is not close");
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive, Close\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && !q.keep_alive, "Connection close token honored in a list");
    CHECK(parse("GET / HTTP/1.0\r\nConnection: xkeep-alivex\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && !q.keep_alive, "1.0 keep-alive needs the exact token");

    /* absolute-form: the path serves as origin-form, authority must match Host */
    CHECK(parse("GET http://x/absolute?y=1 HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && strcmp(q.path, "/absolute") == 0 && strcmp(q.authority, "x") == 0,
          "absolute-form parses to its path");
    CHECK(parse("GET http://a/ HTTP/1.1\r\nHost: b\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "absolute-form and Host disagree: 400");
    CHECK(parse("GET https://x HTTP/1.1\r\nHost: x\r\n\r\n", &q, &used) == MAST_PARSE_OK
          && strcmp(q.path, "/") == 0, "absolute-form without a path is /");

    /* fragmentation: the same request byte-by-byte must parse identically */
    {
        const char *full = "GET /dir/page.html?q=1 HTTP/1.1\r\nHost: example\r\n"
                           "If-None-Match: \"abc\"\r\nX-Pad: 12345\r\n\r\n";
        MastParser p;
        mast_parser_init(&p);
        MastParse r = MAST_PARSE_INCOMPLETE;
        for (size_t i = 0; i < strlen(full); i++) {
            r = mast_parser_feed(&p, full + i, 1, &q, &used);
            if (r == MAST_PARSE_BAD) break;
        }
        CHECK(r == MAST_PARSE_OK, "byte-by-byte feed completes");
        CHECK(q.header_count == 3 && strcmp(q.if_none_match, "\"abc\"") == 0,
              "byte-by-byte headers intact");
        CHECK(strcmp(q.path, "/dir/page.html") == 0, "byte-by-byte path intact");
        mast_parser_free(&p);
    }

    /* caps */
    {
        MastParser p;
        mast_parser_init(&p);
        char big[9000];
        memset(big, 'a', sizeof big);
        MastParse r = mast_parser_feed(&p, big, sizeof big, &q, &used);
        CHECK(r == MAST_PARSE_BAD && strcmp(p.err_status, "414") == 0, "over-8KB request line is 414");
        mast_parser_free(&p);
    }
    {
        MastBuf b;
        mast_buf_init(&b);
        mast_buf_puts(&b, "GET / HTTP/1.1\r\nHost: x\r\n");
        for (int i = 0; i < 65; i++) mast_buf_printf(&b, "X-H%d: v\r\n", i);
        mast_buf_puts(&b, "\r\n");
        MastParser p;
        mast_parser_init(&p);
        CHECK(mast_parser_feed(&p, b.buf, b.len, &q, &used) == MAST_PARSE_BAD
              && strcmp(p.err_status, "431") == 0, "65 headers is 431");
        mast_parser_free(&p);
        mast_buf_free(&b);
    }
    {
        MastBuf b;
        mast_buf_init(&b);
        mast_buf_puts(&b, "GET / HTTP/1.1\r\nHost: x\r\n");
        for (int i = 0; i < 60; i++) {
            mast_buf_printf(&b, "X-Pad-%d: ", i);
            for (int j = 0; j < 600; j++) mast_buf_puts(&b, "x");
            mast_buf_puts(&b, "\r\n");
        }
        mast_buf_puts(&b, "\r\n");
        MastParser p;
        mast_parser_init(&p);
        CHECK(mast_parser_feed(&p, b.buf, b.len, &q, &used) == MAST_PARSE_BAD, "over-32KB headers is 431");
        mast_parser_free(&p);
        mast_buf_free(&b);
    }

    /* header grammar */
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nBad Header: v\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "space in a header name rejected");
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nNoColon\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "header without colon rejected");
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", &q, &used) == MAST_PARSE_BAD,
          "non-numeric Content-Length rejected");
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", &q, &used)
          == MAST_PARSE_BAD, "duplicate Content-Length rejected (smuggling)");
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", &q, &used)
          == MAST_PARSE_BAD, "CL + chunked rejected (smuggling)");
    CHECK(parse("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", &q, &used)
          == MAST_PARSE_BAD, "non-chunked TE is 501");

    /* ---- path sanitizing ---- */
    char path[MAST_MAX_TARGET];
    CHECK(mast_sanitize_path("/", path, sizeof path) && strcmp(path, "/") == 0, "root");
    CHECK(mast_sanitize_path("/a/b.txt", path, sizeof path) && strcmp(path, "/a/b.txt") == 0, "plain");
    CHECK(mast_sanitize_path("/a//b/./c", path, sizeof path) && strcmp(path, "/a/b/c") == 0, "collapse // and /.");
    CHECK(mast_sanitize_path("/a/../b", path, sizeof path) && strcmp(path, "/b") == 0, "dotdot inside root");
    CHECK(!mast_sanitize_path("/..", path, sizeof path), "dotdot escaping root rejected");
    CHECK(!mast_sanitize_path("/a/../../b", path, sizeof path), "deep escape rejected");
    CHECK(!mast_sanitize_path("/%2e%2e/%2e%2e/etc/passwd", path, sizeof path), "encoded escape rejected");
    CHECK(mast_sanitize_path("/a%20b.txt", path, sizeof path) && strcmp(path, "/a b.txt") == 0, "percent space");
    CHECK(!mast_sanitize_path("/a%0zz", path, sizeof path), "bad hex rejected");
    CHECK(!mast_sanitize_path("/a%00b", path, sizeof path), "encoded NUL rejected");
    CHECK(mast_sanitize_path("/dir/?q=1", path, sizeof path) && strcmp(path, "/dir/") == 0,
          "query stripped, trailing slash kept");
    CHECK(!mast_sanitize_path("noslash", path, sizeof path), "non-absolute target rejected");

    /* ---- ranges ---- */
    long long st = 0, ln = 0;
    CHECK(mast_range_parse(NULL, 100, &st, &ln) == 0, "no range header");
    CHECK(mast_range_parse("bytes=0-9", 100, &st, &ln) == 1 && st == 0 && ln == 10, "explicit range");
    CHECK(mast_range_parse("bytes=90-", 100, &st, &ln) == 1 && st == 90 && ln == 10, "open-ended range");
    CHECK(mast_range_parse("bytes=-10", 100, &st, &ln) == 1 && st == 90 && ln == 10, "suffix range");
    CHECK(mast_range_parse("bytes=0-999", 100, &st, &ln) == 1 && st == 0 && ln == 100, "range clamped to size");
    CHECK(mast_range_parse("bytes=100-200", 100, &st, &ln) == -1, "range past EOF unsatisfiable");
    CHECK(mast_range_parse("bytes=0-1,4-5", 100, &st, &ln) == 0, "multi-range ignored: full body (RFC 9110)");
    CHECK(mast_range_parse("items=0-9", 100, &st, &ln) == 0, "unknown range unit ignored");
    CHECK(mast_range_parse("BYTES=0-9", 100, &st, &ln) == 1 && ln == 10, "range unit case-insensitive");
    CHECK(mast_range_parse("bytes= 1-4", 100, &st, &ln) == 0, "range with a space ignored");
    CHECK(mast_range_parse("bytes=+1-4", 100, &st, &ln) == 0, "range with a sign ignored");
    CHECK(mast_range_parse("bytes=-0", 100, &st, &ln) == -1, "zero suffix unsatisfiable");
    CHECK(mast_range_parse("bytes=5-3", 100, &st, &ln) == -1, "last before first unsatisfiable");

    /* ---- dates ---- */
    char date[MAST_DATE_LEN];
    mast_http_date(date, 784111777);
    CHECK(strcmp(date, "Sun, 06 Nov 1994 08:49:37 GMT") == 0, "http date formats IMF-fixdate");
    CHECK(mast_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777, "date round-trips");
    CHECK(mast_parse_http_date("garbage") == -1, "bad date rejected");
    mast_http_date(date, 0);
    CHECK(strcmp(date, "Thu, 01 Jan 1970 00:00:00 GMT") == 0, "epoch formats");

    /* ---- MIME ---- */
    CHECK(strcmp(mast_mime("/x.html"), "text/html; charset=utf-8") == 0, "html mime");
    CHECK(strcmp(mast_mime("/x.wasm"), "application/wasm") == 0, "wasm mime");
    CHECK(strcmp(mast_mime("/x.unknown"), "application/octet-stream") == 0, "unknown mime");
    CHECK(strcmp(mast_mime("/noext"), "application/octet-stream") == 0, "extensionless mime");

    /* ---- body framing ---- */
    {
        MastRequest r;
        memset(&r, 0, sizeof r);
        r.content_length = 5;
        MastBodyReader br;
        memset(&br, 0, sizeof br);
        size_t cu = 0;
        CHECK(mast_body_feed(&r, &br, "he", 2, &cu) == MAST_BODY_INCOMPLETE && cu == 2, "partial body");
        CHECK(mast_body_feed(&r, &br, "llo", 3, &cu) == MAST_BODY_OK && r.body_len == 5
              && memcmp(r.body, "hello", 5) == 0, "body completes");
        free(r.body);
    }
    {
        MastRequest r;
        memset(&r, 0, sizeof r);
        r.chunked = 1;
        MastBodyReader br;
        memset(&br, 0, sizeof br);
        const char *wire = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        size_t cu = 0;
        CHECK(mast_body_feed(&r, &br, wire, strlen(wire), &cu) == MAST_BODY_OK,
              "chunked body decodes");
        CHECK(r.body_len == 11 && memcmp(r.body, "hello world", 11) == 0, "chunked bytes right");
        CHECK(cu == strlen(wire), "chunked consumes the whole frame");
        free(r.body);
    }
    {
        /* chunked byte-by-byte: fragmentation must not change the result */
        MastRequest r;
        memset(&r, 0, sizeof r);
        r.chunked = 1;
        MastBodyReader br;
        memset(&br, 0, sizeof br);
        const char *wire = "5;ext=x\r\nhello\r\n0\r\nX-T: v\r\n\r\n";
        MastBodyRead res = MAST_BODY_INCOMPLETE;
        size_t pos = 0;
        while (pos < strlen(wire) && res == MAST_BODY_INCOMPLETE) {
            size_t cu = 0;
            res = mast_body_feed(&r, &br, wire + pos, 1, &cu);
            pos += cu;   /* the caller re-feeds unconsumed bytes (the server's contract) */
        }
        CHECK(res == MAST_BODY_OK && r.body_len == 5, "chunked byte-by-byte with ext + trailer");
        free(r.body);
    }
    {
        MastRequest r;
        memset(&r, 0, sizeof r);
        r.content_length = MAST_MAX_BODY + 1;
        MastBodyReader br;
        memset(&br, 0, sizeof br);
        size_t cu = 0;
        CHECK(mast_body_feed(&r, &br, "x", 1, &cu) == MAST_BODY_TOO_LARGE,
              "over-1MB length is 413, not 400");
        free(r.body);
    }
    {
        MastRequest r;
        memset(&r, 0, sizeof r);
        r.chunked = 1;
        MastBodyReader br;
        memset(&br, 0, sizeof br);
        size_t cu = 0;
        CHECK(mast_body_feed(&r, &br, "zz\r\nhello\r\n0\r\n\r\n", 18, &cu) == MAST_BODY_BAD,
              "bad chunk size rejected");
        free(r.body);
    }
    {
        MastRequest r;
        memset(&r, 0, sizeof r);
        r.chunked = 1;
        MastBodyReader br;
        size_t cu = 0;
        memset(&br, 0, sizeof br);
        CHECK(mast_body_feed(&r, &br, " 5\r\nhello\r\n0\r\n\r\n", 18, &cu) == MAST_BODY_BAD,
              "chunk size with leading space rejected");
        memset(&br, 0, sizeof br);
        r.body_len = 0;
        CHECK(mast_body_feed(&r, &br, "+5\r\nhello\r\n0\r\n\r\n", 18, &cu) == MAST_BODY_BAD,
              "chunk size with a sign rejected");
        memset(&br, 0, sizeof br);
        r.body_len = 0;
        CHECK(mast_body_feed(&r, &br, "5  \r\nhello\r\n0\r\n\r\n", 19, &cu) == MAST_BODY_BAD,
              "chunk size with trailing space rejected");
        free(r.body);
    }

    /* ---- mast_respond over a temp docroot ---- */
    char root[] = "/tmp/mast-unit-XXXXXX";
    CHECK(mkdtemp(root) != NULL, "temp docroot");
    {
        char p[256];
        snprintf(p, sizeof p, "%s/index.html", root);
        int fd = open(p, O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0, "write index.html");
        const char *html = "<h1>hi</h1>\n";
        CHECK(write(fd, html, strlen(html)) == (ssize_t)strlen(html), "index bytes");
        close(fd);
        snprintf(p, sizeof p, "%s/big.bin", root);
        fd = open(p, O_WRONLY | O_CREAT, 0644);
        char kb[1024];
        memset(kb, 'z', sizeof kb);
        for (int i = 0; i < 100; i++) CHECK(write(fd, kb, sizeof kb) == (ssize_t)sizeof kb || i > 0, "big bytes");
        close(fd);
        snprintf(p, sizeof p, "%s/sub", root);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/sub/index.html", root);
        fd = open(p, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) { const char *x = "sub\n"; ssize_t w = write(fd, x, strlen(x)); (void)w; close(fd); }
        /* a symlink out of the docroot must not be followed */
        snprintf(p, sizeof p, "%s/escape", root);
        if (symlink("/etc/passwd", p) != 0) { printf("FAIL: symlink setup\n"); fails = 1; }
    }

    MastRequest rq;
    MastResponse rs;
    size_t u2 = 0;
    #define RESPOND(req) do { \
        mast_response_init(&rs); \
        CHECK(parse(req, &rq, &u2) == MAST_PARSE_OK, "respond: request parses"); \
        mast_respond(&rq, root, &rs); } while (0)

    RESPOND("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 200 && rs.has_file, "GET / serves index.html");
    CHECK(strstr(rs.head.buf, "Content-Type: text/html"), "index mime");
    CHECK(strstr(rs.head.buf, "Content-Length: 12"), "index length");
    char etag_copy[96];
    snprintf(etag_copy, sizeof etag_copy, "%s", rs.file.etag);
    mast_response_free(&rs);

    RESPOND("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 200 && !rs.has_file && rs.file.fd == -1, "HEAD has no body stream");
    CHECK(strstr(rs.head.buf, "Content-Length: 12"), "HEAD still reports length");
    mast_response_free(&rs);

    RESPOND("GET /missing HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 404 && !rs.has_file, "missing file is 404");
    mast_response_free(&rs);

    RESPOND("GET /sub HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 301 && strstr(rs.head.buf, "Location: /sub/"), "dir without slash redirects");
    mast_response_free(&rs);

    RESPOND("GET /sub/ HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 200 && rs.has_file, "dir with slash serves its index");
    mast_response_free(&rs);

    RESPOND("GET /escape HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 404, "a symlink out of the docroot is not followed");
    mast_response_free(&rs);

    {
        char req[320];
        snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: %s\r\n\r\n", etag_copy);
        RESPOND(req);
        CHECK(rs.status == 304 && !rs.has_file, "matching ETag is 304");
        mast_response_free(&rs);
    }
    {
        char req[320];
        snprintf(req, sizeof req, "GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-1023\r\n\r\n");
        RESPOND(req);
        CHECK(rs.status == 206 && rs.file_len == 1024 && rs.file_off == 0, "range serves 206 with bounds");
        CHECK(strstr(rs.head.buf, "Content-Range: bytes 0-1023/102400"), "content-range header");
        mast_response_free(&rs);
    }
    RESPOND("GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=200000-\r\n\r\n");
    CHECK(rs.status == 416 && strstr(rs.head.buf, "Content-Range: bytes */102400"), "past-EOF range is 416");
    mast_response_free(&rs);

    RESPOND("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n");
    rq.body = malloc(4);
    memcpy(rq.body, "abc", 3);
    rq.body_len = 3;
    mast_respond(&rq, root, &rs);
    CHECK(rs.status == 200 && rs.mem.len == 3 && memcmp(rs.mem.buf, "abc", 3) == 0, "echo returns the body");
    CHECK(strstr(rs.head.buf, "Content-Type: text/plain"), "echo keeps the content type");
    free(rq.body);
    rq.body = NULL;
    mast_response_free(&rs);

    RESPOND("PUT / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 405 && strstr(rs.head.buf, "Allow: GET, HEAD, POST"), "PUT is 405 with Allow");
    mast_response_free(&rs);

    /* error framing: headers end at the blank line, extra headers sit
     * before it, the body is whole and Content-Length matches it */
    RESPOND("PUT / HTTP/1.1\r\nHost: x\r\n\r\n");
    {
        char cl_line[64];
        snprintf(cl_line, sizeof cl_line, "Content-Length: %zu", rs.mem.len);
        CHECK(rs.status == 405 && strstr(rs.head.buf, "Allow: GET, HEAD, POST\r\n"),
              "405 carries Allow");
        CHECK(rs.head.len >= 4 && memcmp(rs.head.buf + rs.head.len - 4, "\r\n\r\n", 4) == 0,
              "405 head ends at the blank line");
        CHECK(strstr(rs.head.buf, cl_line) != NULL, "405 Content-Length matches the body");
        CHECK(rs.mem.len > 0 && memcmp(rs.mem.buf, "405 ", 4) == 0, "405 body intact");
        mast_response_free(&rs);
    }
    RESPOND("GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=999999-\r\n\r\n");
    {
        char cl_line[64];
        snprintf(cl_line, sizeof cl_line, "Content-Length: %zu", rs.mem.len);
        CHECK(rs.status == 416
              && strstr(rs.head.buf, "Content-Range: bytes */102400\r\n") != NULL,
              "416 carries Content-Range before the blank line");
        CHECK(rs.head.len >= 4 && memcmp(rs.head.buf + rs.head.len - 4, "\r\n\r\n", 4) == 0,
              "416 head ends at the blank line");
        CHECK(strstr(rs.head.buf, cl_line) != NULL, "416 Content-Length matches the body");
        mast_response_free(&rs);
    }

    /* HEAD: every error status still reports the GET Content-Length but
     * sends no body */
    RESPOND("HEAD /missing HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 404 && rs.mem.len == 0, "HEAD 404 sends no body");
    CHECK(strstr(rs.head.buf, "Content-Length:") != NULL, "HEAD 404 keeps Content-Length");
    mast_response_free(&rs);
    RESPOND("HEAD /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=999999-\r\n\r\n");
    CHECK(rs.status == 416 && rs.mem.len == 0 && !rs.has_file, "HEAD 416 sends no body");
    mast_response_free(&rs);

    /* If-None-Match: weak comparison over a list */
    {
        char req[320];
        snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: W/%s\r\n\r\n", etag_copy);
        RESPOND(req);
        CHECK(rs.status == 304, "weak prefix still matches (weak comparison)");
        mast_response_free(&rs);
        snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: \"other\", %s\r\n\r\n", etag_copy);
        RESPOND(req);
        CHECK(rs.status == 304, "an entity-tag list matches on any member");
        mast_response_free(&rs);
        RESPOND("GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: \"other\"\r\n\r\n");
        CHECK(rs.status == 200 && rs.has_file, "a non-matching list serves the file");
        mast_response_free(&rs);
    }

    /* If-Range: exact validator -> 206, stale validator -> 200 full body */
    {
        char req[384];
        snprintf(req, sizeof req, "GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\n\r\n");
        RESPOND(req);
        char big_etag[96], big_lm[MAST_DATE_LEN];
        snprintf(big_etag, sizeof big_etag, "%s", rs.file.etag);
        snprintf(big_lm, sizeof big_lm, "%s", rs.file.last_modified);
        CHECK(rs.status == 206, "range baseline");
        mast_response_free(&rs);
        snprintf(req, sizeof req, "GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\nIf-Range: %s\r\n\r\n", big_etag);
        RESPOND(req);
        CHECK(rs.status == 206 && rs.file_len == 10, "If-Range with the current etag serves the range");
        mast_response_free(&rs);
        snprintf(req, sizeof req, "GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\nIf-Range: \"stale\"\r\n\r\n");
        RESPOND(req);
        CHECK(rs.status == 200 && rs.file_len == 102400, "If-Range with a stale etag serves the full body");
        mast_response_free(&rs);
        snprintf(req, sizeof req, "GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\nIf-Range: %s\r\n\r\n", big_lm);
        RESPOND(req);
        CHECK(rs.status == 206, "If-Range accepts a current HTTP-date");
        mast_response_free(&rs);
    }

    /* ETag strength: a same-size rewrite within the same second must still
     * change the tag (nanosecond mtime + inode) */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/index.html", root);
        char first_etag[96];
        snprintf(first_etag, sizeof first_etag, "%s", etag_copy);
        int fd = open(p, O_WRONLY | O_TRUNC);
        const char *html2 = "<h1>HI</h1>\n";   /* same 12 bytes */
        ssize_t w = write(fd, html2, strlen(html2));
        (void)w;
        close(fd);
        RESPOND("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        CHECK(strcmp(rs.file.etag, first_etag) != 0,
              "same-second same-size rewrite changes the etag");
        mast_response_free(&rs);
    }

    /* OPTIONS * answers capabilities with no body */
    RESPOND("OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(rs.status == 204 && rs.mem.len == 0 && !rs.has_file, "OPTIONS * is 204 without a body");
    CHECK(strstr(rs.head.buf, "Allow: GET, HEAD, POST, OPTIONS"), "OPTIONS * lists Allow");
    mast_response_free(&rs);

    /* a root ("/") docroot serves its tree: containment holds at dlen==1 */
    {
        char rp[256];
        snprintf(rp, sizeof rp, "/tmp/mast-rootserve-%d.txt", (int)getpid());
        int fd = open(rp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const char *txt = "root docroot works\n";
        ssize_t w = write(fd, txt, strlen(txt));
        (void)w;
        close(fd);
        char req[384];
        snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", rp);
        mast_response_init(&rs);
        CHECK(parse(req, &rq, &u2) == MAST_PARSE_OK, "respond: request parses");
        mast_respond(&rq, "/", &rs);
        CHECK(rs.status == 200 && rs.has_file && rs.file_len == 19,
              "docroot / serves the filesystem tree");
        mast_response_free(&rs);
        unlink(rp);
    }

    /* parse failures map to clean 4xx through the same path the server uses */
    {
        MastParser p;
        mast_parser_init(&p);
        MastRequest bq;
        size_t bu = 0;
        MastParse pr = mast_parser_feed(&p, "nonsense\r\n\r\n", 12, &bq, &bu);
        CHECK(pr == MAST_PARSE_BAD && strcmp(p.err_status, "400") == 0, "garbage line is 400");
        mast_parser_free(&p);
    }

    printf("%d checks passed\n", checks);
    return fails;
}
