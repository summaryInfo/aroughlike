/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "context.h"
#include "image.h"
#include "tilemap.h"
#include "worker.h"

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

#define TILESET_STATIC 0
#define TILESET_ANIMATED 1
#define TILESET_ENTITIES 2

#define TILE_VOID MKTILE(TILESET_STATIC, 10*7+8)
#define TILE_FLOOR MKTILE(TILESET_STATIC, 10*0+6)
#define TILE_TRAP MKTILE(TILESET_ANIMATED, 24*4+2)
#define TILE_EXIT MKTILE(TILESET_STATIC, 10*3+9)
#define TILE_POISON MKTILE(TILESET_ANIMATED, 4*17+ 3)
#define TILE_POISON_STATIC MKTILE(TILESET_STATIC, 10*8+ 9)

#define WALL '#'
#define TRAP 'T'
#define VOID ' '
#define EXIT 'x'
#define POISON 'P'

struct gamestate {
    struct tilemap *map;
    struct tileset *tilesets[NTILESETS];
    char *mapchars;
    size_t mapchars_size;

    int16_t camera_x;
    int16_t camera_y;

    struct player {
        int16_t x;
        int16_t y;
        double dx;
        double dy;
        tile_t tile;
        int lives;
    } player;

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
    int32_t frame_n;

    struct input_state {
        bool forward : 1;
        bool backward : 1;
        bool left : 1;
        bool right : 1;
    } keys;
} state;

void load_map_from_file(const char *file);

/* This is main drawing function, that is called
 * FPS times a second */
void redraw(struct timespec current) {
    int16_t map_x = state.camera_x + ctx.backbuf.width/2;
    int16_t map_y = state.camera_y + ctx.backbuf.height/2;
    int16_t map_h = ctx.scale*state.map->height*TILE_WIDTH;
    int16_t map_w = ctx.scale*state.map->width*TILE_WIDTH;

    /* Clear screen */
    image_draw_rect(ctx.backbuf, (struct rect){0, 0, ctx.backbuf.width, map_y}, BG_COLOR);
    image_draw_rect(ctx.backbuf, (struct rect){0, map_y, map_x, map_h}, BG_COLOR);
    image_draw_rect(ctx.backbuf, (struct rect){0, map_y + map_h, ctx.backbuf.width, ctx.backbuf.height - map_y - map_h}, BG_COLOR);
    image_draw_rect(ctx.backbuf, (struct rect){map_x + map_w, map_y, ctx.backbuf.width - map_x - map_w, map_h}, BG_COLOR);

    /* Draw map */
    tilemap_draw(ctx.backbuf, state.map, map_x, map_y);

    /* Draw player */
    int16_t player_x = map_x + ctx.scale*TILE_WIDTH*(state.player.x);
    int16_t player_y = map_y + ctx.scale*TILE_HEIGHT*(state.player.y);

    if (state.state == s_normal) {
        // Smoothly animate player movements...

        double dt = 1 - TIMEDIFF(state.last_tick, current)/(10.*SEC/TPS);
        // Bezier curve for ease-in-out effect
        dt = dt * dt * (3. - 2. * dt);

        player_x -= ctx.scale*TILE_WIDTH*state.player.dx*dt;
        player_y -= ctx.scale*TILE_HEIGHT*state.player.dy*dt;
    }

    tileset_draw_tile(ctx.backbuf, state.tilesets[TILESET_ID(state.player.tile)],
                      TILE_ID(state.player.tile), player_x, player_y, ctx.scale);

    /* Draw lives */

    for (int i = 0; i < state.player.lives; i++) {
        int16_t px = 20 + (state.player.lives - i)*TILE_WIDTH*ctx.interface_scale/2;
        int16_t py = 24 - 8*(i & 1) ;
        tileset_draw_tile(ctx.backbuf, state.tilesets[TILESET_ID(TILE_POISON_STATIC)],
                          TILE_ID(TILE_POISON_STATIC), px, py, ctx.interface_scale);
    }


    if (state.state == s_win) /* Draw win screen */ {
        image_draw_rect(ctx.backbuf, (struct rect){ctx.backbuf.width/4,
                        ctx.backbuf.height/4, ctx.backbuf.width/2, ctx.backbuf.height/2}, 0xFF00FF00);
    } else if (state.state == s_game_over) /* Draw game over message */ {
        image_draw_rect(ctx.backbuf, (struct rect){ctx.backbuf.width/4,
                        ctx.backbuf.height/4, ctx.backbuf.width/2, ctx.backbuf.height/2}, 0xFFFF0000);
    }
}

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
        state.keys = (struct input_state) {0};
        state.state = s_normal;
        load_map_from_file(buf);
    } else state.state = s_win;;
}

/* This function is called TPS times a second
 * (this is a place where all game logic resides) */
int64_t tick(struct timespec current) {
    int64_t frame_delta = TIMEDIFF(state.last_frame, current);
    int64_t logic_delta = TIMEDIFF(state.last_tick, current);

    if (frame_delta >= SEC/TPS - 10000LL) {
        state.frame_n = (state.frame_n + 1) % TPS;

        // Tick animaionts eveny 10 ticks
        if (state.frame_n % 10 == 0) {
            tilemap_animation_tick(state.map);
            // Player is not a part of the map
            struct tile *tile = &state.tilesets[TILESET_ID(state.player.tile)]->tiles[TILE_ID(state.player.tile)];
            state.player.tile = MKTILE(TILESET_ID(state.player.tile), tile->next_frame);
        }

        // Move camera towards player

        double x_speed_scale = MIN(ctx.backbuf.width/5, 512)/ctx.scale;
        double y_speed_scale = MIN(ctx.backbuf.height/4, 512)/ctx.scale;
        int32_t cam_dx = -pow((state.camera_x + (state.player.x*TILE_WIDTH +
                TILE_WIDTH/2)*state.map->scale)/x_speed_scale, 3) * frame_delta / (double)(SEC/TPS) / 12;
        int32_t cam_dy = -pow((state.camera_y + (state.player.y*TILE_HEIGHT +
                TILE_HEIGHT/2)*state.map->scale)/y_speed_scale, 3) * frame_delta / (double)(SEC/TPS) / 12;
        state.camera_x += MAX(-ctx.dpi, MIN(cam_dx, ctx.dpi));
        state.camera_y += MAX(-ctx.dpi, MIN(cam_dy, ctx.dpi));

        state.last_frame = current;
        frame_delta = 0;

        if (state.frame_n % 10 == 0 || cam_dx ||
            cam_dy || state.player.dx || state.player.dy) {
            ctx.want_redraw = 1;
        }
    }

    if ((logic_delta >= 10*SEC/TPS - 10000LL || (ctx.tick_early && !state.ticked_early))) {
        state.ticked_early = ctx.tick_early && logic_delta > 10000LL;
        if (state.state == s_normal) {

            // Update traps
            tilemap_random_tick(state.map);

            int16_t dx = state.keys.right - state.keys.left;
            int16_t dy = state.keys.backward - state.keys.forward;

            // It's complicated to allow wall gliding
            if (get_cell(state.player.x + dx, state.player.y + dy) != WALL) {
                state.player.x += dx;
                state.player.y += dy;
            } else if (get_cell(state.player.x + dx, state.player.y) != WALL) {
                state.player.x += dx;
                dy = 0;
            } else if (get_cell(state.player.x, state.player.y + dy) != WALL) {
                state.player.y += dy;
                dx = 0;
            } else {
                dx = 0;
                dy = 0;
            }

            if (state.ticked_early) {
                double dt = 1 - TIMEDIFF(state.last_tick, current)/(10.*SEC/TPS);
                // Bezier curve for ease-in-out effect
                dt = dt * dt * (3. - 2. * dt);
                state.player.dx = state.player.dx*dt + dx;
                state.player.dy = state.player.dy*dt + dy;
            } else {
                state.player.dx = dx;
                state.player.dy = dy;
            }

            switch(get_cell(state.player.x, state.player.y)) {
            case VOID:
            case WALL:
                state.state = s_game_over;
                break;
            case TRAP:
                if (tilemap_get_tile(state.map, state.player.x,
                        state.player.y, 0) != TILE_TRAP && !--state.player.lives) {
                    state.state = s_game_over;
                }
                break;
            case EXIT:
                next_level();
                break;
            case POISON:
                state.player.lives++;
                state.mapchars[(state.map->width + 1)*state.player.y + state.player.x] = '.';
                tilemap_set_tile(state.map, state.player.x, state.player.y, 1, NOTILE);
            }

            ctx.want_redraw = 1;
        }

        state.last_tick = current;
        ctx.tick_early = 0;
        logic_delta = 0;
    }

    return MAX(0, MIN(10*SEC/TPS - logic_delta, SEC/TPS - frame_delta));
}

void reset_game(void) {
    state.level = 0;
    state.player.lives = 1;
    next_level();

    // Select random player model
    int r = rand() % 7;
    state.player.tile = MKTILE(TILESET_ENTITIES, 4*(2*r+(r>2)) + 3);
}

/* This function is called on every key press */
void handle_key(xcb_keycode_t kc, uint16_t st, bool pressed) {
    xcb_keysym_t ksym = get_keysym(kc, st);
    //warn("Key %x (%c) %s", ksym, ksym, pressed ? "down" : "up");
    switch (ksym) {
    case XK_R: // Restart game
        if (pressed) reset_game();
        break;
    case XK_w:
        ctx.tick_early = !state.keys.forward;
        state.keys.forward = pressed;
        break;
    case XK_s:
        ctx.tick_early = !state.keys.backward;
        state.keys.backward = pressed;
        break;
    case XK_a:
        ctx.tick_early = !state.keys.left;
        state.keys.left = pressed;
        break;
    case XK_d:
        ctx.tick_early = !state.keys.right;
        state.keys.right = pressed;
        break;
    case XK_minus:
        ctx.scale = MAX(1., ctx.scale - pressed);
        tilemap_set_scale(state.map, ctx.scale);
        ctx.want_redraw = 1;
        break;
    case XK_equal:
    case XK_plus:
        ctx.scale = MIN(ctx.scale + pressed, 20);
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

tile_t decode_wall_decoration(int x, int y) {
    char bottom = get_cell(x, y + 1);
    char left = get_cell(x - 1, y);
    char cur = get_cell(x, y);

    if (bottom != WALL && bottom != VOID && cur == WALL) {
        // Horizontal wall
        int r = rand();
        if (r % 10 == 3) {
            return  MKTILE(TILESET_ANIMATED, 9*4 + 2); /* Shield */
        } else if (r % 10 == 5) {
            return MKTILE(TILESET_ANIMATED, 26*4 + 2); /* Torch */
        }
    }
    if (left == WALL && cur != WALL && cur != VOID) {
        // Horizontal wall
        if (rand() % 10 == 0) {
            return MKTILE(TILESET_ANIMATED, 25*4 + 2); /* Torch */
        }
    }

    if (cur == '.') {
        int r = rand();
        if (r % 20 == 3) {
            switch((r/20) % 7) {
            case 0: return MKTILE(TILESET_ANIMATED, 5*4 + 2); /* Torch */
            case 1: return MKTILE(TILESET_ANIMATED, 4*4 + 2); /* Torch */
            default:;
            }
        }
    }

    return NOTILE;
}

tile_t decode_floor(int x, int y) {
    char bottom = get_cell(x, y + 1);
    char right = get_cell(x + 1, y);
    char left = get_cell(x - 1, y);
    char top = get_cell(x, y - 1);

    if (top == WALL) {
        if (left == WALL && right != WALL) return MKTILE(0, 1*10+1);
        if (left != WALL && right == WALL) return MKTILE(0, 1*10+4);
        return MKTILE(0, 1*10 + 2+(rand() & 1));
    }

    if (bottom == WALL) {
        if (left == WALL && right != WALL) return MKTILE(0, 3*10+1);
        if (left != WALL && right == WALL) return MKTILE(0, 3*10+4);
        return MKTILE(0, 3*10 + 2+(rand() & 1));
    }

    if (left == WALL) return MKTILE(0, 2*10 + 1);
    if (right == WALL) return MKTILE(0, 2*10 + 4);

    return MKTILE(0, (rand() % 3)*10 + (rand() & 3) + 6);
}


void load_map_from_file(const char *file) {
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
            state.player.x = x, state.player.y = y;
            tilemap_set_tile(state.map, x, y, 0, decode_floor(x, y));
            x++;
            break;
        case TRAP: /* trap */
            tilemap_set_tile(state.map, x++, y, 0, TILE_TRAP);
            break;
        case POISON: /* poison */
            tilemap_set_tile(state.map, x, y, 0, decode_floor(x, y));
            tilemap_set_tile(state.map, x++, y, 1, TILE_POISON);
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

    for (x = 0; x < (int)width; x++) {
        for (y = 0; y < (int)height; y++) {
            // Decorate walls
            tilemap_set_tile(state.map, x, y, 2, decode_wall_decoration(x, y));
        }
    }

    state.player.dx = state.player.dy = 0;
    state.camera_y = state.camera_x = 50*ctx.scale;
}

struct tileset_desc {
    const char *path;
    size_t x, y;
    bool animated;
    size_t i;
};

void do_load(void *varg) {
    struct tileset_desc *arg = varg;

    struct tile *tiles = calloc(arg->x*arg->y, sizeof(*tiles));
    tile_t tile_id = 0;
    for (size_t y = 0; y < arg->y; y++) {
        for (size_t x = 0; x < arg->x; x++) {
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
                .next_frame = arg->animated ?
                        y*arg->x + (x + 1) % arg->x :
                        tile_id,
            };
            tile_id++;
        }
    }
    state.tilesets[arg->i] = create_tileset(arg->path, tiles, tile_id);

}

void init(void) {
    struct tileset_desc descs[NTILESETS] = {
        {"data/tiles.png", 10, 10, 0, TILESET_STATIC},
        {"data/ani.png", 4, 27, 1, TILESET_ANIMATED},
        {"data/ent.png", 4, 14, 1, TILESET_ENTITIES},
    };

    for (size_t i = 0; i < NTILESETS; i++) {
        submit_work(do_load, descs + i, sizeof *descs);
    }

    drain_work();
    reset_game();
}

void cleanup(void) {
    free_tilemap(state.map);
    munmap(state.mapchars, state.mapchars_size);
    for (size_t i = 0; i < NTILESETS; i++) {
        unref_tileset(state.tilesets[i]);
    }
}
