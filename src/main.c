/* main.c - the CLI: mast [-d docroot] [-p port] [-c max-connections].
 * Prints the exact, honest usage and dies on any misuse with exit 2. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
        "mast - a small HTTP/1.1 static file server\n"
        "usage: mast [-d docroot] [-p port] [-c max-connections]\n"
        "  -d docroot          directory to serve (default: .)\n"
        "  -p port             port to listen on (default: 8080)\n"
        "  -c max-connections  simultaneous connection cap (default: 1024)\n");
}

int main(int argc, char **argv) {
    const char *docroot = ".";
    int port = 8080;
    int max_conn = 1024;
    int opt;
    while ((opt = getopt(argc, argv, "d:p:c:h")) != -1) {
        char *endp = NULL;
        switch (opt) {
        case 'd': docroot = optarg; break;
        case 'p':
            port = (int)strtol(optarg, &endp, 10);
            if (!endp || *endp || port < 1 || port > 65535) {
                fprintf(stderr, "mast: port must be 1-65535\n");
                return 2;
            }
            break;
        case 'c':
            max_conn = (int)strtol(optarg, &endp, 10);
            if (!endp || *endp || max_conn < 1 || max_conn > 65536) {
                fprintf(stderr, "mast: max-connections must be 1-65536\n");
                return 2;
            }
            break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return 2;
    }
    char real[PATH_MAX];
    if (!realpath(docroot, real)) {
        fprintf(stderr, "mast: docroot %s: ", docroot);
        perror(NULL);
        return 1;
    }
    MastConfig cfg = {
        .docroot_real = real,
        .port = port,
        .max_conn = max_conn,
        .header_timeout_ms = 10000,
        .idle_timeout_ms = 15000,
        .write_timeout_ms = 30000,
        .body_timeout_ms = 30000,
    };
    return mast_serve(&cfg);
}
