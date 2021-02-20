/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef _CONTEXT_H
#define _CONTEXT_H 1

#include "image.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

#define TRUE_COLOR_ALPHA_DEPTH 32
#define BG_COLOR 0xFFFFFFFF
#define WINDOW_X 100
#define WINDOW_Y 100
#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480
#define FPS 60
#define TPS 10

#define WINDOW_CLASS "SoftRendererExa"
#define WINDOW_TITLE "Rough-like with software rendering"

typedef uint32_t color_t;

enum modifier_mask {
    mask_shift = 1 << 0,
    mask_lock = 1 << 1,
    mask_control = 1 << 2,
    mask_mod_1 = 1 << 3, /* Alt */
    mask_mod_2 = 1 << 4, /* Numlock */
    mask_mod_3 = 1 << 5,
    mask_mod_4 = 1 << 6, /* Super */
    mask_mod_5 = 1 << 7, /* Group */
};

struct context {
    xcb_connection_t *con;
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;

    xcb_window_t wid;
    xcb_gcontext_t gc;

    xcb_shm_seg_t shm_seg;
    xcb_pixmap_t shm_pixmap;
    struct image backbuf;

    bool focused;
    bool active;
    bool force_redraw;
    bool want_exit;
    bool want_redraw;
    bool tick_early;
    bool has_shm;
    bool has_shm_pixmaps;

    struct timespec last_draw;
    /* Game logic is handled in fixed rate,
     * separate from FPS */
    struct timespec last_tick;

    double dpi;
    double scale;

    struct atom_ {
        xcb_atom_t _NET_WM_PID;
        xcb_atom_t _NET_WM_NAME;
        xcb_atom_t _NET_WM_ICON_NAME;
        xcb_atom_t WM_DELETE_WINDOW;
        xcb_atom_t WM_PROTOCOLS;
        xcb_atom_t UTF8_STRING;
    } atom;

    int16_t center_x, center_y;

    xcb_get_keyboard_mapping_reply_t *keymap;

    struct image image;

    struct input_state {
        bool forward : 1;
        bool backward : 1;
        bool left : 1;
        bool right : 1;
    } keys;
};

extern struct context ctx;

void warn(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
_Noreturn void die(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

inline static bool check_void_cookie(xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(ctx.con, ck);
    if (err) {
        warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

void redraw(void);
void tick(struct timespec time);
xcb_keysym_t get_keysym(xcb_keycode_t kc, uint16_t state);
void handle_key(xcb_keycode_t kc, uint16_t state, bool pressed);

#endif
