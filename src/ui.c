#include "ui.h"
#include "protocol.h"
#include "scanner.h"
#include "transfer.h"
#include "network.h"
#include "compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#ifdef HAVE_SDL2_TTF
#include <SDL2/SDL_ttf.h>
#endif

/* ── Font system ──────────────────────────────────────────────── */

#define FONT_W  8
#define FONT_H  16
#define FONT_GLYPHS 95
#define FONT_START   32

/* TTF mode globals */
#ifdef HAVE_SDL2_TTF
static TTF_Font *ttf_font = NULL;
static int       ttf_ptsize = 14;
#endif

/* Try to find a system font that supports CJK.
   Returns path that must be freed by caller, or NULL. */
static char *find_system_font(void)
{
    static const char *candidates[] = {
        /* Linux */
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        /* Windows */
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/arial.ttf",
        /* macOS */
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        FILE *fp = fopen(candidates[i], "rb");
        if (fp) { fclose(fp); return strdup(candidates[i]); }
    }
    return NULL;
}

/* Bitmap font glyph textures */
static SDL_Texture *glyph_tex[FONT_GLYPHS];

/* ── File dialog ─────────────────────────────────────────────── */

#if defined(_WIN32) || defined(_WIN64)
#include <commdlg.h>
#include <shlobj.h>
#define WIN32_FILE_BUF 1
#endif

struct zenity_args {
    char title[256];
    bool directory;
    int  target;  /* 1=send path, 2=recv save path */
};

/* Linux: background thread to run zenity (async) */
#ifndef _WIN32
static void *zenity_thread(void *arg)
{
    struct zenity_args *za = (struct zenity_args *)arg;
    char *path = NULL;
    static char buf[1024];  /* must outlive this function — pushed to SDL event */

    char cmd[512];
    if (za->directory)
        snprintf(cmd, sizeof(cmd), "zenity --file-selection --directory --title=\"%s\" 2>/dev/null", za->title);
    else
        snprintf(cmd, sizeof(cmd), "zenity --file-selection --title=\"%s\" 2>/dev/null", za->title);

    FILE *fp = popen(cmd, "r");
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
#endif /* !_WIN32 */

static void zenity_launch(struct app_state *st, const char *title, bool directory, int target)
{
#if defined(_WIN32) || defined(_WIN64)
    /* Windows: call native dialog synchronously on the MAIN thread.
     * Win32 common dialogs (GetOpenFileName / SHBrowseForFolder)
     * require the calling thread to have an active message pump,
     * which only the main SDL thread has. */
    (void)st;
    static char buf[MAX_PATH];
    char *path = NULL;
    buf[0] = '\0';

    if (directory) {
        BROWSEINFOA bi = {0};
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl) {
            if (SHGetPathFromIDListA(pidl, buf))
                path = buf;
            CoTaskMemFree(pidl);
        }
    } else {
        OPENFILENAMEA ofn = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = buf;
        ofn.lpstrFile[0] = '\0';
        ofn.nMaxFile = sizeof(buf);
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn))
            path = buf;
    }

    /* Push result via SDL event — data1=path or NULL, data2=target */
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = USEREVENT_ZENITY_RESULT;
    event.user.code = target;
    event.user.data1 = path;
    SDL_PushEvent(&event);
#else
    /* Linux: spawn background thread to run zenity (non-blocking) */
    (void)st;
    struct zenity_args *za = malloc(sizeof(*za));
    if (!za) return;
    strncpy(za->title, title, sizeof(za->title) - 1);
    za->directory = directory;
    za->target = target;

    pthread_t tid;
    pthread_create(&tid, NULL, zenity_thread, za);
    pthread_detach(tid);
#endif
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

/* ── Init / cleanup ────────────────────────────────────────── */

int ui_init(void)
{
    memset(glyph_tex, 0, sizeof(glyph_tex));
#ifdef HAVE_SDL2_TTF
    if (TTF_Init() == 0) {
        char *font_path = find_system_font();
        if (font_path) {
            ttf_font = TTF_OpenFont(font_path, ttf_ptsize);
            if (ttf_font) {
                log_write("[ui] loaded system font: %s (%dpt)\n", font_path, ttf_ptsize);
            } else {
                log_write("[ui] TTF_OpenFont failed: %s\n", TTF_GetError());
            }
            free(font_path);
        } else {
            log_write("[ui] no system CJK font found, using built-in bitmap font\n");
        }
        if (!ttf_font) TTF_Quit();  /* no usable font, shutdown TTF */
    } else {
        log_write("[ui] TTF_Init failed: %s\n", TTF_GetError());
    }
#endif
    return 0;
}

void ui_cleanup(void)
{
    for (int i = 0; i < FONT_GLYPHS; i++) {
        if (glyph_tex[i]) SDL_DestroyTexture(glyph_tex[i]);
        glyph_tex[i] = NULL;
    }
#ifdef HAVE_SDL2_TTF
    if (ttf_font) { TTF_CloseFont(ttf_font); ttf_font = NULL; }
    TTF_Quit();
#endif
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

#ifdef HAVE_SDL2_TTF
    if (ttf_font) {
        SDL_Surface *surf = TTF_RenderUTF8_Blended(ttf_font, text, c);
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
            if (tex) {
                SDL_Rect dst = {x, y, surf->w, surf->h};
                SDL_RenderCopy(r, tex, NULL, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_FreeSurface(surf);
        }
        return;
    }
#endif
    /* Fallback: built-in 8x16 bitmap */
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
#ifdef HAVE_SDL2_TTF
    if (ttf_font && text && text[0]) {
        int tw, th;
        if (TTF_SizeUTF8(ttf_font, text, &tw, &th) == 0) {
            if (w) *w = tw;
            if (h) *h = th;
            return;
        }
    }
#endif
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
                          const char *placeholder, bool disabled)
{
    if (disabled) {
        /* Grayed out — disabled state */
        ui_draw_rect(r, x, y, w, h, COLOR_BG);
        SDL_SetRenderDrawColor(r, COLOR_DIM.r, COLOR_DIM.g, COLOR_DIM.b, 100);
        SDL_Rect brect = {x, y, w, h};
        SDL_RenderDrawRect(r, &brect);
        ui_draw_text(r, text && text[0] ? text : placeholder, x + 6, y + (h - FONT_H) / 2, COLOR_DIM);
        return;
    }

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
                                int field_id, const char *value, bool disabled)
{
    if (!ui_in_rect(mx, my, x, y, w, h)) return false;
    if (disabled) return true;  /* consume click but don't activate */

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
    "Scan Devices", "Send File", "Receive File", "History", "Settings"
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
                      st->active_input == 7, st->active_input == 7 ? st->input_cursor : 0, "9876", false);
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

    bool dis = st->send_running;
    SDL_Color ftc = dis ? COLOR_DIM : COLOR_TEXT;
    SDL_Color fbg_c = dis ? COLOR_BG : COLOR_SURFACE;

    ui_draw_text(r, "File:", 20, y + 4, ftc);
    ui_draw_rect(r, 70, y, W - 340, 28, fbg_c);
    ui_draw_text(r, st->send_filepath[0] ? st->send_filepath : "Click Browse...",
                 76, y + 4, st->send_filepath[0] ? ftc : COLOR_DIM);
    /* Dir/File toggle */
    {
        SDL_Color tbg = dis ? COLOR_BG : (st->send_is_dir ? COLOR_ACCENT : COLOR_SURFACE);
        SDL_Color ttc = dis ? COLOR_DIM : (st->send_is_dir ? COLOR_BG : COLOR_TEXT);
        ui_draw_rect(r, W - 260, y, 50, 28, tbg);
        int tw, th; ui_text_size("Dir", &tw, &th);
        ui_draw_text(r, "Dir", W - 260 + (50 - tw) / 2, y + (28 - th) / 2, ttc);
    }
    {
        SDL_Color fbg2 = dis ? COLOR_BG : (st->send_is_dir ? COLOR_SURFACE : COLOR_ACCENT);
        SDL_Color ftc2 = dis ? COLOR_DIM : (st->send_is_dir ? COLOR_TEXT : COLOR_BG);
        ui_draw_rect(r, W - 205, y, 50, 28, fbg2);
        int tw, th; ui_text_size("File", &tw, &th);
        ui_draw_text(r, "File", W - 205 + (50 - tw) / 2, y + (28 - th) / 2, ftc2);
    }
    {
        SDL_Color bb = dis ? COLOR_DIM : COLOR_SURFACE;
        SDL_Color bt = dis ? COLOR_BG : COLOR_TEXT;
        ui_draw_rect(r, W - 150, y, 80, 28, bb);
        int tw, th; ui_text_size("Browse", &tw, &th);
        ui_draw_text(r, "Browse", W - 150 + (80 - tw) / 2, y + (28 - th) / 2, bt);
    }
    y += 40;

    ui_draw_text(r, "Protocol:", 20, y + 4, ftc);
    {
        SDL_Color tcp_bg = dis ? COLOR_BG : COLOR_SURFACE;
        SDL_Color udp_bg = dis ? COLOR_BG : COLOR_SURFACE;
        ui_draw_rect(r, 120, y, 60, 28, tcp_bg);
        ui_draw_text(r, "TCP", 130, y + 4, ftc);
        ui_draw_rect(r, 190, y, 60, 28, udp_bg);
        ui_draw_text(r, "UDP", 200, y + 4, ftc);
    }
    if (!dis) {
        SDL_Color sel = COLOR_ACCENT;
        SDL_SetRenderDrawColor(r, sel.r, sel.g, sel.b, sel.a);
        SDL_Rect pr = {st->send_protocol == 0 ? 120 : 190, y, 60, 28};
        SDL_RenderDrawRect(r, &pr);
    }
    y += 40;

    /* IP + Port row */
    {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", st->send_port);
        ui_draw_text(r, "Receiver IP:", 20, y + 4, COLOR_TEXT);
        ui_text_field(r, 55, y, 170, 28, st->send_target_ip,
                      st->active_input == 2 && !st->send_running,
                      st->active_input == 2 ? st->input_cursor : 0,
                      "receiver ip", st->send_running);
        ui_draw_text(r, "Port:", 235, y + 4, COLOR_TEXT);
        ui_text_field(r, 285, y, 70, 28, pbuf,
                      st->active_input == 5 && !st->send_running,
                      st->active_input == 5 ? st->input_cursor : 0,
                      "9876", st->send_running);
    }
    y += 40;

    /* Send/Stop button */
    {
        ui_draw_rect(r, 20, y, 160, 32, st->send_running ? COLOR_ERROR : COLOR_ACCENT);
        int tw, th;
        const char *label = st->send_running ? "Stop Sending" : "Start Send";
        ui_text_size(label, &tw, &th);
        ui_draw_text(r, label, 20 + (160 - tw) / 2, y + (32 - th) / 2, COLOR_BG);
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

    bool rdis = st->recv_running;
    SDL_Color rftc = rdis ? COLOR_DIM : COLOR_TEXT;
    SDL_Color rfbg = rdis ? COLOR_BG : COLOR_SURFACE;

    ui_draw_text(r, "Save to:", 20, y + 4, rftc);
    ui_draw_rect(r, 100, y, W - 260, 28, rfbg);
    ui_draw_text(r, st->recv_savepath[0] ? st->recv_savepath : "Click Browse...",
                 106, y + 4, st->recv_savepath[0] ? rftc : COLOR_DIM);
    {
        SDL_Color bb = rdis ? COLOR_DIM : COLOR_SURFACE;
        SDL_Color bt = rdis ? COLOR_BG : COLOR_TEXT;
        ui_draw_rect(r, W - 150, y, 80, 28, bb);
        int tw, th; ui_text_size("Browse", &tw, &th);
        ui_draw_text(r, "Browse", W - 150 + (80 - tw) / 2, y + (28 - th) / 2, bt);
    }
    y += 40;

    ui_draw_text(r, "Protocol:", 20, y + 4, rftc);
    {
        ui_draw_rect(r, 120, y, 60, 28, rfbg);
        ui_draw_text(r, "TCP", 130, y + 4, rftc);
        ui_draw_rect(r, 190, y, 60, 28, rfbg);
        ui_draw_text(r, "UDP", 200, y + 4, rftc);
    }
    if (!rdis) {
        SDL_Color sel = COLOR_ACCENT;
        SDL_SetRenderDrawColor(r, sel.r, sel.g, sel.b, sel.a);
        SDL_Rect pr = {st->recv_protocol == 0 ? 120 : 190, y, 60, 28};
        SDL_RenderDrawRect(r, &pr);
    }
    y += 40;

    /* IP + Port row */
    {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", st->recv_port);
        ui_draw_text(r, "Listen IP:", 20, y + 4, COLOR_TEXT);
        ui_text_field(r, 55, y, 170, 28, st->recv_target_ip,
                      st->active_input == 4 && !st->recv_running,
                      st->active_input == 4 ? st->input_cursor : 0,
                      "0.0.0.0", st->recv_running);
        ui_draw_text(r, "Port:", 235, y + 4, COLOR_TEXT);
        ui_text_field(r, 285, y, 70, 28, pbuf,
                      st->active_input == 6 && !st->recv_running,
                      st->active_input == 6 ? st->input_cursor : 0,
                      "9876", st->recv_running);
    }
    y += 40;

    /* Listen/Stop button */
    {
        ui_draw_rect(r, 20, y, 180, 32, st->recv_running ? COLOR_ERROR : COLOR_ACCENT);
        int tw, th;
        const char *label = st->recv_running ? "Stop Listening" : "Listen & Receive";
        ui_text_size(label, &tw, &th);
        ui_draw_text(r, label, 20 + (180 - tw) / 2, y + (32 - th) / 2, COLOR_BG);
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
        /* Truncate long names with ... */
        {
            char name_trunc[32];
            int max_w = col_x[1] - col_x[0] - 10;
            int char_w = FONT_W + 1;
            int max_chars = max_w / char_w;
            if (max_chars < 4) max_chars = 4;
            if ((int)strlen(e->name) > max_chars) {
                snprintf(name_trunc, sizeof(name_trunc), "%.*s...", max_chars - 3, e->name);
                ui_draw_text(r, name_trunc, col_x[0], ry + 3, c);
            } else {
                ui_draw_text(r, e->name, col_x[0], ry + 3, c);
            }
        }
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

/* ── Settings page ──────────────────────────────────────────── */

/* Helper: draw toggle pair [ON] [OFF] for a bool field */
static void render_toggle(SDL_Renderer *r, int x, int y, const char *label,
                          bool value, SDL_Color fg, SDL_Color bg)
{
    ui_draw_text(r, label, x, y + 4, fg);
    int lw, lh; ui_text_size(label, &lw, &lh);
    x += lw + 10;
    {
        SDL_Color on_bg  = value ? COLOR_ACCENT : bg;
        SDL_Color on_txt = value ? COLOR_BG : COLOR_DIM;
        ui_draw_rect(r, x, y, 40, 28, on_bg);
        int tw, th; ui_text_size("ON", &tw, &th);
        ui_draw_text(r, "ON", x + (40 - tw)/2, y + (28 - th)/2, on_txt);
    }
    x += 44;
    {
        SDL_Color off_bg  = !value ? COLOR_ACCENT : bg;
        SDL_Color off_txt = !value ? COLOR_BG : COLOR_DIM;
        ui_draw_rect(r, x, y, 40, 28, off_bg);
        int tw, th; ui_text_size("OFF", &tw, &th);
        ui_draw_text(r, "OFF", x + (40 - tw)/2, y + (28 - th)/2, off_txt);
    }
}

/* Helper: draw a small text field for numeric input */
static void render_num_field(SDL_Renderer *r, int x, int y, int w,
                             int value, int active_input, int cursor,
                             const char *placeholder, bool disabled)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    ui_text_field(r, x, y, w, 28, buf,
                  false, /* numeric fields always show the value */
                  0, "0", disabled);
}

static void render_settings_page(SDL_Renderer *r, struct app_state *st)
{
    int x0 = 20, y = 50, W = st->window_w;
    struct lanft_config *cfg = &st->gui_cfg;
    SDL_Color c_text = COLOR_TEXT;
    SDL_Color c_surf = COLOR_SURFACE;

    /* ── Core ────────────────────────────────────────────── */
    ui_draw_text(r, "Core", x0, y, COLOR_ACCENT); y += 24;
    {
        char pbuf[16]; snprintf(pbuf, sizeof(pbuf), "%d", cfg->port);
        ui_draw_text(r, "Port:", x0, y + 4, c_text);
        ui_text_field(r, x0 + 50, y, 70, 28, pbuf,
                      st->active_input == 10, st->active_input == 10 ? st->input_cursor : 0,
                      "9876", false);
        ui_draw_text(r, "Protocol:", x0 + 135, y + 4, c_text);
        /* TCP/UDP toggle */
        {
            SDL_Color tcp_bg = (cfg->protocol == FT_PROTO_TCP) ? COLOR_ACCENT : c_surf;
            SDL_Color tcp_tx = (cfg->protocol == FT_PROTO_TCP) ? COLOR_BG : c_text;
            ui_draw_rect(r, x0 + 220, y, 50, 28, tcp_bg);
            int tw, th; ui_text_size("TCP", &tw, &th);
            ui_draw_text(r, "TCP", x0 + 220 + (50 - tw)/2, y + (28 - th)/2, tcp_tx);
        }
        {
            SDL_Color udp_bg = (cfg->protocol == FT_PROTO_UDP) ? COLOR_ACCENT : c_surf;
            SDL_Color udp_tx = (cfg->protocol == FT_PROTO_UDP) ? COLOR_BG : c_text;
            ui_draw_rect(r, x0 + 275, y, 50, 28, udp_bg);
            int tw, th; ui_text_size("UDP", &tw, &th);
            ui_draw_text(r, "UDP", x0 + 275 + (50 - tw)/2, y + (28 - th)/2, udp_tx);
        }
        y += 34;
    }
    {
        ui_draw_text(r, "Address:", x0, y + 4, c_text);
        ui_text_field(r, x0 + 80, y, 160, 28, cfg->address,
                      st->active_input == 11, st->active_input == 11 ? st->input_cursor : 0,
                      "0.0.0.0", false);
        ui_draw_text(r, "Mode:", x0 + 250, y + 4, c_text);
        {
            SDL_Color s_bg = (cfg->mode == 0) ? COLOR_ACCENT : c_surf;
            SDL_Color s_tx = (cfg->mode == 0) ? COLOR_BG : c_text;
            ui_draw_rect(r, x0 + 305, y, 55, 28, s_bg);
            int tw, th; ui_text_size("Send", &tw, &th);
            ui_draw_text(r, "Send", x0 + 305 + (55 - tw)/2, y + (28 - th)/2, s_tx);
        }
        {
            SDL_Color r_bg = (cfg->mode == 1) ? COLOR_ACCENT : c_surf;
            SDL_Color r_tx = (cfg->mode == 1) ? COLOR_BG : c_text;
            ui_draw_rect(r, x0 + 365, y, 75, 28, r_bg);
            int tw, th; ui_text_size("Receive", &tw, &th);
            ui_draw_text(r, "Receive", x0 + 365 + (75 - tw)/2, y + (28 - th)/2, r_tx);
        }
        y += 34;
    }
    {
        ui_draw_text(r, "Save Dir:", x0, y + 4, c_text);
        ui_text_field(r, x0 + 80, y, W - x0 - 100 - 170, 28, cfg->save_dir,
                      st->active_input == 12, st->active_input == 12 ? st->input_cursor : 0,
                      "~/Downloads", false);
        /* Browse button */
        SDL_Color bb = COLOR_SURFACE;
        ui_draw_rect(r, W - 170, y, 80, 28, bb);
        int tw, th; ui_text_size("Browse", &tw, &th);
        ui_draw_text(r, "Browse", W - 170 + (80 - tw)/2, y + (28 - th)/2, c_text);
        y += 34;
    }

    /* ── Transfer ─────────────────────────────────────────── */
    y += 4;
    ui_draw_text(r, "Transfer", x0, y, COLOR_ACCENT); y += 24;
    {
        char buf[32];
        ui_draw_text(r, "Buffer:", x0, y + 4, c_text);
        snprintf(buf, sizeof(buf), "%d", cfg->buffer_size);
        ui_text_field(r, x0 + 60, y, 80, 28, buf,
                      st->active_input == 13, st->active_input == 13 ? st->input_cursor : 0,
                      "65536", false);
        ui_draw_text(r, "Timeout:", x0 + 155, y + 4, c_text);
        snprintf(buf, sizeof(buf), "%d", cfg->timeout_seconds);
        ui_text_field(r, x0 + 230, y, 60, 28, buf,
                      st->active_input == 14, st->active_input == 14 ? st->input_cursor : 0,
                      "30", false);
        ui_draw_text(r, "s", x0 + 295, y + 4, c_text);
        y += 34;
    }
    {
        char buf[32];
        ui_draw_text(r, "Max Conn:", x0, y + 4, c_text);
        snprintf(buf, sizeof(buf), "%d", cfg->max_connections);
        ui_text_field(r, x0 + 85, y, 55, 28, buf,
                      st->active_input == 15, st->active_input == 15 ? st->input_cursor : 0,
                      "5", false);
        ui_draw_text(r, "Bandwidth:", x0 + 155, y + 4, c_text);
        snprintf(buf, sizeof(buf), "%d", cfg->send_bandwidth_limit);
        ui_text_field(r, x0 + 245, y, 80, 28, buf,
                      st->active_input == 16, st->active_input == 16 ? st->input_cursor : 0,
                      "0", false);
        ui_draw_text(r, "B/s", x0 + 330, y + 4, c_text);
        y += 34;
    }
    /* Overwrite policy */
    {
        ui_draw_text(r, "Overwrite:", x0, y + 4, c_text);
        const char *policies[] = {"rename", "overwrite", "skip"};
        int px = x0 + 100;
        for (int i = 0; i < 3; i++) {
            bool sel = (strcmp(cfg->overwrite_policy, policies[i]) == 0);
            SDL_Color bg = sel ? COLOR_ACCENT : c_surf;
            SDL_Color tx = sel ? COLOR_BG : c_text;
            int bw = (i == 0) ? 65 : (i == 1 ? 83 : 42);
            ui_draw_rect(r, px, y, bw, 28, bg);
            int tw, th; ui_text_size(policies[i], &tw, &th);
            ui_draw_text(r, policies[i], px + (bw - tw)/2, y + (28 - th)/2, tx);
            px += bw + 8;
        }
        y += 34;
    }
    /* Toggles */
    render_toggle(r, x0, y, "Auto Accept:", cfg->auto_accept, c_text, c_surf); y += 34;
    render_toggle(r, x0, y, "Show Progress:", cfg->show_progress, c_text, c_surf); y += 34;

    /* ── Discovery ────────────────────────────────────────── */
    y += 4;
    ui_draw_text(r, "Discovery", x0, y, COLOR_ACCENT); y += 24;
    render_toggle(r, x0, y, "Discovery:", cfg->discovery_enabled, c_text, c_surf); y += 34;
    {
        char buf[32];
        ui_draw_text(r, "Interval:", x0, y + 4, c_text);
        snprintf(buf, sizeof(buf), "%d", cfg->discovery_interval);
        ui_text_field(r, x0 + 75, y, 50, 28, buf,
                      st->active_input == 17, st->active_input == 17 ? st->input_cursor : 0,
                      "5", false);
        ui_draw_text(r, "s", x0 + 128, y + 4, c_text);
        ui_draw_text(r, "TTL:", x0 + 155, y + 4, c_text);
        snprintf(buf, sizeof(buf), "%d", cfg->discovery_ttl);
        ui_text_field(r, x0 + 195, y, 50, 28, buf,
                      st->active_input == 18, st->active_input == 18 ? st->input_cursor : 0,
                      "1", false);
        y += 34;
    }

    /* ── Logging ──────────────────────────────────────────── */
    y += 4;
    ui_draw_text(r, "Logging", x0, y, COLOR_ACCENT); y += 24;
    {
        ui_draw_text(r, "Level:", x0, y + 4, c_text);
        const char *levels[] = {"debug", "info", "warn", "error"};
        int lx = x0 + 60;
        for (int i = 0; i < 4; i++) {
            bool sel = (strcmp(cfg->log_level, levels[i]) == 0);
            SDL_Color bg = sel ? COLOR_ACCENT : c_surf;
            SDL_Color tx = sel ? COLOR_BG : c_text;
            int bw = (i == 0) ? 55 : (i == 1 ? 40 : (i == 2 ? 45 : 48));
            ui_draw_rect(r, lx, y, bw, 28, bg);
            int tw, th; ui_text_size(levels[i], &tw, &th);
            ui_draw_text(r, levels[i], lx + (bw - tw)/2, y + (28 - th)/2, tx);
            lx += bw + 6;
        }
        y += 34;
    }

    /* ── Save button ──────────────────────────────────────── */
    y += 8;
    {
        SDL_Color sb = COLOR_ACCENT;
        ui_draw_rect(r, x0, y, 140, 32, sb);
        int tw, th; ui_text_size("Save Config", &tw, &th);
        ui_draw_text(r, "Save Config", x0 + (140 - tw)/2, y + (32 - th)/2, COLOR_BG);
    }

    /* Status line */
    y += 50;
    ui_draw_text(r, st->status_text, x0, y, COLOR_DIM);
}

/* ── Main render ───────────────────────────────────────────── */

void ui_render(SDL_Renderer *renderer, struct app_state *state)
{
    ui_draw_rect(renderer, 0, 0, state->window_w, state->window_h, COLOR_BG);
    render_tab_bar(renderer, state);

    switch (state->current_tab) {
    case TAB_SCAN:     render_scan_page(renderer, state); break;
    case TAB_SEND:     render_send_page(renderer, state); break;
    case TAB_RECEIVE:  render_receive_page(renderer, state); break;
    case TAB_HISTORY:  render_history_page(renderer, state); break;
    case TAB_SETTINGS: render_settings_page(renderer, state); break;
    }

    /* Modal overlay — error messages */
    if (state->modal_visible) {
        ui_draw_rect(renderer, 0, 0, state->window_w, state->window_h,
                     (SDL_Color){0, 0, 0, 180});
        int mx = state->window_w / 2 - 200, my = state->window_h / 2 - 60;
        ui_draw_rect(renderer, mx, my, 400, 120, COLOR_SURFACE);
        ui_draw_text(renderer, state->modal_message, mx + 20, my + 20, COLOR_TEXT);
        ui_button(renderer, "OK", mx + 160, my + 70, 80, 30, 0, 0, false);
    }

    /* Modal overlay — incoming transfer prompt */
    if (state->incoming_active) {
        ui_draw_rect(renderer, 0, 0, state->window_w, state->window_h,
                     (SDL_Color){0, 0, 0, 180});
        int mx = state->window_w / 2 - 200, my = state->window_h / 2 - 80;
        ui_draw_rect(renderer, mx, my, 400, 160, COLOR_SURFACE);
        ui_draw_text(renderer, "Incoming Transfer", mx + 20, my + 16, COLOR_ACCENT);
        char buf[512];
        snprintf(buf, sizeof(buf), "From: %s (%s)",
                 state->incoming_ip, state->incoming_hostname);
        ui_draw_text(renderer, buf, mx + 20, my + 44, COLOR_TEXT);
        snprintf(buf, sizeof(buf), "File: %s", state->incoming_filename);
        ui_draw_text(renderer, buf, mx + 20, my + 66, COLOR_TEXT);
        /* Format size */
        const char *units[] = {"B","KB","MB","GB"};
        double s = (double)state->incoming_size;
        int u = 0;
        while (s >= 1024 && u < 3) { s /= 1024; u++; }
        snprintf(buf, sizeof(buf), "Size: %.1f %s", s, units[u]);
        ui_draw_text(renderer, buf, mx + 20, my + 88, COLOR_TEXT);
        ui_button(renderer, "Accept", mx + 60, my + 118, 100, 30, 0, 0, false);
        ui_button(renderer, "Reject", mx + 240, my + 118, 100, 30, 0, 0, false);
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

    /* Modal overlay — error */
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

    /* Modal overlay — incoming transfer prompt */
    if (st->incoming_active) {
        if (e->type == SDL_MOUSEBUTTONDOWN) {
            int modal_mx = st->window_w / 2 - 200;
            int modal_my = st->window_h / 2 - 80;
            /* Accept button */
            if (ui_in_rect(mx, my, modal_mx + 60, modal_my + 118, 100, 30)) {
                st->incoming_active = false;
                transfer_accept();
                return true;
            }
            /* Reject button */
            if (ui_in_rect(mx, my, modal_mx + 240, modal_my + 118, 100, 30)) {
                st->incoming_active = false;
                transfer_reject();
                return true;
            }
        }
        return true;  /* Block all other clicks while prompt is active */
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
                if (ui_text_field_click(st, mx, my, 175, 46, 65, 28, 7, spbuf, false))
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
                        snprintf(st->status_text, sizeof(st->status_text),
                                 "Selected %s — use as receiver IP", st->devices[i].ip);
                        return true;
                    }
                }
            }
            break;

        case TAB_SEND:
            /* Stop/Start button — always enabled */
            if (ui_in_rect(mx, my, 20, 170, 160, 32)) {
                if (st->send_running) {
                    st->send_running = false;
                    strncpy(st->status_text, "Send stopped by user", sizeof(st->status_text) - 1);
                    return true;
                } else if (st->send_filepath[0] && st->send_target_ip[0]) {
                    st->send_running = true;
                    st->send_progress_done = 0;
                    st->send_progress_total = 0;
                    st->active_input = 0;
                    SDL_StopTextInput();
                    strncpy(st->status_text, "Waiting for receiver...", sizeof(st->status_text) - 1);
                    return true;
                }
            }
            /* All other controls disabled during transfer */
            if (st->send_running) break;
            if (ui_in_rect(mx, my, st->window_w - 260, 50, 50, 28)) {
                st->send_is_dir = true; return true;
            }
            if (ui_in_rect(mx, my, st->window_w - 205, 50, 50, 28)) {
                st->send_is_dir = false; return true;
            }
            if (ui_in_rect(mx, my, st->window_w - 150, 50, 80, 28)) {
                if (st->send_is_dir)
                    zenity_launch(st, "Select Directory to Send", true, 1);
                else
                    zenity_launch(st, "Select File to Send", false, 1);
                return true;
            }
            if (ui_in_rect(mx, my, 120, 90, 60, 28)) { st->send_protocol = 0; return true; }
            if (ui_in_rect(mx, my, 190, 90, 60, 28)) { st->send_protocol = 1; return true; }
            {
                char spbuf[16]; snprintf(spbuf, sizeof(spbuf), "%d", st->send_port);
                if (ui_text_field_click(st, mx, my, 55, 130, 170, 28, 2, st->send_target_ip, st->send_running))
                    return true;
                if (ui_text_field_click(st, mx, my, 285, 130, 70, 28, 5, spbuf, st->send_running))
                    return true;
            }
            break;

        case TAB_RECEIVE:
            /* Stop/Listen button — always enabled */
            if (ui_in_rect(mx, my, 20, 170, 180, 32)) {
                if (st->recv_running) {
                    st->recv_running = false;
                    strncpy(st->status_text, "Receive stopped by user", sizeof(st->status_text) - 1);
                    return true;
                } else if (st->recv_savepath[0] && st->recv_target_ip[0]) {
                    st->recv_running = true;
                    st->recv_progress_done = 0;
                    st->recv_progress_total = 0;
                    st->active_input = 0;
                    SDL_StopTextInput();
                    strncpy(st->status_text, "Waiting for sender...", sizeof(st->status_text) - 1);
                    return true;
                }
            }
            /* All other controls disabled during transfer */
            if (st->recv_running) break;
            if (ui_in_rect(mx, my, st->window_w - 150, 50, 80, 28)) {
                zenity_launch(st, "Select Save Directory", true, 2);
                return true;
            }
            if (ui_in_rect(mx, my, 120, 90, 60, 28)) { st->recv_protocol = 0; return true; }
            if (ui_in_rect(mx, my, 190, 90, 60, 28)) { st->recv_protocol = 1; return true; }
            {
                char rpbuf[16]; snprintf(rpbuf, sizeof(rpbuf), "%d", st->recv_port);
                if (ui_text_field_click(st, mx, my, 55, 130, 170, 28, 4, st->recv_target_ip, st->recv_running))
                    return true;
                if (ui_text_field_click(st, mx, my, 285, 130, 70, 28, 6, rpbuf, st->recv_running))
                    return true;
            }
            if (ui_in_rect(mx, my, 20, 170, 180, 32)) {
                if (st->recv_running) {
                    st->recv_running = false;
                    strncpy(st->status_text, "Receive stopped by user", sizeof(st->status_text) - 1);
                    return true;
                } else if (st->recv_savepath[0] && st->recv_target_ip[0]) {
                    st->recv_running = true;
                    st->recv_progress_done = 0;
                    st->recv_progress_total = 0;
                    st->active_input = 0;
                    SDL_StopTextInput();
                    strncpy(st->status_text, "Waiting for sender...", sizeof(st->status_text) - 1);
                    return true;
                }
            }
            break;

        case TAB_SETTINGS: {
            struct lanft_config *cfg = &st->gui_cfg;
            /* ── Core section ──────────────────────────── */
            /* Protocol: TCP (x0+220=240, y=74) */
            if (ui_in_rect(mx, my, 240, 74, 50, 28)) { cfg->protocol = FT_PROTO_TCP; return true; }
            /* Protocol: UDP (x0+275=295, y=74) */
            if (ui_in_rect(mx, my, 295, 74, 50, 28)) { cfg->protocol = FT_PROTO_UDP; return true; }
            /* Port text field (x0+50=70, y=74) */
            { char buf[32]; snprintf(buf, sizeof(buf), "%d", cfg->port);
              if (ui_text_field_click(st, mx, my, 70, 74, 70, 28, 10, buf, false)) return true; }
            /* Mode: Send (x0+305=325, y=108) */
            if (ui_in_rect(mx, my, 325, 108, 55, 28)) { cfg->mode = 0; return true; }
            /* Mode: Receive (x0+365=385, y=108) */
            if (ui_in_rect(mx, my, 385, 108, 75, 28)) { cfg->mode = 1; return true; }
            /* Address text field (x0+80=100, y=108) */
            if (ui_text_field_click(st, mx, my, 100, 108, 160, 28, 11, cfg->address, false)) return true;
            /* Save Dir text field (x0+80=100, y=142) */
            if (ui_text_field_click(st, mx, my, 100, 142, st->window_w - 290, 28, 12, cfg->save_dir, false)) return true;
            /* Browse button for save_dir (W-170, y=142) */
            if (ui_in_rect(mx, my, st->window_w - 170, 142, 80, 28))
                { zenity_launch(st, "Select Save Directory", true, 3); return true; }

            /* ── Transfer section ──────────────────────── */
            /* Buffer (x0+60=80, y=204), Timeout (x0+230=250, y=204) */
            { char buf[32];
              snprintf(buf, sizeof(buf), "%d", cfg->buffer_size);
              if (ui_text_field_click(st, mx, my, 80, 204, 80, 28, 13, buf, false)) return true;
              snprintf(buf, sizeof(buf), "%d", cfg->timeout_seconds);
              if (ui_text_field_click(st, mx, my, 250, 204, 60, 28, 14, buf, false)) return true; }
            /* Max Conn (x0+85=105, y=238), Bandwidth (x0+245=265, y=238) */
            { char buf[32];
              snprintf(buf, sizeof(buf), "%d", cfg->max_connections);
              if (ui_text_field_click(st, mx, my, 105, 238, 55, 28, 15, buf, false)) return true;
              snprintf(buf, sizeof(buf), "%d", cfg->send_bandwidth_limit);
              if (ui_text_field_click(st, mx, my, 265, 238, 80, 28, 16, buf, false)) return true; }
            /* Overwrite policy (px=120/193/284, y=272) */
            if (ui_in_rect(mx, my, 120, 272, 65, 28)) { strncpy(cfg->overwrite_policy, "rename", 15); return true; }
            if (ui_in_rect(mx, my, 193, 272, 83, 28)) { strncpy(cfg->overwrite_policy, "overwrite", 15); return true; }
            if (ui_in_rect(mx, my, 284, 272, 42, 28)) { strncpy(cfg->overwrite_policy, "skip", 15); return true; }
            /* Auto Accept toggle: label ~100px, ON at x0+110=130, OFF at 174 (y=306) */
            if (ui_in_rect(mx, my, 130, 306, 40, 28)) { cfg->auto_accept = true; return true; }
            if (ui_in_rect(mx, my, 174, 306, 40, 28)) { cfg->auto_accept = false; return true; }
            /* Show Progress toggle: label ~117px, ON at x0+127=147, OFF at 191 (y=340) */
            if (ui_in_rect(mx, my, 147, 340, 40, 28)) { cfg->show_progress = true; return true; }
            if (ui_in_rect(mx, my, 191, 340, 40, 28)) { cfg->show_progress = false; return true; }

            /* ── Discovery section ──────────────────────── */
            /* Discovery toggle: label ~80px, ON at x0+90=110 (y=402) */
            if (ui_in_rect(mx, my, 110, 402, 40, 28)) { cfg->discovery_enabled = true; return true; }
            if (ui_in_rect(mx, my, 154, 402, 40, 28)) { cfg->discovery_enabled = false; return true; }
            /* Interval (x0+75=95, y=436), TTL (x0+195=215, y=436) */
            { char buf[32];
              snprintf(buf, sizeof(buf), "%d", cfg->discovery_interval);
              if (ui_text_field_click(st, mx, my, 95, 436, 50, 28, 17, buf, false)) return true;
              snprintf(buf, sizeof(buf), "%d", cfg->discovery_ttl);
              if (ui_text_field_click(st, mx, my, 215, 436, 50, 28, 18, buf, false)) return true; }

            /* ── Logging section ────────────────────────── */
            /* Log level (lx=80/141/187/238, y=498) */
            if (ui_in_rect(mx, my, 80, 498, 55, 28)) { strncpy(cfg->log_level, "debug", 15); return true; }
            if (ui_in_rect(mx, my, 141, 498, 40, 28)) { strncpy(cfg->log_level, "info", 15); return true; }
            if (ui_in_rect(mx, my, 187, 498, 45, 28)) { strncpy(cfg->log_level, "warn", 15); return true; }
            if (ui_in_rect(mx, my, 238, 498, 48, 28)) { strncpy(cfg->log_level, "error", 15); return true; }

            /* ── Save Config button (x0=20, y=540) ──────── */
            if (ui_in_rect(mx, my, 20, 540, 140, 32)) {
                config_save(cfg);
                snprintf(st->status_text, sizeof(st->status_text), "Config saved to %s", config_user_path());
                return true;
            }
            break;
        }
        }
    }

    /* ── Helper: sync input_buffer back to the bound field ────── */
    /* (defined once, used by TEXTINPUT, BACKSPACE, DELETE, RETURN) */

    /* Keyboard input for text fields */
    if (e->type == SDL_TEXTINPUT && st->active_input > 0) {
        size_t len = strlen(st->input_buffer);
        size_t tlen = strlen(e->text.text);
        if (len + tlen < sizeof(st->input_buffer) - 1) {
            /* Insert at cursor position, not just append */
            memmove(st->input_buffer + st->input_cursor + tlen,
                    st->input_buffer + st->input_cursor,
                    len - st->input_cursor + 1);
            memcpy(st->input_buffer + st->input_cursor, e->text.text, tlen);
            st->input_cursor += tlen;
        }
        goto sync_field;
    }

    if (e->type == SDL_KEYDOWN) {
        int len = strlen(st->input_buffer);

        /* Backspace */
        if (e->key.keysym.sym == SDLK_BACKSPACE && st->active_input > 0) {
            if (len > 0 && st->input_cursor > 0) {
                memmove(st->input_buffer + st->input_cursor - 1,
                        st->input_buffer + st->input_cursor,
                        len - st->input_cursor + 1);
                st->input_cursor--;
            }
            goto sync_field;
        }

        /* Delete */
        if (e->key.keysym.sym == SDLK_DELETE && st->active_input > 0) {
            if (st->input_cursor < len) {
                memmove(st->input_buffer + st->input_cursor,
                        st->input_buffer + st->input_cursor + 1,
                        len - st->input_cursor);
            }
            goto sync_field;
        }

        /* Cursor movement */
        if (e->key.keysym.sym == SDLK_LEFT && st->active_input > 0) {
            if (st->input_cursor > 0) st->input_cursor--;
            return true;
        }
        if (e->key.keysym.sym == SDLK_RIGHT && st->active_input > 0) {
            if (st->input_cursor < len) st->input_cursor++;
            return true;
        }
        if (e->key.keysym.sym == SDLK_HOME && st->active_input > 0) {
            st->input_cursor = 0;
            return true;
        }
        if (e->key.keysym.sym == SDLK_END && st->active_input > 0) {
            st->input_cursor = len;
            return true;
        }

        /* Confirm / cancel input */
        if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_ESCAPE) {
            st->active_input = 0;
            SDL_StopTextInput();
            return true;
        }
    }

    return false;

sync_field:
    /* Sync input_buffer to the bound state field */
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
    /* Settings page fields (10-18) */
    if (st->active_input == 10) st->gui_cfg.port = atoi(st->input_buffer);
    if (st->active_input == 11) strncpy(st->gui_cfg.address, st->input_buffer, sizeof(st->gui_cfg.address) - 1);
    if (st->active_input == 12) strncpy(st->gui_cfg.save_dir, st->input_buffer, sizeof(st->gui_cfg.save_dir) - 1);
    if (st->active_input == 13) st->gui_cfg.buffer_size = atoi(st->input_buffer);
    if (st->active_input == 14) st->gui_cfg.timeout_seconds = atoi(st->input_buffer);
    if (st->active_input == 15) st->gui_cfg.max_connections = atoi(st->input_buffer);
    if (st->active_input == 16) st->gui_cfg.send_bandwidth_limit = atoi(st->input_buffer);
    if (st->active_input == 17) st->gui_cfg.discovery_interval = atoi(st->input_buffer);
    if (st->active_input == 18) st->gui_cfg.discovery_ttl = atoi(st->input_buffer);
    return true;

    return false;
}
