#ifndef UI_H
#define UI_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/* ── SDL custom event codes ────────────────────────────────── */
#define USEREVENT_SCAN_FOUND        (SDL_EVENT_USER + 1)
#define USEREVENT_SCAN_DONE         (SDL_EVENT_USER + 2)
#define USEREVENT_PROGRESS          (SDL_EVENT_USER + 3)
#define USEREVENT_XFER_DONE         (SDL_EVENT_USER + 4)
#define USEREVENT_ERROR             (SDL_EVENT_USER + 5)
#define USEREVENT_ZENITY_RESULT     (SDL_EVENT_USER + 6)
#define USEREVENT_INCOMING_TRANSFER (SDL_EVENT_USER + 7)

/* ── Color scheme (dark theme) ─────────────────────────────── */
#define COLOR_BG        ((SDL_Color){0x1e, 0x1e, 0x2e, 255})
#define COLOR_SURFACE   ((SDL_Color){0x31, 0x32, 0x44, 255})
#define COLOR_TEXT      ((SDL_Color){0xcd, 0xd6, 0xf4, 255})
#define COLOR_ACCENT    ((SDL_Color){0x89, 0xb4, 0xfa, 255})
#define COLOR_ERROR     ((SDL_Color){0xf3, 0x8b, 0xa8, 255})
#define COLOR_PROGRESS  ((SDL_Color){0xa6, 0xe3, 0xa1, 255})
#define COLOR_DIM       ((SDL_Color){0x58, 0x5b, 0x70, 255})
#define COLOR_HOVER     ((SDL_Color){0x45, 0x47, 0x5a, 255})

/* ── Tab enum ──────────────────────────────────────────────── */
enum {
    TAB_SCAN = 0,
    TAB_SEND,
    TAB_RECEIVE,
    TAB_HISTORY,
    TAB_SETTINGS,
    TAB_COUNT
};

/* ── Application state ─────────────────────────────────────── */

struct app_state {
    int current_tab;

    /* SDL3: 窗口指针供 SDL_StartTextInput/SDL_StopTextInput 使用 */
    SDL_Window *window;

    /* Scan page */
    char scan_status[256];
    int  scan_port;
    /* Local IPs (shown so user can tell others what to connect to) */
    char local_ips[8][64];
    int  local_ip_count;
    struct {
        char ip[64];
        char hostname[256];
    } devices[256];
    int device_count;
    int selected_device;

    /* Send page */
    char send_filepath[1024];
    char send_target_ip[64];
    int  send_port;
    int  send_protocol;
    bool send_is_dir;   /* true = select directory, false = select file */
    bool send_running;
    uint64_t send_progress_done;
    uint64_t send_progress_total;

    /* Receive page */
    char recv_savepath[1024];
    char recv_target_ip[64];
    int  recv_port;
    int  recv_protocol;
    bool recv_listening;
    bool recv_running;
    uint64_t recv_progress_done;
    uint64_t recv_progress_total;

    /* History */
    struct hist_entry {
        char name[256];
        char start_time[32];
        char end_time[32];
        uint64_t duration_ms;
        int  kind;       /* 0=SEND, 1=RECV */
        int  port;
        int  status;     /* 0=OK, 1=BAD, 2=STOP */
        int  progress;   /* 0-100 percent */
        uint64_t speed;  /* bytes/sec */
    } history[256];
    int history_count;
    /* Pending record (being built during transfer) */
    int  history_pending_kind;
    int  history_pending_port;
    char history_pending_name[256];
    uint64_t history_pending_start_ms;
    uint64_t history_pending_total;

    /* Runtime config (settings page reads/writes this) */
    struct lanft_config gui_cfg;

    /* Modal */
    bool modal_visible;
    char modal_message[512];

    /* Incoming transfer prompt (auto_accept=false) */
    bool incoming_active;
    char incoming_ip[64];
    char incoming_hostname[256];
    char incoming_filename[256];
    uint64_t incoming_size;

    /* General */
    char status_text[256];
    int window_w;
    int window_h;
    int scroll_offset;     /* for pages that overflow */

    /* Text input focus */
    int active_input;  /* 0=none, 1=send path, 2=send ip, 3=recv path, 4=recv ip */
    char input_buffer[1024];
    int  input_cursor;

};

/* ── UI API ────────────────────────────────────────────────── */

int  ui_init(void);
void ui_cleanup(void);
bool ui_handle_event(SDL_Event *e, struct app_state *state);
void ui_render(SDL_Renderer *renderer, struct app_state *state);
void ui_draw_rect(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c);
void ui_draw_text(SDL_Renderer *r, const char *text, int x, int y, SDL_Color c);
void ui_text_size(const char *text, int *w, int *h);
bool ui_in_rect(int mx, int my, int x, int y, int w, int h);

/* History persistence */
void history_load(struct app_state *state);
void history_save(struct app_state *state);

#endif /* UI_H */
