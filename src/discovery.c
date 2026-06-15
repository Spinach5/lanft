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
        ssize_t n = recvfrom(g_disc_fd, buf, sizeof(buf), 0,
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

        sendto(g_disc_fd, resp, sizeof(resp), 0,
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
