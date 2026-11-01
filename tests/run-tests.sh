#!/bin/sh
# CLI + socket tests: starts the real server on a fixture docroot and drives
# it with python3 socket clients (keep-alive, pipelining, chunked bodies,
# partial sends, graceful shutdown). Prints "cli: N checks passed".
set -u
cd "$(dirname "$0")/.."
BIN=${BIN:-./build/mast}
if [ ! -x "$BIN" ]; then echo "run-tests: build first (make)" >&2; exit 1; fi
PORT=18099
PORT2=18100
PORT3=18101
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"; for p in ${SPID:-} ${SPID2:-} ${SPID3:-}; do kill "$p" 2>/dev/null; done' EXIT

printf '<h1>hi</h1>\n' > "$ROOT/index.html"
printf 'plain text\n' > "$ROOT/a.txt"
mkdir "$ROOT/sub"
printf 'sub index\n' > "$ROOT/sub/index.html"
dd if=/dev/urandom of="$ROOT/big.bin" bs=1024 count=256 2>/dev/null
head -c 2000000 /dev/urandom > "$ROOT/two-mb.bin" 2>/dev/null || dd if=/dev/urandom of="$ROOT/two-mb.bin" bs=1024 count=2000 2>/dev/null
# too big to fit in kernel socket buffers: mid-transfer truncation is testable
dd if=/dev/urandom of="$ROOT/fifty-mb.bin" bs=1048576 count=50 2>/dev/null

"$BIN" -d "$ROOT" -p $PORT > "$ROOT/server.log" 2>&1 &
SPID=$!
# a second server with the filesystem root as docroot (containment at "/")
"$BIN" -d / -p $PORT2 > "$ROOT/server2.log" 2>&1 &
SPID2=$!
# a third with short timeouts, for the idle/header/write deadline tests
"$BIN" -d "$ROOT" -p $PORT3 -H 2000 -I 3000 -W 1500 > "$ROOT/server3.log" 2>&1 &
SPID3=$!
sleep 0.4

python3 - "$PORT" "$PORT2" "$PORT3" "$ROOT" <<'PYEOF'
import socket, sys, time, re, os

port = int(sys.argv[1])
port2 = int(sys.argv[2])
port3 = int(sys.argv[3])
root = sys.argv[4]
checks = 0
fails = 0

def ok(cond, label):
    global checks, fails
    checks += 1
    if not cond:
        print(f"FAIL: {label}")
        fails = 1

last_reset = False

def request(raw, read_all=True, shutdown=True, to=port):
    global last_reset
    last_reset = False
    s = socket.create_connection(("127.0.0.1", to), timeout=3)
    s.sendall(raw)
    if shutdown:
        s.shutdown(socket.SHUT_WR)
    out = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        except ConnectionResetError:
            last_reset = True
            break
        if not chunk:
            break
        out += chunk
        if not read_all and out:
            break
    s.close()
    return out

def read_response(s, leftover=b"", head_only=False):
    """Read exactly one response off the wire: head, then precisely
    Content-Length body bytes. Returns (head_block, body, leftover)."""
    data = leftover
    while b"\r\n\r\n" not in data:
        chunk = s.recv(65536)
        if not chunk:
            raise EOFError("peer closed mid-head")
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    m = re.search(rb"Content-Length: (\d+)", head)
    n = int(m.group(1)) if m else 0
    if head_only or head.startswith((b"HTTP/1.1 204", b"HTTP/1.1 304")):
        n = 0
    while len(rest) < n:
        chunk = s.recv(65536)
        if not chunk:
            raise EOFError("peer closed mid-body")
        rest += chunk
    return head + b"\r\n\r\n", rest[:n], rest[n:]

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
s = socket.create_connection(("127.0.0.1", port), timeout=3)
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
s = socket.create_connection(("127.0.0.1", port), timeout=3)
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

# --- framing: a 405 then a pipelined GET must stay perfectly aligned ---
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"PUT /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
head1, body1, rest = read_response(s)
ok(head1.startswith(b"HTTP/1.1 405") and b"Allow: GET, HEAD, POST\r\n" in head1,
   "405: Allow inside the head, before the blank line")
m = re.search(rb"Content-Length: (\d+)", head1)
ok(int(m.group(1)) == len(body1) and body1.startswith(b"405 Method Not Allowed"),
   "405: Content-Length exactly matches the delivered body")
head2, body2, rest = read_response(s, rest)
ok(head2.startswith(b"HTTP/1.1 200") and body2 == b"plain text\n",
   "pipelined response after a 405 parses cleanly")
s.close()

# --- framing: a 416 then a pipelined GET ---
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=999999-\r\n\r\n"
          b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
head1, body1, rest = read_response(s)
ok(head1.startswith(b"HTTP/1.1 416") and b"Content-Range: bytes */262144\r\n" in head1,
   "416: Content-Range inside the head")
m = re.search(rb"Content-Length: (\d+)", head1)
ok(int(m.group(1)) == len(body1), "416: Content-Length exactly matches the delivered body")
head2, body2, rest = read_response(s, rest)
ok(head2.startswith(b"HTTP/1.1 200"), "pipelined response after a 416 parses cleanly")
s.close()

# --- HEAD errors: Content-Length semantics, no body bytes on the wire ---
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"HEAD /nope HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
head1, body1, rest = read_response(s, head_only=True)
ok(head1.startswith(b"HTTP/1.1 404") and b"Content-Length: " in head1 and body1 == b"",
   "HEAD 404 reports a length but sends no body")
head2, body2, rest = read_response(s, rest)
ok(head2.startswith(b"HTTP/1.1 200") and body2 == b"plain text\n",
   "pipelined response after a HEAD 404 parses cleanly")
s.close()

# --- big POST bodies in a single flight (head+body share one send) ---
for n in (32700, 100000, 1048576):
    r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % n + b"y" * n)
    ok(status(r) == 200 and len(r.split(b"\r\n\r\n", 1)[1]) == n,
       f"single-flight {n}-byte body echoes complete")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n" + b"y" * 1048577)
ok(status(r) == 413, "1MB+1 body is 413")
ok(not last_reset and b"413" in r, "the 413 arrives readable, no RST")

# --- Expect: 100-continue ---
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n")
time.sleep(0.3)
interim = s.recv(65536)
ok(interim == b"HTTP/1.1 100 Continue\r\n\r\n", "100 Continue precedes an accepted body")
s.sendall(b"hello")
time.sleep(0.3)
final = s.recv(65536)
ok(final.startswith(b"HTTP/1.1 200") and final.endswith(b"hello"), "body after 100 Continue echoes")
s.close()
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: banana\r\n\r\nhello")
ok(status(r) == 417, "an unknown Expect is 417")
r = request(b"POST /nope HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\nx")
ok(status(r) == 404 and not r.startswith(b"HTTP/1.1 100"),
   "a rejected request skips the interim 100")

# --- OPTIONS * and absolute-form ---
r = request(b"OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 204 and b"Allow: GET, HEAD, POST, OPTIONS" in r, "OPTIONS * is 204 with Allow")
r = request(b"GET http://x/a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200 and b"plain text" in r, "absolute-form serves its path")
r = request(b"GET http://a/a.txt HTTP/1.1\r\nHost: b\r\n\r\n")
ok(status(r) == 400, "absolute-form disagreeing with Host is 400")

# --- header strictness over the wire ---
r = request(b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n")
ok(status(r) == 400, "duplicate Host is 400")
r = request(b"GET /a\x01b HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 400, "raw control byte in the target is 400")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\nhello")
ok(status(r) == 400, "Content-Length with a sign is 400")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
ok(status(r) == 400, "duplicate Transfer-Encoding is 400")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n")
ok(status(r) == 400, "malformed chunk framing is 400, not 413")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n100001\r\n" + b"y" * 64)
ok(status(r) == 413, "an over-cap chunk size is 413")

# --- Connection tokens are exact, not substrings ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: disclose\r\n\r\n"
            b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
ok(r.count(b"HTTP/1.1 200") == 2, "Connection: disclose keeps the connection alive")

# --- If-None-Match list / If-Range over the wire ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: \"nope\", " + etag + b"\r\n\r\n")
ok(status(r) == 304, "If-None-Match list matches a member")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Range: \"stale\"\r\nRange: bytes=0-2\r\n\r\n")
ok(status(r) == 200, "If-Range with a stale validator ignores the Range")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Range: " + etag + b"\r\nRange: bytes=0-2\r\n\r\n")
ok(status(r) == 206, "If-Range with the current etag serves the range")

# --- multi-range falls back to a full 200 ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=0-1,3-4\r\n\r\n")
ok(status(r) == 200 and b"plain text" in r, "multi-range serves the full body")

# --- access log escapes control bytes ---
request(b"GET /\xe2\x98\x83 HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.3)
log = open(os.path.join(root, "server.log"), "rb").read()
ok(b"\\xe2\\x98\\x83" in log, "access log escapes non-printable bytes")

# --- truncated mid-sendfile: close, never spin ---
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"GET /fifty-mb.bin HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.5)
s.recv(65536)   # the head + first body bytes; the rest outruns the client
os.truncate(os.path.join(root, "fifty-mb.bin"), 0)
closed = False
try:
    s.settimeout(4)
    while True:
        c = s.recv(65536)   # each read wakes EPOLLOUT; sendfile now hits EOF
        if not c:
            closed = True
            break
except (socket.timeout, ConnectionResetError):
    pass
ok(closed, "truncated mid-sendfile: connection closes instead of spinning")
s.close()
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200, "server healthy after a truncated transfer")

# --- a client that stops reading dies at the write timeout, junk or not ---
s = socket.create_connection(("127.0.0.1", port3), timeout=3)
s.sendall(b"GET /two-mb.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
s.recv(65536)   # head + some body, then stop reading entirely
deadline = time.time() + 8
closed = False
while time.time() < deadline:
    try:
        s.sendall(b"X")     # junk bytes must not count as progress
    except OSError:
        closed = True
        break
    time.sleep(0.4)
    try:
        s.settimeout(0.01)
        if s.recv(1) == b"":
            closed = True
            break
    except socket.timeout:
        pass
    except ConnectionResetError:
        closed = True
        break
ok(closed, "write timeout fires despite junk bytes on the socket")
s.close()

# --- header timeout for a silent fresh connection (port3: 2s) ---
s = socket.create_connection(("127.0.0.1", port3), timeout=3)
time.sleep(3.5)
s.settimeout(1)
try:
    closed = s.recv(1) == b""
except (ConnectionResetError, socket.timeout):
    closed = True   # a timed-out conn may RST or FIN depending on drain state
ok(closed, "a silent fresh connection dies at the header timeout")
s.close()

# --- keep-alive idle uses the idle timeout (3s), not the header timeout (2s) ---
s = socket.create_connection(("127.0.0.1", port3), timeout=3)
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.3)
s.recv(65536)
time.sleep(2.5)   # past the 2s header timeout, inside the 3s idle timeout
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.3)
r2 = s.recv(65536)
ok(r2.startswith(b"HTTP/1.1 200"), "keep-alive idle survives past the header timeout")
time.sleep(4)     # now past the idle timeout
s.settimeout(1)
try:
    closed = s.recv(1) == b""
except (ConnectionResetError, socket.timeout):
    closed = True
ok(closed, "an idle keep-alive connection dies at the idle timeout")
s.close()

# --- docroot "/" serves the real tree (containment at dlen==1) ---
r = request(b"GET /etc/hostname HTTP/1.1\r\nHost: x\r\n\r\n", to=port2)
ok(status(r) == 200 and len(r.split(b"\r\n\r\n", 1)[1]) > 0,
   "docroot / serves /etc/hostname")
r = request(b"GET /definitely-not-here-xyz HTTP/1.1\r\nHost: x\r\n\r\n", to=port2)
ok(status(r) == 404, "docroot / still 404s a missing path")

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
ok_stop=1
for v in SPID SPID2 SPID3; do
  eval "p=\$$v"
  kill -TERM "$p" 2>/dev/null
  n=0
  while kill -0 "$p" 2>/dev/null && [ $n -lt 10 ]; do sleep 0.5; n=$((n+1)); done
  if kill -0 "$p" 2>/dev/null; then
    echo "run-tests: server $v did not stop on SIGTERM" >&2
    cp "$ROOT/server.log" /tmp/mast-srv1.log 2>/dev/null
    cp "$ROOT/server2.log" /tmp/mast-srv2.log 2>/dev/null
    cp "$ROOT/server3.log" /tmp/mast-srv3.log 2>/dev/null
    ok_stop=0
  fi
  eval "$v="
done
[ $ok_stop -eq 1 ] && echo "shutdown: graceful" || RC=1
exit $RC
