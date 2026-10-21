#ifndef SF_SERVER_H
#define SF_SERVER_H

#include "config.h"

/* Runs the server until SIGINT/SIGTERM. Blocks the calling (main) thread, which
 * becomes the acceptor. Returns 0 after a clean drain, nonzero on startup failure.
 *
 * Thread model:
 *   main      : accept4() loop, pushes fds to the bounded queue (never blocks)
 *   workers   : N pthreads, pop -> read -> route -> respond -> close
 *   signaller : sigwait() thread; wakes the acceptor through a pipe, so no work
 *               happens in an async-signal context at all
 * Shutdown: stop accepting -> close listen fd -> close queue -> workers drain
 * everything already accepted -> join -> return. */
int server_run(const config *cfg);

#endif
