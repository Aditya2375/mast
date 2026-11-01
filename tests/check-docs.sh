#!/bin/sh
# Keeps README claims honest: the unit-check count, line count, demo
# cache-busters and documented limits must match the code.
set -u
cd "$(dirname "$0")/.."
fail=0
if [ ! -x ./build/test-unit ]; then echo "check-docs: build first (make)" >&2; exit 1; fi
actual=$(./build/test-unit | grep -o '[0-9]* checks passed' | head -1 | cut -d' ' -f1)
if ! grep -q "$actual checks" README.md; then
    echo "check-docs: README unit-check count stale (suite: $actual checks passed)"; fail=1
fi
lines=$(cat src/*.c src/*.h | wc -l)
if ! grep -q "$lines lines of C" README.md; then
    echo "check-docs: README line count ($lines) missing or stale"; fail=1
fi
# The demo's cache-busters must match each other and the wasm content hash.
vjs=$(sed -n "s/.*mast\.js?v=\([a-z0-9]\{12\}\).*/\1/p" docs/index.html)
vwasm=$(sed -n "s/.*'?v=\([a-z0-9]\{12\}\)'.*/\1/p" docs/index.html)
vreal=$(sha256sum docs/mast.wasm | cut -c1-12)
if [ -z "$vjs" ] || [ "$vjs" != "$vwasm" ] || [ "$vjs" != "$vreal" ]; then
    echo "check-docs: demo cache-busters stale (js=$vjs wasm=$vwasm actual=$vreal): run sh wasm/build.sh"; fail=1
fi
# Pin documented limits to the code that enforces them: file|code pattern|README phrase
pins="
src/http.h|MAST_MAX_REQUEST_LINE 8192|8 KB request line
src/http.h|MAST_MAX_HEADERS_BYTES 32768|32 KB of headers
src/http.h|MAST_MAX_HEADER_COUNT 64|64 headers
src/http.h|MAST_MAX_TARGET 2048|2048-byte
src/http.h|MAST_MAX_BODY (1024 \* 1024)|1 MB
src/server.c|header_timeout_ms|header timeout
src/server.c|sendfile|sendfile
src/files.c|O_NOFOLLOW|symlink
"
printf '%s' "$pins" | while IFS='|' read -r file src_pat doc_pat; do
    [ -n "$file" ] || continue
    if ! grep -q "$src_pat" "$file"; then
        echo "check-docs: enforcing code missing: $src_pat in $file"; exit 1
    fi
    if ! grep -q "$doc_pat" README.md; then
        echo "check-docs: README does not document: $doc_pat"; exit 1
    fi
done || fail=1
[ "$fail" -eq 0 ] && echo "check-docs: ok"
exit $fail
