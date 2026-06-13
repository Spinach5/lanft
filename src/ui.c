#include "ui.h"
#include "protocol.h"
#include "scanner.h"
#include "transfer.h"
#include "network.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <SDL2/SDL.h>

/* ── Zenity file dialog (async, runs in background thread) ─── */

struct zenity_args {
    char title[256];
    bool directory;
    int  target;  /* 1=send path, 2=recv save path */
};

static void *zenity_thread(void *arg)
{
    struct zenity_args *za = (struct zenity_args *)arg;

    char cmd[512];
    if (za->directory)
        snprintf(cmd, sizeof(cmd), "zenity --file-selection --directory --title=\"%s\" 2>/dev/null", za->title);
    else
        snprintf(cmd, sizeof(cmd), "zenity --file-selection --title=\"%s\" 2>/dev/null", za->title);

    FILE *fp = popen(cmd, "r");
    char *path = NULL;
    static char buf[1024];  /* must outlive this function — pushed to SDL event */

    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);
            if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
            path = buf;
        }
        pclose(fp);
    }

    /* Push result via SDL event — data1=path or NULL, data2=target */
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = USEREVENT_ZENITY_RESULT;
    event.user.code = za->target;
    event.user.data1 = path;  /* pointer to static buf, or NULL if cancelled */
    SDL_PushEvent(&event);

    free(za);
    return NULL;
}

static void zenity_launch(struct app_state *st, const char *title, bool directory, int target)
{
    struct zenity_args *za = malloc(sizeof(*za));
    if (!za) return;
    strncpy(za->title, title, sizeof(za->title) - 1);
    za->directory = directory;
    za->target = target;

    pthread_t tid;
    pthread_create(&tid, NULL, zenity_thread, za);
    pthread_detach(tid);
}

/* ═══════════════════════════════════════════════════════════
   8x16 Bitmap Font — ASCII 32-126 (printable characters)
   Each glyph: 16 bytes, 1 byte per row, MSB = leftmost pixel
   ═══════════════════════════════════════════════════════════ */

static const unsigned char font_8x16[95][16] = {
    /* space */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* ! */    {0,0,0,24,24,24,24,24,24,0,24,24,0,0,0,0},
    /* " */    {0,0,108,108,108,72,0,0,0,0,0,0,0,0,0,0},
    /* # */    {0,0,0,108,108,254,108,108,254,108,108,0,0,0,0,0},
    /* $ */    {0,0,16,124,214,208,124,22,214,124,16,0,0,0,0,0},
    /* % */    {0,0,98,214,108,24,48,102,198,0,0,0,0,0,0,0},
    /* & */    {0,0,56,108,56,112,222,204,204,118,0,0,0,0,0,0},
    /* ' */    {0,0,24,24,24,16,0,0,0,0,0,0,0,0,0,0},
    /* ( */    {0,0,12,24,48,48,48,48,48,24,12,0,0,0,0,0},
    /* ) */    {0,0,48,24,12,12,12,12,12,24,48,0,0,0,0,0},
    /* * */    {0,0,0,108,56,254,56,108,0,0,0,0,0,0,0,0},
    /* + */    {0,0,0,24,24,126,24,24,0,0,0,0,0,0,0,0},
    /* , */    {0,0,0,0,0,0,0,0,24,24,48,0,0,0,0,0},
    /* - */    {0,0,0,0,0,126,0,0,0,0,0,0,0,0,0,0},
    /* . */    {0,0,0,0,0,0,0,0,24,24,0,0,0,0,0,0},
    /* / */    {0,0,6,12,12,24,24,48,48,96,96,0,0,0,0,0},
    /* 0 */    {0,0,56,108,198,198,214,198,198,108,56,0,0,0,0,0},
    /* 1 */    {0,0,24,56,120,24,24,24,24,24,126,0,0,0,0,0},
    /* 2 */    {0,0,124,198,6,12,24,48,96,192,254,0,0,0,0,0},
    /* 3 */    {0,0,124,198,6,60,6,6,198,198,124,0,0,0,0,0},
    /* 4 */    {0,0,12,28,60,108,204,254,12,12,30,0,0,0,0,0},
    /* 5 */    {0,0,254,192,192,252,6,6,6,198,124,0,0,0,0,0},
    /* 6 */    {0,0,60,96,192,252,198,198,198,198,124,0,0,0,0,0},
    /* 7 */    {0,0,254,6,12,12,24,24,48,48,48,0,0,0,0,0},
    /* 8 */    {0,0,124,198,198,124,198,198,198,198,124,0,0,0,0,0},
    /* 9 */    {0,0,124,198,198,198,126,6,12,24,112,0,0,0,0,0},
    /* : */    {0,0,0,0,24,24,0,0,24,24,0,0,0,0,0,0},
    /* ; */    {0,0,0,0,24,24,0,0,24,24,48,0,0,0,0,0},
    /* < */    {0,0,6,12,24,48,96,48,24,12,6,0,0,0,0,0},
    /* = */    {0,0,0,0,0,126,0,0,126,0,0,0,0,0,0,0},
    /* > */    {0,0,96,48,24,12,6,12,24,48,96,0,0,0,0,0},
    /* ? */    {0,0,124,198,6,12,24,24,0,24,24,0,0,0,0,0},
    /* @ */    {0,0,124,198,214,222,222,220,192,124,0,0,0,0,0,0},
    /* A */    {0,0,56,108,198,198,254,198,198,198,0,0,0,0,0,0},
    /* B */    {0,0,252,102,102,124,102,102,102,252,0,0,0,0,0,0},
    /* C */    {0,0,60,102,192,192,192,192,102,60,0,0,0,0,0,0},
    /* D */    {0,0,248,108,102,102,102,102,108,248,0,0,0,0,0,0},
    /* E */    {0,0,254,192,192,248,192,192,192,254,0,0,0,0,0,0},
    /* F */    {0,0,254,192,192,248,192,192,192,192,0,0,0,0,0,0},
    /* G */    {0,0,60,102,192,192,206,102,102,60,0,0,0,0,0,0},
    /* H */    {0,0,198,198,198,254,198,198,198,198,0,0,0,0,0,0},
    /* I */    {0,0,126,24,24,24,24,24,24,126,0,0,0,0,0,0},
    /* J */    {0,0,30,12,12,12,12,204,204,120,0,0,0,0,0,0},
    /* K */    {0,0,198,204,216,240,240,216,204,198,0,0,0,0,0,0},
    /* L */    {0,0,192,192,192,192,192,192,192,254,0,0,0,0,0,0},
    /* M */    {0,0,198,238,254,214,198,198,198,198,0,0,0,0,0,0},
    /* N */    {0,0,198,230,246,222,206,198,198,198,0,0,0,0,0,0},
    /* O */    {0,0,124,198,198,198,198,198,198,124,0,0,0,0,0,0},
    /* P */    {0,0,252,102,102,124,96,96,96,96,0,0,0,0,0,0},
    /* Q */    {0,0,124,198,198,198,214,222,124,14,0,0,0,0,0,0},
    /* R */    {0,0,252,102,102,124,120,108,102,102,0,0,0,0,0,0},
    /* S */    {0,0,124,198,224,124,14,6,198,124,0,0,0,0,0,0},
    /* T */    {0,0,126,24,24,24,24,24,24,24,0,0,0,0,0,0},
    /* U */    {0,0,198,198,198,198,198,198,198,124,0,0,0,0,0,0},
    /* V */    {0,0,198,198,198,198,198,108,56,16,0,0,0,0,0,0},
    /* W */    {0,0,198,198,198,198,214,254,108,108,0,0,0,0,0,0},
    /* X */    {0,0,198,198,108,56,56,108,198,198,0,0,0,0,0,0},
    /* Y */    {0,0,102,102,102,60,24,24,24,24,0,0,0,0,0,0},
    /* Z */    {0,0,254,6,12,24,48,96,192,254,0,0,0,0,0,0},
    /* [ */    {0,0,124,96,96,96,96,96,96,124,0,0,0,0,0,0},
    /* \ */    {0,0,96,48,48,24,24,12,12,6,6,0,0,0,0,0},
    /* ] */    {0,0,124,12,12,12,12,12,12,124,0,0,0,0,0,0},
    /* ^ */    {0,0,16,56,108,198,0,0,0,0,0,0,0,0,0,0},
    /* _ */    {0,0,0,0,0,0,0,0,0,0,0,254,0,0,0,0},
    /* ` */    {0,0,48,24,12,0,0,0,0,0,0,0,0,0,0,0},
    /* a */    {0,0,0,0,60,6,62,102,102,62,0,0,0,0,0,0},
    /* b */    {0,0,96,96,124,102,102,102,102,124,0,0,0,0,0,0},
    /* c */    {0,0,0,0,60,102,192,192,102,60,0,0,0,0,0,0},
    /* d */    {0,0,6,6,62,102,102,102,102,62,0,0,0,0,0,0},
    /* e */    {0,0,0,0,60,102,126,192,102,60,0,0,0,0,0,0},
    /* f */    {0,0,28,48,48,252,48,48,48,48,0,0,0,0,0,0},
    /* g */    {0,0,0,0,62,102,102,62,6,102,60,0,0,0,0,0},
    /* h */    {0,0,96,96,124,102,102,102,102,102,0,0,0,0,0,0},
    /* i */    {0,0,24,0,56,24,24,24,24,60,0,0,0,0,0,0},
    /* j */    {0,0,12,0,28,12,12,12,108,108,56,0,0,0,0,0},
    /* k */    {0,0,96,96,102,108,120,108,102,102,0,0,0,0,0,0},
    /* l */    {0,0,56,24,24,24,24,24,24,60,0,0,0,0,0,0},
    /* m */    {0,0,0,0,236,254,214,214,198,198,0,0,0,0,0,0},
    /* n */    {0,0,0,0,124,102,102,102,102,102,0,0,0,0,0,0},
    /* o */    {0,0,0,0,60,102,102,102,102,60,0,0,0,0,0,0},
    /* p */    {0,0,0,0,124,102,102,124,96,96,96,0,0,0,0,0},
    /* q */    {0,0,0,0,62,102,102,62,6,6,6,0,0,0,0,0},
    /* r */    {0,0,0,0,108,118,96,96,96,96,0,0,0,0,0,0},
    /* s */    {0,0,0,0,62,64,60,2,124,0,0,0,0,0,0,0},
    /* t */    {0,0,48,48,252,48,48,48,48,28,0,0,0,0,0,0},
    /* u */    {0,0,0,0,102,102,102,102,102,62,0,0,0,0,0,0},
    /* v */    {0,0,0,0,102,102,102,60,24,24,0,0,0,0,0,0},
    /* w */    {0,0,0,0,198,198,214,254,108,68,0,0,0,0,0,0},
    /* x */    {0,0,0,0,102,60,24,60,102,0,0,0,0,0,0,0},
    /* y */    {0,0,0,0,102,102,102,62,6,102,60,0,0,0,0,0},
    /* z */    {0,0,0,0,126,12,24,48,126,0,0,0,0,0,0,0},
    /* { */    {0,0,14,24,24,112,24,24,14,0,0,0,0,0,0,0},
    /* | */    {0,0,24,24,24,24,24,24,24,24,24,0,0,0,0,0},
    /* } */    {0,0,112,24,24,14,24,24,112,0,0,0,0,0,0,0},
    /* ~ */    {0,0,98,244,140,0,0,0,0,0,0,0,0,0,0,0},
};

#define FONT_W  8
#define FONT_H  16
#define FONT_GLYPHS 95
#define FONT_START   32

/* ── Global font state ─────────────────────────────────────── */
static SDL_Texture *glyph_tex[FONT_GLYPHS];

/* ── Init / cleanup ────────────────────────────────────────── */

int ui_init(void)
{
    /* Font initialization: create glyph textures */
    /* We'll create these lazily since we need a renderer */
    memset(glyph_tex, 0, sizeof(glyph_tex));
    return 0;
}

void ui_cleanup(void)
{
    for (int i = 0; i < FONT_GLYPHS; i++) {
        if (glyph_tex[i]) SDL_DestroyTexture(glyph_tex[i]);
        glyph_tex[i] = NULL;
    }
}

/* ── Drawing primitives ────────────────────────────────────── */

void ui_draw_rect(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

/* Ensure glyph textures are created (needs renderer) */
static void ensure_glyph(SDL_Renderer *r, int idx)
{
    if (idx < 0 || idx >= FONT_GLYPHS) return;
    if (glyph_tex[idx]) return;

    /* Create a surface with the glyph */
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, FONT_W, FONT_H, 32,
        SDL_PIXELFORMAT_RGBA32);
    if (!surf) return;

    const unsigned char *rows = font_8x16[idx];
    for (int row = 0; row < FONT_H; row++) {
        unsigned char byte = rows[row];
        for (int col = 0; col < FONT_W; col++) {
            uint32_t *pixel = (uint32_t *)((uint8_t *)surf->pixels +
                row * surf->pitch + col * 4);
            if (byte & (0x80 >> col))
                *pixel = 0xFFFFFFFF;  /* white glyph */
            else
                *pixel = 0x00000000;  /* transparent */
        }
    }

    glyph_tex[idx] = SDL_CreateTextureFromSurface(r, surf);
    SDL_FreeSurface(surf);

    /* Set texture blend mode so we can color it */
    SDL_SetTextureBlendMode(glyph_tex[idx], SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(glyph_tex[idx], 255, 255, 255);
}

void ui_draw_text(SDL_Renderer *r, const char *text, int x, int y, SDL_Color c)
{
    if (!text || !text[0]) return;

    int cx = x;
    for (const char *p = text; *p; p++) {
        int idx = (unsigned char)*p - FONT_START;
        if (idx < 0 || idx >= FONT_GLYPHS) {
            if (*p == '\n') { cx = x; y += FONT_H + 4; continue; }
            cx += FONT_W + 1;
            continue;
        }
        ensure_glyph(r, idx);
        if (glyph_tex[idx]) {
            SDL_SetTextureColorMod(glyph_tex[idx], c.r, c.g, c.b);
            SDL_Rect dst = {cx, y, FONT_W, FONT_H};
            SDL_RenderCopy(r, glyph_tex[idx], NULL, &dst);
        }
        cx += FONT_W + 1;
    }
}

void ui_text_size(const char *text, int *w, int *h)
{
    if (w) *w = (text ? (int)strlen(text) * (FONT_W + 1) : 0);
    if (h) *h = FONT_H;
}

bool ui_in_rect(int mx, int my, int x, int y, int w, int h)
{
    return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

/* ── Text field widget ──────────────────────────────────────── */

/* Render a text field with focus border and blinking cursor */
static void ui_text_field(SDL_Renderer *r, int x, int y, int w, int h,
                          const char *text, bool focused, int cursor_pos,
                          const char *placeholder)
{
    SDL_Color border = focused ? COLOR_ACCENT : COLOR_DIM;
    SDL_Color bg = focused ? COLOR_HOVER : COLOR_SURFACE;

    /* Background */
    ui_draw_rect(r, x, y, w, h, bg);

    /* Border (2px when focused) */
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    SDL_Rect brect = {x, y, w, h};
    SDL_RenderDrawRect(r, &brect);
    if (focused) {
        /* Double border for emphasis */
        SDL_Rect brect2 = {x + 1, y + 1, w - 2, h - 2};
        SDL_RenderDrawRect(r, &brect2);
    }

    /* Text */
    const char *display = (text && text[0]) ? text : placeholder;
    SDL_Color tc = (text && text[0]) ? COLOR_TEXT : COLOR_DIM;
    int tx = x + 6;
    int ty = y + (h - FONT_H) / 2;
    ui_draw_text(r, display, tx, ty, tc);

    /* Blinking cursor */
    if (focused) {
        uint32_t ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) {
            /* Calculate cursor X position based on text before cursor */
            char before[128];
            int cp = cursor_pos;
            if (cp < 0) cp = 0;
            if (cp > (int)strlen(display)) cp = (int)strlen(display);
            strncpy(before, display, cp);
            before[cp] = '\0';
            int cw, ch;
            ui_text_size(before, &cw, &ch);
            int cx = tx + cw;
            SDL_SetRenderDrawColor(r, COLOR_TEXT.r, COLOR_TEXT.g, COLOR_TEXT.b, COLOR_TEXT.a);
            SDL_RenderDrawLine(r, cx, y + 4, cx, y + h - 4);
        }
    }
}

/* Handle click on text field — returns true if field was activated */
static bool ui_text_field_click(struct app_state *st, int mx, int my,
                                int x, int y, int w, int h,
                                int field_id, const char *value)
{
    if (!ui_in_rect(mx, my, x, y, w, h)) return false;

    st->active_input = field_id;
    strncpy(st->input_buffer, value, sizeof(st->input_buffer) - 1);
    st->input_cursor = strlen(st->input_buffer);
    SDL_StartTextInput();
    return true;
}

/* ── Button helper ─────────────────────────────────────────── */

static bool ui_button(SDL_Renderer *r, const char *label, int x, int y, int w, int h,
                      int mx, int my, bool clicked)
{
    (void)clicked;
    bool hover = ui_in_rect(mx, my, x, y, w, h);
    SDL_Color bg = hover ? COLOR_HOVER : COLOR_SURFACE;
    SDL_Color border = hover ? COLOR_ACCENT : COLOR_DIM;
    ui_draw_rect(r, x, y, w, h, bg);
    /* Border */
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    SDL_Rect brect = {x, y, w, h};
    SDL_RenderDrawRect(r, &brect);

    int tw, th;
    ui_text_size(label, &tw, &th);
    ui_draw_text(r, label, x + (w - tw) / 2, y + (h - th) / 2, COLOR_TEXT);
    return hover && clicked;
}

/* ── Tab bar ───────────────────────────────────────────────── */

static const char *tab_labels[] = {
    "Scan Devices", "Send File", "Receive File", "History"
};

static void render_tab_bar(SDL_Renderer *r, struct app_state *st)
{
    int tab_w = st->window_w / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        SDL_Color bg = (i == st->current_tab) ? COLOR_ACCENT : COLOR_SURFACE;
        SDL_Color txt = (i == st->current_tab) ? COLOR_BG : COLOR_TEXT;
        ui_draw_rect(r, i * tab_w, 0, tab_w, 36, bg);
        int tw, th;
        ui_text_size(tab_labels[i], &tw, &th);
        ui_draw_text(r, tab_labels[i],
                     i * tab_w + (tab_w - tw) / 2, (36 - th) / 2, txt);
    }
    SDL_SetRenderDrawColor(r, COLOR_DIM.r, COLOR_DIM.g, COLOR_DIM.b, COLOR_DIM.a);
    SDL_RenderDrawLine(r, 0, 36, st->window_w, 36);
}

/* ── Scan page ─────────────────────────────────────────────── */

static void render_scan_page(SDL_Renderer *r, struct app_state *st)
{
    int list_y = 88, list_h = st->window_h - 128;

    ui_button(r, "Scan", 20, 46, 100, 32, 0, 0, false);
    ui_draw_text(r, "Port:", 128, 52, COLOR_TEXT);
    {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", st->scan_port);
        ui_text_field(r, 175, 46, 65, 28, pbuf,
                      st->active_input == 7, st->active_input == 7 ? st->input_cursor : 0, "9876");
    }
    ui_draw_text(r, st->scan_status, 260, 52, COLOR_DIM);

    ui_draw_rect(r, 20, list_y, st->window_w - 40, list_h, COLOR_SURFACE);
    ui_draw_text(r, "IP Address", 30, list_y + 5, COLOR_DIM);
    ui_draw_text(r, "Hostname", 220, list_y + 5, COLOR_DIM);

    for (int i = 0; i < st->device_count; i++) {
        int ey = list_y + 28 + i * 30;
        if (ey > list_y + list_h - 30) break;
        SDL_Color c = (i == st->selected_device) ? COLOR_ACCENT : COLOR_TEXT;
        ui_draw_text(r, st->devices[i].ip, 30, ey, c);
        ui_draw_text(r, st->devices[i].hostname, 220, ey, c);
    }

    if (st->device_count == 0) {
        ui_draw_text(r, "No devices found. Click Scan to search.",
                     30, list_y + 30, COLOR_DIM);
    }
}

/* ── Send page ─────────────────────────────────────────────── */

static void render_send_page(SDL_Renderer *r, struct app_state *st)
{
    int W = st->window_w;
    int y = 50;

    ui_draw_text(r, "File:", 20, y + 4, COLOR_TEXT);
    ui_draw_rect(r, 80, y, W - 240, 28, COLOR_SURFACE);
    ui_draw_text(r, st->send_filepath[0] ? st->send_filepath : "Click Browse...",
                 86, y + 4, st->send_filepath[0] ? COLOR_TEXT : COLOR_DIM);
    ui_button(r, "Browse", W - 150, y, 80, 28, 0, 0, false);
    y += 40;

    ui_draw_text(r, "Protocol:", 20, y + 4, COLOR_TEXT);
    ui_button(r, "TCP", 120, y, 60, 28, 0, 0, false);
    ui_button(r, "UDP", 190, y, 60, 28, 0, 0, false);
    { SDL_Color sel = COLOR_ACCENT;
      SDL_SetRenderDrawColor(r, sel.r, sel.g, sel.b, sel.a);
      SDL_Rect pr = {st->send_protocol == 0 ? 120 : 190, y, 60, 28};
      SDL_RenderDrawRect(r, &pr); }
    y += 40;

    /* IP + Port row */
    {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", st->send_port);
        ui_draw_text(r, "IP:", 20, y + 4, COLOR_TEXT);
        ui_text_field(r, 55, y, 170, 28, st->send_target_ip,
                      st->active_input == 2, st->active_input == 2 ? st->input_cursor : 0,
                      "127.0.0.1");
        ui_draw_text(r, "Port:", 235, y + 4, COLOR_TEXT);
        ui_text_field(r, 285, y, 70, 28, pbuf,
                      st->active_input == 5, st->active_input == 5 ? st->input_cursor : 0,
                      "9876");
    }
    y += 40;

    /* Send button */
    {
        bool en = st->send_filepath[0] && st->send_target_ip[0] && !st->send_running;
        SDL_Color bg = en ? COLOR_ACCENT : COLOR_SURFACE;
        SDL_Color tc = en ? COLOR_BG : COLOR_DIM;
        ui_draw_rect(r, 20, y, 160, 32, bg);
        int tw, th; ui_text_size("Start Send", &tw, &th);
        ui_draw_text(r, "Start Send", 20 + (160 - tw) / 2, y + (32 - th) / 2, tc);
    }
    y += 50;

    /* Progress bar */
    if (st->send_running) {
        int bar_w = W - 40;
        ui_draw_rect(r, 20, y, bar_w, 28, COLOR_SURFACE);
        if (st->send_progress_total > 0) {
            double pct = (double)st->send_progress_done / st->send_progress_total;
            int fill_w = (int)(bar_w * pct);
            if (fill_w > 0) ui_draw_rect(r, 20, y, fill_w, 28, COLOR_PROGRESS);
            char pbuf[128];
            snprintf(pbuf, sizeof(pbuf), "%.0f%% (%lu / %lu bytes)",
                     pct * 100.0,
                     (unsigned long)st->send_progress_done,
                     (unsigned long)st->send_progress_total);
            int tw, th; ui_text_size(pbuf, &tw, &th);
            ui_draw_text(r, pbuf, 20 + (bar_w - tw) / 2, y + 4, COLOR_TEXT);
        }
    }
}

/* ── Receive page ──────────────────────────────────────────── */

static void render_receive_page(SDL_Renderer *r, struct app_state *st)
{
    int W = st->window_w;
    int y = 50;

    ui_draw_text(r, "Save to:", 20, y + 4, COLOR_TEXT);
    ui_draw_rect(r, 100, y, W - 260, 28, COLOR_SURFACE);
    ui_draw_text(r, st->recv_savepath[0] ? st->recv_savepath : "Click Browse...",
                 106, y + 4, st->recv_savepath[0] ? COLOR_TEXT : COLOR_DIM);
    ui_button(r, "Browse", W - 150, y, 80, 28, 0, 0, false);
    y += 40;

    ui_draw_text(r, "Protocol:", 20, y + 4, COLOR_TEXT);
    ui_button(r, "TCP", 120, y, 60, 28, 0, 0, false);
    ui_button(r, "UDP", 190, y, 60, 28, 0, 0, false);
    { SDL_Color sel = COLOR_ACCENT;
      SDL_SetRenderDrawColor(r, sel.r, sel.g, sel.b, sel.a);
      SDL_Rect pr = {st->recv_protocol == 0 ? 120 : 190, y, 60, 28};
      SDL_RenderDrawRect(r, &pr); }
    y += 40;

    /* IP + Port row */
    {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", st->recv_port);
        ui_draw_text(r, "IP:", 20, y + 4, COLOR_TEXT);
        ui_text_field(r, 55, y, 170, 28, st->recv_target_ip,
                      st->active_input == 4, st->active_input == 4 ? st->input_cursor : 0,
                      "127.0.0.1");
        ui_draw_text(r, "Port:", 235, y + 4, COLOR_TEXT);
        ui_text_field(r, 285, y, 70, 28, pbuf,
                      st->active_input == 6, st->active_input == 6 ? st->input_cursor : 0,
                      "9876");
    }
    y += 40;

    /* Listen button */
    {
        bool en = st->recv_savepath[0] && st->recv_target_ip[0] && !st->recv_running;
        SDL_Color bg = en ? COLOR_ACCENT : COLOR_SURFACE;
        SDL_Color tc = en ? COLOR_BG : COLOR_DIM;
        ui_draw_rect(r, 20, y, 180, 32, bg);
        int tw, th; ui_text_size("Listen & Receive", &tw, &th);
        ui_draw_text(r, "Listen & Receive", 20 + (180 - tw) / 2, y + (32 - th) / 2, tc);
    }
    y += 50;

    if (st->recv_running) {
        int bar_w = W - 40;
        ui_draw_rect(r, 20, y, bar_w, 28, COLOR_SURFACE);
        if (st->recv_progress_total > 0) {
            double pct = (double)st->recv_progress_done / st->recv_progress_total;
            int fill_w = (int)(bar_w * pct);
            if (fill_w > 0) ui_draw_rect(r, 20, y, fill_w, 28, COLOR_PROGRESS);
            char pbuf[128];
            snprintf(pbuf, sizeof(pbuf), "%.0f%% (%lu / %lu bytes)",
                     pct * 100.0,
                     (unsigned long)st->recv_progress_done,
                     (unsigned long)st->recv_progress_total);
            int tw, th; ui_text_size(pbuf, &tw, &th);
            ui_draw_text(r, pbuf, 20 + (bar_w - tw) / 2, y + 4, COLOR_TEXT);
        }
    }
}

/* ── History persistence ───────────────────────────────────── */

#define HISTORY_FILE "lanft_history.dat"

void history_load(struct app_state *st)
{
    st->history_count = 0;
    char path[512];
    snprintf(path, sizeof(path), "./" HISTORY_FILE);
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp) && st->history_count < 256) {
        struct hist_entry *r = &st->history[st->history_count];
        unsigned long dur, spd;
        int n = sscanf(line, "%255[^|]|%31[^|]|%31[^|]|%lu|%d|%d|%d|%d|%lu",
                       r->name, r->start_time, r->end_time,
                       &dur, &r->kind, &r->port,
                       &r->status, &r->progress, &spd);
        if (n >= 9) { r->duration_ms = dur; r->speed = spd; st->history_count++; }
    }
    fclose(fp);
}

void history_save(struct app_state *st)
{
    char path[512];
    snprintf(path, sizeof(path), "./" HISTORY_FILE);
    FILE *fp = fopen(path, "w");
    if (!fp) return;

    for (int i = 0; i < st->history_count; i++) {
        struct hist_entry *r = &st->history[i];
        fprintf(fp, "%s|%s|%s|%lu|%d|%d|%d|%d|%lu\n",
                r->name, r->start_time[0] ? r->start_time : "-",
                r->end_time[0] ? r->end_time : "-",
                (unsigned long)r->duration_ms, r->kind, r->port,
                r->status, r->progress, (unsigned long)r->speed);
    }
    fclose(fp);
}

/* ── History page ──────────────────────────────────────────── */

static void render_history_page(SDL_Renderer *r, struct app_state *st)
{
    int W = st->window_w;
    int header_y = 50;
    int row_h = 22;
    static const int col_x[] = {10, 160, 280, 380, 460, 520, 580, 640, 720};
    static const char *headers[] = {"Name","Start","End","Dur(ms)","Kind","Port","Status","%","Speed"};

    /* Header row */
    ui_draw_rect(r, 0, header_y, W, row_h, COLOR_SURFACE);
    for (int c = 0; c < 9; c++) {
        ui_draw_text(r, headers[c], col_x[c], header_y + 3, COLOR_ACCENT);
    }
    /* Separator */
    SDL_SetRenderDrawColor(r, COLOR_DIM.r, COLOR_DIM.g, COLOR_DIM.b, COLOR_DIM.a);
    SDL_RenderDrawLine(r, 0, header_y + row_h, W, header_y + row_h);

    /* Rows */
    int list_start = header_y + row_h + 2;
    int max_rows = (st->window_h - list_start - 32) / row_h;
    for (int i = 0; i < st->history_count && i < max_rows; i++) {
        int ry = list_start + i * row_h;
        bool even = (i % 2 == 0);
        if (even) ui_draw_rect(r, 0, ry, W, row_h, COLOR_BG);

        struct hist_entry *e = &st->history[i];
        SDL_Color c = (e->status == 0) ? COLOR_TEXT : (e->status == 1 ? COLOR_ERROR : COLOR_DIM);

        char buf[64];
        ui_draw_text(r, e->name, col_x[0], ry + 3, c);
        ui_draw_text(r, e->start_time, col_x[1], ry + 3, c);
        ui_draw_text(r, e->end_time, col_x[2], ry + 3, c);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)e->duration_ms);
        ui_draw_text(r, buf, col_x[3], ry + 3, c);
        ui_draw_text(r, e->kind == 0 ? "SEND" : "RECV", col_x[4], ry + 3,
                     e->kind == 0 ? COLOR_ACCENT : COLOR_PROGRESS);
        snprintf(buf, sizeof(buf), "%d", e->port); ui_draw_text(r, buf, col_x[5], ry + 3, c);
        ui_draw_text(r, e->status == 0 ? "OK" : (e->status == 1 ? "BAD" : "STOP"), col_x[6], ry + 3, c);
        snprintf(buf, sizeof(buf), "%d%%", e->progress); ui_draw_text(r, buf, col_x[7], ry + 3, c);
        if (e->speed > 0) {
            if (e->speed >= 1048576) snprintf(buf, sizeof(buf), "%.1fMB/s", e->speed / 1048576.0);
            else if (e->speed >= 1024) snprintf(buf, sizeof(buf), "%luKB/s", (unsigned long)(e->speed / 1024));
            else snprintf(buf, sizeof(buf), "%luB/s", (unsigned long)e->speed);
        } else strncpy(buf, "-", sizeof(buf));
        ui_draw_text(r, buf, col_x[8], ry + 3, c);
    }

    if (st->history_count == 0) {
        ui_draw_text(r, "No transfer history yet.", 20, list_start + 10, COLOR_DIM);
    }
}

/* ── Main render ───────────────────────────────────────────── */

void ui_render(SDL_Renderer *renderer, struct app_state *state)
{
    ui_draw_rect(renderer, 0, 0, state->window_w, state->window_h, COLOR_BG);
    render_tab_bar(renderer, state);

    switch (state->current_tab) {
    case TAB_SCAN:    render_scan_page(renderer, state); break;
    case TAB_SEND:    render_send_page(renderer, state); break;
    case TAB_RECEIVE: render_receive_page(renderer, state); break;
    case TAB_HISTORY: render_history_page(renderer, state); break;
    }

    /* Modal overlay */
    if (state->modal_visible) {
        ui_draw_rect(renderer, 0, 0, state->window_w, state->window_h,
                     (SDL_Color){0, 0, 0, 180});
        int mx = state->window_w / 2 - 200, my = state->window_h / 2 - 60;
        ui_draw_rect(renderer, mx, my, 400, 120, COLOR_SURFACE);
        ui_draw_text(renderer, state->modal_message, mx + 20, my + 20, COLOR_TEXT);
        ui_button(renderer, "OK", mx + 160, my + 70, 80, 30, 0, 0, false);
    }

    /* Status bar */
    int sh = state->window_h - 28;
    ui_draw_rect(renderer, 0, sh, state->window_w, 28, COLOR_SURFACE);
    SDL_SetRenderDrawColor(renderer, COLOR_DIM.r, COLOR_DIM.g, COLOR_DIM.b, COLOR_DIM.a);
    SDL_RenderDrawLine(renderer, 0, sh, state->window_w, sh);
    ui_draw_text(renderer, state->status_text, 10, sh + 6, COLOR_DIM);

    SDL_RenderPresent(renderer);
}

/* ── Event handling ────────────────────────────────────────── */

bool ui_handle_event(SDL_Event *e, struct app_state *st)
{
    int mx = 0, my = 0;
    if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP) {
        mx = e->button.x;
        my = e->button.y;
    }

    /* Tab bar clicks */
    if (e->type == SDL_MOUSEBUTTONDOWN && my < 36) {
        int tab_w = st->window_w / TAB_COUNT;
        st->current_tab = mx / tab_w;
        return true;
    }

    /* Modal overlay */
    if (st->modal_visible) {
        if (e->type == SDL_MOUSEBUTTONDOWN) {
            int bx = st->window_w / 2 - 200 + 160;
            int by = st->window_h / 2 - 60 + 70;
            if (ui_in_rect(mx, my, bx, by, 80, 30)) {
                st->modal_visible = false;
            }
        }
        return true;
    }

    /* Per-tab clicks */
    if (e->type == SDL_MOUSEBUTTONDOWN) {
        switch (st->current_tab) {
        case TAB_SCAN:
            if (ui_in_rect(mx, my, 20, 46, 100, 32)) {
                strncpy(st->scan_status, "Scanning...", sizeof(st->scan_status) - 1);
                st->device_count = 0;
                st->selected_device = -1;
                scanner_start((uint16_t)st->scan_port);
                return true;
            }
            /* Scan port field */
            {
                char spbuf[16]; snprintf(spbuf, sizeof(spbuf), "%d", st->scan_port);
                if (ui_text_field_click(st, mx, my, 175, 46, 65, 28, 7, spbuf))
                    return true;
            }
            /* Device list selection */
            {
                int list_y = 88;
                for (int i = 0; i < st->device_count; i++) {
                    int ey = list_y + 28 + i * 30;
                    if (ui_in_rect(mx, my, 20, ey, st->window_w - 40, 28)) {
                        st->selected_device = i;
                        strncpy(st->send_target_ip, st->devices[i].ip,
                                sizeof(st->send_target_ip) - 1);
                        strncpy(st->recv_target_ip, st->devices[i].ip,
                                sizeof(st->recv_target_ip) - 1);
                        snprintf(st->status_text, sizeof(st->status_text),
                                 "Selected %s", st->devices[i].ip);
                        return true;
                    }
                }
            }
            break;

        case TAB_SEND:
            if (ui_in_rect(mx, my, st->window_w - 150, 50, 80, 28)) {
                zenity_launch(st, "Select File to Send", false, 1);
                return true;
            }
            if (ui_in_rect(mx, my, 120, 90, 60, 28)) { st->send_protocol = 0; return true; }
            if (ui_in_rect(mx, my, 190, 90, 60, 28)) { st->send_protocol = 1; return true; }
            {
                char spbuf[16]; snprintf(spbuf, sizeof(spbuf), "%d", st->send_port);
                if (ui_text_field_click(st, mx, my, 55, 130, 170, 28, 2, st->send_target_ip))
                    return true;
                if (ui_text_field_click(st, mx, my, 285, 130, 70, 28, 5, spbuf))
                    return true;
            }
            if (ui_in_rect(mx, my, 20, 170, 160, 32) &&
                st->send_filepath[0] && st->send_target_ip[0] && !st->send_running) {
                /* Main loop will detect send_running=true and spawn thread */
                st->send_running = true;
                st->send_progress_done = 0;
                st->send_progress_total = 0;
                st->active_input = 0;
                SDL_StopTextInput();
                strncpy(st->status_text, "Connecting...", sizeof(st->status_text) - 1);
                return true;
            }
            break;

        case TAB_RECEIVE:
            if (ui_in_rect(mx, my, st->window_w - 150, 50, 80, 28)) {
                zenity_launch(st, "Select Save Directory", true, 2);
                return true;
            }
            if (ui_in_rect(mx, my, 120, 90, 60, 28)) { st->recv_protocol = 0; return true; }
            if (ui_in_rect(mx, my, 190, 90, 60, 28)) { st->recv_protocol = 1; return true; }
            {
                char rpbuf[16]; snprintf(rpbuf, sizeof(rpbuf), "%d", st->recv_port);
                if (ui_text_field_click(st, mx, my, 55, 130, 170, 28, 4, st->recv_target_ip))
                    return true;
                if (ui_text_field_click(st, mx, my, 285, 130, 70, 28, 6, rpbuf))
                    return true;
            }
            if (ui_in_rect(mx, my, 20, 170, 180, 32) &&
                st->recv_savepath[0] && st->recv_target_ip[0] && !st->recv_running) {
                st->recv_running = true;
                st->recv_progress_done = 0;
                st->recv_progress_total = 0;
                st->active_input = 0;
                SDL_StopTextInput();
                strncpy(st->status_text, "Waiting for sender...", sizeof(st->status_text) - 1);
                return true;
            }
            break;
        }
    }

    /* Keyboard input for text fields */
    if (e->type == SDL_TEXTINPUT && st->active_input > 0) {
        size_t len = strlen(st->input_buffer);
        size_t tlen = strlen(e->text.text);
        if (len + tlen < sizeof(st->input_buffer) - 1) {
            strcat(st->input_buffer, e->text.text);
            st->input_cursor = strlen(st->input_buffer);
            if (st->active_input == 2)
                strncpy(st->send_target_ip, st->input_buffer, sizeof(st->send_target_ip) - 1);
            if (st->active_input == 4)
                strncpy(st->recv_target_ip, st->input_buffer, sizeof(st->recv_target_ip) - 1);
            if (st->active_input == 5)
                st->send_port = atoi(st->input_buffer);
            if (st->active_input == 6)
                st->recv_port = atoi(st->input_buffer);
            if (st->active_input == 7)
                st->scan_port = atoi(st->input_buffer);
            if (st->active_input == 7)
                st->scan_port = atoi(st->input_buffer);
        }
        return true;
    }

    if (e->type == SDL_KEYDOWN) {
        if (e->key.keysym.sym == SDLK_BACKSPACE && st->active_input > 0) {
            int len = strlen(st->input_buffer);
            if (len > 0 && st->input_cursor > 0) {
                memmove(st->input_buffer + st->input_cursor - 1,
                        st->input_buffer + st->input_cursor,
                        len - st->input_cursor + 1);
                st->input_cursor--;
            }
            if (st->active_input == 2)
                strncpy(st->send_target_ip, st->input_buffer, sizeof(st->send_target_ip) - 1);
            if (st->active_input == 4)
                strncpy(st->recv_target_ip, st->input_buffer, sizeof(st->recv_target_ip) - 1);
            if (st->active_input == 5)
                st->send_port = atoi(st->input_buffer);
            if (st->active_input == 6)
                st->recv_port = atoi(st->input_buffer);
            if (st->active_input == 7)
                st->scan_port = atoi(st->input_buffer);
            return true;
        }
        if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_ESCAPE) {
            st->active_input = 0;
            SDL_StopTextInput();
            return true;
        }
    }

    return false;
}
