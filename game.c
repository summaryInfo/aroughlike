/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "context.h"
#include "image.h"
#include "keys.h"
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

#define TILE_WIDTH 16
#define TILE_HEIGHT 16
#define NTILESETS 4

#define TILESET_STATIC 0
#define TILESET_ANIMATED 1
#define TILESET_ENTITIES 2
#define TILESET_ASCII 3

#define TILE_VOID MKTILE(TILESET_STATIC, 10*7+8)
#define TILE_FLOOR MKTILE(TILESET_STATIC, 10*0+6)
#define TILE_TRAP MKTILE(TILESET_ANIMATED, 24*4+2)
#define TILE_EXIT MKTILE(TILESET_STATIC, 10*3+9)
#define TILE_POISON MKTILE(TILESET_ANIMATED, 4*17+3)
#define TILE_POISON_STATIC MKTILE(TILESET_STATIC, 10*8+9)
#define TILE_IPOISON MKTILE(TILESET_ANIMATED, 4*16+3)
#define TILE_IPOISON_STATIC MKTILE(TILESET_STATIC, 10*9+7)
#define TILE_SPOISON MKTILE(TILESET_ANIMATED, 4*14+3)
#define TILE_SPOISON_STATIC MKTILE(TILESET_STATIC, 10*9+8)
#define TILE_SIPOISON MKTILE(TILESET_ANIMATED, 4*15+3)
#define TILE_SIPOISON_STATIC MKTILE(TILESET_STATIC, 10*8+7)
#define TILE_TORCH_TOP MKTILE(TILESET_ANIMATED, 26*4 + 2)
#define TILE_TORCH_LEFT MKTILE(TILESET_ANIMATED, 25*4 + 2)
#define TILE_TORCH_1 MKTILE(TILESET_ANIMATED, 5*4 + 2)
#define TILE_TORCH_2 MKTILE(TILESET_ANIMATED, 4*4 + 2)
#define TILE_BONES_1 MKTILE(TILESET_STATIC, 10*6 + 8)
#define TILE_BONES_2 MKTILE(TILESET_STATIC, 10*7 + 7)
#define TILE_FLAG_TOP MKTILE(TILESET_ANIMATED, 9*4 + 2)

#define WALL '#'
#define TRAP 'T'
#define VOID ' '
#define EXIT 'x'
#define POISON 'P'
#define IPOISON 'I'
#define SPOISON 'p'
#define SIPOISON 'i'
#define FLOOR '.'

#define INV_COLOR 0xFF62ABD4
#define INV_DUR (3*SEC)

#define STATIC_SCREEN_WIDTH 20
#define STATIC_SCREEN_HEIGHT 8

struct gamestate {
    struct tilemap *map;
    struct tilemap *death_screen;
    struct tilemap *win_screen;
    struct tilemap *greet_screen;
    struct tileset *tilesets[NTILESETS];
    char *mapchars;
    size_t mapchars_size;

    double camera_x;
    double camera_y;

    struct player {
        double x;
        double y;
        tile_t tile;
        int lives;
        // End of invincibility
        struct timespec inv_end;
        struct timespec inv_start;
    } player;

    enum state {
        s_normal,
        s_greet,
        s_win,
        s_game_over,
        s_MAX,
    } state;

    struct tilemap *screens[s_MAX];

    int level;

    /* Game logic is handled in fixed rate, separate from FPS */
    struct timespec last_update;
    struct timespec last_tick;
    struct timespec last_damage;
    /* Only one early tick is allowed */
    bool ticked_early;
    bool tick_early;
    int32_t tick_n;

    struct input_state {
        bool forward : 1;
        bool backward : 1;
        bool left : 1;
        bool right : 1;
    } keys;
} state;

struct tileset_desc {
    const char *path;
    size_t x, y;
    bool animated;
    size_t i;
};


void load_map_from_file(const char *file);

/* This is main drawing function, that is called
 * FPS times a second */
void redraw(struct timespec current) {
    int32_t map_x = state.camera_x + backbuf.width/2;
    int32_t map_y = state.camera_y + backbuf.height/2;
    int32_t map_h = state.map->scale*state.map->height*TILE_WIDTH;
    int32_t map_w = state.map->scale*state.map->width*TILE_WIDTH;

    /* Clear screen */
    image_queue_fill(backbuf, (struct rect){0, 0, backbuf.width, map_y}, BG_COLOR);
    image_queue_fill(backbuf, (struct rect){0, map_y, map_x, map_h}, BG_COLOR);
    image_queue_fill(backbuf, (struct rect){0, map_y + map_h, backbuf.width, backbuf.height - map_y - map_h}, BG_COLOR);
    image_queue_fill(backbuf, (struct rect){map_x + map_w, map_y, backbuf.width - map_x - map_w, map_h}, BG_COLOR);

    /* Draw map */
    tilemap_queue_draw(backbuf, state.map, map_x, map_y);

    drain_work();

    /* Draw player */
    int32_t player_x = map_x + state.map->scale*state.player.x;
    int32_t player_y = map_y + state.map->scale*state.player.y;
    tile_t player = state.player.tile;
    if (TIMEDIFF(state.last_damage, current) < SEC/2)
        player += (TILE_ID(player)/4 >= 6 ? -4 : +4);

    tileset_queue_tile(backbuf, state.tilesets[TILESET_ID(player)], TILE_ID(player), player_x, player_y, state.map->scale);

    /* Draw invincibility timer */
    int64_t inv_total = TIMEDIFF(state.player.inv_start, state.player.inv_end);
    int64_t inv_rest = MIN(TIMEDIFF(current, state.player.inv_end), inv_total);
    if (inv_rest > 0) {
        image_queue_fill(backbuf, (struct rect){0, 0,
                inv_rest*backbuf.width/inv_total, 4*scale.interface}, INV_COLOR);
    }

    /* Draw lives */
    for (int i = 0; i < (state.player.lives + 1)/2; i++) {
        int32_t px = 20 + ((state.player.lives + 1)/2 - i)*TILE_WIDTH*scale.interface/2;
        int32_t py = 24 - 8*(i & 1) ;
        tile_t lives_tile;
        if ((state.player.lives & 1) & !i) {
            px -= scale.interface;
            lives_tile = inv_rest > 0 ? TILE_SIPOISON_STATIC : TILE_SPOISON_STATIC;
        } else {
            lives_tile = inv_rest > 0 ? TILE_IPOISON_STATIC : TILE_POISON_STATIC;
        }
        tileset_queue_tile(backbuf, state.tilesets[TILESET_ID(lives_tile)],
                           TILE_ID(lives_tile), px, py, scale.interface);
        drain_work();
    }

    struct tilemap *screen_to_draw = state.screens[state.state];
    if (screen_to_draw) {
        int32_t sx = backbuf.width/2 - screen_to_draw->width*screen_to_draw->tile_width*screen_to_draw->scale/2;
        int32_t sy = backbuf.height/2 - screen_to_draw->height*screen_to_draw->tile_height*screen_to_draw->scale/2;
        tilemap_queue_draw(backbuf, screen_to_draw, sx, sy);
    }

    drain_work();
}

inline static char get_cell(int x, int y) {
    if ((ssize_t)state.map->width <= x || x < 0) return VOID;
    if ((ssize_t)state.map->height <= y || y < 0) return VOID;
    return state.mapchars[x+y*(state.map->width+1)];
}

inline static void set_cell(int x, int y, char cell) {
    if ((ssize_t)state.map->width <= x || x < 0) return;
    if ((ssize_t)state.map->height <= y || y < 0) return;
    state.mapchars[x+y*(state.map->width+1)] = cell;
}

inline static void next_level(void) {
    char buf[20];
    struct stat st;

    snprintf(buf, sizeof buf, "data/map_%d.txt", ++state.level);
    if (stat(buf, &st) == 0) {
        state.keys = (struct input_state) {0};
        state.state = s_normal;
        state.camera_y = state.camera_x = 50*scale.map;
        load_map_from_file(buf);
    } else state.state = s_win;;
}

inline static struct rect get_bounding_box_for(char cell, int32_t x, int16_t y) {
    struct rect bb = {x*TILE_HEIGHT, y*TILE_WIDTH, TILE_WIDTH, TILE_HEIGHT};
    switch (cell) {
    case WALL:
        if (get_cell(x, y + 1) != WALL) bb.height /= 2;
        char left = get_cell(x - 1, y);
        char right = get_cell(x + 1, y);
        if (left == VOID && right != VOID) bb.width /= 2, bb.x += TILE_WIDTH/2;
        else if (left != VOID && right == VOID) bb.width /= 2;
        break;
    case EXIT:
    case VOID:
        return (struct rect) {
            x*TILE_WIDTH + TILE_WIDTH/2,
            y*TILE_HEIGHT + TILE_HEIGHT/2,
            0, 0,
        };
    case POISON:
    case IPOISON:
        return (struct rect) {
            x*TILE_WIDTH + TILE_WIDTH/3,
            y*TILE_HEIGHT + TILE_HEIGHT/3,
            TILE_WIDTH/3, TILE_WIDTH/3,
        };
    default:;
    }
    return bb;
}


/* This function is called TPS times a second
 * (this is a place where all game logic resides) */
int64_t tick(struct timespec current) {
    int64_t update_time = SEC/TPS - TIMEDIFF(state.last_update, current);
    if (update_time <= 10000LL) {
        if (state.tick_n++ % 10 == 0) {
            // Anitmation plays
            // at conanstant rate
            tilemap_animation_tick(state.map);
            if (state.screens[state.state]) {
                tilemap_animation_tick(state.screens[state.state]);
                tilemap_refresh(state.screens[state.state]);
            }
            struct tile *tile = &state.tilesets[TILESET_ID(state.player.tile)]->tiles[TILE_ID(state.player.tile)];
            state.player.tile = MKTILE(TILESET_ID(state.player.tile), tile->next_frame);
        }
        tilemap_random_tick(state.map);
        state.last_update = current;
        update_time = SEC/TPS;
        want_redraw = 1;
    }

    int64_t tick_delta = TIMEDIFF(state.last_tick, current);
    int64_t tick_time = SEC/TPS - tick_delta;
    if (tick_time <= 10000LL || state.tick_early) {
        // Move camera towards player

        double x_speed_scale = MIN(backbuf.width/5, 512)/MAX(state.map->scale, 2);
        double y_speed_scale = MIN(backbuf.height/4, 512)/MAX(state.map->scale, 2);

        double cam_dx = -pow((state.camera_x + (state.player.x + TILE_WIDTH/2)*state.map->scale)/x_speed_scale, 3) * tick_delta / (double)(SEC/TPS) / 12;
        double cam_dy = -pow((state.camera_y + (state.player.y + TILE_HEIGHT/2)*state.map->scale)/y_speed_scale, 3) * tick_delta / (double)(SEC/TPS) / 12;

        if (fabs(cam_dx) < .8) cam_dx = 0;
        if (fabs(cam_dy) < .8) cam_dy = 0;

        state.camera_x += MAX(-scale.dpi, MIN(cam_dx, scale.dpi));
        state.camera_y += MAX(-scale.dpi, MIN(cam_dy, scale.dpi));


        double old_px = state.player.x;
        double old_py = state.player.y;

        if (state.state == s_normal) {
            // Move player

            double speed = state.keys.right - state.keys.left +
                    state.keys.backward - state.keys.forward == 2 ? sqrt(2) : 2;
            double dx = speed*(state.keys.right - state.keys.left) * tick_delta / (double)(SEC/TPS);
            double dy = speed*(state.keys.backward - state.keys.forward) * tick_delta / (double)(SEC/TPS);

            state.player.x += dx;
            state.player.y += dy;

            int32_t px = (state.player.x + TILE_WIDTH/2)/TILE_WIDTH;
            int32_t py = (state.player.y + TILE_HEIGHT/2)/TILE_HEIGHT;

            // Handle collisions

            for (int32_t y = -1; y <= 1; y++) {
                for (int32_t x = -1; x <= 1; x++) {
                    double x0 = state.player.x, y0 = state.player.y;
                    char cell = get_cell(px + x, py + y);
                    struct rect bb = get_bounding_box_for(cell, px + x, py + y);
                    /* Signed depths for x and y axes.
                     * They are equal to the distance player
                     * should be moved to not intersect with
                     * the bounding box.
                     */
                    double hy = ((y0 < bb.y ? MAX(0, y0 + TILE_HEIGHT - bb.y) : MIN(0, y0 - bb.y - bb.height)));
                    double hx = ((x0 < bb.x ? MAX(0, x0 + TILE_WIDTH - bb.x) : MIN(0, x0 - bb.x - bb.width)));
                    switch (cell) {
                    case WALL:
                        if (fabs(hx) < fabs(hy)) state.player.x -= hx;
                        else state.player.y -= hy;
                        break;
                    case POISON:
                    case IPOISON:
                    case SPOISON:
                    case SIPOISON:
                        if (fmin(fabs(hx), fabs(hy)) > 0) {
                            if (cell == POISON) state.player.lives += 2;
                            else if (cell == SPOISON) state.player.lives++;
                            else {
                                int64_t inc = (1 + (cell == IPOISON))*INV_DUR;
                                state.player.inv_start = current;
                                if (TIMEDIFF(state.player.inv_end, current) > 0)
                                    state.player.inv_end = current;
                                TIMEINC(state.player.inv_end, inc);
                            }
                            set_cell(px + x, py + y, FLOOR);
                            tilemap_set_tile(state.map, px+x, py+y, 1, NOTILE);
                            want_redraw = 1;
                        }
                        break;
                    case VOID:
                        if (fmin(fabs(hx), fabs(hy)) > 0) {
                            state.state = s_game_over;
                            want_redraw = 1;
                        }
                        break;
                    case TRAP:
                        if (fmin(fabs(hx), fabs(hy)) > 0) {
                            bool trap_is_active = tilemap_get_tile(state.map, px+x, py+y, 0) != TILE_TRAP;
                            bool damaged_recently = TIMEDIFF(state.last_damage, current) < SEC;
                            bool invincible = TIMEDIFF(state.player.inv_end, current) < 0;
                            if (trap_is_active && !damaged_recently && !invincible) {
                                state.player.lives -= 2;
                                if (state.player.lives <= 0)
                                    state.state = s_game_over;
                                state.last_damage = current;
                                want_redraw = 1;
                            }
                        }
                        break;
                    case EXIT:
                        if (fmin(fabs(hx), fabs(hy)) > 0) {
                            next_level();
                            want_redraw = 1;
                        }
                        break;
                    }
                }
            }
        }

        state.last_tick = current;
        state.tick_early = 0;
        tick_time = TPS/SEC;

        bool camera_moved = cam_dx || cam_dy;
        bool player_moved = (int32_t)old_px != (int32_t)state.player.x ||
                (int32_t)old_py != (int32_t)state.player.y;
        want_redraw |= camera_moved || player_moved;
    }

    tilemap_refresh(state.map);

    return MAX(0, MIN(update_time, tick_time));
}

void reset_game(void) {
    state.level = 0;
    state.player.lives = 1;
    state.player.inv_end = state.player.inv_start = (struct timespec){0};
    next_level();

    // Select random player model
    int r = rand() % 7;
    state.player.tile = MKTILE(TILESET_ENTITIES, 4*(2*r+(r>2)) + 3);
}

/* This function is called on every key press */
void handle_key(uint8_t kc, uint32_t st, bool pressed) {
    uint32_t ksym = get_keysym(kc, st);

    if (ksym < 0xFF && state.state == s_greet) {
        want_redraw = 1;
        state.state = s_normal;
    }

    //warn("Key %x (%c) %s", ksym, ksym, pressed ? "down" : "up");
    switch (ksym) {
    case k_R: // Restart game
        if (pressed) reset_game();
        break;
    case k_w:
    case k_Up:
        state.tick_early = !state.keys.forward;
        state.keys.forward = pressed;
        break;
    case k_s:
    case k_Down:
        state.tick_early = !state.keys.backward;
        state.keys.backward = pressed;
        break;
    case k_a:
    case k_Left:
        state.tick_early = !state.keys.left;
        state.keys.left = pressed;
        break;
    case k_d:
    case k_Right:
        state.tick_early = !state.keys.right;
        state.keys.right = pressed;
        break;
    case k_minus:
        scale.map = MAX(1., scale.map - pressed);
        tilemap_set_scale(state.map, scale.map);
        want_redraw = 1;
        break;
    case k_equal:
    case k_plus:
        scale.map = MIN(scale.map + pressed, 20);
        tilemap_set_scale(state.map, scale.map);
        want_redraw = 1;
        break;
    case k_Escape:
        want_exit = 1;
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
        return MKTILE(TILESET_STATIC, 5*10 + 3 + 2*(rand()&1));
    if (right == WALL && bottom == WALL && bottom_right == VOID)
        return MKTILE(TILESET_STATIC, 5*10 + 4*(rand()&1));

    if (bottom != WALL && bottom != VOID) return MKTILE(TILESET_STATIC, 1 + (rand()&3));
    if (left != WALL && left != VOID) return MKTILE(TILESET_STATIC, 5 + 10*(rand()&3));
    if (right != WALL && right != VOID) return MKTILE(TILESET_STATIC, 10*(rand()&3));

    if (bottom == VOID) {
        if (left == VOID && right == WALL) return MKTILE(TILESET_STATIC, 4*10 + 0);
        else if (left == WALL && right == VOID) return MKTILE(TILESET_STATIC, 4*10 + 5);
        return MKTILE(TILESET_STATIC, 4*10 + 1 + (rand()&3));
    }

    if (left == WALL && right == VOID) return MKTILE(TILESET_STATIC, 5 + 10*(rand()&3));
    if (left == VOID && right == WALL) return MKTILE(TILESET_STATIC, 10*(rand()&3));

    return MKTILE(TILESET_STATIC, 6*10+9);
}

tile_t decode_floor(int x, int y) {
    char bottom = get_cell(x, y + 1);
    char right = get_cell(x + 1, y);
    char left = get_cell(x - 1, y);
    char top = get_cell(x, y - 1);

    if (top == WALL) {
        if (left == WALL && right != WALL) return MKTILE(TILESET_STATIC, 1*10+1);
        if (left != WALL && right == WALL) return MKTILE(TILESET_STATIC, 1*10+4);
        return MKTILE(TILESET_STATIC, 1*10 + 2+(rand() & 1));
    }

    if (bottom == WALL) {
        if (left == WALL && right != WALL) return MKTILE(TILESET_STATIC, 3*10+1);
        if (left != WALL && right == WALL) return MKTILE(TILESET_STATIC, 3*10+4);
        return MKTILE(TILESET_STATIC, 3*10 + 2+(rand() & 1));
    }

    if (left == WALL) return MKTILE(TILESET_STATIC, 2*10 + 1);
    if (right == WALL) return MKTILE(TILESET_STATIC, 2*10 + 4);

    return MKTILE(TILESET_STATIC, (rand() % 3)*10 + (rand() & 3) + 6);
}

tile_t decode_decoration(int x, int y) {
    char bottom = get_cell(x, y + 1);
    char left = get_cell(x - 1, y);
    char cur = get_cell(x, y);
    int r = rand();

    if (bottom != WALL && bottom != VOID && cur == WALL) {
        if (r % 10 == 3) return TILE_FLAG_TOP;
        else if (r % 10 == 5) return TILE_TORCH_TOP;
    }
    if (left == WALL && cur != WALL && cur != VOID) {
        if (r % 10 == 0) return TILE_TORCH_LEFT;
    }

    if (cur == FLOOR) {
        if (r % 20 == 3) {
            switch((r/20) % 17) {
            case 0:
            case 1: return TILE_TORCH_1;
            case 2:
            case 3: return TILE_TORCH_2;
            case 4: return TILE_BONES_1;
            case 5: return TILE_BONES_2;
            default:;
            }
        }
    }

    return NOTILE;
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
    tilemap_set_scale(state.map, scale.map);

    int32_t x = 0, y = 0;
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
            state.player.x = x*TILE_WIDTH;
            state.player.y = y*TILE_HEIGHT;
            set_cell(x, y, FLOOR);
            tilemap_set_tile(state.map, x, y, 0, decode_floor(x, y));
            x++;
            break;
        case TRAP: /* trap */
            tilemap_set_tile(state.map, x++, y, 0, TILE_TRAP);
            break;
        case POISON: /* live poison */
        case IPOISON: /* invincibility poison */
        case SPOISON: /* small live poison */
        case SIPOISON: /* small invincibility poison */ {
            tile_t tile = NOTILE;
            switch (c) {
            case POISON: tile = TILE_POISON; break;
            case IPOISON: tile = TILE_IPOISON; break;
            case SPOISON: tile = TILE_SPOISON; break;
            case SIPOISON: tile = TILE_SIPOISON; break;
            }
            tilemap_set_tile(state.map, x, y, 0, decode_floor(x, y));
            tilemap_set_tile(state.map, x++, y, 1, tile);
            break;
        }
        case EXIT: /* exit */
            tilemap_set_tile(state.map, x, y, 1, TILE_EXIT);
            // fallthrough
        case FLOOR: /* floor */
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
            // Decorate map
            tilemap_set_tile(state.map, x, y, 2, decode_decoration(x, y));
        }
    }
}

struct tilemap *create_screen(size_t width, size_t height) {
    struct tilemap *map  = create_tilemap(width, height, TILE_WIDTH, TILE_HEIGHT, state.tilesets, NTILESETS);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            tile_t tile = NOTILE;
            if (!y) {
                if (!x) tile = MKTILE(TILESET_STATIC, 1*10+1);
                else if (x == width - 1) tile = MKTILE(TILESET_STATIC, 1*10+4);
                else tile = MKTILE(TILESET_STATIC, 1*10 + 2+(rand() & 1));
            } else if (y == height - 1) {
                if (!x) tile = MKTILE(TILESET_STATIC, 3*10+1);
                else if (x == width - 1) tile = MKTILE(TILESET_STATIC, 3*10+4);
                else tile = MKTILE(TILESET_STATIC, 3*10 + 2+(rand() & 1));
            } else {
                if (!x) tile = MKTILE(TILESET_STATIC, 2*10 + 1);
                else if (x == width - 1) tile = MKTILE(TILESET_STATIC, 2*10 + 4);
                else tile = MKTILE(TILESET_STATIC, (rand() % 3)*10 + (rand() & 3) + 6);
            }
            tilemap_set_tile(map, x, y, 0, tile);
        }
    }
    tilemap_set_scale(map, scale.interface/2);
    return map;
}

void draw_message(struct tilemap *map, size_t x, size_t y, const char *message) {
    size_t n = MIN(map->width - x, strlen(message));
    for (size_t i = 0; i < n; i++) {
        tile_t ch = MKTILE(TILESET_ASCII, message[i]);
        tilemap_set_tile(map, x + i, y, 2, ch);
    }
}


static struct tilemap *create_death_screen(void) {
    struct tilemap *map = create_screen(STATIC_SCREEN_WIDTH, STATIC_SCREEN_HEIGHT);
    draw_message(map, 6, 2, "YOU DIED");
    draw_message(map, 1, 3, "Press R to restart");
    draw_message(map, 3, 4, "or ESC to exit");
    for (size_t y = 0; y < map->height; y++) {
        for (size_t x = 0; x < map->width; x++) {
            int r = rand();
            if (r/2 % 7 == 0) {
                tilemap_set_tile(map, x, y, 1,
                        (r & 1 ? MKTILE(TILESET_STATIC, 10*6 + 8) :
                        MKTILE(TILESET_STATIC, 10*7 + 7)));
            }

        }
    }
    tilemap_refresh(map);
    return map;
}

static struct tilemap *create_win_screen(void) {
    struct tilemap *map = create_screen(STATIC_SCREEN_WIDTH, STATIC_SCREEN_HEIGHT);
    tilemap_set_tile(map, 5, 2, 1, MKTILE(TILESET_ANIMATED, 6*4 + 2));
    tilemap_set_tile(map, 13, 2, 1, MKTILE(TILESET_ANIMATED, 6*4 + 2));
    tilemap_set_tile(map, 0, 0, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, STATIC_SCREEN_WIDTH - 1, 0, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, 0, STATIC_SCREEN_HEIGHT - 1, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, STATIC_SCREEN_WIDTH - 1, STATIC_SCREEN_HEIGHT - 1, 1, TILE_FLAG_TOP);
    draw_message(map, 6, 2, "YOU WON");
    draw_message(map, 2, 3, "Congratulations!");
    draw_message(map, 1, 4, "Press R to restart");
    draw_message(map, 3, 5, "or ESC to exit");
    tilemap_refresh(map);
    return map;
}

static struct tilemap *create_greet_screen(void) {
    struct tilemap *map = create_screen(STATIC_SCREEN_WIDTH, STATIC_SCREEN_HEIGHT);
    tilemap_set_tile(map, 0, 0, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, STATIC_SCREEN_WIDTH - 1, 0, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, 0, STATIC_SCREEN_HEIGHT - 1, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, STATIC_SCREEN_WIDTH - 1, STATIC_SCREEN_HEIGHT - 1, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, 4, 2, 1, MKTILE(TILESET_STATIC, 10*6+6));
    tilemap_set_tile(map, 15, 2, 1, MKTILE(TILESET_STATIC, 10*6+7));
    draw_message(map, 5, 2, "GREETINGS!");
    draw_message(map, 11, 4, "ESC wR");
    draw_message(map, 14, 5, "asd");
    tilemap_refresh(map);
    return map;
}

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

void set_tile_types(void) {

}

void init(void) {
    struct tileset_desc descs[NTILESETS] = {
        {"data/tiles.png", 10, 10, 0, TILESET_STATIC},
        {"data/ani.png", 4, 27, 1, TILESET_ANIMATED},
        {"data/ent.png", 4, 14, 1, TILESET_ENTITIES},
        {"data/ascii.png", 16, 16, 0, TILESET_ASCII},
    };

    for (size_t i = 0; i < NTILESETS; i++)
        submit_work(do_load, descs + i, sizeof *descs);

    drain_work();
    set_tile_types();
    reset_game();

    state.screens[s_greet] = create_greet_screen();
    state.screens[s_game_over] = create_death_screen();
    state.screens[s_win] = create_win_screen();
    state.state = s_greet;
}

void cleanup(void) {
    free_tilemap(state.map);
    for (size_t i = 0; i < s_MAX; i++)
        if (state.screens[i]) free_tilemap(state.screens[i]);
    for (size_t i = 0; i < NTILESETS; i++)
        unref_tileset(state.tilesets[i]);
    munmap(state.mapchars, state.mapchars_size);
}
