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
        "  -c max-connections  simultaneous connection cap (default: 1024)\n"
        "  -H header-ms        time to finish a request head (default: 10000)\n"
        "  -I idle-ms          keep-alive idle between requests (default: 15000)\n"
        "  -B body-ms          time to finish a request body (default: 30000)\n"
        "  -W write-ms         stalled response write (default: 30000)\n");
}

/* Numeric flags are strict: digits only, no sign, no leading whitespace -
 * strtol alone would silently accept " 500" or "+5". */
static int all_digits(const char *s) {
    if (!*s) return 0;
    for (const char *q = s; *q; q++)
        if (*q < '0' || *q > '9') return 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *docroot = ".";
    int port = 8080;
    int max_conn = 1024;
    int header_ms = 10000, idle_ms = 15000, body_ms = 30000, write_ms = 30000;
    int opt;
    while ((opt = getopt(argc, argv, "d:p:c:H:I:B:W:h")) != -1) {
        char *endp = NULL;
        switch (opt) {
        case 'd': docroot = optarg; break;
        case 'p':
            if (!all_digits(optarg)) {
                fprintf(stderr, "mast: port must be 1-65535\n");
                return 2;
            }
            port = (int)strtol(optarg, &endp, 10);
            if (!endp || *endp || port < 1 || port > 65535) {
                fprintf(stderr, "mast: port must be 1-65535\n");
                return 2;
            }
            break;
        case 'c':
            if (!all_digits(optarg)) {
                fprintf(stderr, "mast: max-connections must be 1-65536\n");
                return 2;
            }
            max_conn = (int)strtol(optarg, &endp, 10);
            if (!endp || *endp || max_conn < 1 || max_conn > 65536) {
                fprintf(stderr, "mast: max-connections must be 1-65536\n");
                return 2;
            }
            break;
        case 'H': case 'I': case 'B': case 'W': {
            if (!all_digits(optarg)) {
                fprintf(stderr, "mast: timeouts are 100-3600000 ms\n");
                return 2;
            }
            long v = strtol(optarg, &endp, 10);
            if (!endp || *endp || v < 100 || v > 3600000) {
                fprintf(stderr, "mast: timeouts are 100-3600000 ms\n");
                return 2;
            }
            if (opt == 'H') header_ms = (int)v;
            else if (opt == 'I') idle_ms = (int)v;
            else if (opt == 'B') body_ms = (int)v;
            else write_ms = (int)v;
            break;
        }
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
        .header_timeout_ms = header_ms,
        .idle_timeout_ms = idle_ms,
        .write_timeout_ms = write_ms,
        .body_timeout_ms = body_ms,
    };
    return mast_serve(&cfg);
}
