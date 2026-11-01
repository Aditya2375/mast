CC      = cc
CFLAGS  = -std=gnu11 -O2 -g -Wall -Wextra -Werror -Isrc
EXTRA   =
SRCS    := src/main.c src/server.c src/http.c src/handle.c src/files.c
CORE    := src/server.c src/http.c src/handle.c src/files.c

all: build/mast build/test-unit

build:
	mkdir -p build

build/mast: $(SRCS) | build
	$(CC) $(CFLAGS) $(EXTRA) $(SRCS) -o $@

build/test-unit: tests/unit/test-unit.c src/http.c src/handle.c src/files.c | build
	$(CC) $(CFLAGS) $(EXTRA) tests/unit/test-unit.c src/http.c src/handle.c src/files.c -o $@

build/test-fuzz: tests/fuzz-driver.c src/http.c src/handle.c src/files.c | build
	$(CC) $(CFLAGS) $(EXTRA) tests/fuzz-driver.c src/http.c src/handle.c src/files.c -o $@

test: all build/test-fuzz
	./build/test-unit
	./tests/run-tests.sh
	./tests/check-docs.sh

sanitize: clean
	UBSAN_OPTIONS=halt_on_error=1 $(MAKE) EXTRA="-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer" test

clean:
	rm -rf build
	# docs/mast.js and docs/mast.wasm are committed artifacts: rebuild them
	# with sh wasm/build.sh, never delete them here

.PHONY: all test clean sanitize
