#include "network.h"
#include "protocol.h"
#include "compat.h"

#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/**
 * @brief 网络上下文结构体，保存 TCP/UDP 连接的状态。
 *
 * 该结构管理网络连接的整个生命周期，包括套接字描述符、
 * libwebsockets 上下文/虚拟主机/websocket 实例、接收数据
 * 和关闭连接的回调函数、以及异步发送缓冲区。
 */
struct net_context {
    int mode;                    /* FT_PROTO_TCP 或 FT_PROTO_UDP */
    struct lws_context *ctx;    /* libwebsockets 上下文 */
    struct lws_vhost *vhost;    /* libwebsockets 虚拟主机 */
    struct lws *wsi;            /* libwebsockets 连接实例（原始套接字） */

    /* TCP 状态 */
    socket_t listen_fd;         /* 监听套接字（服务器端） */
    socket_t sock_fd;           /* 已连接的数据套接字 */
    bool connected;             /* 是否已建立连接 */
    bool closed;                /* 连接是否已关闭 */
    int error_code;             /* 保存发生的错误码（errno） */

    /* 接收回调函数及用户数据 */
    net_rx_fn rx_cb;            /* 数据接收回调函数指针 */
    void     *rx_user;          /* 传给接收回调的用户数据 */
    net_close_fn close_cb;      /* 连接关闭回调函数指针 */
    void        *close_user;    /* 传给关闭回调的用户数据 */

    /* 待发送数据（用于 lws 的可写回调） */
    uint8_t *tx_buf;            /* 堆上复制的待发送数据副本 */
    const uint8_t *tx_data;     /* 指向当前尚未发送的数据位置 */
    size_t tx_len;              /* 待发送数据总长度 */
    size_t tx_sent;             /* 已发送的字节数 */
    bool tx_pending;            /* 是否有数据等待发送 */

    /* UDP 相关 */
    socket_t udp_fd;            /* UDP 套接字 */
    struct sockaddr_in udp_peer; /* UDP 对端地址（用于 sendto） */
    bool udp_bound;             /* UDP 是否已绑定本地端口 */

    /* 取消标志，用于中断 accept 循环等阻塞操作 */
    volatile bool cancelled;
};

/* ── lws 原始协议回调 ─────────────────────────────────────────────── */

/**
 * @brief 与每个原始 websocket 实例关联的会话结构体。
 */
struct raw_session {
    struct net_context *nc;     /* 指向所属的网络上下文 */
};

/**
 * @brief libwebsockets 的“raw”协议回调处理函数。
 *
 * 该函数处理 libwebsockets 管理的原始套接字连接的各种生命周期事件，
 * 包括套接字接管、数据接收、可写通知和连接关闭。它在 libwebsockets
 * 事件和应用层 net_context 回调之间搭建桥梁。
 *
 * @param wsi      libwebsockets 连接实例。
 * @param reason   回调原因（事件类型）。
 * @param user     与此 wsi 关联的 raw_session 结构指针。
 * @param in       输入数据缓冲区（用于接收数据）。
 * @param len      输入数据的长度。
 * @return int     成功时返回 0。
 */
static int raw_callback(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len)
{
    struct raw_session *sess = (struct raw_session *)user;
    struct net_context *nc;

    switch (reason) {
    case LWS_CALLBACK_PROTOCOL_INIT:
        /* 协议初始化，无需处理 */
        break;

    case LWS_CALLBACK_RAW_ADOPT:
        /* 原始套接字被 libwebsockets 接管时发生 */
        /* 从 lws 上下文中获取 net_context 指针并保存到会话中 */
        sess->nc = (struct net_context *)lws_context_user(lws_get_context(wsi));
        break;

    case LWS_CALLBACK_RAW_RX:
        /* 有数据到达原始套接字 */
        nc = sess->nc;
        if (nc && nc->rx_cb) {
            /* 调用用户注册的接收回调，传递数据缓冲区和长度 */
            nc->rx_cb(nc->rx_user, in, len);
        }
        break;

    case LWS_CALLBACK_RAW_WRITEABLE:
        /* 套接字变为可写状态（由 lws_callback_on_writable 触发） */
        nc = sess->nc;
        if (nc && nc->tx_pending && nc->sock_fd != INVALID_FD) {
            size_t remaining = nc->tx_len - nc->tx_sent;
            /* 尝试发送剩余数据（非阻塞写） */
            ssize_t n = sock_write(nc->sock_fd,
                                  nc->tx_data + nc->tx_sent,
                                  remaining);
            if (n > 0) {
                nc->tx_sent += n;
                if (nc->tx_sent >= nc->tx_len) {
                    /* 所有数据已发送完毕，清理发送缓冲区 */
                    nc->tx_pending = false;
                    free(nc->tx_buf);
                    nc->tx_buf = NULL;
                    nc->tx_data = NULL;
                    nc->tx_len = 0;
                    nc->tx_sent = 0;
                } else {
                    /* 部分发送，继续等待下次可写事件 */
                    lws_callback_on_writable(wsi);
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                /* 发送出错（非阻塞暂时不可写的情况除外），记录错误 */
                nc->error_code = errno;
                nc->closed = true;
            }
        }
        break;

    case LWS_CALLBACK_RAW_CLOSE:
        /* 原始套接字连接关闭 */
        nc = sess->nc;
        if (nc) {
            nc->connected = false;
            nc->closed = true;
            if (nc->close_cb) {
                /* 调用用户注册的连接关闭回调 */
                nc->close_cb(nc->close_user);
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

/* 定义 libwebsockets 协议列表，只有一个“raw”协议 */
static const struct lws_protocols protocols[] = {
    { "raw", raw_callback, sizeof(struct raw_session), 65536, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }  /* 终止标志 */
};

/* ── 公共 API ────────────────────────────────────────────────────── */

/**
 * @brief 创建一个新的网络上下文。
 *
 * 初始化套接字库（Windows 下需要 WSAStartup），创建 libwebsockets
 * 上下文和虚拟主机，并分配 net_context 结构体内存。
 *
 * @param mode 协议模式（FT_PROTO_TCP 或 FT_PROTO_UDP）。
 * @return struct net_context* 新创建的上下文指针，失败返回 NULL。
 */
struct net_context *net_create(int mode)
{
    struct net_context *nc = calloc(1, sizeof(*nc));// 分配内存
    if (!nc) return NULL; // 分配失败直接返回 NULL
    //初始化套接字库
    nc->mode = mode;
    nc->listen_fd = INVALID_FD;
    nc->sock_fd = INVALID_FD;
    nc->udp_fd = INVALID_FD;

    /* 初始化平台相关的套接字库（如 Windows 的 WSAStartup） */
    if (SOCKET_INIT() != 0) { free(nc); return NULL; }

    /* 配置 libwebsockets 上下文创建信息 */
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;  /* 显式创建虚拟主机 */
    info.protocols = protocols;                        /* 注册协议 */
    info.user = nc;                                    /* 将 net_context 作为用户数据 */

    nc->ctx = lws_create_context(&info);
    if (!nc->ctx) {
        free(nc);
        return NULL;
    }

    /* 创建虚拟主机（vhost），不监听任何端口，仅用于原始套接字接管 */
    struct lws_context_creation_info vinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    vinfo.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
    vinfo.port = CONTEXT_PORT_NO_LISTEN;   /* 不监听端口 */
    vinfo.protocols = protocols;
    nc->vhost = lws_create_vhost(nc->ctx, &vinfo);
    if (!nc->vhost) {
        lws_context_destroy(nc->ctx);
        free(nc);
        return NULL;
    }

    return nc;
}

/* ── TCP 辅助函数 ───────────────────────────────────────────────── */

/**
 * @brief 在所有网络接口上开始监听 TCP 连接。
 *
 * 这是 net_listen_ip 的便利包装，绑定到 "0.0.0.0"。
 *
 * @param nc   网络上下文。
 * @param port 监听端口。
 * @return int 成功返回 0，失败返回 -1。
 */
int net_listen(struct net_context *nc, int port)
{
    return net_listen_ip(nc, "0.0.0.0", port);
}

/**
 * @brief 在指定的 IP 地址上开始监听 TCP 连接。
 *
 * 创建 TCP 套接字，设置 SO_REUSEADDR 选项，绑定到指定 IP 和端口，
 * 然后开始监听。
 *
 * @param nc   网络上下文。
 * @param ip   要绑定的 IP 地址字符串。
 * @param port 监听端口。
 * @return int 成功返回 0，失败返回 -1。
 */
int net_listen_ip(struct net_context *nc, const char *ip, int port)
{
    /* 创建 TCP 套接字 */
    nc->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (nc->listen_fd == INVALID_FD) return -1;

    /* 允许重用地址，方便快速重启服务 */
    sock_setopt_int(nc->listen_fd, SOL_SOCKET, SO_REUSEADDR, 1);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    /* 将 IP 字符串转换为二进制地址 */
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close_sock(nc->listen_fd);
        nc->listen_fd = -1;
        return -1;
    }

    /* 绑定地址 */
    if (bind(nc->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_sock(nc->listen_fd);
        nc->listen_fd = -1;
        return -1;
    }
    /* 开始监听，最大待处理连接数为 1（简单场景） */
    if (listen(nc->listen_fd, 1) < 0) {
        close_sock(nc->listen_fd);
        nc->listen_fd = -1;
        return -1;
    }
    return 0;
}

/**
 * @brief 接受一个传入的 TCP 连接。
 *
 * 阻塞直到有连接被接受或上下文被取消。接受连接后，
 * 将套接字作为原始文件描述符交给 libwebsockets 管理。
 *
 * @param nc 网络上下文。
 * @return int 成功返回 0，错误返回 -1，被取消返回 -2。
 */
int net_accept(struct net_context *nc)
{
    /* 循环直到被取消或有连接到达 */
    while (!nc->cancelled) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(nc->listen_fd, &rfds);
        struct timeval tv = {0, 500000};  /* 超时 0.5 秒，以便检查 cancelled 标志 */
        int ret = select(nc->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) return -1;   /* select 错误 */
        if (ret == 0) continue;   /* 超时，继续检查 cancelled */

        /* 接受客户端连接 */
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        nc->sock_fd = accept(nc->listen_fd, (struct sockaddr *)&client, &len);
        if (nc->sock_fd == INVALID_FD) return -1;

        /* 将接受的套接字交给 libwebsockets 作为原始描述符管理 */
        lws_sock_file_fd_type fd;
        fd.sockfd = nc->sock_fd;
        nc->wsi = lws_adopt_descriptor_vhost(nc->vhost, LWS_ADOPT_RAW_FILE_DESC,
                                              fd, "raw", NULL);
        if (!nc->wsi) {
            close_sock(nc->sock_fd);
            nc->sock_fd = -1;
            return -1;
        }
        nc->connected = true;
        return 0;
    }
    return -2;  /* 被取消 */
}

/**
 * @brief 连接到远程 TCP 服务器。
 *
 * 创建 TCP 套接字并连接到指定的 IP 和端口。
 * 成功连接后，将套接字交给 libwebsockets 管理。
 *
 * @param nc   网络上下文。
 * @param ip   远程 IP 地址。
 * @param port 远程端口。
 * @return int 成功返回 0，失败返回 -1。
 */
int net_connect(struct net_context *nc, const char *ip, int port)
{
    /* 创建 TCP 套接字 */
    nc->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (nc->sock_fd == INVALID_FD) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close_sock(nc->sock_fd);
        nc->sock_fd = -1;
        return -1;
    }

    /* 连接服务器（阻塞） */
    if (connect(nc->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_sock(nc->sock_fd);
        nc->sock_fd = -1;
        return -1;
    }

    /* 将已连接套接字交给 libwebsockets 管理 */
    lws_sock_file_fd_type fd;
    fd.sockfd = nc->sock_fd;
    nc->wsi = lws_adopt_descriptor_vhost(nc->vhost, LWS_ADOPT_RAW_FILE_DESC,
                                          fd, "raw", NULL);
    if (!nc->wsi) {
        close_sock(nc->sock_fd);
        nc->sock_fd = -1;
        return -1;
    }
    nc->connected = true;
    return 0;
}

/* ── UDP 辅助函数 ───────────────────────────────────────────────── */

/**
 * @brief 将 UDP 套接字绑定到本地端口。
 *
 * 创建 UDP 套接字并绑定到 INADDR_ANY 上的指定端口。
 *
 * @param nc   网络上下文。
 * @param port 要绑定的本地端口。
 * @return int 成功返回 0，失败返回 -1。
 */
int net_udp_bind(struct net_context *nc, int port)
{
    /* 创建 UDP 套接字 */
    nc->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (nc->udp_fd == INVALID_FD) return -1;

    sock_setopt_int(nc->udp_fd, SOL_SOCKET, SO_REUSEADDR, 1);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  /* 监听所有本地接口 */
    addr.sin_port = htons(port);

    if (bind(nc->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_sock(nc->udp_fd);
        nc->udp_fd = -1;
        return -1;
    }
    nc->udp_bound = true;
    return 0;
}

/**
 * @brief 设置 UDP 发送的目标对端地址。
 *
 * 配置 sockaddr_in 结构，用于后续的 sendto 调用。
 *
 * @param nc   网络上下文。
 * @param ip   目标 IP 地址。
 * @param port 目标端口。
 * @return int 成功返回 0，IP 转换失败返回 -1。
 */
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

/* ── 数据收发 ──────────────────────────────────────────────────── */

/**
 * @brief 设置接收回调函数。
 *
 * 注册一个函数，当连接上收到数据时被调用。
 *
 * @param nc   网络上下文。
 * @param rx   接收回调函数指针。
 * @param user 传递给回调函数的用户数据指针。
 */
void net_set_rx_cb(struct net_context *nc, net_rx_fn rx, void *user)
{
    nc->rx_cb = rx;
    nc->rx_user = user;
}

/**
 * @brief 设置连接关闭回调函数。
 *
 * 注册一个函数，当连接关闭时被调用。
 *
 * @param nc   网络上下文。
 * @param cb   关闭回调函数指针。
 * @param user 传递给回调函数的用户数据指针。
 */
void net_set_close_cb(struct net_context *nc, net_close_fn cb, void *user)
{
    nc->close_cb = cb;
    nc->close_user = user;
}

/**
 * @brief 通过网络连接发送数据。
 *
 * 对于 UDP，立即发送数据到配置的对端。
 * 对于 TCP，将数据复制到堆缓冲区，并通过 libwebsockets 的可写回调
 * 触发异步发送。
 *
 * @param nc   网络上下文。
 * @param data 数据缓冲区指针。
 * @param len  数据长度（字节）。
 * @return int 成功返回 0，失败返回 -1。
 */
int net_send(struct net_context *nc, const void *data, size_t len)
{
    if (nc->mode == FT_PROTO_UDP) {
        if (!nc->udp_bound) return -1;
        /* UDP 同步发送 */
        ssize_t n = sendto(nc->udp_fd, data, len, 0,
                           (const struct sockaddr *)&nc->udp_peer,
                           sizeof(nc->udp_peer));
        return (n == (ssize_t)len) ? 0 : -1;
    }

    /* TCP 异步发送：复制数据到堆，因为调用者的栈缓冲区可能在异步回调时已失效 */
    free(nc->tx_buf);   /* 释放之前未发送完的缓冲区（如有） */
    nc->tx_buf = malloc(len);
    if (!nc->tx_buf) return -1;
    memcpy(nc->tx_buf, data, len);
    nc->tx_data = nc->tx_buf;
    nc->tx_len = len;
    nc->tx_sent = 0;
    nc->tx_pending = true;
    /* 请求 libwebsockets 在套接字可写时触发回调 */
    lws_callback_on_writable(nc->wsi);
    return 0;
}

/**
 * @brief 从 UDP 套接字接收数据。
 *
 * 使用 select() 等待数据到达，支持超时。
 *
 * @param nc         网络上下文。
 * @param buf        存储接收数据的缓冲区。
 * @param len        缓冲区最大长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return int       成功时返回接收的字节数，超时或错误返回 -1。
 */
int net_udp_recv(struct net_context *nc, void *buf, size_t len, int timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(nc->udp_fd, &fds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(nc->udp_fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return -1;   /* 超时或错误 */

    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    return recvfrom(nc->udp_fd, buf, len, 0,
                    (struct sockaddr *)&src, &srclen);
}

/* ── 事件循环 ──────────────────────────────────────────────────── */

/**
 * @brief 处理网络事件循环。
 *
 * 对于 TCP，调用 libwebsockets 的事件处理函数。对于 UDP，此函数不做任何事，
 * 因为本实现中 UDP 是同步处理的。
 *
 * @param nc         网络上下文。
 * @param timeout_ms 事件循环的超时时间（毫秒）。
 * @return int       对于 TCP 返回 lws_service 的返回值，对于 UDP 返回 0。
 */
int net_service(struct net_context *nc, int timeout_ms)
{
    if (nc->mode == FT_PROTO_UDP) return 0;
    return lws_service(nc->ctx, timeout_ms);
}

/* ── 状态查询 ──────────────────────────────────────────────────── */

/**
 * @brief 检查连接当前是否活跃。
 *
 * @param nc 网络上下文。
 * @return bool 已连接且未关闭返回 true，否则 false。
 */
bool net_is_connected(struct net_context *nc)
{
    return nc->connected && !nc->closed;
}

/**
 * @brief 检查连接上是否发生了错误。
 *
 * @param nc 网络上下文。
 * @return bool 如果设置了错误码返回 true，否则 false。
 */
bool net_has_error(struct net_context *nc)
{
    return nc->error_code != 0;
}

/**
 * @brief 检查是否有待发送的数据。
 *
 * @param nc 网络上下文。
 * @return bool 有数据等待发送返回 true，否则 false。
 */
bool net_tx_pending(struct net_context *nc)
{
    return nc->tx_pending;
}

/**
 * @brief 获取底层 libwebsockets 连接实例。
 *
 * @param nc 网络上下文。
 * @return void* lws 结构指针，可能为 NULL。
 */
void *net_get_wsi(struct net_context *nc)
{
    return nc->wsi;
}

/**
 * @brief 获取底层套接字文件描述符。
 *
 * @param nc 网络上下文。
 * @return socket_t 套接字描述符。
 */
socket_t net_get_fd(struct net_context *nc)
{
    return nc->sock_fd;
}

/**
 * @brief 请求取消正在进行的操作（例如 accept 循环）。
 *
 * @param nc 网络上下文。
 */
void net_cancel(struct net_context *nc)
{
    if (nc) nc->cancelled = true;
}

/**
 * @brief 检查上下文是否已被取消。
 *
 * @param nc 网络上下文。
 * @return bool 已取消返回 true，否则 false（若 nc 为 NULL 也返回 false）。
 */
bool net_is_cancelled(struct net_context *nc)
{
    return nc ? nc->cancelled : false;
}

/* ── 资源释放 ──────────────────────────────────────────────────── */

/**
 * @brief 销毁网络上下文并释放所有资源。
 *
 * 释放发送缓冲区，关闭所有套接字，销毁 libwebsockets 上下文，
 * 并清理套接字库。
 *
 * @param nc 要销毁的网络上下文。
 */
void net_destroy(struct net_context *nc)
{
    if (!nc) return;
    free(nc->tx_buf);
    if (nc->listen_fd != INVALID_FD) close_sock(nc->listen_fd);
    if (nc->sock_fd != INVALID_FD) close_sock(nc->sock_fd);
    if (nc->udp_fd != INVALID_FD) close_sock(nc->udp_fd);
    if (nc->ctx) lws_context_destroy(nc->ctx);
    SOCKET_QUIT();   /* 清理平台套接字库（Windows 的 WSACleanup） */
    free(nc);
}