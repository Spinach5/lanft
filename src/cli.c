#define _GNU_SOURCE
#include "protocol.h"
#include "network.h"
#include "transfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLI_VERSION "lanft v1.0"

/* ── Config ────────────────────────────────────────────────── */

struct cli_config {
    int  protocol;
    int  mode;         /* 0=S, 1=R */
    int  port;
    char address[64];
    char path[1024];
    bool show_history;
};

/* ── Progress bar ──────────────────────────────────────────── */

static uint64_t cli_start_ms;
static uint64_t cli_total;
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
    fprintf(stderr, "\r  [");
    for (int i = 0; i < bar_w; i++)
        fputc(i < filled ? '=' : (i == filled ? '>' : ' '), stderr);
    fprintf(stderr, "] %3d%%  ", pct);

    const char *units[] = {"B","KB","MB","GB"};
    int ui = 0; double ds = done;   while (ds >= 1024 && ui < 3) { ds /= 1024; ui++; }
    int uj = 0; double ts = total;  while (ts >= 1024 && uj < 3) { ts /= 1024; uj++; }
    fprintf(stderr, "%.1f%s / %.1f%s", ds, units[ui], ts, units[uj]);

    uint64_t elapsed = now_ms() - cli_start_ms;
    if (elapsed > 0) {
        double speed = (double)done / ((double)elapsed / 1000.0);
        int sk = 0; while (speed >= 1024 && sk < 3) { speed /= 1024; sk++; }
        fprintf(stderr, "  %.1f%s/s", speed, units[sk]);
    }
    fflush(stderr);
}

static void cli_error(const char *msg)
{
    fprintf(stderr, "\nError: %s\n", msg);
    cli_ret = 1;
}

static void cli_done(void)
{
    uint64_t elapsed = now_ms() - cli_start_ms;
    fprintf(stderr, "\n\nTransfer complete!\n");
    fprintf(stderr, "  Size:     ");
    const char *units[] = {"B","KB","MB","GB"};
    int u = 0; double s = cli_total;
    while (s >= 1024 && u < 3) { s /= 1024; u++; }
    fprintf(stderr, "%.1f %s\n", s, units[u]);
    fprintf(stderr, "  Duration: %.1fs\n", elapsed / 1000.0);
    if (elapsed > 0 && cli_total > 0) {
        double speed = (double)cli_total / ((double)elapsed / 1000.0);
        int sk = 0;
        while (speed >= 1024 && sk < 3) { speed /= 1024; sk++; }
        fprintf(stderr, "  Speed:    %.1f %s/s\n", speed, units[sk]);
    }
    cli_ret = 0;
}

/* ── Help ──────────────────────────────────────────────────── */

static void print_help(const char *prog)
{
    printf("Usage: %s [OPTIONS] PATH\n\n", prog);
    printf("CLI mode — LAN file transfer tool\n\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help\n");
    printf("  -v, --version         Show version\n");
    printf("  -S                    Shortcut for --mode=S (send)\n");
    printf("  -R                    Shortcut for --mode=R (receive)\n");
    printf("  --mode=S|R            Transfer mode (required)\n");
    printf("  --protocol=TCP|UDP    Protocol (default: TCP)\n");
    printf("  -p, --port=NUM        Port number (default: 9876)\n");
    printf("  --address=IP          Target IP (send: required; recv: default 0.0.0.0)\n");
    printf("  --history             Show transfer history\n\n");
    printf("Examples:\n");
    printf("  Send:   %s --mode=S --address=192.168.1.100 ./file.pdf\n", prog);
    printf("  Send:   %s -S -p 1234 ./video.mp4\n", prog);
    printf("  Recv:   %s --mode=R ./downloads/\n", prog);
    printf("  Recv:   %s --mode=R --address=10.0.0.5 -p 5555 ./received/\n", prog);
}

/* ── Main ──────────────────────────────────────────────────── */

int cli_main(int argc, char **argv)
{
    struct cli_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.protocol = FT_PROTO_TCP;
    cfg.port = FT_DEFAULT_PORT;
    strncpy(cfg.address, "0.0.0.0", sizeof(cfg.address) - 1);
    cfg.mode = -1;

    static struct option long_opts[] = {
        {"help",     no_argument,       0, 'h'},
        {"version",  no_argument,       0, 'v'},
        {"protocol", required_argument, 0, 1000},
        {"mode",     required_argument, 0, 1001},
        {"port",     required_argument, 0, 'p'},
        {"address",  required_argument, 0, 1002},
        {"history",  no_argument,       0, 1003},
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
            cfg.show_history = true;
            break;
        default:
            print_help(argv[0]);
            return 1;
        }
    }

    /* ── History ─────────────────────────────────────────── */
    if (cfg.show_history) {
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

    /* ── Validation ──────────────────────────────────────── */
    if (cfg.mode < 0) {
        fprintf(stderr, "Error: --mode is required (S=send, R=receive)\n\n");
        print_help(argv[0]);
        return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "Error: missing PATH argument\n\n");
        print_help(argv[0]);
        return 1;
    }
    strncpy(cfg.path, argv[optind], sizeof(cfg.path) - 1);

    struct stat st;
    if (cfg.mode == 0) {
        if (stat(cfg.path, &st) != 0) {
            fprintf(stderr, "Error: file/directory not found: %s\n", cfg.path);
            return 1;
        }
    } else {
        if (stat(cfg.path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Error: save directory not found: %s\n", cfg.path);
            return 1;
        }
    }

    /* ── Set CLI callbacks ────────────────────────────────── */
    transfer_set_callbacks(cli_progress, cli_error, cli_done);

    /* ── Execute ──────────────────────────────────────────── */
    struct net_context *nc = net_create(cfg.protocol);
    if (!nc) {
        fprintf(stderr, "Error: failed to create network context\n");
        return 1;
    }

    cli_start_ms = now_ms();
    cli_ret = 1;

    if (cfg.mode == 0) {
        /* Send — client connects to receiver */
        fprintf(stderr, "Connecting to %s:%d...\n", cfg.address, cfg.port);
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
            fprintf(stderr, "\r  Retrying... (%d/60)", tries);
            fflush(stderr);
            sleep(1);
        }
        if (tries >= 60) {
            fprintf(stderr, "\nError: failed to connect to %s:%d\n", cfg.address, cfg.port);
            net_destroy(nc);
            return 1;
        }
        fprintf(stderr, "\nConnected! Sending %s...\n", cfg.path);
        transfer_send(nc, cfg.path, cfg.protocol);
    } else {
        /* Receive — server listens */
        fprintf(stderr, "Listening on %s:%d...\n", cfg.address, cfg.port);
        if (cfg.protocol == FT_PROTO_TCP) {
            if (net_listen_ip(nc, cfg.address, cfg.port) != 0) {
                fprintf(stderr, "Error: failed to listen on %s:%d\n", cfg.address, cfg.port);
                net_destroy(nc);
                return 1;
            }
        } else {
            if (net_udp_bind(nc, cfg.port) != 0) {
                fprintf(stderr, "Error: failed to bind UDP port %d\n", cfg.port);
                net_destroy(nc);
                return 1;
            }
        }
        fprintf(stderr, "Waiting for sender...\n");
        transfer_recv(nc, cfg.path, cfg.protocol);
    }

    net_destroy(nc);
    return cli_ret;
}
