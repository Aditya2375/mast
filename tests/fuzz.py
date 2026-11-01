#!/usr/bin/env python3
"""Fuzz the HTTP parser/body/respond path: random byte blobs plus mutated
valid requests, fed through tests/fuzz-driver. The point is never crashing,
never hanging - any input gets a clean parse, a clean 4xx, or a clean
answer. --programs N mutated requests, --blobs N random blobs."""
import argparse, random, subprocess, sys, os

ROOT = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.join(ROOT, "..", "build", "test-fuzz")

SEEDS = [
    b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello",
    b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    b"GET /a%20b.txt?x=1 HTTP/1.1\r\nHost: h\r\nIf-None-Match: \"e\"\r\nRange: bytes=1-4\r\n\r\n",
    b"HEAD / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
]

def mutate(rng, seed):
    data = bytearray(seed)
    for _ in range(rng.randint(1, 8)):
        op = rng.randint(0, 3)
        pos = rng.randrange(max(1, len(data)))
        if op == 0 and data:
            data[pos] = rng.randrange(256)
        elif op == 1:
            data.insert(pos, rng.randrange(256))
        elif op == 2 and len(data) > 1:
            del data[pos]
        else:
            data[pos:pos] = bytes([rng.randrange(256)]) * rng.randint(1, 4)
    return bytes(data)

def run_one(payload):
    p = subprocess.run([DRIVER], input=payload, capture_output=True, timeout=10)
    err = p.stderr.decode("utf-8", "replace")
    if p.returncode != 0 or "runtime error:" in err or "ERROR: AddressSanitizer" in err:
        print("fuzz: driver failed", p.returncode)
        print(err[:2000])
        return False
    return True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--programs", type=int, default=100)
    ap.add_argument("--blobs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()
    if not os.path.exists(DRIVER):
        print("fuzz: build/test-fuzz missing - run make first", file=sys.stderr)
        sys.exit(1)
    rng = random.Random(args.seed if args.seed is not None else int.from_bytes(os.urandom(4), "little"))
    for i in range(args.programs):
        if not run_one(mutate(rng, rng.choice(SEEDS))):
            print(f"fuzz: FAILED on mutated program {i}")
            sys.exit(1)
    for i in range(args.blobs):
        blob = bytes(rng.randrange(256) for _ in range(rng.randint(0, 2048)))
        if not run_one(blob):
            print(f"fuzz: FAILED on blob {i}")
            sys.exit(1)
    print(f"fuzz: {args.programs} mutated requests + {args.blobs} byte blobs passed"
          + (f" (seed {args.seed})" if args.seed is not None else ""))

if __name__ == "__main__":
    main()
