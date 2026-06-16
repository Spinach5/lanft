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
#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#endif

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

    /* Write progress bar directly to stderr — \r keeps it on one line.
       Must not go through log_write() which would add timestamps. */
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
    printf("  --scan                Show your IPs and scan LAN for devices\n");
    printf("  --save-dir=DIR        Save directory (default: ~/Downloads)\n");
    printf("  --buffer-size=N       Transfer buffer size (default: 65536)\n");
    printf("  --timeout=N           Timeout in seconds (default: 30)\n");
    printf("  --overwrite=POLICY    rename | overwrite | skip (default: rename)\n");
    printf("  --no-progress         Disable progress bar\n");
    printf("  --no-discovery        Disable LAN discovery\n");
    printf("  --log-level=LEVEL     debug | info | warn | error (default: info)\n");
    printf("  --log-file=PATH       Log file path (dir/ for date-based naming)\n");
    printf("  --max-connections=N   Max transfers before stopping (0=unlimited)\n");
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
        {"max-connections", required_argument, 0, 2015},
        {"bandwidth-limit",required_argument, 0, 2009},
        {"auto-accept",    no_argument,       0, 2010},
        {"no-auto-accept", no_argument,       0, 2011},
        {"scan",           no_argument,       0, 2014},
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
        case 2015: /* --max-connections */
            cfg.max_connections = atoi(optarg);
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
        case 2014: /* --scan */
            cfg.show_progress = false; /* signal: do scan and exit */
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

    /* ── Scan mode ─────────────────────────────────────────── */
    {
        bool do_scan = false;
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], "--scan") == 0) { do_scan = true; break; }
        if (do_scan) {
            /* Show local IPs */
            char local_ips[8][64];
            int n = scanner_get_local_ips(local_ips, 8);
            printf("Your IPs:\n");
            for (int i = 0; i < n; i++)
                printf("  %s\n", local_ips[i]);
            if (n == 0) printf("  (none found)\n");
            printf("\nScanning LAN on port %d...\n", cfg.port);

            /* Get subnets */
            int subnet_count = 0;
            char subnets[16][64];
            {
                /* Quick subnet collection — same logic as scanner.c */
#ifdef _WIN32
                ULONG bufLen = 15000;
                IP_ADAPTER_ADDRESSES *adapters = malloc(bufLen);
                if (adapters && GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX,
                    NULL, adapters, &bufLen) == 0) {
                    for (IP_ADAPTER_ADDRESSES *a = adapters; a && subnet_count < 16; a = a->Next) {
                        if (a->OperStatus != IfOperStatusUp) continue;
                        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
                            uint32_t ip = ntohl(sin->sin_addr.s_addr);
                            if ((ip & 0xFF000000) == 0x7F000000) continue;
                            char s[64];
                            snprintf(s, sizeof(s), "%d.%d.%d",
                                     (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF);
                            bool dup = false;
                            for (int j = 0; j < subnet_count; j++)
                                if (strcmp(subnets[j], s) == 0) { dup = true; break; }
                            if (!dup) { strncpy(subnets[subnet_count], s, 63); subnet_count++; }
                        }
                    }
                    free(adapters);
                }
#else
                struct ifaddrs *ifaddr, *ifa;
                if (getifaddrs(&ifaddr) == 0) {
                    for (ifa = ifaddr; ifa && subnet_count < 16; ifa = ifa->ifa_next) {
                        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
                        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
                        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
                        uint32_t ip = ntohl(addr->sin_addr.s_addr);
                        if ((ip & 0xFF000000) == 0x7F000000) continue;
                        char s[64];
                        snprintf(s, sizeof(s), "%d.%d.%d",
                                 (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF);
                        bool dup = false;
                        for (int j = 0; j < subnet_count; j++)
                            if (strcmp(subnets[j], s) == 0) { dup = true; break; }
                        if (!dup) { strncpy(subnets[subnet_count], s, 63); subnet_count++; }
                    }
                    freeifaddrs(ifaddr);
                }
#endif
            }

            /* UDP broadcast probe */
            for (int s = 0; s < subnet_count; s++) {
                socket_t udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (udp_fd == INVALID_FD) continue;
                int broadcast = 1, reuse = 1;
                setsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST,
                           (const char *)&broadcast, sizeof(broadcast));
                setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR,
                           (const char *)&reuse, sizeof(reuse));
                struct timeval tv = {0, 500000};
                setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO,
                           (const char *)&tv, sizeof(tv));

                /* Bind to any port to receive responses */
                struct sockaddr_in baddr;
                memset(&baddr, 0, sizeof(baddr));
                baddr.sin_family = AF_INET;
                baddr.sin_addr.s_addr = INADDR_ANY;
                baddr.sin_port = 0;
                bind(udp_fd, (struct sockaddr *)&baddr, sizeof(baddr));

                /* Send probe to broadcast */
                char bcast_ip[64];
                snprintf(bcast_ip, sizeof(bcast_ip), "%s.255", subnets[s]);
                memset(&baddr, 0, sizeof(baddr));
                baddr.sin_family = AF_INET;
                baddr.sin_port = htons(cfg.port);
                inet_pton(AF_INET, bcast_ip, &baddr.sin_addr);

                uint32_t magic = FT_MAGIC;
                sendto(udp_fd, (const char *)&magic, 4, 0,
                       (struct sockaddr *)&baddr, sizeof(baddr));

                /* Collect responses */
                for (int r = 0; r < 4; r++) {
                    uint8_t buf[260];
                    struct sockaddr_in src;
                    socklen_t srclen = sizeof(src);
                    ssize_t nr = recvfrom(udp_fd, (char *)buf, sizeof(buf), 0,
                                         (struct sockaddr *)&src, &srclen);
                    if (nr < 4) continue;
                    uint32_t rmagic;
                    memcpy(&rmagic, buf, 4);
                    if (rmagic != FT_MAGIC) continue;
                    char ip[64];
                    inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
                    char host[256] = "";
                    if (nr >= 260) strncpy(host, (char *)(buf + 4), 255);
                    printf("  %-18s %s\n", ip, host);
                }
                close_sock(udp_fd);
            }
            printf("\nScan complete.\n");
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
    transfer_set_bandwidth_limit(cfg.send_bandwidth_limit);
    transfer_set_max_connections(cfg.max_connections);

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

    /* Receive — persistent listener, stops after max_connections if set */
    log_write("Persistent listener on %s:%d (Ctrl+C to stop)\n\n",
            cfg.address, cfg.port);
    int transfer_count = 0;
    while (1) {
        if (cfg.max_connections > 0 && transfer_count >= cfg.max_connections) {
            log_write("Reached max_connections limit (%d), stopping.\n",
                      cfg.max_connections);
            break;
        }
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
