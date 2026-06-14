#include "scanner.h"
#include "protocol.h"
#include "compat.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdbool.h>

#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#endif

#ifdef BUILD_GUI
#include <SDL2/SDL.h>
#endif

#define SCANNER_THREADS 32
#define MAX_SUBNETS     16

static const char *reverse_dns(const char *ip)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr.sin_addr);

    static char host[256];
    int ret = getnameinfo((const struct sockaddr *)&addr, sizeof(addr),
                          host, sizeof(host), NULL, 0, 0);
    if (ret == 0) return host;
    return "";
}

/* Collect ALL non-loopback /24 subnets from local interfaces */
static int get_all_subnets(char subnets[][64], int max)
{
#ifdef _WIN32
    /* Windows: use GetAdaptersAddresses */
    ULONG bufLen = 15000;
    IP_ADAPTER_ADDRESSES *adapters = malloc(bufLen);
    if (!adapters) return -1;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, adapters, &bufLen) != 0) {
        free(adapters); return -1;
    }
    int count = 0;
    for (IP_ADAPTER_ADDRESSES *a = adapters; a && count < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
            uint32_t ip = ntohl(sin->sin_addr.s_addr);
            /* Skip loopback */
            if ((ip & 0xFF000000) == 0x7F000000) continue;
            char subnet[64];
            snprintf(subnet, sizeof(subnet), "%d.%d.%d", (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF);
            bool dup = false;
            for (int i = 0; i < count; i++) if (strcmp(subnets[i], subnet) == 0) { dup = true; break; }
            if (!dup) { strncpy(subnets[count], subnet, 63); count++; }
        }
    }
    free(adapters);
    return count;
#else
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return -1;

    int count = 0;
    for (ifa = ifaddr; ifa != NULL && count < max; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;

        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        uint32_t ip = ntohl(addr->sin_addr.s_addr);
        char subnet[64];
        snprintf(subnet, sizeof(subnet), "%d.%d.%d",
                 (ip >> 24) & 0xFF,
                 (ip >> 16) & 0xFF,
                 (ip >> 8) & 0xFF);

        /* Skip duplicates */
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(subnets[i], subnet) == 0) { dup = true; break; }
        }
        if (!dup) {
            strncpy(subnets[count], subnet, 63);
            count++;
        }
    }
    freeifaddrs(ifaddr);
    return count;
#endif
}

typedef struct {
    atomic_uint *counter;
    int port;
    char subnet[64];
} scan_thread_args;

static void *scan_ip_thread(void *arg)
{
    scan_thread_args *args = (scan_thread_args *)arg;
    unsigned int idx;
    char ip[128];

    while (1) {
        idx = atomic_fetch_add(args->counter, 1);
        if (idx >= 1 && idx <= 254) {
            snprintf(ip, sizeof(ip), "%s.%u", args->subnet, idx);
        } else {
            break;
        }

        socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == INVALID_FD) continue;

        sock_set_nonblock(fd);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(args->port);
        inet_pton(AF_INET, ip, &addr.sin_addr);

        connect(fd, (struct sockaddr *)&addr, sizeof(addr));

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 800000;  /* 800ms — enough for high-latency VPN */

        if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
            sock_getopt_int(fd, SOL_SOCKET, SO_ERROR, &err);
            if (err == 0) {
                struct event_scan_found *evt = calloc(1, sizeof(*evt));
                strncpy(evt->ip, ip, sizeof(evt->ip) - 1);
                const char *host = reverse_dns(ip);
                strncpy(evt->hostname, host, sizeof(evt->hostname) - 1);

                SDL_Event event;
                SDL_memset(&event, 0, sizeof(event));
                event.type = SDL_USEREVENT + 1;
                event.user.data1 = evt;
                SDL_PushEvent(&event);
            }
        }
        close_sock(fd);
    }
    return NULL;
}

/* Scan one /24 subnet */
static void scan_subnet(const char *subnet, uint16_t port)
{
    atomic_uint counter;
    atomic_init(&counter, 1);

    scan_thread_args args;
    args.counter = &counter;
    args.port = port;
    strncpy(args.subnet, subnet, sizeof(args.subnet) - 1);

    pthread_t threads[SCANNER_THREADS];
    for (int i = 0; i < SCANNER_THREADS; i++) {
        pthread_create(&threads[i], NULL, scan_ip_thread, &args);
    }
    for (int i = 0; i < SCANNER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
}

/* The real scan logic — runs in a worker thread, scans ALL subnets */
static void scanner_run(uint16_t port)
{
    char subnets[MAX_SUBNETS][64];
    int subnet_count = get_all_subnets(subnets, MAX_SUBNETS);

    if (subnet_count <= 0) {
        struct event_error *err = calloc(1, sizeof(*err));
        snprintf(err->message, sizeof(err->message),
                 "Cannot determine local subnets");
        SDL_Event event;
        SDL_memset(&event, 0, sizeof(event));
        event.type = SDL_USEREVENT + 5;
        event.user.data1 = err;
        SDL_PushEvent(&event);
        return;
    }

    /* Scan each subnet */
    for (int s = 0; s < subnet_count; s++) {
        log_write("[SCAN] scanning subnet %s.x (port %d)...\n", subnets[s], port);
        scan_subnet(subnets[s], port);
    }

    /* All done */
    struct event_scan_done *done = calloc(1, sizeof(*done));
    done->total_found = -1;

    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = SDL_USEREVENT + 2;
    event.user.data1 = done;
    SDL_PushEvent(&event);
}

/* Entry point for the scanner thread */
static void *scanner_thread_entry(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    scanner_run(port);
    return NULL;
}

/* Non-blocking: spawns scanner in its own thread */
void scanner_start(uint16_t port)
{
    pthread_t tid;
    pthread_create(&tid, NULL, scanner_thread_entry, (void *)(uintptr_t)port);
    pthread_detach(tid);
}
