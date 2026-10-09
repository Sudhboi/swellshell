/* Control socket. The client sends its arguments, each NUL-terminated, then
 * shuts down its write side. The daemon answers "ok\n" or "error\n"
 * followed by free-form text, and closes the connection. */
#ifndef SWELL_IPC_H
#define SWELL_IPC_H

#include <stdbool.h>
#include <stddef.h>

#include "loop.h"

struct reply {
  char *buf;
  size_t len, cap;
};

void reply_printf(struct reply *r, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void reply_free(struct reply *r);

/* Returns 0 for success; anything else is reported as an error. */
typedef int (*ipc_handler_fn)(int argc, char **argv, struct reply *r,
                              void *data);

/* $SWELL_SOCK, else $XDG_RUNTIME_DIR/swell.sock. Static buffer. */
const char *ipc_socket_path(void);

/* Listen on path. If a daemon already answers there: with replace, ask it to
 * quit and wait (up to 3 s) for it to go; otherwise fail. */
bool ipc_server_start(struct loop *loop, const char *path, bool replace,
                      ipc_handler_fn fn, void *data);
/* Close and unlink the socket. */
void ipc_server_stop(void);

/* Send argv to the daemon at path. Returns -1 if nothing is listening,
 * otherwise 0 and sets *ok and fills out with the reply text. */
int ipc_client_send(const char *path, int argc, char *const argv[],
                    struct reply *out, bool *ok);

#endif
