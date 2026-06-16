#include "log.h"
#include "config.h"
#include "compat.h"
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static FILE *g_log_fp = NULL;
static int   g_log_level = 0;   /* 0=debug, 1=info, 2=warn, 3=error */
static bool  g_mute_stderr = false;

void log_mute_stderr(bool mute) { g_mute_stderr = mute; }

static int level_from_str(const char *s)
{
    if (!s) return 1;
    if (strcmp(s, "debug") == 0) return 0;
    if (strcmp(s, "info")  == 0) return 1;
    if (strcmp(s, "warn")  == 0) return 2;
    if (strcmp(s, "error") == 0) return 3;
    return 1; /* default info */
}

int log_init(const struct lanft_config *cfg)
{
    /* Close previous log file if re-initializing */
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }

    g_log_level = level_from_str(cfg->log_level);
    const char *path = config_log_path(cfg);
    if (!path) return 0;  /* empty → stderr only */

    /* Create parent directories */
    char dirbuf[1024];
    strncpy(dirbuf, path, sizeof(dirbuf) - 1);
    char *slash = strrchr(dirbuf, '/');
#ifdef _WIN32
    char *bslash = strrchr(dirbuf, '\\');
    if (bslash > slash) slash = bslash;
#endif
    if (slash) {
        *slash = '\0';
        for (char *p = dirbuf; *p; p++) {
            if ((*p == '/' || *p == '\\') && p > dirbuf) {
                char saved = *p; *p = '\0';
                mkdir(dirbuf, 0755);
                *p = saved;
            }
        }
        mkdir(dirbuf, 0755);
    }

    g_log_fp = fopen(path, "a");
    if (!g_log_fp) {
        /* silently fall back to stderr-only — log file is optional */
        return 0;
    }
    /* Only print this on first successful open */
    return 0;
}

void log_close(void)
{
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
}

/* ── Core write ─────────────────────────────────────────────── */

static void log_vwrite(int level, const char *fmt, va_list ap)
{
    /* Timestamp */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    static const char *labels[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    const char *label = (level >= 0 && level <= 3) ? labels[level] : "";

    /* Format the message */
    char msgbuf[4096];
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);

    /* stderr — skip debug/info/warn when muted, only errors pass */
    if (!g_mute_stderr || level >= 3) {
        fprintf(stderr, "%s", msgbuf);
        fflush(stderr);
    }

    /* Log file (always gets timestamp + level prefix) */
    if (g_log_fp) {
        fprintf(g_log_fp, "[%s] [%s] %s", ts, label, msgbuf);
        fflush(g_log_fp);
    }
}

/* ── Public API ─────────────────────────────────────────────── */

void log_debug(const char *fmt, ...)
{
    if (g_log_level > 0) return;
    va_list ap; va_start(ap, fmt);
    log_vwrite(0, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...)
{
    if (g_log_level > 1) return;
    va_list ap; va_start(ap, fmt);
    log_vwrite(1, fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    if (g_log_level > 2) return;
    va_list ap; va_start(ap, fmt);
    log_vwrite(2, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_vwrite(3, fmt, ap);
    va_end(ap);
}

void log_write(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    /* Write unconditionally — use INFO format without level filtering */
    {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char ts[32];
        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

        char msgbuf[4096];
        vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);

        fprintf(stderr, "%s", msgbuf);
        fflush(stderr);

        if (g_log_fp) {
            fprintf(g_log_fp, "[%s] %s", ts, msgbuf);
            fflush(g_log_fp);
        }
    }
    va_end(ap);
}
