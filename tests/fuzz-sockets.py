#!/usr/bin/env python3
"""Stateful socket fuzzer: drives the live server with random sessions -
mutated seed requests in random chunk splits, pipelined batches, partial
bodies, half-closes and rude RSTs, oversized header blocks, outright
floods and concurrent bursts. Individual replies are not checked
(any 4xx or close is a valid answer to garbage); what must hold is that
the server never crashes, never hangs, and still answers cleanly after
the storm. BIN overrides the binary (default ./build/mast)."""
import argparse, os, random, socket, struct, subprocess, sys, tempfile, threading, time

SEEDS = [
    b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
    b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello",
    b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    b"GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=0-99\r\n\r\n",
    b"PUT /a.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc",
    b"GET / HTTP/1.0\r\n\r\n",
]

def mutate(rng, seed):
    data = bytearray(seed)
    for _ in range(rng.randint(0, 6)):
        op = rng.randint(0, 3)
        pos = rng.randrange(max(1, len(data)))
        if op == 0 and data:
            data[pos] = rng.randrange(256)
        elif op == 1:
            data.insert(pos, rng.randrange(256))
        elif op == 2 and len(data) > 1:
            del data[pos]
        else:
            data[pos:pos] = bytes([rng.randrange(256)]) * rng.randint(1, 3)
    return bytes(data)

def one_session(rng, port, i):
    """One fuzz session: a plain mutated stream, an oversized header
    block, an all-at-once flood, or a pipelined storm."""
    kind = rng.random()
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
    except OSError:
        return
    s.settimeout(0.6)
    try:
        if kind < 0.12:
            # oversized head: 40 KB of headers, over the 32 KB cap
            head = b"GET /a.txt HTTP/1.1\r\nHost: x\r\n" + \
                b"".join(b"X-Pad-%d: %s\r\n" % (k, b"y" * 700)
                         for k in range(rng.randint(40, 60))) + b"\r\n"
            s.sendall(head)
        elif kind < 0.27:
            # flood: a large pipelined batch fired all at once
            payload = mutate(rng, rng.choice(SEEDS)) * rng.randint(40, 120)
            s.sendall(payload)
        else:
            for _ in range(rng.randint(1, 4)):
                payload = mutate(rng, rng.choice(SEEDS))
                if rng.random() < 0.3:
                    payload = payload * rng.randint(2, 6)   # pipelined batch
                # random fragmentation
                while payload:
                    n = rng.randint(1, len(payload))
                    s.sendall(payload[:n])
                    payload = payload[n:]
                    if rng.random() < 0.2:
                        time.sleep(rng.random() * 0.02)
                if rng.random() < 0.4:
                    # read a little, or everything briefly
                    try:
                        s.recv(rng.choice([64, 4096, 65536]))
                    except (socket.timeout, ConnectionResetError):
                        pass
                if rng.random() < 0.3:
                    s.shutdown(socket.SHUT_WR)
                    break
        try:
            while s.recv(65536):
                pass
        except (socket.timeout, ConnectionResetError):
            pass
    except (OSError, ConnectionResetError, BrokenPipeError):
        pass
    if rng.random() < 0.2:
        # rude close: RST instead of a clean FIN
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     struct.pack("ii", 1, 0))
    s.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sessions", type=int, default=60)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--port", type=int, default=18200)
    args = ap.parse_args()
    binpath = os.path.abspath(os.environ.get("BIN", "./build/mast"))
    if not os.path.exists(binpath):
        print("fuzz-sockets: build the server first (make)", file=sys.stderr)
        sys.exit(1)
    rng = random.Random(args.seed if args.seed is not None
                        else int.from_bytes(os.urandom(4), "little"))

    root = tempfile.mkdtemp(prefix="mast-fuzz-")
    with open(os.path.join(root, "a.txt"), "w") as f:
        f.write("plain text\n")
    with open(os.path.join(root, "index.html"), "w") as f:
        f.write("<h1>hi</h1>\n")
    with open(os.path.join(root, "big.bin"), "wb") as f:
        f.write(os.urandom(262144))

    srv = subprocess.Popen([binpath, "-d", root, "-p", str(args.port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    try:
        for i in range(args.sessions):
            if srv.poll() is not None:
                print(f"fuzz-sockets: server died after session {i}", file=sys.stderr)
                sys.exit(1)
            if i % 12 == 11:
                # a concurrent burst: several sessions at once
                threads = [threading.Thread(target=one_session,
                                            args=(rng, args.port, i))
                           for _ in range(rng.randint(4, 10))]
                for th in threads:
                    th.start()
                for th in threads:
                    th.join()
                continue
            one_session(rng, args.port, i)
        # after the storm: a clean request must get a clean answer
        s = socket.create_connection(("127.0.0.1", args.port), timeout=5)
        s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        out = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        s.close()
        if not (out.startswith(b"HTTP/1.1 200") and out.endswith(b"plain text\n")):
            print("fuzz-sockets: health check after the storm failed", file=sys.stderr)
            sys.exit(1)
        if srv.poll() is not None:
            print("fuzz-sockets: server died during the health check", file=sys.stderr)
            sys.exit(1)
    finally:
        srv.terminate()
        srv.wait(timeout=5)
    print(f"fuzz-sockets: {args.sessions} sessions, server alive and healthy"
          + (f" (seed {args.seed})" if args.seed is not None else ""))

if __name__ == "__main__":
    main()
