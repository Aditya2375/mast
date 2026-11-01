/* server.h - the epoll edge-triggered event loop: non-blocking accept,
 * pipelined request parsing, streaming file bodies with sendfile, idle and
 * write timeouts, a connection cap, and graceful shutdown on SIGINT/SIGTERM. */
#ifndef MAST_SERVER_H
#define MAST_SERVER_H

typedef struct {
    const char *docroot_real;   /* resolved docroot */
    int port;
    int max_conn;               /* default 1024 */
    int header_timeout_ms;      /* a request head must complete: 10000 */
    int idle_timeout_ms;        /* keep-alive idle between requests: 15000 */
    int write_timeout_ms;       /* a stalled client write: 30000 */
    int body_timeout_ms;        /* a body must finish: 30000 */
    int max_requests;           /* requests per connection, then close: 1000 */
    int max_age_ms;             /* no connection outlives this: 300000 */
    int mem_budget_mb;          /* all connection buffers together: 256 */
} MastConfig;

int mast_serve(const MastConfig *cfg);   /* blocks until shutdown */

#endif
