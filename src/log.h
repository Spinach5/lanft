/* Simple logging module — writes to stderr + log file simultaneously.
 *
 * Usage:
 *   log_init(&cfg);            // once at startup
 *   log_debug("[SEND] ...");   // level-filtered
 *   log_info("Listening...");  // always shown
 *   log_warn("timeout");
 *   log_error("failed");
 *   log_write("raw message");  // unconditional, mirrors to both outputs
 *   log_close();               // on exit
 *
 * Filtered by cfg->log_level: debug < info < warn < error.
 */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>

struct lanft_config;

/* Initialize logging. Opens the log file (creating parent dirs if needed).
   Call once at startup. Safe to call even if log_file is empty (stderr-only). */
int  log_init(const struct lanft_config *cfg);

/* Close log file. Call on exit. */
void log_close(void);

/* ── Level-filtered ─────────────────────────────────────────── */
/* These obey log_level — debug messages are suppressed when level>debug, etc. */
void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

/* ── Unconditional ───────────────────────────────────────────── */
/* Always writes to stderr + log file, regardless of log_level. */
void log_write(const char *fmt, ...);

#endif /* LOG_H */
