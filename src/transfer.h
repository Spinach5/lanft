#ifndef TRANSFER_H
#define TRANSFER_H

#include <stdint.h>

struct net_context;

/* Progress callbacks — set by GUI or CLI before transfer */
typedef void (*transfer_progress_fn)(uint64_t done, uint64_t total);
typedef void (*transfer_error_fn)(const char *msg);
typedef void (*transfer_done_fn)(void);

void transfer_set_callbacks(transfer_progress_fn prog,
                            transfer_error_fn err,
                            transfer_done_fn done);

/* Send a file over the network.
   nc must already be connected/listening.
   Runs the transfer loop — call from a worker thread. */
void transfer_send(struct net_context *nc, const char *filepath, int protocol);
void transfer_recv(struct net_context *nc, const char *savepath, int protocol);

#endif /* TRANSFER_H */
