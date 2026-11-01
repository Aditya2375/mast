# mast

mast is a small HTTP/1.1 server written in C11 plus Linux/POSIX
(epoll, sendfile, realpath), with no dependencies beyond libc. It serves
static files with keep-alive, pipelining, range requests, conditional
requests and chunked request bodies, and it echoes POSTs. The server core is
2648 lines of C.

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
`-c N` maximum simultaneous connections (default 1024), `-H`/`-I`/`-B`/`-W`
to retune the header, idle, body and write timeouts in milliseconds,
`-R N` requests per connection before a graceful close (default 1000),
`-A MS` maximum connection age in milliseconds (default 300000) and `-M MB`
total budget for dynamic connection buffers - parse backlog, request
bodies and response pages (default 256; pressure earns the offending
connection a clean 503, never a process crash). The fixed per-connection
struct (about 49 KB times `-c` connections) and the static file buffers
are not charged against it.

## What it does

- **Static files.** GET and HEAD over a document root, `index.html` for
  directories, a 301 redirect when a directory URL lacks its trailing slash
  (the query string rides along), and a content-type table for the common text, image, font, audio, video
  and archive types.
- **Conditional requests.** Every file carries a strong ETag
  (`"inode-size-seconds-nanoseconds"`, so a same-second same-size rewrite
  still changes it) and a Last-Modified date. Preconditions run in RFC
  9110 13.2.2 order: `If-Match` (strong comparison over a list, `*`
  included) and `If-Unmodified-Since` answer 412 on failure - but a
  precondition on a path that does not exist is a plain 404, like nginx
  and Apache: there is no current representation to precondition; then
  `If-None-Match` - including comma lists and weak `W/` prefixes - and
  `If-Modified-Since` get a 304 with no body. `If-Range` gates a range on
  its validator, strictly: the exact entity-tag (a weak tag never holds)
  or a date exactly equal to Last-Modified - a future or stale date gets
  the full 200 body. The two list headers combine when repeated
  (`If-Match: "a"` plus `If-Match: "b"` is one list, per RFC 9110 5.3);
  a repeated `If-Modified-Since`, `If-Unmodified-Since` or `If-Range`
  is ambiguous, so it is a 400. Dates parse strictly, as the fixed
  29-byte IMF-fixdate shape with zero-padded unsigned fields: a made-up
  weekday name, a sign, a lone digit or a year before 1970 means the
  header is ignored, never rejected and never honored. `POST /echo` stores nothing, so any
  `If-Match` there is a 412.
- **Range requests.** A single byte range per request - `bytes=0-99`,
  `bytes=500-` or the last N bytes with `bytes=-64` - answered 206 with a
  Content-Range header; an unsatisfiable range is a 416. A suffix range
  on an empty file is 416 too - no byte exists to satisfy it (RFC 9110
  14.2 leaves the choice to implementations). Unknown units,
  malformed ranges, invalid specs (`bytes=-` and `bytes=-0` are invalid,
  not unsatisfiable) and multi-range requests are ignored and get the
  full 200 body, as RFC 9110 permits.
- **POST /echo.** The request body comes back verbatim with its
  content-type, whether it arrived with a Content-Length or chunked
  transfer-coding (chunk extensions and trailers included, though a
  control byte in either - HTAB aside - is a 400).
  `Expect: 100-continue` gets an interim 100 only when the body will be
  accepted, the final error otherwise; any other Expect is a 417, and a
  repeated Expect with a different value is a 400.
- **Keep-alive and pipelining.** HTTP/1.1 connections stay open across
  requests; several requests written in one flight are answered in order,
  with a per-turn budget (8 responses or 512 KB, and a single large
  response's write included) so one fast client cannot starve the others. A pipelined flood past the header cap
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
  keep-alive connections immediately, and let in-flight responses finish
  writing - up to a 5-second grace cap, so one non-reading client cannot
  hold the process hostage; a second signal exits at once.
- **OPTIONS and absolute-form.** `OPTIONS *` and `OPTIONS /any/path`
  answer 204 with the same Allow list (conditional headers are ignored
  there - OPTIONS describes the server, not a representation);
  `GET http://host/path` serves its path, and a 400 when the authority
  and the Host header disagree - a deliberate hardening deviation from
  RFC 9112 3.2.2, which says to ignore Host on an absolute-form target:
  mast treats the disagreement as smuggling bait and refuses.
- **HEAD stays bodyless.** A response to HEAD never carries a body - not
  even an error page, and not even when the error fires before the parser
  ran: the header-cap 431, the budget 503 and the stall 408 judge the
  method from the failed head's buffered prefix. The Content-Length still
  reports what a GET would have received.
- **Access log.** One line per request on stdout: peer, method, target,
  status, bytes - with control bytes escaped as \xNN so a hostile request
  cannot inject terminal escapes or fake log lines.

## Request limits

Every cap below is enforced while parsing, before any memory is committed to
the request:

- an 8 KB request line and 32 KB of headers in total,
- at most 64 headers per request, at most 8 empty lines before the
  request (RFC 9112 3.5 asks a server to tolerate one - and mast counts
  them across dribbled reads, not per recv), chunk-size lines of 1021
  bytes and trailer lines of 4092 (both plus the CRLF; room for long
  chunk extensions),
- a request target of at most 2047 bytes (the 2048th byte is a 414),
- a 1 MB request body (413 past that, for both Content-Length and chunked),
- every phase has two clocks: a stall timeout refreshed by real
  read/write progress (a 10-second header timeout, 15 s idle between
  keep-alive requests, 30 s for a stalled body or a stalled write) and an
  absolute
  deadline progress cannot move - the header phase ends 10 s after the
  first byte of the request head, the body phase 30 s after the body
  begins, and the write phase 8 times the write timeout after the response
  begins. A trickler feeding one byte at a time dies on schedule; a head
  or body that stalls mid-request gets a 408 before the close
  (`-H`/`-I`/`-B`/`-W` retune the deadlines); a merely idle or silent
  connection closes quietly,
- per-connection caps: at most 1000 requests per connection (the last
  response says `Connection: close`) and a maximum connection age of 5
  minutes, lingering closes included, so no connection lives forever no
  matter how politely it behaves (`-R` and `-A` retune both),
- at most 1024 simultaneous connections (configurable with `-c`); past
  the cap, a small set of overflow slots answers 503 through the normal
  machinery - a full flush, then a lingering close that drains the unread
  request, so a well-behaved client always receives the answer instead of
  a reset that looks like a network fault. Only when even those slots are
  busy does mast fall back to one best-effort write before closing. Keep `-c` comfortably below `ulimit -n` (each connection needs a
  file descriptor, plus the listener, epoll and open files); if accept
  still hits EMFILE, the listener re-arms and retries once slots free.

## Safety posture

- **Path containment.** Targets are percent-decoded, split into segments and
  normalized; `..` can never climb above the document root, control bytes
  and NUL are rejected. Files are opened with openat2(RESOLVE_BENEATH):
  the kernel resolves the whole path atomically beneath the docroot, so a
  symlink that stays inside the tree is served, one that escapes fails
  with a 404, and there is no check-then-open window. An encoded slash
  decodes to a real separator (`/dir%2Findex.html` is `/dir/index.html`)
  - never an escape, since normalization happens after decoding - so a
  fronting proxy that routes on the raw path should normalize first. On kernels without
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
  timeout, and a byte-at-a-time trickler hits the absolute header deadline
  even though every byte refreshes the stall clock; partial bodies and
  blocked writes have their own stall clocks plus absolute caps, and the
  request and age caps close even a perfectly behaved connection.
- **No directory leaks.** An unreadable or missing file is the same 404, so
  the response never reveals what exists on disk.
- **Only regular files are served.** Every open carries O_NONBLOCK and
  the result must pass S_ISREG, so a FIFO, socket or device node in the
  docroot is a fast 404 - it can never block the single-threaded loop on
  open. If openat2 is missing or a seccomp filter blocks it (ENOSYS,
  EPERM, EINVAL), the realpath fallback takes over.

## Honest limits

- Linux only - the event loop is epoll, file bodies use sendfile, and
  containment uses openat2 (with the realpath fallback above).
- No TLS. Put mast behind a TLS-terminating proxy if you need HTTPS.
- Single process, single thread. One core serves everything.
- No virtual hosts, no content negotiation, no compression, no CGI.
- One range per request; multipart/byteranges is not implemented (a
  multi-range header - including a repeated Range header, which
  combines into one list per RFC 9110 5.3 no matter how many lines - is
  ignored and the full body served).
- An HTTP/1.1 request with an empty Host header is a 400 (the
  conservative reading of RFC 9112 3.2).
- The -M budget bounds the dynamic buffers held at once: each
  connection's parse backlog (pipelined bytes waiting their turn),
  request body and response pages - error pages and the 100-continue
  interim included, and an echo's response copy projected at its
  buffer-doubling target before the body is accepted. A connection that would push past it
  gets a 503 with Retry-After: 1 and closes. The fixed Conn struct
  (about 49 KB per connection - the request, response,
  body-reader and parser structs alone are 49152 bytes, so `-c 1024`
  carries roughly 50 MB whether the budget is reached or not) and the static file buffers are
  separate. A malloc failure inside the budget means the host itself is
  out of memory, which no static server survives usefully - mast exits
  70 then.
- Graceful shutdown lets in-flight writes finish for up to 5 seconds;
  a second SIGINT/SIGTERM exits at once.
- HTTP/1.0 and HTTP/1.1 only - no HTTP/2, no WebSockets.

## Testing

`make test` runs three layers:

- a **353 checks** unit suite over the parser (including byte-by-byte
  fragmented feeds and a CPU bound on a 26 KB head dribbled in one byte
  at a time through the library feed API - the socket server's side of
  the same guarantee is tested live, below), path sanitizer, ranges,
  dates, chunked bodies and the full response pipeline against a
  temporary document root;
- a **213 checks** live-socket suite that runs the real server and talks
  to it over TCP: keep-alive, pipelining, chunked POSTs, exact wire framing
  after 405/416/HEAD-404, single-flight 1 MB bodies, a clean readable 413,
  100-continue (and its absence on HTTP/1.0), OPTIONS on * and on paths
  (the 204 head carrying no Content-Length, per RFC 9110 8.6),
  absolute-form, suffix and overflowing ranges, 304s with their 200's
  validators, strict If-Range,
  If-Match / If-Unmodified-Since 412s, smuggling rejections, identical
  duplicate Content-Lengths, bare-LF heads, in-tree symlinks served and
  escaping ones refused, the 2047/2048-byte target boundary, mid-sendfile
  truncation, write/idle/header/body timeout deadlines with a 408 for
  stalled requests, a byte-drip trickler dying at the absolute header
  deadline while a slow-but-legitimate client under it is served,
  Expect-rejection responses saying `Connection: close` on the wire, the
  request cap closing after the cap-th response, the connection-age cap
  beating a longer idle timeout, a flooding peer failing to pin the
  lingering-close drain, a 503 at the connection cap, pipelining fairness
  under flood, fast SIGTERM shutdown with idle keep-alive conns while
  in-flight responses complete - capped by a 5-second grace and a second
  signal that forces exit - special files (FIFO, socket, device) as fast
  404s, blank-line floods, the usage text naming every flag, the memory budget refusing with 503 (plain
  bodies, a chunked batch whose buffer doubling projects past it, and an
  exact ledger across keep-alive reuse), a dribbled no-LF head never
  blocking other clients - including a 28 KB header line dribbled in one
  byte at a time past the 8 KB request-line cap, the server's /proc CPU
  for the whole dribble bounded at 600 ms - an 800-byte directory redirect with a
  Content-Length that matches the delivered body byte for byte, invalid
  suffix ranges ignored with a 200, one Range honored but two through
  five ignored, junk dates ignored, control bytes (NUL included) in
  chunk extensions and trailers refused, conditional repeats combined or
  refused, Transfer-Encoding lines combined across lines, a suffix
  range on an empty file answered 416, the blank-line cap counted
  across dribbled reads and reset per request, a pipelined head refused
  before recv when the memory budget cannot fund its buffer - the
  pending 2 MB response completing first, a differing repeated Expect refused, error responses charged
  to the memory budget and handed back, bodyless error pages to HEAD
  from the pre-parser paths (a capped flood's 431, a budget 503 over a
  buffered head prefix, a stalled head's 408), a 408-response budget
  ledger probed with an exact-fit fill, a directory 301 keeping its
  query string, the Expect-gated oversize 413 bodyless on HEAD, an echo
  refused when its projected response doubling would pass the budget,
  two concurrent 2 MB downloads completing byte-exact under the
  per-turn write budget, a lingering close reaped by the age cap, the
  at-cap 503 carrying Date and Server and arriving complete even with
  the client's request still unread, EMFILE
  recovery, a root-document-tree server and 50 concurrent clients;
- `tests/fuzz-sockets.py`, a stateful socket fuzzer: random mutated
  sessions with fragmentation, pipelining, half-closes, RSTs, oversized
  header blocks, all-at-once floods and concurrent bursts against the
  live server, asserting it never crashes and still answers cleanly
  after the storm;
- `tests/check-docs.sh`, which pins this README's numbers and limits to the
  code that enforces them.

`make sanitize` reruns the whole suite under AddressSanitizer and
UndefinedBehaviorSanitizer, and `python3 tests/fuzz.py` throws mutated and
random byte streams at the parser (default 100 programs, 200 blobs) with the
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
