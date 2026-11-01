/* fuzz-driver: reads stdin and hammers the parser/body/respond path with
 * it. Never crashes, never hangs; any accepted request is answered against
 * an empty temp docroot. Run under ASan/UBSan in CI. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../src/http.h"
#include "../src/handle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char root[] = "/tmp/mast-fuzz-XXXXXX";
    if (!mkdtemp(root)) return 1;
    char idx[256];
    snprintf(idx, sizeof idx, "%s/index.html", root);
    FILE *f = fopen(idx, "w");
    if (f) { fputs("fuzz\n", f); fclose(f); }

    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf, stdin);

    /* feed the bytes in random-looking fragment sizes (deterministic:
     * derived from the bytes themselves) */
    MastParser p;
    mast_parser_init(&p);
    MastRequest req;
    MastBodyReader br;
    memset(&br, 0, sizeof br);
    size_t off = 0;
    int parsed = 0;
    size_t consumed = 0;
    while (off < n && !parsed) {
        size_t frag = 1 + (unsigned char)buf[off] % 97;
        if (frag > n - off) frag = n - off;
        MastParse r = mast_parser_feed(&p, buf + off, frag, &req, &consumed);
        off += frag;
        if (r == MAST_PARSE_BAD) break;
        if (r == MAST_PARSE_OK) {
            parsed = 1;
            /* body bytes: whatever remains, in fragments too */
            if (req.chunked || req.content_length > 0) {
                MastBodyRead br_res = MAST_BODY_INCOMPLETE;
                while (off < n && br_res == MAST_BODY_INCOMPLETE) {
                    size_t bfrag = 1 + (unsigned char)buf[off] % 53;
                    if (bfrag > n - off) bfrag = n - off;
                    size_t bc = 0;
                    br_res = mast_body_feed(&req, &br, buf + off, bfrag, &bc);
                    off += bc ? bc : bfrag;   /* consumed or drop: fuzzing, not serving */
                    if (br_res == MAST_BODY_BAD) break;
                }
            }
        }
    }
    if (parsed) {
        MastResponse resp;
        mast_response_init(&resp);
        mast_respond(&req, root, &resp);
        /* touch the response so nothing is optimized away */
        volatile size_t total = resp.head.len + resp.mem.len;
        (void)total;
        mast_response_free(&resp);
    }
    free(req.body);
    mast_parser_free(&p);
    return 0;
}
