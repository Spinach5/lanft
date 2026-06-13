#include "protocol.h"
#include "network.h"
#include "scanner.h"
#include "transfer.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <SDL2/SDL.h>

/* ═══════════════════════════════════════════════════════════
   Transfer thread wrappers
   ═══════════════════════════════════════════════════════════ */

static struct net_context *active_nc = NULL;
static bool transfer_stopped = false;

typedef struct {
    struct net_context *nc;
    char filepath[1024];
    int protocol;
} send_thread_args;

static void *send_thread_func(void *arg)
{
    send_thread_args *a = (send_thread_args *)arg;
    transfer_send(a->nc, a->filepath, a->protocol);
    net_destroy(a->nc);
    free(a);
    return NULL;
}

typedef struct {
    struct net_context *nc;
    char savepath[1024];
    char target_ip[64];
    int  target_port;
    int protocol;
} recv_thread_args;

static void *recv_thread_func(void *arg)
{
    recv_thread_args *a = (recv_thread_args *)arg;
    fprintf(stderr, "[RECV] thread started, connecting to %s:%d...\n",
            a->target_ip, a->target_port);
    while (!net_is_cancelled(a->nc)) {
        if (a->protocol == FT_PROTO_TCP) {
            if (net_connect(a->nc, a->target_ip, a->target_port) == 0) {
                fprintf(stderr, "[RECV] connected!\n");
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
        transfer_recv(a->nc, a->savepath, a->protocol);
    }
    net_destroy(a->nc);
    free(a);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
   Network orchestration — called from main thread
   ═══════════════════════════════════════════════════════════ */

static void start_send(struct app_state *state)
{
    struct net_context *nc = net_create(state->send_protocol);
    if (!nc) {
        struct event_error *err = calloc(1, sizeof(*err));
        snprintf(err->message, sizeof(err->message), "Failed to create network context");
        SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
        ev.type = USEREVENT_ERROR; ev.user.data1 = err;
        SDL_PushEvent(&ev);
        return;
    }

    if (state->send_protocol == FT_PROTO_TCP) {
        if (net_listen(nc, state->send_port) != 0) {
            struct event_error *err = calloc(1, sizeof(*err));
            snprintf(err->message, sizeof(err->message), "Failed to listen on port %d", state->send_port);
            SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
            ev.type = USEREVENT_ERROR; ev.user.data1 = err;
            SDL_PushEvent(&ev);
            net_destroy(nc);
            return;
        }
    } else {
        if (net_udp_bind(nc, state->send_port) != 0) {
            struct event_error *err = calloc(1, sizeof(*err));
            snprintf(err->message, sizeof(err->message), "Failed to bind UDP port %d", state->send_port);
            SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
            ev.type = USEREVENT_ERROR; ev.user.data1 = err;
            SDL_PushEvent(&ev);
            net_destroy(nc);
            return;
        }
        net_udp_set_peer(nc, state->send_target_ip, state->send_port);
    }

    send_thread_args *args = malloc(sizeof(*args));
    args->nc = nc;
    strncpy(args->filepath, state->send_filepath, sizeof(args->filepath) - 1);
    args->protocol = state->send_protocol;

    active_nc = nc;
    transfer_stopped = false;
    pthread_t tid;
    pthread_create(&tid, NULL, send_thread_func, args);
    pthread_detach(tid);
    fprintf(stderr, "[MAIN] send thread spawned, port=%d\n", state->send_port);
}

static void start_recv(struct app_state *state)
{
    struct net_context *nc = net_create(state->recv_protocol);
    if (!nc) {
        struct event_error *err = calloc(1, sizeof(*err));
        snprintf(err->message, sizeof(err->message), "Failed to create network context");
        SDL_Event ev; SDL_memset(&ev, 0, sizeof(ev));
        ev.type = USEREVENT_ERROR; ev.user.data1 = err;
        SDL_PushEvent(&ev);
        return;
    }

    recv_thread_args *args = malloc(sizeof(*args));
    args->nc = nc;
    strncpy(args->savepath, state->recv_savepath, sizeof(args->savepath) - 1);
    strncpy(args->target_ip, state->recv_target_ip, sizeof(args->target_ip) - 1);
    args->target_port = state->recv_port;
    args->protocol = state->recv_protocol;

    active_nc = nc;
    transfer_stopped = false;
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread_func, args);
    pthread_detach(tid);
    fprintf(stderr, "[MAIN] recv thread spawned, will connect to %s:%d\n",
            state->recv_target_ip, state->recv_port);
}

/* ═══════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "LAN File Transfer",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        800, 600,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (ui_init() != 0) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Init app state */
    struct app_state state;
    memset(&state, 0, sizeof(state));
    history_load(&state);
    state.current_tab = TAB_SCAN;
    state.selected_device = -1;
    state.active_input = 0;
    state.scan_port = FT_DEFAULT_PORT;
    state.send_port = FT_DEFAULT_PORT;
    state.recv_port = FT_DEFAULT_PORT;
    state.send_protocol = FT_PROTO_TCP;
    state.recv_protocol = FT_PROTO_TCP;
    strncpy(state.status_text, "Ready — select a tab to begin",
            sizeof(state.status_text) - 1);
    SDL_GetWindowSize(window, &state.window_w, &state.window_h);

    bool pending_send = false;
    bool pending_recv = false;
    bool running = true;
    SDL_Event event;

    while (running) {
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

            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    if (state.modal_visible)
                        state.modal_visible = false;
                    else if (state.send_running || state.recv_running) {
                        /* Cancel active transfer */
                        if (active_nc) net_cancel(active_nc);
                        transfer_stopped = true;
                        state.send_running = false;
                        state.recv_running = false;
                        pending_send = false;
                        pending_recv = false;
                        strncpy(state.status_text, "Transfer stopped", sizeof(state.status_text) - 1);
                    } else {
                        state.active_input = 0;
                        SDL_StopTextInput();
                    }
                    break;  /* ESC handled */
                }
                /* Pass other keys to UI handler */
                ui_handle_event(&event, &state);
                break;

            /* ── Custom events from worker threads ────── */

            case USEREVENT_SCAN_FOUND: {
                struct event_scan_found *evt = (struct event_scan_found *)event.user.data1;
                if (evt && state.device_count < 256) {
                    strncpy(state.devices[state.device_count].ip, evt->ip, 63);
                    strncpy(state.devices[state.device_count].hostname, evt->hostname, 255);
                    state.device_count++;
                }
                free(evt);
                break;
            }

            case USEREVENT_SCAN_DONE:
                snprintf(state.scan_status, sizeof(state.scan_status),
                         "Scan complete — %d device(s) found", state.device_count);
                if (state.device_count == 0)
                    snprintf(state.status_text, sizeof(state.status_text),
                             "No devices found on the LAN");
                free(event.user.data1);
                break;

            case USEREVENT_PROGRESS: {
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

            case USEREVENT_XFER_DONE: {
                /* Finalize history entry */
                if (state.history_count < 256) {
                    struct hist_entry *he = &state.history[state.history_count];
                    struct timeval tv; gettimeofday(&tv, NULL);
                    uint64_t end_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                    state.history_pending_total = state.send_running
                        ? state.send_progress_total : state.recv_progress_total;

                    strncpy(he->name, state.history_pending_name, 255);
                    /* start_time already set at transfer start */
                    time_t now = time(NULL);
                    strftime(he->end_time, 32, "%H:%M:%S", localtime(&now));
                    he->duration_ms = end_ms - state.history_pending_start_ms;
                    he->kind = state.history_pending_kind;
                    he->port = state.history_pending_port;
                    he->status = 0; /* OK */
                    he->progress = 100;
                    if (he->duration_ms > 0 && state.history_pending_total > 0)
                        he->speed = state.history_pending_total * 1000 / he->duration_ms;
                    else he->speed = 0;
                    state.history_count++;
                    history_save(&state);
                }
                state.send_running = false;
                state.recv_running = false;
                pending_send = false;
                pending_recv = false;
                active_nc = NULL;
                strncpy(state.status_text, "Transfer complete!", sizeof(state.status_text) - 1);
                break;
            }

            case USEREVENT_ZENITY_RESULT: {
                /* Zenity dialog returned — update file path */
                const char *path = (const char *)event.user.data1;
                int target = event.user.code;
                if (path && target == 1) {
                    strncpy(state.send_filepath, path, sizeof(state.send_filepath) - 1);
                    snprintf(state.status_text, sizeof(state.status_text), "Selected: %s", path);
                } else if (path && target == 2) {
                    strncpy(state.recv_savepath, path, sizeof(state.recv_savepath) - 1);
                    snprintf(state.status_text, sizeof(state.status_text), "Save to: %s", path);
                }
                break;
            }

            case USEREVENT_ERROR: {
                struct event_error *evt = (struct event_error *)event.user.data1;
                if (evt) {
                    strncpy(state.modal_message, evt->message, sizeof(state.modal_message) - 1);
                    state.modal_visible = true;
                    /* Record failed history */
                    if (state.history_count < 256) {
                        struct hist_entry *he = &state.history[state.history_count];
                        struct timeval tv; gettimeofday(&tv, NULL);
                        uint64_t end_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                        strncpy(he->name, state.history_pending_name, 255);
                        time_t now = time(NULL);
                        strftime(he->end_time, 32, "%H:%M:%S", localtime(&now));
                        he->duration_ms = (end_ms > state.history_pending_start_ms) ? end_ms - state.history_pending_start_ms : 0;
                        he->kind = state.history_pending_kind;
                        he->port = state.history_pending_port;
                        he->status = 1; /* BAD */
                        if (state.history_pending_total > 0) {
                            uint64_t done = state.send_running ? state.send_progress_done : state.recv_progress_done;
                            he->progress = (int)(done * 100 / state.history_pending_total);
                            if (he->duration_ms > 0) he->speed = done * 1000 / he->duration_ms;
                            else he->speed = 0;
                        } else { he->progress = 0; he->speed = 0; }
                        state.history_count++;
                        history_save(&state);
                    }
                    state.send_running = false;
                    state.recv_running = false;
                    pending_send = false;
                    pending_recv = false;
                    active_nc = NULL;
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

        /* ── Detect send/receive start requests ────────── */

        /* Cancel if user clicked Stop */
        if (!state.send_running && pending_send && active_nc) {
            net_cancel(active_nc);
            pending_send = false;
        }
        if (!state.recv_running && pending_recv && active_nc) {
            net_cancel(active_nc);
            pending_recv = false;
        }

        if (state.send_running && !pending_send && state.send_progress_total == 0) {
            fprintf(stderr, "[MAIN] Starting send: file=%s, target=%s, port=%d, proto=%d\n",
                    state.send_filepath, state.send_target_ip, state.send_port, state.send_protocol);
            pending_send = true;
            /* Record history entry */
            {
                const char *fn = strrchr(state.send_filepath, '/');
                strncpy(state.history_pending_name, fn ? fn + 1 : state.send_filepath, 255);
                state.history_pending_kind = 0; /* SEND */
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

        if (state.recv_running && !pending_recv && state.recv_progress_total == 0) {
            fprintf(stderr, "[MAIN] Starting recv: save=%s, target=%s, port=%d, proto=%d\n",
                    state.recv_savepath, state.recv_target_ip, state.recv_port, state.recv_protocol);
            pending_recv = true;
            /* Record history entry */
            {
                const char *fn = strrchr(state.recv_savepath, '/');
                strncpy(state.history_pending_name, fn ? fn + 1 : state.recv_savepath, 255);
                state.history_pending_kind = 1; /* RECV */
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

        ui_render(renderer, &state);
        SDL_Delay(16);
    }

    history_save(&state);
    ui_cleanup();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
