#!/bin/sh
# CLI + socket tests: starts the real server on a fixture docroot and drives
# it with python3 socket clients (keep-alive, pipelining, chunked bodies,
# partial sends, graceful shutdown). Prints "cli: N checks passed".
set -u
cd "$(dirname "$0")/.."
BIN=./build/mast
if [ ! -x "$BIN" ]; then echo "run-tests: build first (make)" >&2; exit 1; fi
PORT=18099
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"; [ -n "${SPID:-}" ] && kill "$SPID" 2>/dev/null' EXIT

printf '<h1>hi</h1>\n' > "$ROOT/index.html"
printf 'plain text\n' > "$ROOT/a.txt"
mkdir "$ROOT/sub"
printf 'sub index\n' > "$ROOT/sub/index.html"
dd if=/dev/urandom of="$ROOT/big.bin" bs=1024 count=256 2>/dev/null
head -c 2000000 /dev/urandom > "$ROOT/two-mb.bin" 2>/dev/null || dd if=/dev/urandom of="$ROOT/two-mb.bin" bs=1024 count=2000 2>/dev/null

"$BIN" -d "$ROOT" -p $PORT > "$ROOT/server.log" 2>&1 &
SPID=$!
sleep 0.4

python3 - "$PORT" <<'PYEOF'
import socket, sys, time

port = int(sys.argv[1])
checks = 0
fails = 0

def ok(cond, label):
    global checks, fails
    checks += 1
    if not cond:
        print(f"FAIL: {label}")
        fails = 1

def request(raw, read_all=True, shutdown=True):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(raw)
    if shutdown:
        s.shutdown(socket.SHUT_WR)
    out = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        out += chunk
        if not read_all and out:
            break
    s.close()
    return out

def status(resp):
    return int(resp.split(b" ", 2)[1])

# basics
r = request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200 and b"<h1>hi</h1>" in r, "GET / serves index.html")
ok(b"ETag:" in r and b"Last-Modified:" in r and b"Accept-Ranges: bytes" in r, "validators present")

r = request(b"HEAD /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
head, _, body = r.partition(b"\r\n\r\n")
ok(status(r) == 200 and body == b"", "HEAD sends headers only")
ok(b"Content-Length: 11" in head, "HEAD reports the real length")

r = request(b"GET /nope HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 404 and b"nothing lives" in r, "404 page")

r = request(b"GET / HTTP/1.0\r\n\r\n")
ok(status(r) == 200, "HTTP/1.0 works")

# keep-alive: two requests, one connection, sequential
s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.15)
first = s.recv(65536)
ok(status(first) == 200, "keep-alive: first request answered")
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
second = b""
while True:
    c = s.recv(65536)
    if not c:
        break
    second += c
ok(status(second) == 200 and b"plain text" in second, "keep-alive: second request answered")
s.close()

# pipelining: two requests in one write, two responses in order
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
ok(r.count(b"HTTP/1.1 200") == 2, "pipelined requests both answered")
ok(r.find(b"plain text") < r.find(b"<h1>hi</h1>"), "pipelined responses stay in order")

# fragmented: byte-by-byte writes
s = socket.create_connection(("127.0.0.1", port), timeout=5)
for b in b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n":
    s.send(bytes([b]))
    time.sleep(0.002)
frag = b""
while True:
    c = s.recv(65536)
    if not c:
        break
    frag += c
ok(status(frag) == 200 and b"plain text" in frag, "byte-by-byte request answered")
s.close()

# POST echo: content-length and chunked
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello")
ok(status(r) == 200 and r.endswith(b"hello"), "POST /echo content-length")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
ok(status(r) == 200 and r.endswith(b"hello world"), "POST /echo chunked")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n2;ext=x\r\nhi\r\n0\r\nX-T: v\r\n\r\n")
ok(status(r) == 200 and r.endswith(b"hi"), "chunked with extension and trailer")
r = request(b"POST /elsewhere HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx")
ok(status(r) == 404, "POST to a non-echo path is 404")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n" + b"x" * 100)
ok(status(r) == 413, "over-cap declared body is 413")

# large file + range
r = request(b"GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200 and len(r.split(b"\r\n\r\n", 1)[1]) == 262144, "256KB file streams complete")
r = request(b"GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=-100\r\n\r\n")
ok(status(r) == 206 and len(r.split(b"\r\n\r\n", 1)[1]) == 100, "suffix range")
r = request(b"GET /two-mb.bin HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200 and len(r.split(b"\r\n\r\n", 1)[1]) == 2000000, "2MB file streams complete")

# conditional
import re
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
etag = re.search(rb'ETag: ("[^"]+")', r).group(1)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: " + etag + b"\r\n\r\n")
ok(status(r) == 304, "If-None-Match short-circuits")

# errors and abuse
r = request(b"GARBAGE\r\n\r\n")
ok(status(r) == 400, "garbage request line is 400")
r = request(b"GET / HTTP/2.0\r\nHost: x\r\n\r\n")
ok(status(r) == 505, "HTTP/2 is 505")
r = request(b"PUT /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 405 and b"Allow:" in r, "PUT is 405 with Allow")
r = request(b"GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 400, "encoded traversal is 400")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n")
ok(status(r) == 400, "CL+TE smuggling is 400")
r = request(b"GET / HTTP/1.1\r\nHost: x\r\n" + b"X-Pad: " + b"y" * 33000 + b"\r\n\r\n")
ok(status(r) in (431, 414), "oversize headers are 431/414")
r = request(b"GET /" + b"z" * 3000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 414, "oversize target is 414")

# concurrency: 50 simultaneous clients
import threading
results = []
def worker(i):
    try:
        r = request(f"GET /a.txt?n={i} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
        results.append(status(r) == 200)
    except Exception:
        results.append(False)
threads = [threading.Thread(target=worker, args=(i,)) for i in range(50)]
for t in threads: t.start()
for t in threads: t.join()
ok(all(results), "50 concurrent clients all served")

print(f"cli: {checks} checks passed")
sys.exit(fails)
PYEOF
RC=$?
kill -TERM $SPID 2>/dev/null
sleep 0.3
if kill -0 $SPID 2>/dev/null; then echo "run-tests: server did not stop on SIGTERM" >&2; RC=1; else echo "shutdown: graceful"; fi
SPID=
exit $RC
