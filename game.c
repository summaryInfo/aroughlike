/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "context.h"
#include "image.h"

/* This is a copy of key sym definitions
 * file from Xlib is shipped with the
 * program in order to prevent X11 from being
 * a dependency (only libxcb and libxcb-shm are required) */
#define XK_MISCELLANY
#define XK_LATIN1
#include "keysymdef.h"

/* This is main drawing function, that is called
 * FPS times a second */
void redraw(void) {
    double scale = ctx.scale;
    int32_t scr_w = ctx.backbuf.width, scr_h = ctx.backbuf.height;
    int32_t im_w = ctx.image.width, im_h = ctx.image.height;

    /* Clear screen */
    image_draw_rect(ctx.backbuf, (struct rect){0, 0, scr_w, scr_h}, BG_COLOR);

    /* Draw test candles */
    image_blt(ctx.backbuf, (struct rect){scr_w/2 - 10*scale, scr_h/2 - 10*scale, im_w*scale, im_h*scale},
              ctx.image, (struct rect){0, im_h, im_w, -im_h}, 0);
    image_blt(ctx.backbuf, (struct rect){scr_w/2 + ctx.center_x*scale, scr_h/2 + ctx.center_y*scale, im_w*scale, im_h*scale},
              ctx.image, (struct rect){0, 0, im_w, im_h}, 0);
}

/* This function is called TPS times a second
 * (this is a place where all game logic resides) */
void tick(struct timespec time) {
    (void)time;
    if (ctx.keys.forward) { ctx.center_y--; ctx.want_redraw = 1; }
    if (ctx.keys.backward) { ctx.center_y++; ctx.want_redraw = 1; }
    if (ctx.keys.left) { ctx.center_x--; ctx.want_redraw = 1; }
    if (ctx.keys.right) { ctx.center_x++; ctx.want_redraw = 1; }
}

/* This function is called on every key press */
void handle_key(xcb_keycode_t kc, uint16_t state, bool pressed) {
    xcb_keysym_t ksym = get_keysym(kc, state);
    switch (ksym) {
    case XK_w:
        ctx.keys.forward = pressed;
        ctx.tick_early = !ctx.keys.forward && pressed;
        break;
    case XK_s:
        ctx.keys.backward = pressed;
        ctx.tick_early = !ctx.keys.backward && pressed;
        break;
    case XK_a:
        ctx.keys.left = pressed;
        ctx.tick_early = !ctx.keys.left && pressed;
        break;
    case XK_d:
        ctx.keys.right = pressed;
        ctx.tick_early = !ctx.keys.right && pressed;
        break;
    case XK_minus:
        ctx.scale = MAX(1, ctx.scale - pressed);
        ctx.want_redraw = 1;
        break;
    case XK_equal:
    case XK_plus:
        ctx.scale += pressed;
        ctx.want_redraw = 1;
        break;
    case XK_Escape:
        ctx.want_exit = 1;
        break;
    }
}

void init(void) {
    ctx.image = create_image("test.png");
}

void cleanup(void) {
    free_image(&ctx.image);
}
