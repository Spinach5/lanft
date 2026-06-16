#ifndef TRANSFER_H
#define TRANSFER_H

#include <stdbool.h>
#include <stdint.h>

struct net_context;

/* Progress callbacks — set by GUI or CLI before transfer */
typedef void (*transfer_progress_fn)(uint64_t done, uint64_t total);
typedef void (*transfer_error_fn)(const char *msg);
typedef void (*transfer_done_fn)(void);

/* Accept callback — called when auto_accept=false to ask user for confirmation.
   ip, hostname, filename, size describe the incoming transfer.
   Must return 1 to accept or 0 to reject.
   Called from the transfer thread — implementation should block until user responds. */
typedef int (*transfer_accept_fn)(const char *ip, const char *hostname,
                                  const char *filename, uint64_t size);

void transfer_set_callbacks(transfer_progress_fn prog,
                            transfer_error_fn err,
                            transfer_done_fn done);

/* Set auto_accept mode. When false, the accept callback is invoked before
   each incoming transfer. */
void transfer_set_auto_accept(bool enabled);

/* Set the accept callback. Only used when auto_accept is false. */
void transfer_set_accept_callback(transfer_accept_fn cb);

/* Called from the main/GUI thread to respond to an incoming transfer prompt.
   transfer_accept()  → proceeds with transfer
   transfer_reject()  → rejects and closes connection */
void transfer_accept(void);
void transfer_reject(void);

/* Apply runtime config to transfer module */
void transfer_set_buffer_size(int size);
void transfer_set_timeout(int seconds);
void transfer_set_overwrite_policy(const char *policy);  /* "rename"|"overwrite"|"skip" */
void transfer_set_bandwidth_limit(int limit);            /* bytes/sec, 0=unlimited */
void transfer_set_max_connections(int max_conn);         /* 0=unlimited */

/* Prepare a path for sending. If it's a directory, compress to a temp .tar.gz.
   Returns the actual path to send (original or compressed temp file).
   Caller must free *out_path if it differs from filepath.
   out_size receives the total byte count. Reports progress via callback. */
char *transfer_prepare_send(const char *filepath, uint64_t *out_size);

/* Clean up after send — deletes temp archive if path was compressed. */
void transfer_cleanup_send(char *prepared_path, const char *original_path);

/* Send a file over the network.
   nc must already be connected/listening.
   Runs the transfer loop — call from a worker thread. */
void transfer_send(struct net_context *nc, const char *filepath, int protocol);
void transfer_recv(struct net_context *nc, const char *savepath, int protocol);

/* Get the filename from the last received meta (for history logging).
   Returns NULL if no transfer has occurred yet. */
const char *transfer_last_recv_name(void);

#endif /* TRANSFER_H */
