/* handle.h - turn a parsed request into a response. Socket-independent:
 * the epoll server and the wasm demo both call mast_respond. */
#ifndef MAST_HANDLE_H
#define MAST_HANDLE_H

#include "http.h"
#include "files.h"

typedef struct {
    MastBuf head;       /* status line + headers, always filled */
    MastFile file;      /* 200 answers: the open file to stream */
    int has_file;       /* stream file.fd after head */
    long long file_off, file_len;   /* the byte range to stream */
    MastBuf mem;        /* in-memory body (errors, echo, 301 page) */
    int close_after;    /* the connection ends after this response */
    int status;         /* for the access log */
} MastResponse;

void mast_response_init(MastResponse *r);
void mast_response_free(MastResponse *r);

/* Build the response for a fully-parsed request (body already read).
 * docroot_real is the realpath'd served root. */
void mast_respond(const MastRequest *req, const char *docroot_real, MastResponse *r);

#endif
