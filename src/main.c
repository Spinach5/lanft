/* 包含自定义头文件 */
#include "protocol.h"   /* 协议定义（TCP/UDP模式常量） */
#include "network.h"    /* 网络抽象层接口 */
#include "scanner.h"    /* 局域网扫描功能 */
#include "transfer.h"   /* 文件传输核心逻辑 */
#include "ui.h"         /* 用户界面（SDL）相关 */
#include "config.h"     /* 配置加载/保存 */
#include "log.h"        /* 日志记录 */
#include "discovery.h"  /* UDP发现响应 */

#include <stdbool.h>    /* bool 类型 */
#include <stdio.h>      /* 标准输入输出 */
#include <stdlib.h>     /* 标准库（内存分配等） */
#include <string.h>     /* 字符串操作 */
#include <time.h>       /* 时间函数 */
#include <sys/time.h>   /* gettimeofday */
#include <unistd.h>     /* usleep, 其他POSIX函数 */
#include <pthread.h>    /* 多线程（发送/接收工作线程） */
#include <SDL2/SDL.h>   /* SDL2图形库 */

/* ═══════════════════════════════════════════════════════════
   传输线程包装器（Transfer thread wrappers）
   ═══════════════════════════════════════════════════════════ */

/* 全局变量：当前活动的网络上下文（用于取消操作） */
static struct net_context *active_nc = NULL;
/* 发送/接收停止标志 */
static bool transfer_stopped = false;
static bool recv_stop_requested = false;

/* 发送线程的参数结构体 */
typedef struct {
    struct net_context *nc;      /* 网络上下文 */
    char filepath[1024];         /* 要发送的文件路径 */
    char target_ip[64];          /* 目标IP地址 */
    int  target_port;            /* 目标端口 */
    int protocol;                /* 协议（TCP/UDP） */
} send_thread_args;

/* 发送线程函数（在独立线程中运行） */
static void *send_thread_func(void *arg)
{
    send_thread_args *a = (send_thread_args *)arg;

    /* Prepare (compress) before connecting — avoids receiver timeout */
    uint64_t total_size = 0;
    char *send_path = transfer_prepare_send(a->filepath, &total_size);
    if (!send_path) {
        log_write("[SEND] failed to prepare file\n");
        net_destroy(a->nc);
        free(a);
        return NULL;
    }

    log_write("[SEND] prepared, connecting to receiver %s:%d...\n",
            a->target_ip, a->target_port);
    while (!net_is_cancelled(a->nc)) {
        if (a->protocol == FT_PROTO_TCP) {
            if (net_connect(a->nc, a->target_ip, a->target_port) == 0) {
                log_write("[SEND] connected to receiver!\n");
                break;
            }
        } else {
            if (net_udp_bind(a->nc, a->target_port) == 0) {
                net_udp_set_peer(a->nc, a->target_ip, a->target_port);
                break;
            }
        }
        usleep(500000);
    }
    if (!net_is_cancelled(a->nc)) {
        transfer_send(a->nc, send_path, a->protocol);
    }
    net_destroy(a->nc);
    transfer_cleanup_send(send_path, a->filepath);
    free(a);
    return NULL;
}

/* 接收线程的参数结构体 */
typedef struct {
    char savepath[1024];    /* 保存路径 */
    char listen_ip[64];     /* 监听IP地址 */
    int  port;              /* 监听端口 */
    int  protocol;          /* 协议 */
    int  max_connections;   /* 最大连接数，0=无限 */
} recv_thread_args;

/* 接收线程函数（持久监听，可接受多次传输） */
static void *recv_thread_func(void *arg)
{
    recv_thread_args *a = (recv_thread_args *)arg;
    char savepath[1024];
    strncpy(savepath, a->savepath, sizeof(savepath) - 1);
    char ip[64];
    strncpy(ip, a->listen_ip, sizeof(ip) - 1);
    int port    = a->port;
    int proto   = a->protocol;
    int max_conn = a->max_connections;
    free(a);   /* 参数不再需要，释放 */

    log_write("[RECV] persistent listener started on %s:%d (max %d)\n",
              ip, port, max_conn);

    int transfer_count = 0;
    /* 循环接受传输，直到收到停止请求 */
    while (!recv_stop_requested) {
        if (max_conn > 0 && transfer_count >= max_conn) {
            log_write("[RECV] max_connections (%d) reached, stopping.\n", max_conn);
            break;
        }
        struct net_context *nc = net_create(proto);
        if (!nc) {
            recv_stop_requested = true;
            /* 推送错误事件到UI线程 */
            struct event_error *err = calloc(1, sizeof(*err));
            snprintf(err->message, sizeof(err->message),
                     "Failed to create network context");
            SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
            ev.type = USEREVENT_ERROR; ev.user.data1 = err;
            SDL_PushEvent(&ev);
            break;
        }

        if (proto == FT_PROTO_TCP) {
            /* TCP模式：监听指定IP和端口 */
            if (net_listen_ip(nc, ip, port) != 0) {
                recv_stop_requested = true;
                struct event_error *err = calloc(1, sizeof(*err));
                snprintf(err->message, sizeof(err->message),
                         "Failed to listen on %s:%d", ip, port);
                SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
                ev.type = USEREVENT_ERROR; ev.user.data1 = err;
                SDL_PushEvent(&ev);
                net_destroy(nc);
                break;
            }
        } else {
            /* UDP模式：绑定本地端口 */
            if (net_udp_bind(nc, port) != 0) {
                recv_stop_requested = true;
                struct event_error *err = calloc(1, sizeof(*err));
                snprintf(err->message, sizeof(err->message),
                         "Failed to bind UDP port %d", port);
                SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
                ev.type = USEREVENT_ERROR; ev.user.data1 = err;
                SDL_PushEvent(&ev);
                net_destroy(nc);
                break;
            }
        }

        active_nc = nc;   /* 设置全局活动上下文，以便取消 */
        transfer_count++;
        log_write("[RECV] waiting for sender (transfer #%d)...\n",
                transfer_count);
        transfer_recv(nc, savepath, proto);   /* 阻塞直到一次传输完成 */
        net_destroy(nc);
        active_nc = NULL;

        if (recv_stop_requested) break;
        log_write("[RECV] ready for next transfer...\n");
    }

    log_write("[RECV] listener stopped\n");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
   网络编排函数（由主线程调用）
   ═══════════════════════════════════════════════════════════ */

/* 启动发送过程（创建线程） */
static void start_send(struct app_state *state)
{
    struct net_context *nc = net_create(state->send_protocol);
    if (!nc) {
        /* 创建失败：推送错误事件 */
        struct event_error *err = calloc(1, sizeof(*err));
        snprintf(err->message, sizeof(err->message), "Failed to create network context");
        SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
        ev.type = USEREVENT_ERROR; ev.user.data1 = err;
        SDL_PushEvent(&ev);
        return;
    }

    /* 填充发送线程参数 */
    send_thread_args *args = malloc(sizeof(*args));
    args->nc = nc;
    strncpy(args->filepath, state->send_filepath, sizeof(args->filepath) - 1);
    strncpy(args->target_ip, state->send_target_ip, sizeof(args->target_ip) - 1);
    args->target_port = state->send_port;
    args->protocol = state->send_protocol;

    active_nc = nc;
    transfer_stopped = false;
    pthread_t tid;
    pthread_create(&tid, NULL, send_thread_func, args);
    pthread_detach(tid);   /* 分离线程，使其自动回收 */
    log_write("[MAIN] send thread spawned, will connect to %s:%d\n",
            state->send_target_ip, state->send_port);
}

/* 启动接收过程（创建持久监听线程） */
static void start_recv(struct app_state *state)
{
    recv_thread_args *args = malloc(sizeof(*args));
    if (!args) return;
    strncpy(args->savepath, state->recv_savepath, sizeof(args->savepath) - 1);
    strncpy(args->listen_ip, state->recv_target_ip, sizeof(args->listen_ip) - 1);
    args->port     = state->recv_port;
    args->protocol        = state->recv_protocol;
    args->max_connections = state->gui_cfg.max_connections;

    recv_stop_requested = false;
    transfer_stopped = false;
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread_func, args);
    pthread_detach(tid);
    log_write("[MAIN] persistent recv thread spawned on %s:%d\n",
            state->recv_target_ip, state->recv_port);
}

/* ═══════════════════════════════════════════════════════════
   主函数
   ═══════════════════════════════════════════════════════════ */

/* CLI模式入口（定义在cli.c中） */
int cli_main(int argc, char **argv);

/* GUI模式下，传输模块的回调函数：更新进度条 */
static void gui_progress(uint64_t done, uint64_t total)
{
    struct event_progress *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->bytes_done = done;
    p->bytes_total = total;
    SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
    ev.type = USEREVENT_PROGRESS; ev.user.data1 = p;
    SDL_PushEvent(&ev);   /* 推送自定义事件到主线程 */
}

/* GUI模式下，传输模块的回调函数：报告错误 */
static void gui_error(const char *msg)
{
    struct event_error *e = calloc(1, sizeof(*e));
    if (!e) return;
    strncpy(e->message, msg, sizeof(e->message) - 1);
    SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
    ev.type = USEREVENT_ERROR; ev.user.data1 = e;
    SDL_PushEvent(&ev);
}

/* GUI模式下，传输模块的回调函数：传输完成 */
static void gui_done(void)
{
    SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
    ev.type = USEREVENT_XFER_DONE;
    SDL_PushEvent(&ev);
}

/* ── GUI端接受传入传输的询问回调（在传输线程中调用） ──────── */

/* 在transfer.c中定义的全局变量，用于跨线程通信 */
extern volatile int g_accept_response;   /* 用户响应（1=接受，0=拒绝） */
extern volatile bool g_accept_pending;   /* 是否等待用户响应 */

/* 当收到传入传输请求时，由transfer_recv调用此回调，
   询问用户是否接受文件 */
static int gui_accept_cb(const char *ip, const char *hostname,
                         const char *filename, uint64_t size)
{
    struct event_incoming_transfer *evt = calloc(1, sizeof(*evt));
    if (!evt) return 0;
    strncpy(evt->ip, ip, sizeof(evt->ip) - 1);
    strncpy(evt->hostname, hostname, sizeof(evt->hostname) - 1);
    strncpy(evt->filename, filename, sizeof(evt->filename) - 1);
    evt->file_size = size;

    g_accept_response = 0;
    g_accept_pending = true;

    /* 推送传入传输事件到UI线程 */
    SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
    ev.type = USEREVENT_INCOMING_TRANSFER;
    ev.user.data1 = evt;
    SDL_PushEvent(&ev);

    /* 等待UI线程设置g_accept_response */
    while (g_accept_pending) {
        usleep(100000);  /* 100ms轮询 */
    }

    return (g_accept_response > 0) ? 1 : 0;
}

int main(int argc, char **argv)
{
    /* Determine mode:
       - No arguments (double-click / plain ./lanft) → GUI
       - --gui flag → GUI
       - Anything else → CLI */
    bool gui_mode = (argc <= 1);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gui") == 0) {
            gui_mode = true;
            break;
        }
    }

    if (!gui_mode) {
        return cli_main(argc, argv);
    }

    /* ── GUI mode init ── */

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        log_write("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* 创建窗口 */
    SDL_Window *window = SDL_CreateWindow(
        "LAN File Transfer",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        800, 600,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!window) {
        log_write("SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* 创建渲染器 */
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* 初始化UI模块（加载字体、创建纹理等） */
    if (ui_init() != 0) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* 设置传输模块的回调函数（将事件推送到主线程） */
    transfer_set_callbacks(gui_progress, gui_error, gui_done);
    transfer_set_accept_callback(gui_accept_cb);

    /* 初始化应用状态结构体 */
    struct app_state state;
    memset(&state, 0, sizeof(state));
    history_load(&state);                     /* 加载传输历史记录 */
    state.current_tab = TAB_SCAN;             /* 默认显示扫描标签页 */
    state.selected_device = -1;
    state.active_input = 0;
    config_load(&state.gui_cfg);              /* 加载用户配置 */
    log_init(&state.gui_cfg);                 /* 初始化日志 */
    if (state.gui_cfg.discovery_enabled)        /* 启动UDP发现应答 */
        discovery_start((uint16_t)state.gui_cfg.port);
    /* 从配置中填充默认值 */
    state.local_ip_count = scanner_get_local_ips(state.local_ips, 8);
    state.scan_port = state.gui_cfg.port;
    state.send_port = state.gui_cfg.port;
    state.recv_port = state.gui_cfg.port;
    strncpy(state.recv_target_ip, state.gui_cfg.address, sizeof(state.recv_target_ip) - 1);
    state.send_protocol = state.gui_cfg.protocol;
    state.recv_protocol = state.gui_cfg.protocol;
    if (state.gui_cfg.mode == 0) {
        state.current_tab = TAB_SEND;
    } else if (state.gui_cfg.mode == 1) {
        state.current_tab = TAB_RECEIVE;
    }
    if (state.gui_cfg.save_dir[0]) {
        const char *expanded = config_expand_path(state.gui_cfg.save_dir);
        strncpy(state.recv_savepath, expanded, sizeof(state.recv_savepath) - 1);
    }
    /* 将配置应用到传输模块 */
    transfer_set_auto_accept(state.gui_cfg.auto_accept);
    transfer_set_buffer_size(state.gui_cfg.buffer_size);
    transfer_set_timeout(state.gui_cfg.timeout_seconds);
    transfer_set_overwrite_policy(state.gui_cfg.overwrite_policy);
    transfer_set_bandwidth_limit(state.gui_cfg.send_bandwidth_limit);
    transfer_set_max_connections(state.gui_cfg.max_connections);
    state.status_text[0] = '\0';
    SDL_GetWindowSize(window, &state.window_w, &state.window_h);

    bool pending_send = false;   /* 发送请求待处理标志 */
    bool pending_recv = false;   /* 接收请求待处理标志 */
    bool running = true;
    SDL_Event event;

    /* 主事件循环 */
    while (running) {
        /* 处理所有待处理的SDL事件 */
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    state.window_w = event.window.data1;
                    state.window_h = event.window.data2;
                }
                break;

            case SDL_MOUSEWHEEL:
                /* Scroll content on tabs that overflow */
                if (event.wheel.y != 0) {
                    state.scroll_offset += event.wheel.y * 30;
                    if (state.scroll_offset > 0) state.scroll_offset = 0;
                    /* Don't scroll past content: settings page is ~560px tall */
                    int visible = state.window_h - 64;
                    int min_scroll = -(560 - visible);
                    if (min_scroll > 0) min_scroll = 0;
                    if (state.scroll_offset < min_scroll) state.scroll_offset = min_scroll;
                }
                break;

            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    /* ESC键行为：
                       - 如果模态对话框可见，关闭它
                       - 否则如果有活动传输，则取消传输
                       - 否则停止文本输入 */
                    if (state.modal_visible)
                        state.modal_visible = false;
                    else if (state.send_running || state.recv_running) {
                        if (active_nc) net_cancel(active_nc);
                        transfer_stopped = true;
                        recv_stop_requested = true;
                        state.send_running = false;
                        state.recv_running = false;
                        pending_send = false;
                        pending_recv = false;
                        strncpy(state.status_text, "Transfer stopped", sizeof(state.status_text) - 1);
                    } else {
                        state.active_input = 0;
                        SDL_StopTextInput();
                    }
                    break;
                }
                /* 其他按键交给UI处理 */
                ui_handle_event(&event, &state);
                break;

            /* ── 自定义事件（从工作线程发出） ────── */

            case USEREVENT_SCAN_FOUND:   /* 扫描发现设备 */
            {
                struct event_scan_found *evt = (struct event_scan_found *)event.user.data1;
                if (evt && state.device_count < 256) {
                    strncpy(state.devices[state.device_count].ip, evt->ip, 63);
                    strncpy(state.devices[state.device_count].hostname, evt->hostname, 255);
                    state.device_count++;
                }
                free(evt);
                break;
            }

            case USEREVENT_SCAN_DONE:    /* 扫描完成 */
                snprintf(state.scan_status, sizeof(state.scan_status),
                         "Scan complete — %d device(s) found", state.device_count);
                if (state.device_count == 0)
                    snprintf(state.status_text, sizeof(state.status_text),
                             "No devices found on the LAN");
                free(event.user.data1);
                break;

            case USEREVENT_PROGRESS:     /* 传输进度更新 */
            {
                struct event_progress *evt = (struct event_progress *)event.user.data1;
                if (evt) {
                    if (state.send_running) {
                        state.send_progress_done = evt->bytes_done;
                        state.send_progress_total = evt->bytes_total;
                    }
                    if (state.recv_running) {
                        state.recv_progress_done = evt->bytes_done;
                        state.recv_progress_total = evt->bytes_total;
                    }
                    state.history_pending_total = evt->bytes_total;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Transferring... %lu / %lu bytes",
                             (unsigned long)evt->bytes_done, (unsigned long)evt->bytes_total);
                    strncpy(state.status_text, buf, sizeof(state.status_text) - 1);
                }
                free(evt);
                break;
            }

            case USEREVENT_XFER_DONE:   /* 传输完成 */
            {
                /* 将本次传输记录到历史 */
                if (state.history_count < 256) {
                    struct hist_entry *he = &state.history[state.history_count];
                    struct timeval tv; gettimeofday(&tv, NULL);
                    uint64_t end_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                    state.history_pending_total = state.send_running
                        ? state.send_progress_total : state.recv_progress_total;

                    const char *recv_name = transfer_last_recv_name();
                    strncpy(he->name, recv_name ? recv_name : state.history_pending_name, 255);
                    time_t now = time(NULL);
                    strftime(he->end_time, 32, "%H:%M:%S", localtime(&now));
                    he->duration_ms = end_ms - state.history_pending_start_ms;
                    he->kind = state.history_pending_kind;
                    he->port = state.history_pending_port;
                    he->status = 0; /* 成功 */
                    he->progress = 100;
                    if (he->duration_ms > 0 && state.history_pending_total > 0)
                        he->speed = state.history_pending_total * 1000 / he->duration_ms;
                    else he->speed = 0;
                    state.history_count++;
                    history_save(&state);
                    /* 重置开始时间，为下一次持久接收做准备 */
                    state.history_pending_start_ms = end_ms;
                }
                if (state.send_running) {
                    state.send_running = false;
                    pending_send = false;
                    active_nc = NULL;
                }
                /* 接收模式下，保持运行（持久监听） */
                if (state.recv_running) {
                    state.recv_progress_done = 0;
                    state.recv_progress_total = 0;
                }
                strncpy(state.status_text, "Transfer complete!", sizeof(state.status_text) - 1);
                break;
            }

            case USEREVENT_ZENITY_RESULT:   /* 文件选择对话框结果（通过外部zenity） */
            {
                const char *path = (const char *)event.user.data1;
                int target = event.user.code;
                if (path && target == 1) {
                    strncpy(state.send_filepath, path, sizeof(state.send_filepath) - 1);
                    snprintf(state.status_text, sizeof(state.status_text), "Selected: %s", path);
                } else if (path && target == 2) {
                    strncpy(state.recv_savepath, path, sizeof(state.recv_savepath) - 1);
                    snprintf(state.status_text, sizeof(state.status_text), "Save to: %s", path);
                } else if (path && target == 3) {
                    strncpy(state.gui_cfg.save_dir, path, sizeof(state.gui_cfg.save_dir) - 1);
                    snprintf(state.status_text, sizeof(state.status_text), "Save dir: %s", path);
                }
                break;
            }

            case USEREVENT_INCOMING_TRANSFER:  /* 收到传入传输请求，需用户确认 */
            {
                struct event_incoming_transfer *evt =
                    (struct event_incoming_transfer *)event.user.data1;
                if (evt) {
                    strncpy(state.incoming_ip, evt->ip,
                            sizeof(state.incoming_ip) - 1);
                    strncpy(state.incoming_hostname, evt->hostname,
                            sizeof(state.incoming_hostname) - 1);
                    strncpy(state.incoming_filename, evt->filename,
                            sizeof(state.incoming_filename) - 1);
                    state.incoming_size = evt->file_size;
                    state.incoming_active = true;   /* 显示询问对话框 */
                }
                free(evt);
                break;
            }

            case USEREVENT_ERROR:   /* 发生错误 */
            {
                struct event_error *evt = (struct event_error *)event.user.data1;
                if (evt) {
                    strncpy(state.modal_message, evt->message, sizeof(state.modal_message) - 1);
                    state.modal_visible = true;
                    /* 记录失败的历史条目 */
                    if (state.history_count < 256) {
                        struct hist_entry *he = &state.history[state.history_count];
                        struct timeval tv; gettimeofday(&tv, NULL);
                        uint64_t end_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                        const char *recv_name = transfer_last_recv_name();
                        strncpy(he->name, recv_name ? recv_name : state.history_pending_name, 255);
                        time_t now = time(NULL);
                        strftime(he->end_time, 32, "%H:%M:%S", localtime(&now));
                        he->duration_ms = (end_ms > state.history_pending_start_ms) ? end_ms - state.history_pending_start_ms : 0;
                        he->kind = state.history_pending_kind;
                        he->port = state.history_pending_port;
                        he->status = 1; /* 失败 */
                        if (state.history_pending_total > 0) {
                            uint64_t done = state.send_running ? state.send_progress_done : state.recv_progress_done;
                            he->progress = (int)(done * 100 / state.history_pending_total);
                            if (he->duration_ms > 0) he->speed = done * 1000 / he->duration_ms;
                            else he->speed = 0;
                        } else { he->progress = 0; he->speed = 0; }
                        state.history_count++;
                        history_save(&state);
                        state.history_pending_start_ms = end_ms;
                    }
                    if (state.send_running) {
                        state.send_running = false;
                        pending_send = false;
                        active_nc = NULL;
                    }
                    /* 接收线程继续运行（持久监听） */
                    snprintf(state.status_text, sizeof(state.status_text), "Error: %s", evt->message);
                }
                free(evt);
                break;
            }

            default:
                ui_handle_event(&event, &state);
                break;
            }
        }

        /* ── 检测发送/接收启动请求（由UI触发） ────────── */

        /* 如果用户点击停止，则取消活动传输 */
        if (!state.send_running && pending_send && active_nc) {
            net_cancel(active_nc);
            pending_send = false;
        }
        if (!state.recv_running && pending_recv && active_nc) {
            recv_stop_requested = true;
            net_cancel(active_nc);
            pending_recv = false;
        }

        /* 当发送被启用且尚未启动，且没有正在进行中的传输时，开始发送 */
        if (state.send_running && !pending_send && state.send_progress_total == 0) {
            log_write("[MAIN] Starting send: file=%s, target=%s, port=%d, proto=%d\n",
                    state.send_filepath, state.send_target_ip, state.send_port, state.send_protocol);
            pending_send = true;
            /* 预先创建历史记录条目 */
            {
                const char *fn = strrchr(state.send_filepath, '/');
                strncpy(state.history_pending_name, fn ? fn + 1 : state.send_filepath, 255);
                state.history_pending_kind = 0; /* 发送 */
                state.history_pending_port = state.send_port;
                struct timeval tv; gettimeofday(&tv, NULL);
                state.history_pending_start_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                state.history_pending_total = 0;
                time_t now = time(NULL);
                strftime(state.history[state.history_count].start_time, 32, "%H:%M:%S", localtime(&now));
            }
            start_send(&state);
        }
        if (!state.send_running) pending_send = false;

        /* 当接收被启用且尚未启动，且没有进行中的传输时，开始持久接收 */
        if (state.recv_running && !pending_recv && state.recv_progress_total == 0) {
            log_write("[MAIN] Starting recv: save=%s, target=%s, port=%d, proto=%d\n",
                    state.recv_savepath, state.recv_target_ip, state.recv_port, state.recv_protocol);
            pending_recv = true;
            {
                const char *fn = strrchr(state.recv_savepath, '/');
                strncpy(state.history_pending_name, fn ? fn + 1 : state.recv_savepath, 255);
                state.history_pending_kind = 1; /* 接收 */
                state.history_pending_port = state.recv_port;
                struct timeval tv; gettimeofday(&tv, NULL);
                state.history_pending_start_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                state.history_pending_total = 0;
                time_t now = time(NULL);
                strftime(state.history[state.history_count].start_time, 32, "%H:%M:%S", localtime(&now));
            }
            start_recv(&state);
        }
        if (!state.recv_running) pending_recv = false;

        /* 同步“自动接受”设置（可能用户在设置页面修改了） */
        transfer_set_auto_accept(state.gui_cfg.auto_accept);

        /* 绘制UI */
        ui_render(renderer, &state);
        SDL_Delay(16);   /* 约60 FPS */
    }

    /* 程序退出，保存状态和配置 */
    history_save(&state);
    config_save(&state.gui_cfg);
    discovery_stop();
    log_close();
    ui_cleanup();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}