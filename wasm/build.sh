#!/bin/sh
# Rebuild the browser demo in docs/ (requires emsdk on PATH).
set -e
cd "$(dirname "$0")/.."
emcc -O2 -Isrc wasm/wrapper.c src/http.c src/handle.c src/files.c \
    -o docs/mast.js \
    -sEXPORTED_FUNCTIONS=_mast_boot,_mast_conn_new,_mast_conn_free,_mast_recv,_mast_out_ptr,_mast_out_len,_mast_out_drain,_mast_conn_closed,_mast_conn_requests,_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=ccall,writeArrayToMemory,HEAPU8 \
    -sMODULARIZE=1 -sEXPORT_NAME=MastModule \
    -sSTACK_SIZE=4194304 -sALLOW_MEMORY_GROWTH=1

# cache-bust both fetches with the wasm content hash, in lockstep
ver=$(sha256sum docs/mast.wasm | cut -c1-12)
sed -i "s/mast\.js?v=[a-z0-9]\{12\}/mast.js?v=$ver/" docs/index.html
sed -i "s/locateFile: (path) => path + '?v=[a-z0-9]\{12\}'/locateFile: (path) => path + '?v=$ver'/" docs/index.html
