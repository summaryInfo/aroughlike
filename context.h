/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef _CONTEXT_H
#define _CONTEXT_H 1

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define TRUE_COLOR_ALPHA_DEPTH 32
#define BG_COLOR 0xFF25131A
#define WINDOW_X 100
#define WINDOW_Y 100
#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480
#define FPS 300

#define WINDOW_CLASS "SoftRendererExa"
#define WINDOW_TITLE "Rough-like with software rendering"

typedef uint32_t color_t;

struct scale {
    double map;
    double interface;
    double dpi;
};

extern bool want_redraw;
extern bool want_exit;
extern struct scale scale;
extern struct image backbuf;

/* Callbacks from game.c */
void init(void);
void cleanup(void);
void redraw(struct timespec current);
int64_t tick(struct timespec current);
void handle_key(uint8_t kc, uint32_t state, bool pressed);

/* Helpers from window.c */
uint32_t get_keysym(uint8_t kc, uint32_t state);

#endif
