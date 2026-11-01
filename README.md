# mast

mast is a small HTTP/1.1 server written in C11 plus Linux/POSIX
(epoll, sendfile, realpath), with no dependencies beyond libc. It serves
static files with keep-alive, pipelining, range requests, conditional
requests and chunked request bodies, and it echoes POSTs. The server core is
1414 lines of C.

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
`-c N` maximum simultaneous connections (default 1024).

## What it does

- **Static files.** GET and HEAD over a document root, `index.html` for
  directories, a 301 redirect when a directory URL lacks its trailing slash,
  and a content-type table for the common text, image, font, audio, video
  and archive types.
- **Conditional requests.** Every file carries a strong ETag
  (`"mtime-size"`) and a Last-Modified date; `If-None-Match` and
  `If-Modified-Since` get a 304 with no body.
- **Range requests.** A single byte range per request - `bytes=0-99`,
  `bytes=500-` or the last N bytes with `bytes=-64` - answered 206 with a
  Content-Range header; an unsatisfiable range is a 416. Multi-range
  requests fall back to a full 200.
- **POST /echo.** The request body comes back verbatim with its
  content-type, whether it arrived with a Content-Length or chunked
  transfer-coding (chunk extensions and trailers included).
- **Keep-alive and pipelining.** HTTP/1.1 connections stay open across
  requests; several requests written in one flight are answered in order.
  HTTP/1.0 and `Connection: close` get their answer and a clean close.
- **One epoll loop.** Edge-triggered epoll, non-blocking sockets, and
  sendfile for file bodies - the kernel moves bytes from page cache to
  socket without a userspace copy.
- **Graceful shutdown.** SIGINT/SIGTERM stop accepting, let in-flight
  responses finish writing, and close.
- **Access log.** One line per request on stdout: peer, method, target,
  status, bytes.

## Request limits

Every cap below is enforced while parsing, before any memory is committed to
the request:

- an 8 KB request line and 32 KB of headers in total,
- at most 64 headers per request,
- a 2048-byte request target (414 past that),
- a 1 MB request body (413 past that, for both Content-Length and chunked),
- a 10-second header timeout, 15 seconds between keep-alive requests,
  30 seconds for a stalled body or a stalled write,
- at most 1024 simultaneous connections (configurable with `-c`).

## Safety posture

- **Path containment.** Targets are percent-decoded, split into segments and
  normalized; `..` can never climb above the document root, control bytes
  and NUL are rejected, and the resolved path must stay under the realpath
  of the root. Symlinks that escape the root fail containment, and every
  open uses O_NOFOLLOW so a symlink swapped in after the check cannot be
  followed. `GET /%2e%2e/%2e%2e/etc/passwd` is a plain 400.
- **Request-smuggling guards.** Duplicate Content-Length headers, a
  Content-Length alongside Transfer-Encoding, and any Transfer-Encoding
  other than `chunked` are rejected before the body is read; a required
  `Expect` the server cannot meet is a 417.
- **Timeouts and caps everywhere.** Slowloris-style stalls hit the header
  timeout; partial bodies and blocked clients hit their own deadlines.
- **No directory leaks.** An unreadable or missing file is the same 404, so
  the response never reveals what exists on disk.

## Honest limits

- Linux only - the event loop is epoll and file bodies use sendfile.
- No TLS. Put mast behind a TLS-terminating proxy if you need HTTPS.
- Single process, single thread. One core serves everything.
- No virtual hosts, no content negotiation, no compression, no CGI.
- One range per request; multipart/byteranges is not implemented.
- HTTP/1.0 and HTTP/1.1 only - no HTTP/2, no WebSockets.

## Testing

`make test` runs three layers:

- a **193 checks** unit suite over the parser (including byte-by-byte
  fragmented feeds), path sanitizer, ranges, dates, chunked bodies and the
  full response pipeline against a temporary document root;
- a **28 checks** live-socket suite that runs the real server and talks to
  it over TCP: keep-alive, pipelining, chunked POSTs, 256 KB and 2 MB
  bodies, suffix ranges, 304s, smuggling rejections, 50 concurrent clients
  and a graceful SIGTERM shutdown;
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
