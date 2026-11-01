# mast

mast is a small HTTP/1.1 server written in C11 plus Linux/POSIX
(epoll, sendfile, realpath), with no dependencies beyond libc. It serves
static files with keep-alive, pipelining, range requests, conditional
requests and chunked request bodies, and it echoes POSTs. The server core is
1976 lines of C.

Try it in your browser: **https://aditya2375.github.io/mast/** - the same
parser, path sanitizer and response builder compiled to WebAssembly,
answering against an in-memory filesystem entirely on your machine.

## Quick start

```sh
make                    # builds build/mast and build/test-unit
make test               # unit suite + live-socket CLI suite + docs check
./build/mast -d /var/www -p 8080
curl -i http://localhost:8080/
curl -i -H 'Range: bytes=0-99' http://localhost:8080/big.bin
curl -i -X POST --data 'hello' http://localhost:8080/echo
```

Flags: `-d DIR` document root (default `.`), `-p PORT` (default 8080),
`-c N` maximum simultaneous connections (default 1024), and `-H`/`-I`/`-B`/`-W`
to retune the header, idle, body and write timeouts in milliseconds.

## What it does

- **Static files.** GET and HEAD over a document root, `index.html` for
  directories, a 301 redirect when a directory URL lacks its trailing slash,
  and a content-type table for the common text, image, font, audio, video
  and archive types.
- **Conditional requests.** Every file carries a strong ETag
  (`"inode-size-seconds-nanoseconds"`, so a same-second same-size rewrite
  still changes it) and a Last-Modified date. Preconditions run in RFC
  9110 13.2.2 order: `If-Match` (strong comparison over a list, `*`
  included) and `If-Unmodified-Since` answer 412 on failure; then
  `If-None-Match` - including comma lists and weak `W/` prefixes - and
  `If-Modified-Since` get a 304 with no body. `If-Range` gates a range on
  its validator, strictly: the exact entity-tag (a weak tag never holds)
  or a date exactly equal to Last-Modified - a future or stale date gets
  the full 200 body. `POST /echo` stores nothing, so any `If-Match`
  there is a 412.
- **Range requests.** A single byte range per request - `bytes=0-99`,
  `bytes=500-` or the last N bytes with `bytes=-64` - answered 206 with a
  Content-Range header; an unsatisfiable range is a 416. Unknown units,
  malformed ranges and multi-range requests are ignored and get the full
  200 body, as RFC 9110 permits.
- **POST /echo.** The request body comes back verbatim with its
  content-type, whether it arrived with a Content-Length or chunked
  transfer-coding (chunk extensions and trailers included).
  `Expect: 100-continue` gets an interim 100 only when the body will be
  accepted, the final error otherwise; any other Expect is a 417.
- **Keep-alive and pipelining.** HTTP/1.1 connections stay open across
  requests; several requests written in one flight are answered in order,
  with a per-turn budget (8 responses or 512 KB) so one fast pipelining
  client cannot starve the others. A pipelined flood past the header cap
  while a response is pending is held back by TCP backpressure - the
  pending answer is never destroyed, the flood then gets its 431.
  HTTP/1.0 and an exact `Connection: close` token get their answer and a
  clean close. Closing connections linger briefly (a bounded number of
  slots, so abandoned drainers cannot pin the server) to drain the peer's
  unread bytes, so a final 4xx is never destroyed by an RST.
- **One epoll loop.** Edge-triggered epoll, non-blocking sockets, and
  sendfile for file bodies - the kernel moves bytes from page cache to
  socket without a userspace copy.
- **Graceful shutdown.** SIGINT/SIGTERM stop accepting, close idle
  keep-alive connections immediately, let in-flight responses finish
  writing, and close.
- **OPTIONS and absolute-form.** `OPTIONS *` and `OPTIONS /any/path`
  answer 204 with the same Allow list; `GET http://host/path` serves its
  path, and a 400 when the authority and the Host header disagree.
- **Access log.** One line per request on stdout: peer, method, target,
  status, bytes - with control bytes escaped as \xNN so a hostile request
  cannot inject terminal escapes or fake log lines.

## Request limits

Every cap below is enforced while parsing, before any memory is committed to
the request:

- an 8 KB request line and 32 KB of headers in total,
- at most 64 headers per request,
- a request target of at most 2047 bytes (the 2048th byte is a 414),
- a 1 MB request body (413 past that, for both Content-Length and chunked),
- a 10-second header timeout, 15 seconds idle between keep-alive
  requests, 30 seconds for a stalled body or a stalled write - all measured
  from real read/write progress, so junk bytes cannot keep a dead
  connection alive. A head or body that stalls mid-request gets a 408
  before the close (`-H`/`-I`/`-B`/`-W` retune the deadlines); a merely
  idle or silent connection closes quietly,
- at most 1024 simultaneous connections (configurable with `-c`); past
  the cap, new connections get a best-effort 503 instead of a silent
  reset. Keep `-c` comfortably below `ulimit -n` (each connection needs a
  file descriptor, plus the listener, epoll and open files); if accept
  still hits EMFILE, the listener re-arms and retries once slots free.

## Safety posture

- **Path containment.** Targets are percent-decoded, split into segments and
  normalized; `..` can never climb above the document root, control bytes
  and NUL are rejected. Files are opened with openat2(RESOLVE_BENEATH):
  the kernel resolves the whole path atomically beneath the docroot, so a
  symlink that stays inside the tree is served, one that escapes fails
  with a 404, and there is no check-then-open window. On kernels without
  openat2 the fallback is realpath + a containment check + O_NOFOLLOW on
  the resolved leaf (safe while only the operator can write the docroot -
  the standing assumption for a static server).
  `GET /%2e%2e/%2e%2e/etc/passwd` is a plain 400.
- **Request-smuggling guards.** Conflicting Content-Length headers
  (identical repeats merge, as RFC 9110 8.6 permits), a Content-Length
  alongside Transfer-Encoding, and any Transfer-Encoding other than
  `chunked` are rejected before the body is read; a required `Expect` the
  server cannot meet is a 417. Bare-LF request heads are tolerated (RFC
  9112 2.2), while chunk framing stays CRLF-strict. A syntactically valid
  but unknown method - `get`, `brew` - is a 405 with an Allow list, never
  a parser-level 400.
- **Timeouts and caps everywhere.** Slowloris-style stalls hit the header
  timeout; partial bodies and blocked clients hit their own deadlines.
- **No directory leaks.** An unreadable or missing file is the same 404, so
  the response never reveals what exists on disk.

## Honest limits

- Linux only - the event loop is epoll, file bodies use sendfile, and
  containment uses openat2 (with a realpath fallback on older kernels).
- No TLS. Put mast behind a TLS-terminating proxy if you need HTTPS.
- Single process, single thread. One core serves everything.
- No virtual hosts, no content negotiation, no compression, no CGI.
- One range per request; multipart/byteranges is not implemented (a
  multi-range header is ignored and the full body served).
- HTTP/1.0 and HTTP/1.1 only - no HTTP/2, no WebSockets.

## Testing

`make test` runs three layers:

- a **290 checks** unit suite over the parser (including byte-by-byte
  fragmented feeds), path sanitizer, ranges, dates, chunked bodies and the
  full response pipeline against a temporary document root;
- a **100 checks** live-socket suite that runs the real server and talks
  to it over TCP: keep-alive, pipelining, chunked POSTs, exact wire framing
  after 405/416/HEAD-404, single-flight 1 MB bodies, a clean readable 413,
  100-continue (and its absence on HTTP/1.0), OPTIONS on * and on paths,
  absolute-form, suffix and overflowing ranges, 304s, strict If-Range,
  If-Match / If-Unmodified-Since 412s, smuggling rejections, identical
  duplicate Content-Lengths, bare-LF heads, in-tree symlinks served and
  escaping ones refused, the 2047/2048-byte target boundary, mid-sendfile
  truncation, write/idle/header/body timeout deadlines with a 408 for
  stalled requests, a 503 at the connection cap, pipelining fairness under
  flood, fast SIGTERM shutdown with idle keep-alive conns while in-flight
  responses complete, EMFILE recovery, a root-document-tree server and 50
  concurrent clients;
- `tests/fuzz-sockets.py`, a stateful socket fuzzer: random mutated
  sessions with fragmentation, pipelining, half-closes and RSTs against
  the live server, asserting it never crashes and still answers cleanly
  after the storm;
- `tests/check-docs.sh`, which pins this README's numbers and limits to the
  code that enforces them.

`make sanitize` reruns the whole suite under AddressSanitizer and
UndefinedBehaviorSanitizer, and `python3 tests/fuzz.py` throws mutated and
random byte streams at the parser (default 20 programs, 30 blobs) with the
same suite available in a sanitized build.

## Layout

```
src/http.h, src/http.c      incremental HTTP/1.1 parser, path sanitizer,
                            chunked body reader, response builders
src/files.h, src/files.c    realpath containment, ETag/Last-Modified,
                            conditional + range logic, content types
src/handle.h, src/handle.c  request -> response decisions
src/server.h, src/server.c  epoll loop, connection state machine, timeouts
src/main.c                  command line
wasm/                       browser demo wrapper (emscripten)
tests/                      unit, live-socket, fuzz and docs suites
docs/                       the wasm demo page (GitHub Pages)
```
