#include "file_browser.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FB_MAX_ENTRIES 512
#define FB_VISIBLE     15

static char fb_current_dir[1024];
static char fb_entries[FB_MAX_ENTRIES][256];
static int  fb_entry_types[FB_MAX_ENTRIES]; /* 0=dir, 1=file */
static int  fb_entry_count;
static int  fb_scroll_offset;
static int  fb_selected;  /* selected entry index, -1 if none */

void file_browser_init(struct app_state *state)
{
    (void)state;
    if (!getcwd(fb_current_dir, sizeof(fb_current_dir))) {
        strncpy(fb_current_dir, "/", sizeof(fb_current_dir) - 1);
    }
    fb_scroll_offset = 0;
    fb_selected = -1;

    fb_entry_count = 0;
    DIR *d = opendir(fb_current_dir);
    if (!d) {
        /* Can't open — go to parent or root */
        char *slash = strrchr(fb_current_dir, '/');
        if (slash && slash != fb_current_dir) *slash = '\0';
        else strncpy(fb_current_dir, "/", sizeof(fb_current_dir) - 1);
        d = opendir(fb_current_dir);
        if (!d) return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && fb_entry_count < FB_MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0) continue;
        char full[1280];
        snprintf(full, sizeof(full), "%s/%s", fb_current_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        strncpy(fb_entries[fb_entry_count], de->d_name, 255);
        fb_entry_types[fb_entry_count] = S_ISDIR(st.st_mode) ? 0 : 1;
        fb_entry_count++;
    }
    closedir(d);
}

void file_browser_render(SDL_Renderer *r, struct app_state *state)
{
    int w = state->window_w, h = state->window_h;
    int bx = w / 2 - 280, by = h / 2 - 200;
    int bw = 560, bh = 400;

    /* Dim background */
    ui_draw_rect(r, 0, 0, w, h, (SDL_Color){0, 0, 0, 190});

    /* Browser window */
    ui_draw_rect(r, bx, by, bw, bh, COLOR_SURFACE);

    /* Title bar */
    ui_draw_rect(r, bx, by, bw, 30, COLOR_ACCENT);
    ui_draw_text(r, "File Browser  [↑↓ navigate  Enter open  Backspace up]", bx + 10, by + 6, COLOR_BG);

    /* Current path — click to go up */
    ui_draw_rect(r, bx, by + 32, bw, 24, COLOR_BG);
    ui_draw_text(r, fb_current_dir, bx + 6, by + 36, COLOR_ACCENT);
    ui_draw_text(r, " (click to go up)", bx + 6 + (int)strlen(fb_current_dir) * 9, by + 36, COLOR_DIM);

    /* Separator */
    SDL_SetRenderDrawColor(r, COLOR_DIM.r, COLOR_DIM.g, COLOR_DIM.b, COLOR_DIM.a);
    SDL_RenderDrawLine(r, bx, by + 58, bx + bw, by + 58);

    /* ".." parent directory entry */
    int list_y = by + 62;
    if (strcmp(fb_current_dir, "/") != 0) {
        SDL_Color c = (fb_selected == -1) ? COLOR_ACCENT : COLOR_DIM;
        ui_draw_text(r, "[↑] ..", bx + 10, list_y, c);
        list_y += 24;
    }

    /* Entry list */
    int list_start_y = list_y;
    for (int i = fb_scroll_offset; i < fb_entry_count && i < fb_scroll_offset + FB_VISIBLE; i++) {
        int ey = list_y + (i - fb_scroll_offset) * 24;
        SDL_Color c = COLOR_TEXT;
        if (i == fb_selected)
            c = COLOR_ACCENT;
        else if (fb_entry_types[i] == 0)
            c = COLOR_TEXT;

        char label[280];
        snprintf(label, sizeof(label), "%s %s",
                 fb_entry_types[i] == 0 ? "[DIR]" : "     ", fb_entries[i]);
        ui_draw_text(r, label, bx + 10, ey, c);
    }

    /* No entries */
    if (fb_entry_count == 0) {
        ui_draw_text(r, "(empty directory)", bx + 10, list_y, COLOR_DIM);
    }

    /* More entries indicator */
    if (fb_entry_count > FB_VISIBLE) {
        char info[64];
        snprintf(info, sizeof(info), "%d-%d of %d",
                 fb_scroll_offset + 1,
                 fb_scroll_offset + FB_VISIBLE < fb_entry_count ? fb_scroll_offset + FB_VISIBLE : fb_entry_count,
                 fb_entry_count);
        ui_draw_text(r, info, bx + bw - 120, by + bh - 40, COLOR_DIM);
    }

    /* Buttons */
    int by2 = by + bh - 36;
    SDL_SetRenderDrawColor(r, COLOR_DIM.r, COLOR_DIM.g, COLOR_DIM.b, COLOR_DIM.a);
    SDL_RenderDrawLine(r, bx, by2 - 4, bx + bw, by2 - 4);
    /* Select button */
    {
        SDL_Color bg = COLOR_ACCENT;
        ui_draw_rect(r, bx + bw - 180, by2 - 2, 80, 28, COLOR_ACCENT);
        int tw, th; ui_text_size("Select", &tw, &th);
        ui_draw_text(r, "Select", bx + bw - 180 + (80 - tw) / 2, by2 + 2, COLOR_BG);
    }
    /* Cancel button */
    {
        ui_draw_rect(r, bx + bw - 90, by2 - 2, 80, 28, COLOR_SURFACE);
        int tw, th; ui_text_size("Cancel", &tw, &th);
        ui_draw_text(r, "Cancel", bx + bw - 90 + (80 - tw) / 2, by2 + 2, COLOR_TEXT);
    }
}

bool file_browser_handle_event(SDL_Event *e, struct app_state *state)
{
    int w = state->window_w, h = state->window_h;
    int bx = w / 2 - 280, by = h / 2 - 200;
    int bw = 560, bh = 400;
    int list_y = by + 62;

    /* Adjust list_y if ".." is shown */
    if (strcmp(fb_current_dir, "/") != 0) list_y += 24;

    if (e->type == SDL_MOUSEBUTTONDOWN) {
        int mx = e->button.x, my = e->button.y;

        /* Cancel button */
        if (ui_in_rect(mx, my, bx + bw - 90, by + bh - 38, 80, 28)) {
            state->file_browser_visible = false;
            return true;
        }

        /* Select button */
        if (ui_in_rect(mx, my, bx + bw - 180, by + bh - 38, 80, 28)) {
            /* Use selected entry if any, else use current directory */
            if (fb_selected >= 0 && fb_selected < fb_entry_count) {
                snprintf(state->file_browser_result, sizeof(state->file_browser_result),
                         "%s/%s", fb_current_dir, fb_entries[fb_selected]);
            } else {
                strncpy(state->file_browser_result, fb_current_dir,
                        sizeof(state->file_browser_result) - 1);
            }
            if (state->file_browser_target == 1) {
                strncpy(state->send_filepath, state->file_browser_result,
                        sizeof(state->send_filepath) - 1);
                snprintf(state->status_text, sizeof(state->status_text),
                         "Selected: %s", state->send_filepath);
            } else if (state->file_browser_target == 2) {
                strncpy(state->recv_savepath, state->file_browser_result,
                        sizeof(state->recv_savepath) - 1);
                snprintf(state->status_text, sizeof(state->status_text),
                         "Save to: %s", state->recv_savepath);
            }
            state->file_browser_visible = false;
            return true;
        }

        /* ".." parent directory click */
        if (strcmp(fb_current_dir, "/") != 0 &&
            ui_in_rect(mx, my, bx + 10, by + 62, bw - 30, 22)) {
            char *slash = strrchr(fb_current_dir, '/');
            if (slash && slash != fb_current_dir) *slash = '\0';
            else if (slash == fb_current_dir) fb_current_dir[1] = '\0';
            fb_scroll_offset = 0;
            fb_selected = -1;
            file_browser_init(state);
            return true;
        }

        /* Entry click */
        for (int i = fb_scroll_offset; i < fb_entry_count && i < fb_scroll_offset + FB_VISIBLE; i++) {
            int ey = list_y + (i - fb_scroll_offset) * 24;
            if (ui_in_rect(mx, my, bx + 10, ey, bw - 30, 24)) {
                fb_selected = i;
                if (fb_entry_types[i] == 0) {
                    /* Directory — double-click to enter */
                    char newdir[1024];
                    snprintf(newdir, sizeof(newdir), "%s/%s", fb_current_dir, fb_entries[i]);
                    strncpy(fb_current_dir, newdir, sizeof(fb_current_dir) - 1);
                    fb_scroll_offset = 0;
                    fb_selected = -1;
                    file_browser_init(state);
                } else {
                    /* File — double-click to select immediately */
                    snprintf(state->file_browser_result, sizeof(state->file_browser_result),
                             "%s/%s", fb_current_dir, fb_entries[i]);
                    if (state->file_browser_target == 1)
                        strncpy(state->send_filepath, state->file_browser_result,
                                sizeof(state->send_filepath) - 1);
                    else if (state->file_browser_target == 2)
                        strncpy(state->recv_savepath, state->file_browser_result,
                                sizeof(state->recv_savepath) - 1);
                    state->file_browser_visible = false;
                }
                return true;
            }
        }

        /* Click on path bar to go up */
        if (ui_in_rect(mx, my, bx, by + 32, bw, 24)) {
            char *slash = strrchr(fb_current_dir, '/');
            if (slash && slash != fb_current_dir) *slash = '\0';
            else if (slash == fb_current_dir) fb_current_dir[1] = '\0';
            fb_scroll_offset = 0;
            fb_selected = -1;
            file_browser_init(state);
            return true;
        }

        /* Click outside = close */
        if (!ui_in_rect(mx, my, bx, by, bw, bh)) {
            state->file_browser_visible = false;
            return true;
        }
    }

    /* Scroll wheel */
    if (e->type == SDL_MOUSEWHEEL) {
        fb_scroll_offset -= e->wheel.y;
        if (fb_scroll_offset < 0) fb_scroll_offset = 0;
        if (fb_scroll_offset > fb_entry_count - FB_VISIBLE)
            fb_scroll_offset = fb_entry_count - FB_VISIBLE;
        if (fb_scroll_offset < 0) fb_scroll_offset = 0;
        return true;
    }

    /* Keyboard navigation */
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
        case SDLK_ESCAPE:
            state->file_browser_visible = false;
            return true;
        case SDLK_UP:
            if (fb_selected > -1) fb_selected--;
            else fb_selected = fb_entry_count - 1;
            /* Adjust scroll */
            if (fb_selected < fb_scroll_offset) fb_scroll_offset = fb_selected;
            return true;
        case SDLK_DOWN:
            if (fb_selected < fb_entry_count - 1) fb_selected++;
            else fb_selected = -1;
            /* Adjust scroll */
            if (fb_selected >= fb_scroll_offset + FB_VISIBLE)
                fb_scroll_offset = fb_selected - FB_VISIBLE + 1;
            return true;
        case SDLK_RETURN:
            if (fb_selected >= 0 && fb_selected < fb_entry_count) {
                if (fb_entry_types[fb_selected] == 0) {
                    /* Enter directory */
                    char newdir[1024];
                    snprintf(newdir, sizeof(newdir), "%s/%s", fb_current_dir, fb_entries[fb_selected]);
                    strncpy(fb_current_dir, newdir, sizeof(fb_current_dir) - 1);
                    fb_scroll_offset = 0;
                    fb_selected = -1;
                    file_browser_init(state);
                } else {
                    /* Select file */
                    snprintf(state->file_browser_result, sizeof(state->file_browser_result),
                             "%s/%s", fb_current_dir, fb_entries[fb_selected]);
                    if (state->file_browser_target == 1)
                        strncpy(state->send_filepath, state->file_browser_result,
                                sizeof(state->send_filepath) - 1);
                    else if (state->file_browser_target == 2)
                        strncpy(state->recv_savepath, state->file_browser_result,
                                sizeof(state->recv_savepath) - 1);
                    state->file_browser_visible = false;
                }
            } else if (fb_selected == -1 && strcmp(fb_current_dir, "/") != 0) {
                /* Enter selected on ".." */
                char *slash = strrchr(fb_current_dir, '/');
                if (slash && slash != fb_current_dir) *slash = '\0';
                else if (slash == fb_current_dir) fb_current_dir[1] = '\0';
                fb_scroll_offset = 0;
                fb_selected = -1;
                file_browser_init(state);
            }
            return true;
        case SDLK_BACKSPACE:
            /* Go to parent directory */
            {
                char *slash = strrchr(fb_current_dir, '/');
                if (slash && slash != fb_current_dir) *slash = '\0';
                else if (slash == fb_current_dir) fb_current_dir[1] = '\0';
                fb_scroll_offset = 0;
                fb_selected = -1;
                file_browser_init(state);
            }
            return true;
        default:
            break;
        }
    }

    return true;
}
