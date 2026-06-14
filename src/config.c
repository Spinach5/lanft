#include "config.h"
#include "compat.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <shlobj.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

#include "tomlc17/tomlc17.h"

/* ── Path helpers ───────────────────────────────────────────── */

const char *config_system_path(void)
{
    return "/etc/lanft/config.toml";
}

#ifdef _WIN32
const char *config_user_path(void)
{
    static char path[512];
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        snprintf(path, sizeof(path), "%s/lanft/config.toml", appdata);
    } else {
        snprintf(path, sizeof(path), "lanft_config.toml");
    }
    return path;
}
#else
const char *config_user_path(void)
{
    static char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(path, sizeof(path), "%s/lanft/config.toml", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(path, sizeof(path), "%s/.config/lanft/config.toml", home);
    }
    return path;
}
#endif

const char *config_expand_path(const char *path)
{
    static char buf[1024];
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\\' || path[1] == '\0')) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        if (!home) home = ".";
        snprintf(buf, sizeof(buf), "%s%s", home, path + 1);
        return buf;
    }
    return path;
}

const char *config_log_path(const struct lanft_config *cfg)
{
    static char buf[1024];

    if (!cfg->log_file[0]) return NULL;  /* empty → stderr */

    size_t len = strlen(cfg->log_file);
    bool is_dir = (cfg->log_file[len - 1] == '/' || cfg->log_file[len - 1] == '\\');
    if (!is_dir) {
        /* Check if the path is an existing directory */
        struct stat st;
        if (stat(cfg->log_file, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = true;
    }

    if (is_dir) {
        /* Directory → generate date-based filename: YYYYMMDD-level.log */
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", cfg->log_file);
        /* Ensure trailing slash */
        if (dir[len - 1] != '/' && dir[len - 1] != '\\') {
            if (len < sizeof(dir) - 1) { dir[len] = '/'; dir[len + 1] = '\0'; }
        }
        snprintf(buf, sizeof(buf), "%s%04d%02d%02d-%s.log",
                 dir,
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 cfg->log_level[0] ? cfg->log_level : "info");
        return buf;
    }

    return cfg->log_file;  /* exact file path */
}

/* ── Defaults ──────────────────────────────────────────────── */

void config_set_defaults(struct lanft_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port               = 9876;
    cfg->protocol           = FT_PROTO_TCP;
    strncpy(cfg->address, "0.0.0.0", sizeof(cfg->address) - 1);
    strncpy(cfg->save_dir, "~/Downloads", sizeof(cfg->save_dir) - 1);
    cfg->mode               = -1;
    cfg->buffer_size        = 65536;
    cfg->timeout_seconds    = 30;
    cfg->max_connections    = 5;
    cfg->auto_accept        = false;
    strncpy(cfg->overwrite_policy, "rename", sizeof(cfg->overwrite_policy) - 1);
    cfg->show_progress      = true;
    cfg->discovery_enabled  = true;
    cfg->discovery_interval = 5;
    cfg->discovery_ttl      = 1;
    strncpy(cfg->log_level, "info", sizeof(cfg->log_level) - 1);
    strncpy(cfg->log_file, "/var/log/lanft/", sizeof(cfg->log_file) - 1);
    cfg->send_bandwidth_limit = 0;
}

/* ── Apply TOML table ──────────────────────────────────────── */

/* Apply values from a parsed TOML table onto cfg. Overwrites only keys present. */
static void apply_toml_table(struct lanft_config *cfg, toml_datum_t table)
{
    toml_datum_t v;

    v = toml_get(table, "port");
    if (v.type == TOML_INT64) cfg->port = (int)v.u.int64;

    v = toml_get(table, "protocol");
    if (v.type == TOML_STRING) {
        if (strcasecmp(v.u.s, "UDP") == 0)
            cfg->protocol = FT_PROTO_UDP;
        else
            cfg->protocol = FT_PROTO_TCP;
    }

    v = toml_get(table, "address");
    if (v.type == TOML_STRING) {
        strncpy(cfg->address, v.u.s, sizeof(cfg->address) - 1);
    }

    v = toml_get(table, "save_dir");
    if (v.type == TOML_STRING) {
        strncpy(cfg->save_dir, v.u.s, sizeof(cfg->save_dir) - 1);
    }

    v = toml_get(table, "mode");
    if (v.type == TOML_STRING) {
        if (strcasecmp(v.u.s, "S") == 0 || strcasecmp(v.u.s, "send") == 0)
            cfg->mode = 0;
        else if (strcasecmp(v.u.s, "R") == 0 || strcasecmp(v.u.s, "receive") == 0)
            cfg->mode = 1;
    }

    v = toml_get(table, "buffer_size");
    if (v.type == TOML_INT64) cfg->buffer_size = (int)v.u.int64;

    v = toml_get(table, "timeout_seconds");
    if (v.type == TOML_INT64) cfg->timeout_seconds = (int)v.u.int64;

    v = toml_get(table, "max_connections");
    if (v.type == TOML_INT64) cfg->max_connections = (int)v.u.int64;

    v = toml_get(table, "auto_accept");
    if (v.type == TOML_BOOLEAN) cfg->auto_accept = v.u.boolean;

    v = toml_get(table, "overwrite_policy");
    if (v.type == TOML_STRING) {
        strncpy(cfg->overwrite_policy, v.u.s, sizeof(cfg->overwrite_policy) - 1);
    }

    v = toml_get(table, "show_progress");
    if (v.type == TOML_BOOLEAN) cfg->show_progress = v.u.boolean;

    v = toml_get(table, "discovery_enabled");
    if (v.type == TOML_BOOLEAN) cfg->discovery_enabled = v.u.boolean;

    v = toml_get(table, "discovery_interval");
    if (v.type == TOML_INT64) cfg->discovery_interval = (int)v.u.int64;

    v = toml_get(table, "discovery_ttl");
    if (v.type == TOML_INT64) cfg->discovery_ttl = (int)v.u.int64;

    v = toml_get(table, "log_level");
    if (v.type == TOML_STRING) {
        strncpy(cfg->log_level, v.u.s, sizeof(cfg->log_level) - 1);
    }

    v = toml_get(table, "log_file");
    if (v.type == TOML_STRING) {
        strncpy(cfg->log_file, v.u.s, sizeof(cfg->log_file) - 1);
    }

    v = toml_get(table, "send_bandwidth_limit");
    if (v.type == TOML_INT64) cfg->send_bandwidth_limit = (int)v.u.int64;
}

/* ── Load: layered config ──────────────────────────────────── */

/* Create all parent directories for a file path (like mkdir -p).
   Modifies pathbuf temporarily but restores it. */
static void mkdir_parents(char *pathbuf)
{
    char *slash = strrchr(pathbuf, '/');
#ifdef _WIN32
    char *bslash = strrchr(pathbuf, '\\');
    if (bslash > slash) slash = bslash;
#endif
    if (!slash) return;
    *slash = '\0';
    for (char *p = pathbuf; *p; p++) {
        if ((*p == '/' || *p == '\\') && p > pathbuf) {
            char saved = *p; *p = '\0';
#ifdef _WIN32
            mkdir(pathbuf);
#else
            mkdir(pathbuf, 0755);
#endif
            *p = saved;
        }
    }
#ifdef _WIN32
    mkdir(pathbuf);
#else
    mkdir(pathbuf, 0755);
#endif
    *slash = '/';  /* restore */
}

int config_load(struct lanft_config *cfg)
{
    config_set_defaults(cfg);

    /* ── Layer 1: System config (/etc/lanft/config.toml) ──── */
    toml_result_t sys_res = toml_parse_file_ex(config_system_path());
    if (sys_res.ok) {
        apply_toml_table(cfg, sys_res.toptab);
        log_write("[config] loaded system config: %s\n", config_system_path());
    }
    toml_free(sys_res);
    /* Missing or parse error → silently skip */

    /* ── Layer 2: User config (~/.config/lanft/config.toml) ── */
    const char *user_path = config_user_path();
    struct stat st;
    int user_exists = (stat(user_path, &st) == 0);

    if (!user_exists) {
        /* Try to copy system config as initial user config */
        FILE *fsrc = fopen(config_system_path(), "r");
        if (fsrc) {
            /* Create parent directory */
            char dirbuf[512];
            strncpy(dirbuf, user_path, sizeof(dirbuf) - 1);
            mkdir_parents(dirbuf);

            FILE *fdst = fopen(user_path, "w");
            if (fdst) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0)
                    fwrite(buf, 1, n, fdst);
                fclose(fdst);
                log_write("[config] created initial user config: %s\n", user_path);
            }
            fclose(fsrc);
        } else {
            log_write("[config] no config files found, using built-in defaults\n");
        }
    }

    /* Now parse user config (whether just created or pre-existing) */
    toml_result_t user_res = toml_parse_file_ex(user_path);
    if (user_res.ok) {
        apply_toml_table(cfg, user_res.toptab);
    }
    toml_free(user_res);
    /* Parse error → already have built-in + system, just skip */

    return 0;
}

/* ── Load: single file ─────────────────────────────────────── */

int config_load_file(struct lanft_config *cfg, const char *path)
{
    toml_result_t res = toml_parse_file_ex(path);
    if (!res.ok) {
        log_write("Warning: failed to parse config: %s\n", path);
        toml_free(res);
        return -1;
    }
    apply_toml_table(cfg, res.toptab);
    toml_free(res);
    log_write("[config] loaded: %s\n", path);
    return 0;
}

/* ── Save ──────────────────────────────────────────────────── */

int config_save(const struct lanft_config *cfg)
{
    const char *path = config_user_path();

    /* Create parent directory */
    char dirbuf[512];
    strncpy(dirbuf, path, sizeof(dirbuf) - 1);
    mkdir_parents(dirbuf);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        log_write("Error: cannot write config to %s: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "# lanft configuration\n");
    fprintf(fp, "# Generated automatically — edit and re-run\n\n");

    fprintf(fp, "port = %d\n", cfg->port);
    fprintf(fp, "protocol = \"%s\"\n", cfg->protocol == FT_PROTO_UDP ? "UDP" : "TCP");
    fprintf(fp, "address = \"%s\"\n", cfg->address);
    fprintf(fp, "save_dir = \"%s\"\n", cfg->save_dir);
    fprintf(fp, "mode = \"%s\"\n",
            cfg->mode == 0 ? "S" : (cfg->mode == 1 ? "R" : ""));
    fprintf(fp, "\n");
    fprintf(fp, "# Advanced\n");
    fprintf(fp, "buffer_size = %d\n", cfg->buffer_size);
    fprintf(fp, "timeout_seconds = %d\n", cfg->timeout_seconds);
    fprintf(fp, "max_connections = %d\n", cfg->max_connections);
    fprintf(fp, "auto_accept = %s\n", cfg->auto_accept ? "true" : "false");
    fprintf(fp, "overwrite_policy = \"%s\"\n", cfg->overwrite_policy);
    fprintf(fp, "show_progress = %s\n", cfg->show_progress ? "true" : "false");
    fprintf(fp, "\n");
    fprintf(fp, "# Discovery\n");
    fprintf(fp, "discovery_enabled = %s\n", cfg->discovery_enabled ? "true" : "false");
    fprintf(fp, "discovery_interval = %d\n", cfg->discovery_interval);
    fprintf(fp, "discovery_ttl = %d\n", cfg->discovery_ttl);
    fprintf(fp, "\n");
    fprintf(fp, "# Logging\n");
    fprintf(fp, "log_level = \"%s\"\n", cfg->log_level);
    if (cfg->log_file[0])
        fprintf(fp, "log_file = \"%s\"\n", cfg->log_file);
    else
        fprintf(fp, "# log_file = \"\"\n");
    fprintf(fp, "send_bandwidth_limit = %d\n", cfg->send_bandwidth_limit);

    fclose(fp);
    log_write("[config] saved to %s\n", path);
    return 0;
}

/* ── Print ─────────────────────────────────────────────────── */

void config_print(const struct lanft_config *cfg)
{
    printf("# Effective configuration\n\n");
    printf("port = %d\n", cfg->port);
    printf("protocol = \"%s\"\n", cfg->protocol == FT_PROTO_UDP ? "UDP" : "TCP");
    printf("address = \"%s\"\n", cfg->address);
    printf("save_dir = \"%s\"\n", cfg->save_dir);
    printf("mode = \"%s\"\n",
           cfg->mode == 0 ? "S" : (cfg->mode == 1 ? "R" : "(unspecified)"));
    printf("\n# Advanced\n");
    printf("buffer_size = %d\n", cfg->buffer_size);
    printf("timeout_seconds = %d\n", cfg->timeout_seconds);
    printf("max_connections = %d\n", cfg->max_connections);
    printf("auto_accept = %s\n", cfg->auto_accept ? "true" : "false");
    printf("overwrite_policy = \"%s\"\n", cfg->overwrite_policy);
    printf("show_progress = %s\n", cfg->show_progress ? "true" : "false");
    printf("\n# Discovery\n");
    printf("discovery_enabled = %s\n", cfg->discovery_enabled ? "true" : "false");
    printf("discovery_interval = %d\n", cfg->discovery_interval);
    printf("discovery_ttl = %d\n", cfg->discovery_ttl);
    printf("\n# Logging\n");
    printf("log_level = \"%s\"\n", cfg->log_level);
    {
        const char *lp = config_log_path(cfg);
        printf("log_file = \"%s\"\n", lp ? lp : "(stderr)");
    }
    printf("send_bandwidth_limit = %d\n", cfg->send_bandwidth_limit);
}
