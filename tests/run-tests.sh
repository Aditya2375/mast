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
trap 'rm -rf "$ROOT"; for p in ${SPID:-} ${SPID2:-} ${SPID3:-} ${SPID4:-} ${SPID5:-} ${SPID6:-} ${SPID7:-} ${SPID8:-} ${SPID9:-}; do kill "$p" 2>/dev/null; done' EXIT

printf '<h1>hi</h1>\n' > "$ROOT/index.html"
printf 'plain text\n' > "$ROOT/a.txt"
: > "$ROOT/empty.txt"
mkdir "$ROOT/sub"
printf 'sub index\n' > "$ROOT/sub/index.html"
dd if=/dev/urandom of="$ROOT/big.bin" bs=1024 count=256 2>/dev/null
head -c 2000000 /dev/urandom > "$ROOT/two-mb.bin" 2>/dev/null || dd if=/dev/urandom of="$ROOT/two-mb.bin" bs=1024 count=2000 2>/dev/null
# too big to fit in kernel socket buffers: mid-transfer truncation is testable
dd if=/dev/urandom of="$ROOT/fifty-mb.bin" bs=1048576 count=50 2>/dev/null
# symlink fixtures: one inside the tree (served), one escaping it (refused)
ln -s a.txt "$ROOT/link.txt"
ln -s /etc/passwd "$ROOT/escape.txt"
# a deep directory tree: the 301 body carries the full ~800-byte target
DEEP="$ROOT"
for i in $(seq 0 31); do DEEP="$DEEP/d$i-xxxxxxxxxxxxxxxxxxxx"; done
mkdir -p "$DEEP"
printf 'deep\n' > "$DEEP/index.html"

"$BIN" -d "$ROOT" -p $PORT > "$ROOT/server.log" 2>&1 &
SPID=$!
# a second server with the filesystem root as docroot (containment at "/")
"$BIN" -d / -p $PORT2 > "$ROOT/server2.log" 2>&1 &
SPID2=$!
# a third with short timeouts, for the idle/header/write deadline tests
"$BIN" -d "$ROOT" -p $PORT3 -H 2000 -I 3000 -B 2000 -W 1500 > "$ROOT/server3.log" 2>&1 &
SPID3=$!
# a fourth with a connection cap of 4, for the 503-at-cap test
PORT4=18102
"$BIN" -d "$ROOT" -p $PORT4 -c 4 > "$ROOT/server4.log" 2>&1 &
SPID4=$!
# round 4 servers: absolute deadlines, request cap, age cap
PORT5=18105
"$BIN" -d "$ROOT" -p $PORT5 -H 1000 > "$ROOT/server5.log" 2>&1 &
SPID5=$!
PORT6=18106
"$BIN" -d "$ROOT" -p $PORT6 -R 3 > "$ROOT/server6.log" 2>&1 &
SPID6=$!
PORT7=18107
"$BIN" -d "$ROOT" -p $PORT7 -A 2000 -I 30000 > "$ROOT/server7.log" 2>&1 &
SPID7=$!
# round 5: a small memory budget, for the 503-on-pressure test
PORT8=18108
"$BIN" -d "$ROOT" -p $PORT8 -M 16 > "$ROOT/server8.log" 2>&1 &
SPID8=$!
# round 8: a small budget with a fast header sweep, for the 408 ledger test
PORT9=18110
"$BIN" -d "$ROOT" -p $PORT9 -M 16 -H 400 > "$ROOT/server9.log" 2>&1 &
SPID9=$!
sleep 0.4

python3 - "$PORT" "$PORT2" "$PORT3" "$ROOT" "$PORT4" "$BIN" "$PORT5" "$PORT6" "$PORT7" "$PORT8" "$PORT9" "$SPID" <<'PYEOF'
import socket, sys, time, re, os
import threading

port = int(sys.argv[1])
port2 = int(sys.argv[2])
port3 = int(sys.argv[3])
root = sys.argv[4]
port4 = int(sys.argv[5])
binpath = os.path.abspath(sys.argv[6])
port5 = int(sys.argv[7])
port6 = int(sys.argv[8])
port7 = int(sys.argv[9])
port8 = int(sys.argv[10])
port9 = int(sys.argv[11])
spid = int(sys.argv[12])
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

# --- round 9: two fast 2 MB downloads interleave under the per-turn write budget ---
pa = socket.create_connection(("127.0.0.1", port), timeout=10)
pb = socket.create_connection(("127.0.0.1", port), timeout=10)
pa.sendall(b"GET /two-mb.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
pb.sendall(b"GET /two-mb.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
bufs = {pa: b"", pb: b""}
open_conns = [pa, pb]
t_end = time.time() + 15
while open_conns and time.time() < t_end:
    for sc in list(open_conns):
        try:
            sc.settimeout(0.5)
            chunk = sc.recv(65536)
            if not chunk:
                open_conns.remove(sc)
            else:
                bufs[sc] += chunk
        except socket.timeout:
            pass
la = len(bufs[pa].partition(b"\r\n\r\n")[2])
lb = len(bufs[pb].partition(b"\r\n\r\n")[2])
ok(la == 2000000 and lb == 2000000,
   f"two concurrent 2 MB downloads both complete byte-exact ({la}, {lb})")
pa.close()
pb.close()

# conditional
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
etag = re.search(rb'ETag: ("[^"]+")', r).group(1)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: " + etag + b"\r\n\r\n")
ok(status(r) == 304 and b"ETag:" in r and b"Last-Modified:" in r,
   "If-None-Match short-circuits with the 200's validators (RFC 9110 15.4.5)")

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

# --- round 8: a pre-parser 431 to a HEAD carries no body (RFC 9110) ---
r = request(b"HEAD / HTTP/1.1\r\nHost: x\r\n" + b"X-Pad: " + b"y" * 33000 + b"\r\n\r\n")
ok(status(r) == 431, "HEAD past the header cap is a 431")
ok(len(r.partition(b"\r\n\r\n")[2]) == 0, "the pre-parser 431 to a HEAD sends no body")
r = request(b"GET / HTTP/1.1\r\nHost: x\r\n" + b"X-Pad: " + b"y" * 33000 + b"\r\n\r\n")
ok(status(r) == 431 and len(r.partition(b"\r\n\r\n")[2]) > 0,
   "the same 431 to a GET keeps its body")
r = request(b"GET /" + b"z" * 3000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 414, "oversize target is 414")

# --- framing: a 405 then a pipelined GET must stay perfectly aligned ---
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"PUT /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
head1, body1, rest = read_response(s)
ok(head1.startswith(b"HTTP/1.1 405") and b"Allow: GET, HEAD, POST, OPTIONS\r\n" in head1,
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
ok(b"Content-Length" not in r.split(b"\r\n\r\n")[0],
   "the 204 head carries no Content-Length (RFC 9110 8.6)")
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

# --- round 3: OPTIONS on a path carries the same Allow list ---
r = request(b"OPTIONS /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 204 and b"Allow: GET, HEAD, POST, OPTIONS" in r, "OPTIONS /a.txt is 204 with Allow")

# --- round 3: a lowercase method is 405, not 400 ---
r = request(b"get /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 405 and b"Allow:" in r, "lowercase get is 405")

# --- round 3: bare-LF heads are tolerated (RFC 9112 2.2) ---
r = request(b"GET /a.txt HTTP/1.1\nHost: x\n\n")
ok(status(r) == 200 and b"plain text" in r, "bare LF head is answered")
r = request(b"GET /a.txt HTTP/1.1\nHost: x\nConnection: close\n\n")
ok(status(r) == 200, "bare LF Connection token still honored")

# --- round 3: duplicate Content-Length - identical merges, conflicting 400 ---
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello")
ok(status(r) == 200 and r.endswith(b"hello"), "identical duplicate Content-Length merges")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello")
ok(status(r) == 400, "conflicting Content-Length is 400")

# --- round 3: HTTP/1.0 ignores Expect: 100-continue (no interim) ---
r = request(b"POST /echo HTTP/1.0\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello")
ok(status(r) == 200 and r.endswith(b"hello") and b"100 Continue" not in r,
   "HTTP/1.0 with Expect gets no interim 100")

# --- round 3: strict If-Range ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
lm = re.search(rb"Last-Modified: ([^\r]+)", r).group(1)
etag = re.search(rb'ETag: ("[^"]+")', r).group(1)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Range: " + lm + b"\r\nRange: bytes=0-2\r\n\r\n")
ok(status(r) == 206, "If-Range with the exact Last-Modified serves the range")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Range: Sun, 06 Nov 2094 08:49:37 GMT\r\nRange: bytes=0-2\r\n\r\n")
ok(status(r) == 200, "If-Range with a future date serves the full body")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Range: W/" + etag + b"\r\nRange: bytes=0-2\r\n\r\n")
ok(status(r) == 200, "If-Range with a weak etag serves the full body")

# --- round 3: overflowing range positions clamp per RFC 9110 14.2 ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=0-99999999999999999999999\r\n\r\n")
ok(status(r) == 206 and r.endswith(b"plain text\n"), "overflowing last-byte-pos clamps to 206")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=99999999999999999999999-\r\n\r\n")
ok(status(r) == 416, "overflowing first-byte-pos is 416")
r = request(b"GET /empty.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=-5\r\n\r\n")
ok(status(r) == 416, "a suffix range on an empty file is 416 (no byte to satisfy)")

# --- round 3: symlinks: inside the tree served, escaping refused ---
r = request(b"GET /link.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200 and b"plain text" in r, "in-docroot symlink is served")
r = request(b"GET /escape.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 404 and b"/etc/passwd" not in r, "escaping symlink is refused")

# --- round 3: If-Match / If-Unmodified-Since over the wire ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Match: \"stale\"\r\n\r\n")
ok(status(r) == 412, "If-Match with a stale etag is 412")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Match: *\r\n\r\n")
ok(status(r) == 200, "If-Match: * passes")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Match: " + etag + b"\r\n\r\n")
ok(status(r) == 200, "If-Match with the current etag passes")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Unmodified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n")
ok(status(r) == 412, "If-Unmodified-Since older than mtime is 412")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nIf-Match: *\r\n\r\nx")
ok(status(r) == 412, "If-Match on /echo fails (nothing stored)")

# --- round 3: the 414 boundary: 2047-byte target served, 2048 is 414 ---
r = request(b"GET /" + b"z" * 2046 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 404, "a 2047-byte target is still served (404 here)")
r = request(b"GET /" + b"z" * 2047 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 414, "a 2048-byte target is 414")

# --- round 3: a pipelined flood behind a pending response keeps it intact ---
s = socket.create_connection(("127.0.0.1", port), timeout=15)
s.sendall(b"GET /two-mb.bin HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET / HTTP/1.1\r\nHost: x\r\nX-Flood: " + b"y" * 1500000 + b"\r\n\r\n")
head1, body1, rest = read_response(s)
ok(head1.startswith(b"HTTP/1.1 200"), "flood behind a pending response: head arrives")
ok(len(body1) == 2000000, "flood behind a pending response: the 2MB body is not destroyed")
try:
    head2, body2, rest = read_response(s, rest)
    ok(head2.startswith(b"HTTP/1.1 431"), "the flood itself is answered with a clean 431")
except EOFError:
    ok(False, "the flood itself is answered with a clean 431")
s.close()

# --- round 3: 408 for a head or body that times out mid-request (port3: 2s) ---
s = socket.create_connection(("127.0.0.1", port3), timeout=6)
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nX-Part: 1\r\n")   # partial head
time.sleep(3)
r408 = s.recv(65536)
ok(r408.startswith(b"HTTP/1.1 408"), "a stalled partial head gets a 408")
s.close()
s = socket.create_connection(("127.0.0.1", port3), timeout=6)
s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc")   # partial body
time.sleep(3)
r408 = s.recv(65536)
ok(r408.startswith(b"HTTP/1.1 408"), "a stalled partial body gets a 408")
s.close()

# --- round 3: the connection cap answers 503 instead of an RST ---
held = [socket.create_connection(("127.0.0.1", port4), timeout=3) for _ in range(4)]
time.sleep(0.7)   # the accept loop must have taken all four
s503 = socket.create_connection(("127.0.0.1", port4), timeout=3)
s503.settimeout(4)
data503 = b""
try:
    while True:
        chunk = s503.recv(4096)
        if not chunk:
            break
        data503 += chunk
except (socket.timeout, ConnectionResetError):
    pass
s503.close()
ok(data503.startswith(b"HTTP/1.1 503") and b"Connection: close" in data503,
   "a full server answers 503, not an RST")
ok(b"\r\nDate: " in data503 and b"\r\nServer: mast\r\n" in data503,
   "the at-cap 503 carries Date and Server like every other response")
for h in held:
    h.close()
time.sleep(0.3)
held = [socket.create_connection(("127.0.0.1", port4), timeout=3) for _ in range(4)]
time.sleep(0.7)
s503b = socket.create_connection(("127.0.0.1", port4), timeout=3)
s503b.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")   # request in flight before the answer
s503b.settimeout(4)
full503 = b""
try:
    while True:
        chunk = s503b.recv(4096)
        if not chunk:
            break
        full503 += chunk
except (socket.timeout, ConnectionResetError):
    pass
s503b.close()
ok(full503.startswith(b"HTTP/1.1 503") and full503.endswith(b"try again later\n"),
   "the at-cap 503 arrives complete with the client's request unread")
for h in held:
    h.close()
time.sleep(0.3)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port4)
ok(status(r) == 200, "the capped server recovers once slots free up")

# --- round 3: pipelining fairness - a flooder must not starve a probe ---
stop_flood = threading.Event()
def flooder():
    batch = b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n" * 400
    while not stop_flood.is_set():
        try:
            fs = socket.create_connection(("127.0.0.1", port), timeout=5)
            fs.sendall(batch)
            data = b""
            while data.count(b"HTTP/1.1 200") < 400:
                chunk = fs.recv(65536)
                if not chunk:
                    break
                data += chunk
            fs.close()
        except OSError:
            break
flooder_threads = [threading.Thread(target=flooder) for _ in range(4)]
for t in flooder_threads: t.start()
time.sleep(0.5)   # let the flood saturate
lat = []
for _ in range(5):
    t0 = time.time()
    r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
    lat.append(time.time() - t0)
stop_flood.set()
for t in flooder_threads: t.join()
mean = sum(lat) / len(lat)
ok(all(l < 5 for l in lat) and mean < 1.5,
   f"pipelining fairness: probe mean {mean*1000:.0f} ms while 4 flooders run")

# --- round 3: graceful shutdown is fast with idle keep-alive conns (M1) ---
import subprocess
srv = subprocess.Popen([binpath, "-d", root, "-p", "18103"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.4)
idle = socket.create_connection(("127.0.0.1", 18103), timeout=3)
idle.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.3)
idle.recv(65536)   # response read; the connection now idles in keep-alive
t0 = time.time()
srv.terminate()
srv.wait(timeout=5)
dt = time.time() - t0
ok(dt < 2.0, f"SIGTERM with an idle keep-alive conn exits fast ({dt*1000:.0f} ms)")
idle.close()

# an in-flight response still finishes during shutdown
srv = subprocess.Popen([binpath, "-d", root, "-p", "18103"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.4)
s = socket.create_connection(("127.0.0.1", 18103), timeout=10)
s.sendall(b"GET /two-mb.bin HTTP/1.1\r\nHost: x\r\n\r\n")
head1, body1, rest = read_response(s)   # head + first body bytes, still streaming
srv.terminate()
got = len(body1) + len(rest)
while got < 2000000:
    chunk = s.recv(65536)
    if not chunk:
        break
    got += len(chunk)
ok(got == 2000000, "an in-flight response completes across a SIGTERM")
s.close()
srv.wait(timeout=5)

# --- round 3: EMFILE on accept re-arms the listener instead of stalling ---
def cap_fds():
    import resource
    resource.setrlimit(resource.RLIMIT_NOFILE, (48, 48))
srv = subprocess.Popen([binpath, "-d", root, "-p", "18104", "-c", "60"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       preexec_fn=cap_fds)
time.sleep(0.5)
flood = []
for _ in range(45):
    try:
        flood.append(socket.create_connection(("127.0.0.1", 18104), timeout=3))
    except OSError:
        break
time.sleep(0.5)
for f in flood:
    f.close()
time.sleep(0.7)
try:
    r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=18104)
    ok(status(r) == 200, "accept recovers after EMFILE once fds free up")
except OSError:
    ok(False, "accept recovers after EMFILE once fds free up")
srv.terminate()
srv.wait(timeout=5)

# --- round 4: Expect rejections must say Connection: close (RFC 9112 9.6) ---
r = request(b"POST /elsewhere HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\nx")
ok(status(r) == 404 and b"Connection: close\r\n" in r and b"keep-alive" not in r,
   "Expect rejected with 404 says Connection: close")
r = request(b"PUT /a.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\nx")
ok(status(r) == 405 and b"Connection: close\r\n" in r and b"keep-alive" not in r,
   "Expect rejected with 405 says Connection: close")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\nExpect: 100-continue\r\n\r\n" + b"x" * 100)
ok(status(r) == 413 and b"Connection: close\r\n" in r, "Expect rejected with 413 says Connection: close")

# --- round 4: a byte-drip trickler dies at the absolute header deadline ---
s = socket.create_connection(("127.0.0.1", port5), timeout=6)   # -H 1000
t0 = time.time()
s.sendall(b"G")
closed_at = None
resp = b""
for i in range(6):
    time.sleep(0.7)
    try:
        s.sendall(b"E")
    except OSError:
        closed_at = time.time() - t0
        break
    try:
        s.settimeout(0.01)
        chunk = s.recv(65536)
        if chunk:
            resp += chunk
            closed_at = time.time() - t0
            break
    except socket.timeout:
        pass
ok(closed_at is not None and closed_at < 3.0,
   f"trickler (1 byte per 0.7s) dies at the absolute -H deadline ({closed_at and round(closed_at, 1)}s)")
ok(resp.startswith(b"HTTP/1.1 408") or closed_at is not None, "the trickled conn gets a 408 or a close")
s.close()

# a slow but legitimate client under the deadline still works
s = socket.create_connection(("127.0.0.1", port5), timeout=4)
time.sleep(0.8)   # inside the 1s absolute header window
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
r = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    r += chunk
ok(status(r) == 200 and b"plain text" in r, "slow-but-legitimate client under the deadline is served")
s.close()

# --- round 8: the timeout 408 to a stalled HEAD carries no body ---
s = socket.create_connection(("127.0.0.1", port5), timeout=8)   # -H 1000
s.sendall(b"HEAD /a.txt HTTP/1.1\r\nHost: x\r\n")
h408 = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        h408 += chunk
except socket.timeout:
    pass
ok(h408.startswith(b"HTTP/1.1 408"), "a stalled HEAD earns a 408")
ok(len(h408.partition(b"\r\n\r\n")[2]) == 0, "the 408 to a HEAD sends no body")
s.close()
s = socket.create_connection(("127.0.0.1", port5), timeout=8)
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n")
g408 = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        g408 += chunk
except socket.timeout:
    pass
ok(g408.startswith(b"HTTP/1.1 408") and len(g408.partition(b"\r\n\r\n")[2]) > 0,
   "the 408 to a GET keeps its body")
s.close()

# --- round 4: the per-connection request cap says close on the last one ---
s = socket.create_connection(("127.0.0.1", port6), timeout=4)   # -R 3
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
h1, b1, rest = read_response(s)
h2, b2, rest = read_response(s, rest)
h3, b3, rest = read_response(s, rest)
ok(b"Connection: keep-alive" in h1 and b"Connection: keep-alive" in h2,
   "request cap: the first responses keep the connection")
ok(b"Connection: close" in h3, "request cap: the last response says Connection: close")
s.settimeout(2)
try:
    closed = s.recv(1) == b""
except (socket.timeout, ConnectionResetError):
    closed = True
ok(closed, "request cap: the server closes after the cap-th response")
s.close()

# --- round 4: the max connection age beats the idle timeout ---
s = socket.create_connection(("127.0.0.1", port7), timeout=5)   # -A 2000 -I 30000
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
time.sleep(0.3)
s.recv(65536)
time.sleep(2.4)   # past the 2s age cap, far inside the 30s idle timeout
s.settimeout(1.5)
try:
    closed = s.recv(1) == b""
except (socket.timeout, ConnectionResetError):
    closed = True
ok(closed, "a connection older than -A closes even mid keep-alive")
s.close()

# --- round 9: a lingering close honors the connection-age cap too ---
s = socket.create_connection(("127.0.0.1", port7), timeout=5)   # -A 2000
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
r = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    r += chunk
ok(status(r) == 200 and b"Connection: close" in r,
   "age-cap fixture: the close response lands before the linger")
t0 = time.time()
died = None
for i in range(25):   # the peer never FINs; the server drains junk until a cap
    time.sleep(0.4)
    try:
        s.sendall(b"x")
    except OSError:
        died = time.time() - t0
        break
ok(died is not None and died < 4.5,
   f"lingering close dies at the age cap, not the 5s drain deadline ({died and round(died, 1)}s)")
s.close()

# --- round 4: a flooding peer cannot pin the lingering-close drain ---
# port4 caps at 4 connections; a half-closed flooding drainer must free its
# slot by the drain deadline even while it stays continuously readable
fa = socket.create_connection(("127.0.0.1", port4), timeout=4)
fa.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
fa.settimeout(4)
resp = b""
while True:
    chunk = fa.recv(65536)
    if not chunk:
        break
    resp += chunk
ok(status(resp) == 200, "drainer got its full response before the flood")
flood_t0 = time.time()
flood_stop = threading.Event()
def drainer_flood():
    try:
        while not flood_stop.is_set():
            fa.sendall(b"z" * 65536)
    except OSError:
        pass
ft = threading.Thread(target=drainer_flood)
ft.start()
holders = []
for _ in range(3):
    h = socket.create_connection(("127.0.0.1", port4), timeout=4)
    holders.append(h)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", to=port4)
ok(status(r) == 503, "the cap counts the flooding drainer while it drains")
time.sleep(max(0, 6.0 - (time.time() - flood_t0)))
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", to=port4)
ok(status(r) == 200, "flooding drainer's slot freed by the drain deadline")
flood_stop.set()
ft.join()
fa.close()
for h in holders:
    h.close()

# --- round 5: a FIFO, a unix socket or a device in the docroot is a fast
# 404, never a blocked open ---
import stat as statmod
os.mkfifo(os.path.join(root, "pipe.fifo"))
usock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
usock.bind(os.path.join(root, "sock.sock"))
t0 = time.time()
r = request(b"GET /pipe.fifo HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 404 and time.time() - t0 < 2, "a FIFO in the docroot is a fast 404")
t0 = time.time()
r = request(b"GET /sock.sock HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 404 and time.time() - t0 < 2, "a unix socket in the docroot is a fast 404")
t0 = time.time()
r = request(b"GET /dev/null HTTP/1.1\r\nHost: x\r\n\r\n", to=port2)
ok(status(r) == 404 and time.time() - t0 < 2, "a device node is a fast 404")
t0 = time.time()
r = request(b"GET /dev/zero HTTP/1.1\r\nHost: x\r\n\r\n", to=port2)
ok(status(r) == 404 and time.time() - t0 < 2, "an infinite device is a fast 404")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200 and b"plain text" in r, "server responsive after special-file probes")
usock.close()

# --- round 5: a flood of blank lines dies fast, not in quadratic time ---
s = socket.create_connection(("127.0.0.1", port), timeout=5)
t0 = time.time()
s.sendall(b"\n" * 32000 + b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
r = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    r += chunk
elapsed = time.time() - t0
ok(status(r) == 400 and elapsed < 2.0,
   f"32000 leading blank lines rejected fast ({elapsed:.2f}s)")
s.close()
r = request(b"\r\n\r\nGET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200, "two leading blank lines still tolerated")

# --- round 12: the blank-line cap counts across dribbled feeds ---
s = socket.create_connection(("127.0.0.1", port), timeout=5)
for _ in range(8):
    s.sendall(b"\r\n")
    time.sleep(0.02)
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
head, body, rest = read_response(s)
ok(head.startswith(b"HTTP/1.1 200"), "eight dribbled blank lines stay within the cap")
for _ in range(8):
    s.sendall(b"\r\n")
    time.sleep(0.02)
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n")
head, body, rest = read_response(s)
ok(head.startswith(b"HTTP/1.1 200"), "the next request counts its own blank lines")
s.close()
s = socket.create_connection(("127.0.0.1", port), timeout=5)
for _ in range(9):
    s.sendall(b"\r\n")
    time.sleep(0.02)
r = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        r += chunk
except socket.timeout:
    pass
ok(r.startswith(b"HTTP/1.1 400"), "a ninth dribbled blank line is a 400")
s.close()

# --- round 5: absolute-form with a query and no path serves / ---
r = request(b"GET http://x?q=1 HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 200 and b"<h1>hi</h1>" in r, "absolute-form query without path serves /")

# --- round 5: duplicate Range headers combine to a multi-range -> 200 ---
r = request(b"GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\nRange: bytes=20-29\r\n\r\n")
ok(status(r) == 200 and len(r.split(b"\r\n\r\n", 1)[1]) == 262144, "duplicate Range serves the full body")

# --- round 5: Transfer-Encoding list edge cases on the wire ---
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked,\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
ok(status(r) == 200 and r.endswith(b"hello"), "Transfer-Encoding: chunked, (trailing comma) echoes")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
ok(status(r) == 400, "gzip+chunked stays rejected")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\n0\r\n\r\n")
ok(status(r) == 400, "TE split across lines combines: chunked + gzip is 400 (RFC 9110 5.3)")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: deflate\r\n\r\n0\r\n\r\n")
ok(status(r) == 501, "two non-chunked TE lines stay a 501")

# --- round 5: an impossible date is not a date; the precondition is ignored ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Tue, 31 Feb 2026 08:49:37 GMT\r\n\r\n")
ok(status(r) == 200, "If-Modified-Since with February 31st is ignored, not 304")

# --- round 5: a long chunk extension is legal ---
ext = b"x" * 400
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5;" + ext + b"\r\nhello\r\n0\r\n\r\n")
ok(status(r) == 200 and r.endswith(b"hello"), "a 400-byte chunk extension echoes")

# --- round 5: CONNECT is a 405 with Allow, like any unknown method ---
r = request(b"CONNECT x:443 HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 405 and b"Allow: GET, HEAD, POST, OPTIONS" in r, "CONNECT gets 405 + Allow")

# --- round 5: a failed HEAD still answers without a body ---
r = request(b"HEAD / HTTP/1.1\r\nHost: x\r\nBadHeader\r\n\r\n")
head = r.split(b"\r\n\r\n")[0]
ok(status(r) == 400 and r.split(b"\r\n\r\n", 1)[1] == b"",
   "a parse error on HEAD carries no body")

# --- round 9: HEAD + Expect + an oversize announced body gets a bodyless 413 ---
r = request(b"HEAD /a.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\nExpect: 100-continue\r\n\r\n")
ok(status(r) == 413, "HEAD with an oversize announced body is a 413")
ok(len(r.partition(b"\r\n\r\n")[2]) == 0, "the oversize 413 to a HEAD sends no body")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\nExpect: 100-continue\r\n\r\n")
ok(status(r) == 413 and len(r.partition(b"\r\n\r\n")[2]) > 0,
   "the same 413 to a POST keeps its body")

# --- round 5: HTTP/1.0 with both close and keep-alive closes ---
s = socket.create_connection(("127.0.0.1", port), timeout=4)
s.sendall(b"GET /a.txt HTTP/1.0\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n")
r = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    r += chunk
ok(status(r) == 200 and b"Connection: close" in r, "1.0 close+keep-alive: close wins on the wire")
s.close()

# --- round 5: an empty Host on HTTP/1.1 is a 400 ---
r = request(b"GET / HTTP/1.1\r\nHost:\r\n\r\n")
ok(status(r) == 400, "empty Host rejected on the wire")

# --- round 5: graceful shutdown is capped; a second signal forces exit ---
import subprocess, signal
with open(os.path.join(root, "grace.bin"), "wb") as gf:
    gf.write(os.urandom(50 * 1024 * 1024))
for label, second in (("grace cap", False), ("second signal", True)):
    p = None
    try:
        p = subprocess.Popen([binpath, "-d", root, "-p", "18109"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.4)
        sc = socket.create_connection(("127.0.0.1", 18109), timeout=4)
        sc.sendall(b"GET /grace.bin HTTP/1.1\r\nHost: x\r\n\r\n")
        time.sleep(0.3)   # the write stalls: this client never reads
        t0 = time.time()
        p.send_signal(signal.SIGTERM)
        if second:
            time.sleep(0.5)
            p.send_signal(signal.SIGTERM)
        p.wait(timeout=12)
        dt = time.time() - t0
        sc.close()
        if second:
            ok(dt < 3.0, f"a second SIGTERM forces exit ({dt:.1f}s)")
        else:
            ok(4.0 < dt < 8.0, f"shutdown grace caps a stalled writer ({dt:.1f}s, not the 30s write timeout)")
    finally:
        if p and p.poll() is None:
            p.kill()
        if p:
            p.wait()

# --- round 5: the memory budget refuses the connection, not the process ---
# -M 16: each incomplete 900 KB body holds its full buffer from the first
# byte; at ~18 of them the 16 MB budget must trip with a clean 503 while
# the earlier connections wait unharmed
held = []
codes = []
for k in range(22):
    sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
    sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 900000\r\n\r\n" + b"z" * 100)
    sc.settimeout(0.6)
    try:
        h = sc.recv(65536)   # only a refused connection answers now
        codes.append(status(h) if h else 0)
    except socket.timeout:
        codes.append("held")
    held.append(sc)
n503 = sum(1 for x in codes if x == 503)
nheld = sum(1 for x in codes if x == "held")
ok(n503 >= 1, f"memory budget: pressure earns a clean 503, not a crash ({n503} refused, {nheld} held)")
ok(nheld >= 10, f"memory budget: connections within budget wait unharmed ({nheld} held)")
# free four held connections first, so the echo fits the budget again
freed = 0
for k, code in enumerate(codes):
    if code == "held" and freed < 4:
        held[k].close()
        freed += 1
time.sleep(0.6)
fin = [held[k] for k, x in enumerate(codes) if x == "held"][freed]
fin.sendall(b"z" * 899900)
fin.settimeout(8)
r = b""
while b"\r\n\r\n" not in r:
    r += fin.recv(65536)
ok(status(r) == 200, "a held body completes and echoes once sent")
for sc in held:
    sc.close()
time.sleep(0.5)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port8)
ok(status(r) == 200, "server healthy after memory pressure")

# --- round 6: a chunked batch projects its buffer doubling into the -M budget ---
# 16 held 950 KB bodies plus two 500 KB chunked fills leave ~450 KB of
# headroom; a 100 KB follow-up batch on the first chunked connection then
# needs a 512 KB buffer doubling it used to sneak past the check (the batch
# is smaller than the current cap, so the old projection computed growth 0)
held2 = []
for k in range(16):
    sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
    sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 950000\r\n\r\n" + b"z" * 100)
    held2.append(sc)
time.sleep(0.4)
cka = socket.create_connection(("127.0.0.1", port8), timeout=8)
cka.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n")
cka.sendall(b"7a120\r\n" + b"c" * 500000 + b"\r\n")
ckb = socket.create_connection(("127.0.0.1", port8), timeout=8)
ckb.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n")
ckb.sendall(b"7a120\r\n" + b"c" * 500000 + b"\r\n")
time.sleep(0.3)
cka.sendall(b"186a0\r\n" + b"c" * 100000)
cka.settimeout(4)
got = b""
try:
    while b"\r\n\r\n" not in got:
        part = cka.recv(65536)
        if not part:
            break
        got += part
except socket.timeout:
    pass
ok(status(got) == 503, "a chunked batch projects its buffer doubling into the budget (503)")
cka.close()
ckb.close()
for sc in held2:
    sc.close()
time.sleep(0.5)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port8)
ok(status(r) == 200, "server healthy after chunked budget pressure")

# --- round 6: keep-alive budget accounting hands back exactly what it counted ---
ka = socket.create_connection(("127.0.0.1", port8), timeout=10)
ka.settimeout(10)
seq_ok = True
for k in range(4):
    ka.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 900000\r\n\r\n" + b"q" * 900000)
    got = b""
    while True:
        part = ka.recv(65536)
        if not part:
            break
        got += part
        if b"\r\n\r\n" in got:
            h2, _, bdy = got.partition(b"\r\n\r\n")
            m2 = re.search(rb"Content-Length: (\d+)", h2)
            if m2 and len(bdy) >= int(m2.group(1)):
                break
    if not got.startswith(b"HTTP/1.1 200"):
        seq_ok = False
        break
ok(seq_ok, "four sequential 900 KB echoes on one keep-alive connection")
ka.close()
time.sleep(0.3)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port8)
ok(status(r) == 200, "the budget ledger stays exact across keep-alive reuse")

# --- round 8: a backlog 503 to a buffered HEAD prefix carries no body ---
# sized bodies fill the 16 MB budget to a ~6 KB headroom (each held
# connection charges its 4 KB parser buffer plus content_length + 1):
# the first 4 KB of a fresh head still fits, its next doubling does not
held3 = []
for k in range(15):
    sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
    sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048575\r\n\r\n" + b"z" * 100)
    held3.append(sc)
sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 977024\r\n\r\n" + b"z" * 100)
held3.append(sc)
time.sleep(0.4)
hc = socket.create_connection(("127.0.0.1", port8), timeout=8)
hc.sendall(b"HEAD /a.txt HTTP/1.1\r\nHost: x\r\n")
time.sleep(0.3)   # the first batch must land (and stay buffered) on its own
hc.sendall(b"X-Pad: " + b"a" * 5000)
h503 = b""
hc.settimeout(8)
try:
    while True:
        chunk = hc.recv(65536)
        if not chunk:
            break
        h503 += chunk
except socket.timeout:
    pass
ok(h503.startswith(b"HTTP/1.1 503"), "budget pressure earns the buffered head a 503")
ok(len(h503.partition(b"\r\n\r\n")[2]) == 0, "the backlog 503 to a HEAD sends no body")
hc.close()
time.sleep(0.5)   # the closed connection hands its buffers back first
gc = socket.create_connection(("127.0.0.1", port8), timeout=8)
gc.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n")
time.sleep(0.3)
gc.sendall(b"X-Pad: " + b"a" * 5000)
g503 = b""
gc.settimeout(8)
try:
    while True:
        chunk = gc.recv(65536)
        if not chunk:
            break
        g503 += chunk
except socket.timeout:
    pass
ok(g503.startswith(b"HTTP/1.1 503") and len(g503.partition(b"\r\n\r\n")[2]) > 0,
   "the same backlog 503 to a GET keeps its body")
gc.close()
for sc in held3:
    sc.close()
time.sleep(0.5)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port8)
ok(status(r) == 200, "server healthy after the backlog-503 pair")

# --- round 12: budget pressure sizes the read; a pending flush never drops bytes ---
# the same ~6 KB headroom fill as the backlog-503 pair; the pipelined
# second head's parser doubling does not fit, so the server must refuse
# before recv - taking the batch and dropping it would splice the head
held6 = []
for k in range(15):
    sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
    sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048575\r\n\r\n" + b"z" * 100)
    held6.append(sc)
sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 977024\r\n\r\n" + b"z" * 100)
held6.append(sc)
time.sleep(0.4)
sx = socket.create_connection(("127.0.0.1", port8), timeout=15)
sx.sendall(b"GET /two-mb.bin HTTP/1.1\r\nHost: x\r\n\r\n"
           b"GET /a.txt HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"p" * 5000 + b"\r\n\r\n")
head1, body1, rest = read_response(sx)
ok(head1.startswith(b"HTTP/1.1 200") and len(body1) == 2000000,
   "the pending 2 MB response completes under budget pressure")
sx.settimeout(8)
r2 = rest
try:
    while True:
        chunk = sx.recv(65536)
        if not chunk:
            break
        r2 += chunk
except socket.timeout:
    pass
ok(r2.startswith(b"HTTP/1.1 503") and b"out of buffer room" in r2,
   "the refused pipelined head gets a clean 503, not a splice")
sx.close()
for sc in held6:
    sc.close()
time.sleep(0.5)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port8)
ok(status(r) == 200, "server healthy after the sized-read refusal")

# --- round 9: the /echo budget pre-check projects the response doubling ---
# 14 max-size bodies plus one tuner leave a headroom that fits a 400 KB
# echo (projected growth 512 KB) but not a 600 KB one (projected 1 MB),
# though either bare body length would fit
held5 = []
for k in range(14):
    sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
    sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048575\r\n\r\n" + b"z" * 100)
    held5.append(sc)
sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 595903\r\n\r\n" + b"z" * 100)
held5.append(sc)
time.sleep(0.4)
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 400000\r\nConnection: close\r\n\r\n" + b"e" * 400000, to=port8)
ok(status(r) == 200 and r.partition(b"\r\n\r\n")[2] == b"e" * 400000,
   "an echo that fits its projected growth succeeds")
time.sleep(0.5)
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 600000\r\nConnection: close\r\n\r\n" + b"e" * 600000, to=port8)
ok(status(r) == 503 and b"Retry-After: 1" in r,
   "an echo whose response doubling would pass the budget is a 503")
for sc in held5:
    sc.close()
time.sleep(0.5)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port8)
ok(status(r) == 200, "server healthy after the echo projection probes")

# --- round 8: the 408 path charges its response to the -M ledger ---
# 24 stalled heads each grow a 408 response; once they close, the ledger
# must be exactly whole again: a fill sized to the byte still fits, and
# one connection more is refused
stall = []
for k in range(24):
    sc = socket.create_connection(("127.0.0.1", port9), timeout=8)
    sc.sendall(b"HEAD /a.txt HTTP/1.1\r\nHost: x\r\n")
    stall.append(sc)
n408 = 0
for sc in stall:
    sc.settimeout(8)
    d = b""
    try:
        while True:
            chunk = sc.recv(65536)
            if not chunk:
                break
            d += chunk
    except socket.timeout:
        pass
    if d.startswith(b"HTTP/1.1 408") and len(d.partition(b"\r\n\r\n")[2]) == 0:
        n408 += 1
    sc.close()
ok(n408 == 24, f"24 stalled HEADs each get a bodyless 408 ({n408}/24)")
time.sleep(0.5)
held4 = []
for k in range(15):
    sc = socket.create_connection(("127.0.0.1", port9), timeout=8)
    sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048575\r\n\r\n" + b"z" * 100)
    held4.append(sc)
sc = socket.create_connection(("127.0.0.1", port9), timeout=8)
sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 977024\r\n\r\n" + b"z" * 100)
held4.append(sc)
time.sleep(0.4)
x = socket.create_connection(("127.0.0.1", port9), timeout=8)
x.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 900000\r\n\r\n" + b"z" * 100)
x.settimeout(8)
xr = b""
try:
    while b"\r\n\r\n" not in xr:
        part = x.recv(65536)
        if not part:
            break
        xr += part
except socket.timeout:
    pass
ok(status(xr) == 503, "after the 408 storm the exact-fit fill holds: one more body is 503")
x.close()
for sc in held4:
    sc.close()
time.sleep(0.5)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port9)
ok(status(r) == 200, "server healthy after the 408 ledger storm")

# --- round 6: a long directory redirect carries an honest Content-Length ---
deep = "/".join("d%d-xxxxxxxxxxxxxxxxxxxx" % i for i in range(32))
r = request(("GET /" + deep + " HTTP/1.1\r\nHost: x\r\n\r\n").encode())
head, _, body = r.partition(b"\r\n\r\n")
m = re.search(rb"Content-Length: (\d+)", head)
ok(head.startswith(b"HTTP/1.1 301"), "an 800-byte directory target redirects 301")
ok(m and int(m.group(1)) == len(body) and len(body) > 800,
   "long 301: Content-Length exactly matches the delivered body")
ok(("Location: /" + deep + "/").encode() in body, "long 301: the body carries the full target")

# --- round 8: the directory 301 keeps the query string ---
r = request(b"GET /sub?x=1&y=2 HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 301 and b"Location: /sub/?x=1&y=2\r\n" in r, "the 301 keeps the query string")
r = request(b"GET /sub HTTP/1.1\r\nHost: x\r\n\r\n")
ok(status(r) == 301 and b"Location: /sub/\r\n" in r, "the 301 without a query stays bare")

# --- round 6: a byte-dribbled head must not pin the loop ---
drib = socket.create_connection(("127.0.0.1", port), timeout=3)
drib.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nX-Pad: ")
drib_ok = True
for k in range(2000):
    drib.sendall(b"y")
    if k % 200 == 199:
        if not request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n").startswith(b"HTTP/1.1 200"):
            drib_ok = False
            break
ok(drib_ok, "a dribbled no-LF head never blocks other clients")
drib.sendall(b"\r\n\r\n")
r = b""
drib.settimeout(3)
try:
    while b"\r\n\r\n" not in r:
        part = drib.recv(4096)
        if not part:
            break
        r += part
except socket.timeout:
    pass
ok(r.startswith(b"HTTP/1.1 200"), "the dribbled head still completes into a 200")
drib.close()

# --- round 6: invalid suffix ranges are ignored, not 416 (RFC 9110 14.2) ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=-\r\n\r\n")
ok(status(r) == 200 and r.endswith(b"plain text\n"), "Range: bytes=- is ignored (200), not 416")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=-0\r\n\r\n")
ok(status(r) == 200 and r.endswith(b"plain text\n"), "Range: bytes=-0 is ignored (200), not 416")

# --- round 6: dates before the epoch or with a junk weekday are ignored ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sun, 06 Nov 1955 08:49:37 GMT\r\n\r\n")
ok(status(r) == 200, "a pre-1970 If-Modified-Since is ignored, not 304")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: ZZZ, 06 Nov 2026 08:49:37 GMT\r\n\r\n")
ok(status(r) == 200, "a fake-weekday If-Modified-Since is ignored, not 304")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sun, 06 Nov 2094 08:49:37 GMT\r\n\r\n")
ok(status(r) == 304, "a valid future If-Modified-Since still short-circuits")

# --- round 6: control bytes in chunk extensions and trailers are refused ---
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5;a=\x01b\r\nhello\r\n0\r\n\r\n")
ok(status(r) == 400, "a control byte in a chunk extension is 400")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-Bad: \x02\r\n\r\n")
ok(status(r) == 400, "a control byte in a trailer is 400")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5;a=ok-1\r\nhello\r\n0\r\nX-Trace: fine\r\n\r\n")
ok(status(r) == 200 and r.endswith(b"hello"), "a clean chunk extension and trailer still pass")

# --- round 6: conditional repeats: list headers combine, singletons are 400 ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: " + etag + b"\r\nIf-None-Match: \"other\"\r\n\r\n")
ok(status(r) == 304, "a repeated If-None-Match combines into the list")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Match: " + etag + b"\r\nIf-Match: \"other\"\r\n\r\n")
ok(status(r) == 200, "a repeated If-Match combines into the list")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\nIf-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n")
ok(status(r) == 400, "a repeated If-Modified-Since is 400")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Unmodified-Since: Sun, 06 Nov 2094 08:49:37 GMT\r\nIf-Unmodified-Since: Sun, 06 Nov 2094 08:49:37 GMT\r\n\r\n")
ok(status(r) == 400, "a repeated If-Unmodified-Since is 400")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Range: " + etag + b"\r\nIf-Range: " + etag + b"\r\nRange: bytes=0-2\r\n\r\n")
ok(status(r) == 400, "a repeated If-Range is 400")

# --- round 7: a >8 KB dribbled header line stays O(1) per byte on the socket path ---
def srv_cpu():
    try:
        data = open("/proc/%d/stat" % spid).read()
        rest = data[data.rindex(")") + 2:].split()
        return (int(rest[11]) + int(rest[12])) / os.sysconf("SC_CLK_TCK")
    except Exception:
        return None

cpu0 = srv_cpu()
bigdrib = socket.create_connection(("127.0.0.1", port), timeout=5)
bigdrib.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nX-Pad: ")
live = True
for k in range(28 * 1024):
    bigdrib.sendall(b"y")
    if k % 7168 == 7167:
        if not request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n").startswith(b"HTTP/1.1 200"):
            live = False
            break
bigdrib.sendall(b"\r\n\r\n")
r = b""
bigdrib.settimeout(5)
try:
    while b"\r\n\r\n" not in r:
        part = bigdrib.recv(4096)
        if not part:
            break
        r += part
except socket.timeout:
    pass
ok(live, "other clients are served during a 28 KB no-LF dribble")
ok(r.startswith(b"HTTP/1.1 200"), "the 28 KB dribbled header line completes into a 200")
cpu1 = srv_cpu()
if cpu0 is not None and cpu1 is not None:
    ok(cpu1 - cpu0 < 0.150, "server CPU for a 28 KB no-LF dribble is bounded (%.3fs)" % (cpu1 - cpu0))
bigdrib.close()

# --- round 7: any repeated Range ignores the header, however many lines ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=0-3\r\n\r\n")
ok(status(r) == 206, "one Range is honored")
for n in (2, 3, 4, 5):
    hdrs = b"".join(b"Range: bytes=%d-%d\r\n" % (i, i + 1) for i in range(n))
    r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n" + hdrs + b"\r\n")
    ok(status(r) == 200 and r.endswith(b"plain text\n"),
       "%d Range headers are ignored, full 200" % n)

# --- round 7: an embedded NUL cannot hide control bytes in chunk framing ---
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\x00\x01\x02\r\nhello\r\n0\r\n\r\n")
ok(status(r) == 400, "a NUL in a chunk size line is 400")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-T: a\x00b\r\n\r\n")
ok(status(r) == 400, "a NUL in a trailer is 400")

# --- round 7: signed or misshapen date fields are ignored, never honored ---
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sun, 01 Jan 2099 -5:00:00 GMT\r\n\r\n")
ok(status(r) == 200, "a signed hour is not a date (ignored, not 304)")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sun, 01 Jan 2099 +5:00:00 GMT\r\n\r\n")
ok(status(r) == 200, "a +-signed hour is not a date either")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sun, 1 Jan 2099 08:49:37 GMT\r\n\r\n")
ok(status(r) == 200, "an unpadded day is not a date")
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sun, 01 Jan 2099 08:49:37 GMT\r\n\r\n")
ok(status(r) == 304, "the well-formed future date still short-circuits")

# --- round 7: a repeated Expect must agree with itself ---
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nExpect: 100-continue\r\nExpect: 100-continue\r\n\r\nhi")
ok(b"HTTP/1.1 100 Continue" in r and r.endswith(b"hi"), "identical repeated Expect merges")
r = request(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nExpect: 100-continue\r\nExpect: max\r\n\r\nhi")
ok(status(r) == 400, "a differing repeated Expect is 400")

# --- round 7: error responses are charged to the budget and handed back ---
ledger_ok = True
for k in range(20):
    sc = socket.create_connection(("127.0.0.1", port8), timeout=8)
    sc.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 900000\r\n\r\n" + b"q" * 900000)
    sc.settimeout(8)
    got = b""
    try:
        while b"\r\n\r\n" not in got:
            got += sc.recv(65536)
    except socket.timeout:
        pass
    if not got.startswith(b"HTTP/1.1 200"):
        ledger_ok = False
        sc.close()
        break
    sc.sendall(b"GET /a.txt HTTP/1.1\r\n\r\n")   # no Host: a 400, then close
    try:
        while sc.recv(65536):
            pass
    except socket.timeout:
        pass
    sc.close()
time.sleep(0.4)
r = request(b"GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", to=port8)
ok(ledger_ok and status(r) == 200, "20 echo-then-400 cycles leave the budget ledger exact")

# -M validation
p = subprocess.run([binpath, "-d", root, "-M", "5"], capture_output=True)
ok(p.returncode == 2, "-M below the range is a usage error")
p = subprocess.run([binpath, "-h"], capture_output=True)
ok(p.returncode == 0 and b"-M megabytes" in p.stdout and b"-H header-ms" in p.stdout
   and b"-R requests" in p.stdout,
   "the usage text names every flag")

# concurrency: 50 simultaneous clients

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
for v in SPID SPID2 SPID3 SPID4 SPID5 SPID6 SPID7 SPID8 SPID9; do
  eval "p=\$$v"
  kill -TERM "$p" 2>/dev/null
  n=0
  while kill -0 "$p" 2>/dev/null && [ $n -lt 10 ]; do sleep 0.5; n=$((n+1)); done
  if kill -0 "$p" 2>/dev/null; then
    echo "run-tests: server $v did not stop on SIGTERM" >&2
    cp "$ROOT/server.log" /tmp/mast-srv1.log 2>/dev/null
    cp "$ROOT/server2.log" /tmp/mast-srv2.log 2>/dev/null
    cp "$ROOT/server3.log" /tmp/mast-srv3.log 2>/dev/null
    cp "$ROOT/server4.log" /tmp/mast-srv4.log 2>/dev/null
    ok_stop=0
  fi
  eval "$v="
done
[ $ok_stop -eq 1 ] && echo "shutdown: graceful" || RC=1
exit $RC
