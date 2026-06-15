/* UDP Discovery Responder — runs a background thread that responds to
   broadcast probes, so idle instances (just opened, not yet listening
   for transfers) are still findable by the scanner. */

#include "discovery.h"
#include "protocol.h"
#include "compat.h"
#include "log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#endif

static volatile bool g_disc_running = false;
static socket_t g_disc_fd = INVALID_FD;

static char *discovery_hostname(void)
{
    static char host[256] = "";
    if (host[0]) return host;
    if (gethostname(host, sizeof(host)) != 0)
        strncpy(host, "unknown", sizeof(host) - 1);
    return host;
}

static void *discovery_thread(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;

    g_disc_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_disc_fd == INVALID_FD) return NULL;

    int reuse = 1;
    setsockopt(g_disc_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_disc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_sock(g_disc_fd);
        g_disc_fd = INVALID_FD;
        return NULL;
    }

    struct timeval tv = {1, 0};
    setsockopt(g_disc_fd, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&tv, sizeof(tv));

    g_disc_running = true;

    while (g_disc_running) {
        uint8_t buf[512];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(g_disc_fd, (char *)buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &srclen);
        if (n < 4) continue;

        uint32_t magic;
        memcpy(&magic, buf, 4);
        if (magic != FT_MAGIC) continue;

        /* Send response: magic + hostname (260 bytes) */
        char *host = discovery_hostname();
        uint8_t resp[260];
        memcpy(resp, &magic, 4);
        memset(resp + 4, 0, 256);
        strncpy((char *)(resp + 4), host, 255);

        sendto(g_disc_fd, (const char *)resp, sizeof(resp), 0,
               (struct sockaddr *)&src, srclen);
    }

    close_sock(g_disc_fd);
    g_disc_fd = INVALID_FD;
    return NULL;
}

void discovery_start(uint16_t port)
{
    if (g_disc_running) return;
    pthread_t tid;
    pthread_create(&tid, NULL, discovery_thread,
                   (void *)(uintptr_t)port);
    pthread_detach(tid);
}

void discovery_stop(void)
{
    g_disc_running = false;
    if (g_disc_fd != INVALID_FD) {
        close_sock(g_disc_fd);
        g_disc_fd = INVALID_FD;
    }
}

/* Collect local non-loopback IPv4 addresses */
int scanner_get_local_ips(char ips[][64], int max_count)
{
    int count = 0;
#ifdef _WIN32
    ULONG bufLen = 15000;
    IP_ADAPTER_ADDRESSES *adapters = malloc(bufLen);
    if (!adapters) return 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, adapters, &bufLen) != 0)
        { free(adapters); return 0; }
    for (IP_ADAPTER_ADDRESSES *a = adapters; a && count < max_count; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
            uint32_t ip = ntohl(sin->sin_addr.s_addr);
            if ((ip & 0xFF000000) == 0x7F000000) continue;
            inet_ntop(AF_INET, &sin->sin_addr, ips[count], 63);
            count++;
            if (count >= max_count) break;
        }
    }
    free(adapters);
#else
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return 0;
    for (ifa = ifaddr; ifa && count < max_count; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        uint32_t ip = ntohl(addr->sin_addr.s_addr);
        if ((ip & 0xFF000000) == 0x7F000000) continue;
        char ip_str[64];
        inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
        if (strcmp(ip_str, "0.0.0.0") == 0) continue;
        bool dup = false;
        for (int i = 0; i < count; i++)
            if (strcmp(ips[i], ip_str) == 0) { dup = true; break; }
        if (!dup) { strncpy(ips[count], ip_str, 63); count++; }
    }
    freeifaddrs(ifaddr);
#endif
    return count;
}
