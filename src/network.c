#include "network.h"
#include "protocol.h"

#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

struct net_context {
    int mode;                    /* FT_PROTO_TCP or FT_PROTO_UDP */
    struct lws_context *ctx;
    struct lws_vhost *vhost;
    struct lws *wsi;

    /* TCP state */
    int listen_fd;
    int sock_fd;
    bool connected;
    bool closed;
    int error_code;

    /* RX callback and user data */
    net_rx_fn rx_cb;
    void     *rx_user;
    net_close_fn close_cb;
    void        *close_user;

    /* TX pending data (for lws WRITEABLE callback) */
    uint8_t *tx_buf;            /* heap copy of data to send */
    const uint8_t *tx_data;
    size_t tx_len;
    size_t tx_sent;
    bool tx_pending;

    /* UDP */
    int udp_fd;
    struct sockaddr_in udp_peer;
    bool udp_bound;

    /* Cancel flag */
    volatile bool cancelled;
};

/* ── lws raw protocol callback ─────────────────────────────── */

struct raw_session {
    struct net_context *nc;
};

static int raw_callback(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len)
{
    struct raw_session *sess = (struct raw_session *)user;
    struct net_context *nc;

    switch (reason) {
    case LWS_CALLBACK_PROTOCOL_INIT:
        break;

    case LWS_CALLBACK_RAW_ADOPT:
        sess->nc = (struct net_context *)lws_context_user(lws_get_context(wsi));
        break;

    case LWS_CALLBACK_RAW_RX:
        nc = sess->nc;
        if (nc && nc->rx_cb) {
            nc->rx_cb(nc->rx_user, in, len);
        }
        break;

    case LWS_CALLBACK_RAW_WRITEABLE:
        nc = sess->nc;
        if (nc && nc->tx_pending && nc->sock_fd >= 0) {
            size_t remaining = nc->tx_len - nc->tx_sent;
            ssize_t n = write(nc->sock_fd,
                              nc->tx_data + nc->tx_sent,
                              remaining);
            if (n > 0) {
                nc->tx_sent += n;
                if (nc->tx_sent >= nc->tx_len) {
                    nc->tx_pending = false;
                    free(nc->tx_buf);
                    nc->tx_buf = NULL;
                    nc->tx_data = NULL;
                    nc->tx_len = 0;
                    nc->tx_sent = 0;
                } else {
                    lws_callback_on_writable(wsi);
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                nc->error_code = errno;
                nc->closed = true;
            }
        }
        break;

    case LWS_CALLBACK_RAW_CLOSE:
        nc = sess->nc;
        if (nc) {
            nc->connected = false;
            nc->closed = true;
            if (nc->close_cb) {
                nc->close_cb(nc->close_user);
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "raw", raw_callback, sizeof(struct raw_session), 65536, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

/* ── Public API ────────────────────────────────────────────── */

struct net_context *net_create(int mode)
{
    struct net_context *nc = calloc(1, sizeof(*nc));
    if (!nc) return NULL;
    nc->mode = mode;
    nc->listen_fd = -1;
    nc->sock_fd = -1;
    nc->udp_fd = -1;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
    info.protocols = protocols;
    info.user = nc;

    nc->ctx = lws_create_context(&info);
    if (!nc->ctx) {
        free(nc);
        return NULL;
    }

    struct lws_context_creation_info vinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    vinfo.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
    vinfo.port = CONTEXT_PORT_NO_LISTEN;
    vinfo.protocols = protocols;
    nc->vhost = lws_create_vhost(nc->ctx, &vinfo);
    if (!nc->vhost) {
        lws_context_destroy(nc->ctx);
        free(nc);
        return NULL;
    }

    return nc;
}

/* ── TCP helpers ───────────────────────────────────────────── */

int net_listen(struct net_context *nc, int port)
{
    return net_listen_ip(nc, "0.0.0.0", port);
}

int net_listen_ip(struct net_context *nc, const char *ip, int port)
{
    nc->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (nc->listen_fd < 0) return -1;

    int opt = 1;
    setsockopt(nc->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(nc->listen_fd);
        nc->listen_fd = -1;
        return -1;
    }

    if (bind(nc->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(nc->listen_fd);
        nc->listen_fd = -1;
        return -1;
    }
    if (listen(nc->listen_fd, 1) < 0) {
        close(nc->listen_fd);
        nc->listen_fd = -1;
        return -1;
    }
    return 0;
}

int net_accept(struct net_context *nc)
{
    while (!nc->cancelled) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(nc->listen_fd, &rfds);
        struct timeval tv = {0, 500000};
        int ret = select(nc->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) return -1;
        if (ret == 0) continue;

        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        nc->sock_fd = accept(nc->listen_fd, (struct sockaddr *)&client, &len);
        if (nc->sock_fd < 0) return -1;

        lws_sock_file_fd_type fd;
        fd.sockfd = nc->sock_fd;
        nc->wsi = lws_adopt_descriptor_vhost(nc->vhost, LWS_ADOPT_RAW_FILE_DESC,
                                              fd, "raw", NULL);
        if (!nc->wsi) {
            close(nc->sock_fd);
            nc->sock_fd = -1;
            return -1;
        }
        nc->connected = true;
        return 0;
    }
    return -2;
}

int net_connect(struct net_context *nc, const char *ip, int port)
{
    nc->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (nc->sock_fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(nc->sock_fd);
        nc->sock_fd = -1;
        return -1;
    }

    if (connect(nc->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(nc->sock_fd);
        nc->sock_fd = -1;
        return -1;
    }

    lws_sock_file_fd_type fd;
    fd.sockfd = nc->sock_fd;
    nc->wsi = lws_adopt_descriptor_vhost(nc->vhost, LWS_ADOPT_RAW_FILE_DESC,
                                          fd, "raw", NULL);
    if (!nc->wsi) {
        close(nc->sock_fd);
        nc->sock_fd = -1;
        return -1;
    }
    nc->connected = true;
    return 0;
}

/* ── UDP helpers ───────────────────────────────────────────── */

int net_udp_bind(struct net_context *nc, int port)
{
    nc->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (nc->udp_fd < 0) return -1;

    int opt = 1;
    setsockopt(nc->udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(nc->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(nc->udp_fd);
        nc->udp_fd = -1;
        return -1;
    }
    nc->udp_bound = true;
    return 0;
}

int net_udp_set_peer(struct net_context *nc, const char *ip, int port)
{
    memset(&nc->udp_peer, 0, sizeof(nc->udp_peer));
    nc->udp_peer.sin_family = AF_INET;
    nc->udp_peer.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &nc->udp_peer.sin_addr) != 1) {
        return -1;
    }
    return 0;
}

/* ── Data I/O ──────────────────────────────────────────────── */

void net_set_rx_cb(struct net_context *nc, net_rx_fn rx, void *user)
{
    nc->rx_cb = rx;
    nc->rx_user = user;
}

void net_set_close_cb(struct net_context *nc, net_close_fn cb, void *user)
{
    nc->close_cb = cb;
    nc->close_user = user;
}

int net_send(struct net_context *nc, const void *data, size_t len)
{
    if (nc->mode == FT_PROTO_UDP) {
        if (!nc->udp_bound) return -1;
        ssize_t n = sendto(nc->udp_fd, data, len, 0,
                           (const struct sockaddr *)&nc->udp_peer,
                           sizeof(nc->udp_peer));
        return (n == (ssize_t)len) ? 0 : -1;
    }

    /* Copy data to heap — caller may use stack buffer that goes
       out of scope before the async writable callback fires */
    free(nc->tx_buf);
    nc->tx_buf = malloc(len);
    if (!nc->tx_buf) return -1;
    memcpy(nc->tx_buf, data, len);
    nc->tx_data = nc->tx_buf;
    nc->tx_len = len;
    nc->tx_sent = 0;
    nc->tx_pending = true;
    lws_callback_on_writable(nc->wsi);
    return 0;
}

int net_udp_recv(struct net_context *nc, void *buf, size_t len, int timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(nc->udp_fd, &fds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(nc->udp_fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;

    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    return recvfrom(nc->udp_fd, buf, len, 0,
                    (struct sockaddr *)&src, &srclen);
}

/* ── Event loop ────────────────────────────────────────────── */

int net_service(struct net_context *nc, int timeout_ms)
{
    if (nc->mode == FT_PROTO_UDP) return 0;
    return lws_service(nc->ctx, timeout_ms);
}

/* ── Status ────────────────────────────────────────────────── */

bool net_is_connected(struct net_context *nc)
{
    return nc->connected && !nc->closed;
}

bool net_has_error(struct net_context *nc)
{
    return nc->error_code != 0;
}

bool net_tx_pending(struct net_context *nc)
{
    return nc->tx_pending;
}

void *net_get_wsi(struct net_context *nc)
{
    return nc->wsi;
}

int net_get_fd(struct net_context *nc)
{
    return nc->sock_fd;
}

void net_cancel(struct net_context *nc)
{
    if (nc) nc->cancelled = true;
}

bool net_is_cancelled(struct net_context *nc)
{
    return nc ? nc->cancelled : false;
}

/* ── Cleanup ───────────────────────────────────────────────── */

void net_destroy(struct net_context *nc)
{
    if (!nc) return;
    free(nc->tx_buf);
    if (nc->listen_fd >= 0) close(nc->listen_fd);
    if (nc->sock_fd >= 0) close(nc->sock_fd);
    if (nc->udp_fd >= 0) close(nc->udp_fd);
    if (nc->ctx) lws_context_destroy(nc->ctx);
    free(nc);
}
