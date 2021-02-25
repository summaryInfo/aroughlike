/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "context.h"
#include "image.h"
#include "tilemap.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define TILE_VOID MKTILE(0, 10*7+8)
#define TILE_FLOOR MKTILE(0, 10*0+6)
#define TILE_PLAYER MKTILE(2, 0*4+0)
#define TILE_TRAP MKTILE(1, 24*4+2)
#define TILE_EXIT MKTILE(0, 10*3+9)

struct gamestate {
    struct tilemap *map;
    struct tileset *tilesets[NTILESETS];
    char *mapchars;
    size_t mapchars_size;

    int16_t camera_x;
    int16_t camera_y;
    int16_t char_x;
    int16_t char_y;

    enum state {
        s_normal,
        s_win,
        s_game_over,
    } state;

    int level;

    /* Game logic is handled in fixed rate, separate from FPS */
    struct timespec last_tick;
    /* Animations are also handled separately */
    struct timespec last_frame;
    /* Only one early tick is allowed */
    bool ticked_early;

    struct input_state {
        bool forward : 1;
        bool backward : 1;
        bool left : 1;
        bool right : 1;
    } keys;
} state;

void load_map_from_file(const char *file);

#define ENT_LAYER 2


/* This is main drawing function, that is called
 * FPS times a second */
void redraw(int64_t delta) {
    (void)delta;

    /* Clear screen */
    image_draw_rect(ctx.backbuf, (struct rect){0, 0, ctx.backbuf.width, ctx.backbuf.height}, BG_COLOR);

    /* Draw map */
    tilemap_draw(ctx.backbuf, state.map, state.camera_x, state.camera_y);

    if (state.state == s_win) /* Draw win screen */ {
        image_draw_rect(ctx.backbuf, (struct rect){ctx.backbuf.width/4,
                        ctx.backbuf.height/4, ctx.backbuf.width/2, ctx.backbuf.height/2}, 0xFF0000FF);
    } else if (state.state == s_game_over) /* Draw game over message */ {
        image_draw_rect(ctx.backbuf, (struct rect){ctx.backbuf.width/4,
                        ctx.backbuf.height/4, ctx.backbuf.width/2, ctx.backbuf.height/2}, 0xFFFF0000);
    }
}

#define WALL '#'
#define TRAP 'T'
#define VOID ' '
#define EXIT 'x'

inline static char get_cell(int x, int y) {
    if ((ssize_t)state.map->width <= x || x < 0) return VOID;
    if ((ssize_t)state.map->height <= y || y < 0) return VOID;
    return state.mapchars[x+y*(state.map->width+1)];
}

void next_level(void) {
    char buf[20];
    struct stat st;

    snprintf(buf, sizeof buf, "data/map_%d.txt", ++state.level);
    if (stat(buf, &st) == 0) {
        state.state = s_normal;
        load_map_from_file(buf);
    } else state.state = s_win;;
}

/* This function is called TPS times a second
 * (this is a place where all game logic resides) */
int64_t tick(struct timespec current) {
    int64_t frame_delta = TIMEDIFF(state.last_frame, current);
    if (frame_delta >= SEC/TPS + 10000LL) {
        tilemap_animation_tick(state.map);
        state.last_frame = current;
        frame_delta = 0;
        ctx.want_redraw = 1;
    }

    int64_t logic_delta = TIMEDIFF(state.last_tick, current);
    if ((logic_delta >= SEC/TPS + 10000LL || (ctx.tick_early && !state.ticked_early))) {
        if (state.state == s_normal) {
            tile_t old_player = tilemap_set_tile(state.map, state.char_x, state.char_y, ENT_LAYER, NOTILE);

            int16_t dx = state.keys.right - state.keys.left;
            int16_t dy = state.keys.backward - state.keys.forward;

            // It's complicated to allow wall gliding
            if (get_cell(state.char_x + dx, state.char_y + dy) != WALL) {
                state.char_x += dx;
                state.char_y += dy;
            } else if (get_cell(state.char_x + dx, state.char_y) != WALL) {
                state.char_x += dx;
            } else if (get_cell(state.char_x, state.char_y + dy) != WALL) {
                state.char_y += dy;
            }

            tilemap_set_tile(state.map, state.char_x, state.char_y, ENT_LAYER, old_player);

            switch(get_cell(state.char_x, state.char_y)) {
            case VOID:
            case WALL:
            case TRAP:
                state.state = s_game_over;
                break;
            case EXIT:
                next_level();
            }

            state.ticked_early = ctx.tick_early && logic_delta > 10000LL;
            state.last_tick = current;
            ctx.tick_early = 0;
            ctx.want_redraw = 1;
        }
        logic_delta = 0;
    }

    return MAX(0, SEC/TPS - MAX(logic_delta, frame_delta));
}

/* This function is called on every key press */
void handle_key(xcb_keycode_t kc, uint16_t st, bool pressed) {
    xcb_keysym_t ksym = get_keysym(kc, st);
    switch (ksym) {
    case XK_R: // Restart game
        state.level = 0;
        next_level();
        break;
    case XK_w:
        ctx.tick_early = !state.keys.forward && pressed;
        state.keys.forward = pressed;
        break;
    case XK_s:
        ctx.tick_early = !state.keys.backward && pressed;
        state.keys.backward = pressed;
        break;
    case XK_a:
        ctx.tick_early = !state.keys.left && pressed;
        state.keys.left = pressed;
        break;
    case XK_d:
        ctx.tick_early = !state.keys.right && pressed;
        state.keys.right = pressed;
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

tile_t decode_wall(int x, int y) {
    char bottom = get_cell(x, y + 1);
    char right = get_cell(x + 1, y);
    char left = get_cell(x - 1, y);
    char bottom_right = get_cell(x + 1, y + 1);
    char bottom_left = get_cell(x - 1, y + 1);

    // Unfortunately tileset I use does not
    // contain all combinations of walls...
    // But lets try to get the best approximation

    if (left == WALL && bottom == WALL && bottom_left == VOID)
        return MKTILE(0, 5*10 + 3 + 2*(rand()&1));
    if (right == WALL && bottom == WALL && bottom_right == VOID)
        return MKTILE(0, 5*10 + 4*(rand()&1));

    if (bottom != WALL && bottom != VOID) return MKTILE(0, 1 + (rand()&3));
    if (left != WALL && left != VOID) return MKTILE(0, 5 + 10*(rand()&3));
    if (right != WALL && right != VOID) return MKTILE(0, 10*(rand()&3));

    if (bottom == VOID) {
        if (left == VOID && right == WALL) return MKTILE(0, 4*10 + 0);
        else if (left == WALL && right == VOID) return MKTILE(0, 4*10 + 5);
        return MKTILE(0, 4*10 + 1 + (rand()&3));
    }

    if (left == WALL && right == VOID) return MKTILE(0, 5 + 10*(rand()&3));
    if (left == VOID && right == WALL) return MKTILE(0, 10*(rand()&3));

    return MKTILE(0, 6*10+9);
}

tile_t decode_floor(int x, int y) {
    char bottom = get_cell(x, y + 1);
    char right = get_cell(x + 1, y);
    char left = get_cell(x - 1, y);
    char top = get_cell(x, y - 1);

    if (top == WALL) {
        if (left == WALL && right != WALL)
            return MKTILE(0, 1*10+1);
        if (left != WALL && right == WALL)
            return MKTILE(0, 1*10+4);
        return MKTILE(0, 1*10 + 2+(rand() & 1));
    }

    if (bottom == WALL) {
        if (left == WALL && right != WALL)
            return MKTILE(0, 3*10+1);
        if (left != WALL && right == WALL)
            return MKTILE(0, 3*10+4);
        return MKTILE(0, 3*10 + 2+(rand() & 1));
    }

    if (left == WALL) return MKTILE(0, 2*10 + 1);
    if (right == WALL) return MKTILE(0, 2*10 + 4);

    return MKTILE(0, (rand() % 3)*10 + (rand() & 3) + 6);
}


void load_map_from_file(const char *file) {
    srand(123);

    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        warn("Can't open tile map '%s': %s", file, strerror(errno));
        return;
    }

    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0) {
        warn("Can't stat tile map '%s': %s", file, strerror(errno));
        close(fd);
        return;
    }

    // +1 to make file contents NULL-terminated
    char *addr = mmap(NULL, statbuf.st_size + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        warn("Can't mmap tile map '%s': %s", file, strerror(errno));
        return;
    }

    if (state.mapchars) munmap(state.mapchars, state.mapchars_size);
    state.mapchars = addr;
    state.mapchars_size = statbuf.st_size + 1;

    size_t width = 0, height = 0;
    char *nel = addr;
    do {
        char *newnel = strchr(nel, '\n');
        if (newnel) {
            height++;
            if (width) {
                if ((ssize_t)width != newnel - nel)
                    goto format_error;
            } else {
                width = newnel - nel;
            }
            newnel++;
        }
        nel = newnel;
    } while (nel);

    if (!height) {
format_error:
        warn("Wrong tile tile map '%s' format", file);
        munmap(addr, statbuf.st_size + 1);
        return;
    }

    if (state.map) free_tilemap(state.map);
    state.map = create_tilemap(width, height, TILE_WIDTH, TILE_HEIGHT, state.tilesets, NTILESETS);
    tilemap_set_scale(state.map, ctx.scale);

    int16_t x = 0, y = 0;
    for (char *ptr = addr, c; (c = *ptr); ptr++) {
        switch(c) {
        case WALL: /* wall */;
            tilemap_set_tile(state.map, x, y, 0, decode_wall(x, y));
            x++;
            break;
        case VOID: /* void */
            tilemap_set_tile(state.map, x++, y, 0, TILE_VOID);
            break;
        case '@': /* player start */
            state.char_x = x, state.char_y = y;
            tilemap_set_tile(state.map, x, y, 0, decode_floor(x, y));
            tilemap_set_tile(state.map, x++, y, ENT_LAYER, TILE_PLAYER);
            break;
        case TRAP: /* trap */
            tilemap_set_tile(state.map, x++, y, 1, TILE_TRAP);
            break;
        case 'x': /* exit */
            tilemap_set_tile(state.map, x, y, 1, TILE_EXIT);
            // fallthrough
        case '.': /* floor */
            tilemap_set_tile(state.map, x, y, 0, decode_floor(x, y));
            x++;
            break;
        case '\n': /* new line */
            x = 0, y++;
            break;
        default:
            free_tilemap(state.map);
            state.map = NULL;
            goto format_error;
        }
    }
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
                    .next_frame = tileset_descs[i].animated ?
                            y*tileset_descs[i].x + (x + 1) % tileset_descs[i].x :
                            tile_id,
                };
                tile_id++;
            }
        }
        state.tilesets[i] = create_tileset(tileset_descs[i].path, tiles, tile_id);
    }

    next_level();
}

void cleanup(void) {
    free_tilemap(state.map);
    munmap(state.mapchars, state.mapchars_size);
    for (size_t i = 0; i < NTILESETS; i++) unref_tileset(state.tilesets[i]);
}
