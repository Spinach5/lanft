#include "protocol.h"
#include "network.h"
#include "transfer.h"
#include "compat.h"
#include "config.h"
#include "log.h"
#include "discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define CLI_VERSION "lanft v1.0"

/* --- Progress bar ------------------------------------------------------------------ */

static uint64_t cli_start_ms;
static uint64_t cli_total;
static char g_cli_path[1024];
static int cli_ret = 1;

static uint64_t now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void cli_progress(uint64_t done, uint64_t total)
{
    cli_total = total;
    if (total == 0) return;
    int pct = (int)(done * 100 / total);
    int bar_w = 30;
    int filled = bar_w * pct / 100;
    log_write("\r  [");
    for (int i = 0; i < bar_w; i++)
        fputc(i < filled ? '=' : (i == filled ? '>' : ' '), stderr);
    log_write("] %3d%%  ", pct);

    const char *units[] = {"B","KB","MB","GB"};
    int ui = 0; double ds = done;   while (ds >= 1024 && ui < 3) { ds /= 1024; ui++; }
    int uj = 0; double ts = total;  while (ts >= 1024 && uj < 3) { ts /= 1024; uj++; }
    log_write("%.1f%s / %.1f%s", ds, units[ui], ts, units[uj]);

    uint64_t elapsed = now_ms() - cli_start_ms;
    if (elapsed > 0) {
        double speed = (double)done / ((double)elapsed / 1000.0);
        int sk = 0; while (speed >= 1024 && sk < 3) { speed /= 1024; sk++; }
        log_write("  %.1f%s/s", speed, units[sk]);
    }
    fflush(stderr);
}

static void cli_error(const char *msg)
{
    log_write("\nError: %s\n", msg);
    cli_ret = 1;
}

static void cli_done(void)
{
    uint64_t elapsed = now_ms() - cli_start_ms;
    log_write("\n\nTransfer complete!\n");
    log_write("  Size:     ");
    const char *units[] = {"B","KB","MB","GB"};
    int u = 0; double s = cli_total;
    while (s >= 1024 && u < 3) { s /= 1024; u++; }
    log_write("%.1f %s\n", s, units[u]);
    log_write("  Duration: %.1fs\n", elapsed / 1000.0);
    if (elapsed > 0 && cli_total > 0) {
        double speed = (double)cli_total / ((double)elapsed / 1000.0);
        int sk = 0;
        while (speed >= 1024 && sk < 3) { speed /= 1024; sk++; }
        log_write("  Speed:    %.1f %s/s\n", speed, units[sk]);
    }
    cli_ret = 0;
}

/* CLI accept callback — prompt user on stdin for incoming transfers */
static int cli_accept_cb(const char *ip, const char *hostname,
                         const char *filename, uint64_t size)
{
    const char *units[] = {"B","KB","MB","GB"};
    int u = 0; double s = (double)size;
    while (s >= 1024 && u < 3) { s /= 1024; u++; }
    log_write("\n--- Incoming Transfer ---\n");
    log_write("From: %s (%s)\n", ip, hostname[0] ? hostname : "unknown");
    log_write("File: %s\n", filename);
    log_write("Size: %.1f %s\n", s, units[u]);
    log_write("Accept? [y/N]: ");
    fflush(stderr);

    char answer[16];
    if (fgets(answer, sizeof(answer), stdin)) {
        if (answer[0] == 'y' || answer[0] == 'Y') {
            log_write("Accepted.\n\n");
            return 1;
        }
    }
    log_write("Rejected.\n\n");
    return 0;
}

/* --- Help ------------------------------------------------------------------------------ */

static void print_help(const char *prog)
{
    printf("Usage: %s [OPTIONS] PATH\n\n", prog);
    printf("LAN file transfer tool — run without arguments to launch GUI\n\n");
    printf("Options:\n");
    printf("  --gui                 Launch SDL2 GUI (graphical mode)\n");
    printf("  -h, --help            Show this help\n");
    printf("  -v, --version         Show version\n");
    printf("  -S                    Shortcut for --mode=S (send)\n");
    printf("  -R                    Shortcut for --mode=R (receive)\n");
    printf("  --mode=S|R            Transfer mode (required)\n");
    printf("  --protocol=TCP|UDP    Protocol (default: TCP)\n");
    printf("  -p, --port=NUM        Port number (default: 9876)\n");
    printf("  --address=IP          Target IP (send: required; recv: default 0.0.0.0)\n");
    printf("  --history             Show transfer history\n");
    printf("  --save-dir=DIR        Save directory (default: ~/Downloads)\n");
    printf("  --buffer-size=N       Transfer buffer size (default: 65536)\n");
    printf("  --timeout=N           Timeout in seconds (default: 30)\n");
    printf("  --overwrite=POLICY    rename | overwrite | skip (default: rename)\n");
    printf("  --no-progress         Disable progress bar\n");
    printf("  --no-discovery        Disable LAN discovery\n");
    printf("  --log-level=LEVEL     debug | info | warn | error (default: info)\n");
    printf("  --log-file=PATH       Log file path (dir/ for date-based naming)\n");
    printf("  --bandwidth-limit=N   Send bandwidth limit in bytes/sec (0=unlimited)\n");
    printf("  --auto-accept         Auto-accept incoming files\n");
    printf("  --no-auto-accept      Prompt before accepting (default)\n");
    printf("  --show-config         Print effective config and exit\n");
    printf("  --save-config         Save current settings to user config\n");
    printf("  --config=PATH         Use custom config file\n\n");
    printf("Examples:\n");
    printf("  Send:   %s --mode=S --address=192.168.1.100 ./file.pdf\n", prog);
    printf("  Send:   %s -S -p 1234 ./video.mp4\n", prog);
    printf("  Recv:   %s --mode=R ./downloads/\n", prog);
    printf("  Recv:   %s --mode=R --address=10.0.0.5 -p 5555 ./received/\n", prog);
}

/* --- Main ------------------------------------------------------------------------------ */

int cli_main(int argc, char **argv)
{
    /* Windows: set console to UTF-8 so Chinese/box-drawing chars render */
#ifdef _WIN32
    system("chcp 65001 > nul 2>&1");
#endif
    struct lanft_config cfg;
    config_load(&cfg);
    log_init(&cfg);
    g_cli_path[0] = '\0';

    static struct option long_opts[] = {
        {"help",           no_argument,       0, 'h'},
        {"version",        no_argument,       0, 'v'},
        {"gui",            no_argument,       0, 1004},
        {"protocol",       required_argument, 0, 1000},
        {"mode",           required_argument, 0, 1001},
        {"port",           required_argument, 0, 'p'},
        {"address",        required_argument, 0, 1002},
        {"history",        no_argument,       0, 1003},
        {"save-config",    no_argument,       0, 2000},
        {"show-config",    no_argument,       0, 2001},
        {"save-dir",       required_argument, 0, 2002},
        {"buffer-size",    required_argument, 0, 2003},
        {"timeout",        required_argument, 0, 2004},
        {"overwrite",      required_argument, 0, 2005},
        {"no-progress",    no_argument,       0, 2006},
        {"no-discovery",   no_argument,       0, 2007},
        {"log-level",      required_argument, 0, 2008},
        {"bandwidth-limit",required_argument, 0, 2009},
        {"auto-accept",    no_argument,       0, 2010},
        {"no-auto-accept", no_argument,       0, 2011},
        {"log-file",       required_argument, 0, 2013},
        {"config",         required_argument, 0, 2012},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hvSRp:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h': print_help(argv[0]); return 0;
        case 'v': printf("%s\n", CLI_VERSION); return 0;
        case 'S': cfg.mode = 0; break;
        case 'R': cfg.mode = 1; break;
        case 'p': cfg.port = atoi(optarg); break;
        case 1000:
            cfg.protocol = (strcasecmp(optarg, "UDP") == 0) ? FT_PROTO_UDP : FT_PROTO_TCP;
            break;
        case 1001:
            cfg.mode = (strcasecmp(optarg, "R") == 0) ? 1 : 0;
            break;
        case 1002:
            strncpy(cfg.address, optarg, sizeof(cfg.address) - 1);
            break;
        case 1003:
            /* --history: handled after the switch */
            break;
        case 1004:
            log_write("GUI mode is not available in this build.\n");
            log_write("Rebuild with: cmake .. -DBUILD_GUI=ON\n");
            return 1;
        case 2000: /* --save-config */
            config_save(&cfg);
            return 0;
        case 2001: /* --show-config */
            config_print(&cfg);
            return 0;
        case 2002: /* --save-dir */
            strncpy(cfg.save_dir, optarg, sizeof(cfg.save_dir) - 1);
            break;
        case 2003: /* --buffer-size */
            cfg.buffer_size = atoi(optarg);
            break;
        case 2004: /* --timeout */
            cfg.timeout_seconds = atoi(optarg);
            break;
        case 2005: /* --overwrite */
            strncpy(cfg.overwrite_policy, optarg, sizeof(cfg.overwrite_policy) - 1);
            break;
        case 2006: /* --no-progress */
            cfg.show_progress = false;
            break;
        case 2007: /* --no-discovery */
            cfg.discovery_enabled = false;
            break;
        case 2008: /* --log-level */
            strncpy(cfg.log_level, optarg, sizeof(cfg.log_level) - 1);
            break;
        case 2009: /* --bandwidth-limit */
            cfg.send_bandwidth_limit = atoi(optarg);
            break;
        case 2010: /* --auto-accept */
            cfg.auto_accept = true;
            break;
        case 2011: /* --no-auto-accept */
            cfg.auto_accept = false;
            break;
        case 2013: /* --log-file */
            strncpy(cfg.log_file, optarg, sizeof(cfg.log_file) - 1);
            break;
        case 2012: /* --config */
            config_load_file(&cfg, optarg);
            break;
        default:
            print_help(argv[0]);
            return 1;
        }
    }

    /* --- History ---------------------------------------------------------------─ */
    {
        bool show_history = false;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--history") == 0) {
                show_history = true;
                break;
            }
        }
        if (show_history) {
            char path[512];
            snprintf(path, sizeof(path), "./lanft_history.dat");
            FILE *fp = fopen(path, "r");
            if (!fp) { printf("No history found.\n"); return 0; }
            char line[1024];
            printf("%-20s %8s %8s %8s %4s %5s %5s %3s %s\n",
                   "Name","Start","End","Dur(ms)","Kind","Port","St","%","Speed");
            while (fgets(line, sizeof(line), fp)) {
                char name[256]="",st[32]="",et[32]="";
                unsigned long dur=0,spd=0; int k=0,port=0,stt=0,prog=0;
                sscanf(line,"%255[^|]|%31[^|]|%31[^|]|%lu|%d|%d|%d|%d|%lu",
                       name, st, et, &dur, &k, &port, &stt, &prog, &spd);
                printf("%-20s %8s %8s %8lu %4s %5d %5s %3d%% %lu\n",
                       name, st, et, dur,
                       k==0?"SEND":"RECV", port,
                       stt==0?"OK":(stt==1?"BAD":"STOP"), prog, spd);
            }
            fclose(fp);
            return 0;
        }
    }

    /* Re-init log with CLI-overridden config (e.g. --log-file) */
    log_init(&cfg);

    /* Start discovery responder so other instances can find us */
    if (cfg.discovery_enabled)
        discovery_start((uint16_t)cfg.port);

    /* --- Validation ------------------------------------------------------------ */
    if (cfg.mode < 0) {
        log_write("Error: --mode is required (S=send, R=receive)\n\n");
        print_help(argv[0]);
        return 1;
    }

    /* Determine the path: CLI positional arg takes priority.
       If no arg given, fall back to config save_dir. */
    const char *work_path;
    if (optind < argc) {
        strncpy(g_cli_path, argv[optind], sizeof(g_cli_path) - 1);
        work_path = g_cli_path;
    } else {
        work_path = config_expand_path(cfg.save_dir);
    }

    struct stat path_st;
    if (cfg.mode == 0) {
        /* Send: positional arg is required */
        if (optind >= argc) {
            log_write("Error: missing PATH argument\n\n");
            print_help(argv[0]);
            return 1;
        }
        if (stat(work_path, &path_st) != 0) {
            log_write("Error: file/directory not found: %s\n", work_path);
            return 1;
        }
    } else {
        /* Receive: use work_path as save directory.
           If it doesn't exist, try to create it. */
        if (stat(work_path, &path_st) != 0) {
            log_write("Directory '%s' not found, creating...\n", work_path);
            if (mkdir(work_path, 0755) != 0) {
                log_write("Error: failed to create directory: %s\n", work_path);
                return 1;
            }
        } else if (!S_ISDIR(path_st.st_mode)) {
            log_write("Error: not a directory: %s\n", work_path);
            return 1;
        }
    }

    /* --- Set CLI callbacks --------------------------------------------------- */
    transfer_set_callbacks(cli_progress, cli_error, cli_done);
    transfer_set_auto_accept(cfg.auto_accept);
    transfer_set_buffer_size(cfg.buffer_size);
    transfer_set_timeout(cfg.timeout_seconds);
    transfer_set_overwrite_policy(cfg.overwrite_policy);

    /* CLI accept callback — prompt user on stdin */
    transfer_set_accept_callback(cli_accept_cb);

    /* --- Execute ------------------------------------------------------------------ */

    cli_ret = 1;

    if (cfg.mode == 0) {
        /* Send */
        struct net_context *nc = net_create(cfg.protocol);
        if (!nc) {
            log_write("Error: failed to create network context\n");
            return 1;
        }

        log_write("Connecting to %s:%d...\n", cfg.address, cfg.port);
        cli_start_ms = now_ms();
        int tries = 0;
        while (tries < 60) {
            if (cfg.protocol == FT_PROTO_TCP) {
                if (net_connect(nc, cfg.address, cfg.port) == 0) break;
            } else {
                if (net_udp_bind(nc, cfg.port) == 0) {
                    net_udp_set_peer(nc, cfg.address, cfg.port);
                    break;
                }
            }
            tries++;
            log_write("\r  Retrying... (%d/60)", tries);
            fflush(stderr);
            sleep(1);
        }
        if (tries >= 60) {
            log_write("\nError: failed to connect to %s:%d\n",
                    cfg.address, cfg.port);
            net_destroy(nc);
            return 1;
        }
        log_write("\nConnected! Sending %s...\n", g_cli_path);
        transfer_send(nc, g_cli_path, cfg.protocol);
        net_destroy(nc);
        return cli_ret;
    }

    /* Receive — persistent listener */
    log_write("Persistent listener on %s:%d (Ctrl+C to stop)\n\n",
            cfg.address, cfg.port);
    int transfer_count = 0;
    while (1) {
        struct net_context *nc = net_create(cfg.protocol);
        if (!nc) {
            log_write("Error: failed to create network context\n");
            return 1;
        }

        if (cfg.protocol == FT_PROTO_TCP) {
            if (net_listen_ip(nc, cfg.address, cfg.port) != 0) {
                log_write("Error: failed to listen on %s:%d\n",
                        cfg.address, cfg.port);
                net_destroy(nc);
                return 1;
            }
        } else {
            if (net_udp_bind(nc, cfg.port) != 0) {
                log_write("Error: failed to bind UDP port %d\n", cfg.port);
                net_destroy(nc);
                return 1;
            }
        }

        transfer_count++;
        cli_start_ms = now_ms();
        cli_ret = 1;
        log_write("[%d] Waiting for sender...\n", transfer_count);
        transfer_recv(nc, work_path, cfg.protocol);
        net_destroy(nc);

        if (cli_ret != 0) {
            log_write("\n[%d] Transfer failed, continuing to listen...\n\n",
                    transfer_count);
        } else {
            log_write("\n[%d] Done, listening for next...\n\n",
                    transfer_count);
        }
    }
}
