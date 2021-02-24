/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "context.h"
#include "image.h"
#include "tilemap.h"

/* This is a copy of key sym definitions
 * file from Xlib is shipped with the
 * program in order to prevent X11 from being
 * a dependency (only libxcb and libxcb-shm are required) */
#define XK_MISCELLANY
#define XK_LATIN1
#include "keysymdef.h"

#define TILE_WIDTH 16
#define TILE_HEIGHT 16
#define NTILESETS 3

struct gamestate {
    struct tilemap *map;
    struct tileset *tilesets[NTILESETS];

    int16_t camera_x;
    int16_t camera_y;

    struct input_state {
        bool forward : 1;
        bool backward : 1;
        bool left : 1;
        bool right : 1;
    } keys;
} state;

/* This is main drawing function, that is called
 * FPS times a second */
void redraw(void) {
    /* Clear screen */
    image_draw_rect(ctx.backbuf, (struct rect){0, 0, ctx.backbuf.width, ctx.backbuf.height}, BG_COLOR);
    /* Draw map */
    tilemap_draw(ctx.backbuf, state.map, state.camera_x, state.camera_y);
}

/* This function is called TPS times a second
 * (this is a place where all game logic resides) */
void tick(struct timespec time) {
    (void)time;

    if (state.keys.forward) { state.camera_y--; ctx.want_redraw = 1; }
    if (state.keys.backward) { state.camera_y++; ctx.want_redraw = 1; }
    if (state.keys.left) { state.camera_x--; ctx.want_redraw = 1; }
    if (state.keys.right) { state.camera_x++; ctx.want_redraw = 1; }
}

/* This function is called on every key press */
void handle_key(xcb_keycode_t kc, uint16_t st, bool pressed) {
    xcb_keysym_t ksym = get_keysym(kc, st);
    switch (ksym) {
    case XK_w:
        state.keys.forward = pressed;
        ctx.tick_early = !state.keys.forward && pressed;
        break;
    case XK_s:
        state.keys.backward = pressed;
        ctx.tick_early = !state.keys.backward && pressed;
        break;
    case XK_a:
        state.keys.left = pressed;
        ctx.tick_early = !state.keys.left && pressed;
        break;
    case XK_d:
        state.keys.right = pressed;
        ctx.tick_early = !state.keys.right && pressed;
        break;
    case XK_minus:
        ctx.scale = MAX(1, ctx.scale - pressed);
        tilemap_set_scale(state.map, ctx.scale);
        ctx.want_redraw = 1;
        break;
    case XK_equal:
    case XK_plus:
        ctx.scale += pressed;
        tilemap_set_scale(state.map, ctx.scale);
        ctx.want_redraw = 1;
        break;
    case XK_Escape:
        ctx.want_exit = 1;
        break;
    }
}

struct tilemap *create_tilemap_from_tile(const char *file) {
    // TODO Load map
    int16_t map_width = 128;
    int16_t map_height = 128;
    (void)file;

    struct tilemap *map = create_tilemap(map_width, map_height, TILE_WIDTH, TILE_HEIGHT, state.tilesets, NTILESETS);

    return map;
}

void init(void) {
    struct {
        const char *path;
        size_t x, y;
        bool animated;
    } tileset_descs[NTILESETS] = {
        {"data/tiles.png", 10, 10, 0},
        {"data/ani.png", 4, 26, 1},
        {"data/ent.png", 4, 14, 1},
    };

    for (size_t i = 0; i < sizeof(tileset_descs)/sizeof(*tileset_descs); i++) {
        struct tile *tiles = calloc(tileset_descs[i].x*tileset_descs[i].y, sizeof(*tiles));
        tile_t tile_id = 0;
        for (size_t y = 0; y < tileset_descs[i].y; y++) {
            for (size_t x = 0; x < tileset_descs[i].x; x++) {
                tiles[tile_id] = (struct tile) {
                    .pos = (struct rect){
                        x*TILE_WIDTH,
                        y*TILE_HEIGHT,
                        TILE_WIDTH,
                        TILE_HEIGHT
                    },
                    .origin_x = 0,
                    .origin_y = 0,
                    /* In animated tileset_descs one row of tileset
                     * is one animation, and the width of tileset
                     * is the number of frames of animation */
                    .next_frame = tileset_descs[i].animated ? tile_id +
                        (tile_id + 1 % tileset_descs[i].x) - (tile_id % tileset_descs[i].x) : tile_id,
                };
            }
            tile_id++;
        }
        state.tilesets[i] = create_tileset(tileset_descs[i].path, tiles, tile_id);
    }

    state.map = create_tilemap_from_tile("data/map_1.txt");
}

void cleanup(void) {
    free_tilemap(state.map);
    for (size_t i = 0; i < NTILESETS; i++) unref_tileset(state.tilesets[i]);
}
