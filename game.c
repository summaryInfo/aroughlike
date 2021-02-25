/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "context.h"
#include "image.h"
#include "tilemap.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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

#define ENT_LAYER 2

enum wall_type {
    t_void,
    t_floor,
    t_wall,
};

inline static enum wall_type get_wall_type(char *ptr, int width, int height, int x, int y) {
    if (width == x || x == -1) return t_void;
    if (height == y || y == -1) return t_void;

    switch (ptr[x+y*(width+1)]) {
    case ' ':
        return t_void;
    case '.':
    case '@':
    case 'x':
    case 'T':
        return t_floor;
    case '#':
        return t_wall;
    default:
        assert(0);
    }
}

/* This function is called TPS times a second
 * (this is a place where all game logic resides) */
void tick(struct timespec time) {
    (void)time;

    tilemap_animation_tick(state.map);
    
    tile_t old_player = tilemap_set_tile(state.map, state.char_x, state.char_y, ENT_LAYER, NOTILE);
    int dx = state.keys.right - state.keys.left;
    int dy = state.keys.backward - state.keys.forward;

    enum wall_type wt = get_wall_type(state.mapchars, state.map->width,
                                      state.map->height, state.char_x + dx, state.char_y + dy);
    enum wall_type wt1 = get_wall_type(state.mapchars, state.map->width,
                                      state.map->height, state.char_x + dx, state.char_y);
    enum wall_type wt2 = get_wall_type(state.mapchars, state.map->width,
                                      state.map->height, state.char_x, state.char_y + dy);
    if (wt == t_floor) {
        state.char_x += dx;
        state.char_y += dy;
    } else if (wt1 == t_floor) {
        state.char_x += dx;
    } else if (wt2 == t_floor) {
        state.char_y += dy;
    }

    tilemap_set_tile(state.map, state.char_x, state.char_y, ENT_LAYER, old_player);

    ctx.want_redraw = 1;
}

/* This function is called on every key press */
void handle_key(xcb_keycode_t kc, uint16_t st, bool pressed) {
    xcb_keysym_t ksym = get_keysym(kc, st);
    switch (ksym) {
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

tile_t decode_wall(char *a, int w, int h, int x, int y) {
    enum wall_type bottom = get_wall_type(a, w, h, x, y + 1);
    enum wall_type right = get_wall_type(a, w, h, x + 1, y);
    enum wall_type left = get_wall_type(a, w, h, x - 1, y);
    enum wall_type bottom_right = get_wall_type(a, w, h, x + 1, y + 1);
    enum wall_type bottom_left = get_wall_type(a, w, h, x - 1, y + 1);

    // Unfortunately tileset I use does not
    // contain all combinations of walls...
    // But lets try to get the best approximation

    if (left == t_wall && bottom == t_wall && bottom_left == t_void)
        return MKTILE(0, 5*10 + 3 + 2*(rand()&1));
    if (right == t_wall && bottom == t_wall && bottom_right == t_void)
        return MKTILE(0, 5*10 + 4*(rand()&1));

    if (bottom == t_floor) return MKTILE(0, 1 + (rand()&3));
    if (left == t_floor) return MKTILE(0, 5 + 10*(rand()&3));
    if (right == t_floor) return MKTILE(0, 10*(rand()&3));

    if (bottom == t_void) {
        if (left == t_void && right == t_wall) return MKTILE(0, 4*10 + 0);
        else if (left == t_wall && right == t_void) return MKTILE(0, 4*10 + 5);
        return MKTILE(0, 4*10 + 1 + (rand()&3));
    }

    if (left == t_wall && right == t_void) return MKTILE(0, 5 + 10*(rand()&3));
    if (left == t_void && right == t_wall) return MKTILE(0, 10*(rand()&3));

    return MKTILE(0, 6*10+9);
}

tile_t decode_floor(char *a, int w, int h, int x, int y) {
    enum wall_type bottom = get_wall_type(a, w, h, x, y + 1);
    enum wall_type right = get_wall_type(a, w, h, x + 1, y);
    enum wall_type left = get_wall_type(a, w, h, x - 1, y);
    enum wall_type top = get_wall_type(a, w, h, x, y - 1);

    if (top == t_wall) {
        if (left == t_wall && right != t_wall)
            return MKTILE(0, 1*10+1);
        if (left != t_wall && right == t_wall)
            return MKTILE(0, 1*10+4);
        return MKTILE(0, 1*10 + 2+(rand() & 1));
    }

    if (bottom == t_wall) {
        if (left == t_wall && right != t_wall)
            return MKTILE(0, 3*10+1);
        if (left != t_wall && right == t_wall)
            return MKTILE(0, 3*10+4);
        return MKTILE(0, 3*10 + 2+(rand() & 1));
    }

    if (left == t_wall) return MKTILE(0, 2*10 + 1);
    if (right == t_wall) return MKTILE(0, 2*10 + 4);

    return MKTILE(0, (rand() % 3)*10 + (rand() & 3) + 6);
}


struct tilemap *create_tilemap_from_tile(const char *file) {
    srand(123);

    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        warn("Can't open tile map '%s': %s", file, strerror(errno));
        return NULL;
    }

    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0) {
        warn("Can't stat tile map '%s': %s", file, strerror(errno));
        close(fd);
        return NULL;
    }

    // +1 to make file contents NULL-terminated
    char *addr = mmap(NULL, statbuf.st_size + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        warn("Can't mmap tile map '%s': %s", file, strerror(errno));
        return NULL;
    }

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
        return NULL;
    }
    struct tilemap *map = create_tilemap(width, height, TILE_WIDTH, TILE_HEIGHT, state.tilesets, NTILESETS);

    int16_t x = 0, y = 0;
    for (char *ptr = addr, c; (c = *ptr); ptr++) {
        switch(c) {
        case '#': /* wall */;
            tilemap_set_tile(map, x, y, 0, decode_wall(addr, width, height, x, y));
            x++;
            break;
        case ' ': /* void */
            tilemap_set_tile(map, x++, y, 0, TILE_VOID);
            break;
        case '@': /* player start */
            state.char_x = x, state.char_y = y;
            tilemap_set_tile(map, x, y, ENT_LAYER, TILE_PLAYER);
            tilemap_set_tile(map, x, y, 0, decode_floor(addr, width, height, x, y));
            x++;
            break;
        case 'T': /* trap */
            tilemap_set_tile(map, x++, y, 1, TILE_TRAP);
            break;
        case 'x': /* exit */
            tilemap_set_tile(map, x, y, 1, TILE_EXIT);
            // fallthrough
        case '.': /* floor */
            tilemap_set_tile(map, x, y, 0, decode_floor(addr, width, height, x, y));
            x++;
            break;
        case '\n': /* new line */
            x = 0, y++;
            break;
        default:
            free_tilemap(map);
            goto format_error;
        }
    }

    if (state.mapchars)
        munmap(state.mapchars, state.mapchars_size);

    state.mapchars = addr;
    state.mapchars_size = statbuf.st_size + 1;
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
                    .next_frame = tileset_descs[i].animated ?
                            y*tileset_descs[i].x + (x + 1) % tileset_descs[i].x :
                            tile_id,
                };
                tile_id++;
            }
        }
        state.tilesets[i] = create_tileset(tileset_descs[i].path, tiles, tile_id);
    }

    state.map = create_tilemap_from_tile("data/map_1.txt");
    tilemap_set_scale(state.map, ctx.scale);
}

void cleanup(void) {
    free_tilemap(state.map);
    munmap(state.mapchars, state.mapchars_size);
    for (size_t i = 0; i < NTILESETS; i++) unref_tileset(state.tilesets[i]);
}
