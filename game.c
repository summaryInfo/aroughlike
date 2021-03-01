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

#define TILE_VOID                    MKTILE(TILESET_STATIC,   10 * 7  + 8)
#define TILE_TRAP                    MKTILE(TILESET_ANIMATED,  4 * 24 + 2)
#define TILE_TRAP_0                  MKTILE(TILESET_ANIMATED,  4 * 24 + 0)
#define TILE_TRAP_1                  MKTILE(TILESET_ANIMATED,  4 * 24 + 1)
#define TILE_TRAP_2                  MKTILE(TILESET_ANIMATED,  4 * 24 + 3)
#define TILE_EXIT                    MKTILE(TILESET_STATIC,   10 * 3  + 9)

#define TILE_POISON                  MKTILE(TILESET_ANIMATED,  4 * 17 + 3)
#define TILE_POISON_0                MKTILE(TILESET_ANIMATED,  4 * 17 + 0)
#define TILE_POISON_1                MKTILE(TILESET_ANIMATED,  4 * 17 + 1)
#define TILE_POISON_2                MKTILE(TILESET_ANIMATED,  4 * 17 + 2)
#define TILE_IPOISON                 MKTILE(TILESET_ANIMATED,  4 * 16 + 3)
#define TILE_IPOISON_0               MKTILE(TILESET_ANIMATED,  4 * 16 + 0)
#define TILE_IPOISON_1               MKTILE(TILESET_ANIMATED,  4 * 16 + 1)
#define TILE_IPOISON_2               MKTILE(TILESET_ANIMATED,  4 * 16 + 2)
#define TILE_SPOISON                 MKTILE(TILESET_ANIMATED,  4 * 14 + 3)
#define TILE_SPOISON_0               MKTILE(TILESET_ANIMATED,  4 * 14 + 0)
#define TILE_SPOISON_1               MKTILE(TILESET_ANIMATED,  4 * 14 + 1)
#define TILE_SPOISON_2               MKTILE(TILESET_ANIMATED,  4 * 14 + 2)
#define TILE_SIPOISON                MKTILE(TILESET_ANIMATED,  4 * 15 + 3)
#define TILE_SIPOISON_0              MKTILE(TILESET_ANIMATED,  4 * 15 + 0)
#define TILE_SIPOISON_1              MKTILE(TILESET_ANIMATED,  4 * 15 + 1)
#define TILE_SIPOISON_2              MKTILE(TILESET_ANIMATED,  4 * 15 + 2)
#define TILE_POISON_STATIC           MKTILE(TILESET_STATIC,   10 * 8  + 9)
#define TILE_IPOISON_STATIC          MKTILE(TILESET_STATIC,   10 * 9  + 7)
#define TILE_SPOISON_STATIC          MKTILE(TILESET_STATIC,   10 * 9  + 8)
#define TILE_SIPOISON_STATIC         MKTILE(TILESET_STATIC,   10 * 8  + 7)

#define TILE_TORCH_TOP               MKTILE(TILESET_ANIMATED,  4 * 26 + 2)
#define TILE_TORCH_LEFT              MKTILE(TILESET_ANIMATED,  4 * 25 + 2)
#define TILE_TORCH_1                 MKTILE(TILESET_ANIMATED,  4 * 5  + 2)
#define TILE_TORCH_2                 MKTILE(TILESET_ANIMATED,  4 * 4  + 2)
#define TILE_BONES_1                 MKTILE(TILESET_STATIC,   10 * 6  + 8)
#define TILE_BONES_2                 MKTILE(TILESET_STATIC,   10 * 7  + 7)
#define TILE_FLAG_TOP                MKTILE(TILESET_ANIMATED,  4 * 9  + 2)
#define TILE_CHEST_1                 MKTILE(TILESET_ANIMATED,  4 * 6  + 2)
#define TILE_DOOR_LEFT               MKTILE(TILESET_STATIC,   10 * 6  + 6)
#define TILE_DOOR_RIGHT              MKTILE(TILESET_STATIC,   10 * 6  + 7)

#define TILE_FLOOR_TOP_LEFT          MKTILE(TILESET_STATIC,   10 * 1  + 1)
#define TILE_FLOOR_TOP_(x)           MKTILE(TILESET_STATIC,   10 * 1  + 2 + ((x) & 1))
#define TILE_FLOOR_TOP_RIGHT         MKTILE(TILESET_STATIC,   10 * 1  + 4)
#define TILE_FLOOR_BOTTOM_LEFT       MKTILE(TILESET_STATIC,   10 * 3  + 1)
#define TILE_FLOOR_BOTTOM_(x)        MKTILE(TILESET_STATIC,   10 * 3  + 2 + ((x) & 1))
#define TILE_FLOOR_BOTTOM_RIGHT      MKTILE(TILESET_STATIC,   10 * 3  + 4)
#define TILE_FLOOR_LEFT              MKTILE(TILESET_STATIC,   10 * 2  + 1)
#define TILE_FLOOR_RIGHT             MKTILE(TILESET_STATIC,   10 * 2  + 4)
#define TILE_FLOOR_(x)               MKTILE(TILESET_STATIC,   10 * ((x) / 4 % 3) + ((x) % 4) + 6)

#define TILE_WALL_LEFT_(x)           MKTILE(TILESET_STATIC,   10 * ((x) & 3) + 5)
#define TILE_WALL_RIGHT_(x)          MKTILE(TILESET_STATIC,   10 * ((x) & 3) + 0)
#define TILE_WALL_BOTTOM_LEFT        MKTILE(TILESET_STATIC,   10 * 4 + 0)
#define TILE_WALL_BOTTOM_RIGHT       MKTILE(TILESET_STATIC,   10 * 4 + 5)
#define TILE_WALL_BOTTOM_(x)         MKTILE(TILESET_STATIC,   10 * 4 + 1 + ((x) & 3))
#define TILE_WALL_TOP_(x)            MKTILE(TILESET_STATIC,   10 * 0 + 1 + ((x) & 3))
#define TILE_WALL                    MKTILE(TILESET_STATIC,   10 * 6 + 9)
#define TILE_WALL_BOTTOM_LEFT_EX(x)  MKTILE(TILESET_STATIC,   10 * 5 + 3 + 2 * ((x) & 1))
#define TILE_WALL_BOTTOM_RIGHT_EX(x) MKTILE(TILESET_STATIC,   10 * 5 + 4 * ((x) & 1))

#define TILE_PLAYER_(x)              MKTILE(TILESET_ENTITIES,  4 * (2 * ((r) % 7) + ((r % 7) > 2)) + 3);

#define WALL '#'
#define TRAP 'T'
#define ACTIVETRAP 't'
#define PLAYER '@'
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
    struct timespec last_frame;
    bool tick_early;
    size_t tick_n;

    int64_t last_delta;
    double avg_delta;

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


static void load_map_from_file(const char *file);

static void queue_fps(struct timespec current) {
    state.avg_delta = TIMEDIFF(state.last_frame, current)*0.01 + state.avg_delta*0.99;
    state.last_frame = current;
    int64_t fps = SEC/state.avg_delta, i = 0;
    do {
        tileset_queue_tile(backbuf, state.tilesets[TILESET_ASCII], '0' + (fps % 10),
                backbuf.width - scale.interface/2*TILE_WIDTH*++i - 20, 20, scale.interface/2);
    } while (fps /= 10);
}

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

    queue_fps(current);

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

    /* Draw message screen if required by state */
    struct tilemap *screen_to_draw = state.screens[state.state];
    if (screen_to_draw) {
        int32_t sx = backbuf.width/2 - screen_to_draw->width*screen_to_draw->tile_width*screen_to_draw->scale/2;
        int32_t sy = backbuf.height/2 - screen_to_draw->height*screen_to_draw->tile_height*screen_to_draw->scale/2;
        tilemap_queue_draw(backbuf, screen_to_draw, sx, sy);
    }

    drain_work();
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

inline static char get_tiletype(int x, int y) {
    uint32_t type = TILE_TYPE_CHAR(tilemap_get_tiletype(state.map, x, y, 1));
    return (type == VOID) ? TILE_TYPE_CHAR(tilemap_get_tiletype(state.map, x, y, 0)) : type;
}

inline static struct rect get_bounding_box_for(char cell, int32_t x, int32_t y) {
    struct rect bb = {x*TILE_HEIGHT, y*TILE_WIDTH, TILE_WIDTH, TILE_HEIGHT};
    switch (cell) {
    case WALL:
        if (get_tiletype(x, y + 1) != WALL) bb.height /= 2;
        char left = get_tiletype(x - 1, y);
        char right = get_tiletype(x + 1, y);
        if (left == VOID && right != VOID) bb.width /= 2, bb.x += TILE_WIDTH/2;
        else if (left != VOID && right == VOID) bb.width /= 2;
        break;
    case TRAP:
        return (struct rect) {
            x*TILE_WIDTH + TILE_WIDTH/4,
            y*TILE_HEIGHT + TILE_HEIGHT/4,
            TILE_WIDTH/2, TILE_WIDTH/2,
        };
    case EXIT:
    case VOID:
        return (struct rect) {
            x*TILE_WIDTH + TILE_WIDTH/2,
            y*TILE_HEIGHT + TILE_HEIGHT/2,
            0, 0,
        };
    case POISON:
    case IPOISON:
    case SPOISON:
    case SIPOISON:
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
    //int64_t fps = MIN(SEC*state.tick_n/TIMEDIFF(state.fps_start, current), FPS);
    int64_t update_time = SEC/FPS - TIMEDIFF(state.last_update, current);

    if (update_time <= 10000LL) {
        if (state.tick_n % (FPS/6) == 0) {
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
        if (state.tick_n % (FPS/60) == 0) {
            tilemap_random_tick(state.map);
        }
        state.last_update = current;
        update_time = SEC/FPS;
        state.tick_n++;
        want_redraw = 1;
    }

    int64_t tick_delta = TIMEDIFF(state.last_tick, current);
    int64_t tick_time = SEC/FPS - tick_delta;
    if (tick_time <= 10000LL || state.tick_early) {
        //warn("%ld %ld %f", tick_delta, tick_time, tick_delta/(double)tick_time);

        // Move camera towards player

        double x_speed_scale = MIN(backbuf.width/5, 512)/MAX(state.map->scale, 2);
        double y_speed_scale = MIN(backbuf.height/4, 512)/MAX(state.map->scale, 2);

#define CAM_SPEED (5e-8/12)
#define PLAYER_SPEED (6e-8)

        double cam_dx = -pow((state.camera_x + (state.player.x + TILE_WIDTH/2)*state.map->scale)/x_speed_scale, 3) * tick_delta * CAM_SPEED;
        double cam_dy = -pow((state.camera_y + (state.player.y + TILE_HEIGHT/2)*state.map->scale)/y_speed_scale, 3) * tick_delta * CAM_SPEED;

        if (fabs(cam_dx) < .8) cam_dx = 0;
        if (fabs(cam_dy) < .8) cam_dy = 0;

        state.camera_x += MAX(-scale.dpi, MIN(cam_dx, scale.dpi));
        state.camera_y += MAX(-scale.dpi, MIN(cam_dy, scale.dpi));

        double old_px = state.player.x;
        double old_py = state.player.y;

        if (state.state == s_normal) {
            // Move player

            double speed = PLAYER_SPEED*(state.keys.right - state.keys.left +
                    state.keys.backward - state.keys.forward == 2 ? sqrt(2) : 2);
            double dx = speed*(state.keys.right - state.keys.left) * tick_delta;
            double dy = speed*(state.keys.backward - state.keys.forward) * tick_delta;

            state.player.x += dx;
            state.player.y += dy;

            int32_t px = (state.player.x + TILE_WIDTH/2)/TILE_WIDTH;
            int32_t py = (state.player.y + TILE_HEIGHT/2)/TILE_HEIGHT;

            // Handle collisions

            for (int32_t y = -1; y <= 1; y++) {
                for (int32_t x = -1; x <= 1; x++) {
                    char cell = get_tiletype(px + x, py + y);
                    struct rect bb = get_bounding_box_for(cell, px + x, py + y);
                    /* Signed depths for x and y axes.
                     * They are equal to the distance player
                     * should be moved to not intersect with
                     * the bounding box.
                     */
                    double x0 = state.player.x, y0 = state.player.y;
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
                    case ACTIVETRAP:
                        if (fmin(fabs(hx), fabs(hy)) > 0) {
                            bool damaged_recently = TIMEDIFF(state.last_damage, current) < SEC;
                            bool invincible = TIMEDIFF(state.player.inv_end, current) < 0;
                            if (!damaged_recently && !invincible) {
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
        tick_time = SEC/FPS;

        bool camera_moved = cam_dx || cam_dy;
        bool player_moved = (int32_t)old_px != (int32_t)state.player.x ||
                (int32_t)old_py != (int32_t)state.player.y;
        want_redraw |= camera_moved || player_moved;
    }

    tilemap_refresh(state.map);

    return MAX(0, MIN(update_time, tick_time));
}

static void reset_game(void) {
    state.level = 0;
    state.player.lives = 1;
    state.player.inv_end = state.player.inv_start = (struct timespec){0};
    next_level();

    // Select random player model
    int r = rand();
    state.player.tile = TILE_PLAYER_(r);
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
    case k_Delete: // Restart game
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

inline static char get_cell(const char *map, ssize_t width, ssize_t height, ssize_t x, ssize_t y) {
    if (x < 0 || x >= width) return VOID;
    if (y < 0 || y >= height) return VOID;
    return map[(width + 1)*y + x];
}

static tile_t decode_wall(const char *m, ssize_t w, ssize_t h, int x, int y) {
    char bottom = get_cell(m, w, h, x, y + 1);
    char right = get_cell(m, w, h, x + 1, y);
    char left = get_cell(m, w, h, x - 1, y);
    char bottom_right = get_cell(m, w, h, x + 1, y + 1);
    char bottom_left = get_cell(m, w, h, x - 1, y + 1);

    // Unfortunately tileset I use does not
    // contain all combinations of walls...
    // But lets try to get the best approximation

    int r = rand();

    if (left == WALL && bottom == WALL && bottom_left == VOID)
        return TILE_WALL_BOTTOM_LEFT_EX(r);
    if (right == WALL && bottom == WALL && bottom_right == VOID)
        return TILE_WALL_BOTTOM_RIGHT_EX(r);

    if (bottom != WALL && bottom != VOID) return TILE_WALL_TOP_(r);
    if (left != WALL && left != VOID) return TILE_WALL_LEFT_(r);
    if (right != WALL && right != VOID) return TILE_WALL_RIGHT_(r);

    if (bottom == VOID) {
        if (left == VOID && right == WALL) return TILE_WALL_BOTTOM_LEFT;
        else if (left == WALL && right == VOID) return TILE_WALL_BOTTOM_RIGHT;
        return TILE_WALL_BOTTOM_(r);
    }

    if (left == WALL && right == VOID) return TILE_WALL_LEFT_(r);
    if (left == VOID && right == WALL) return TILE_WALL_RIGHT_(r);

    return TILE_WALL;
}

static tile_t decode_floor(const char *m, ssize_t w, ssize_t h, int x, int y) {
    char bottom = get_cell(m, w, h, x, y + 1);
    char right = get_cell(m, w, h, x + 1, y);
    char left = get_cell(m, w, h, x - 1, y);
    char top = get_cell(m, w, h, x, y - 1);
    int r = rand();

    if (top == WALL) {
        if (left == WALL && right != WALL) return TILE_FLOOR_TOP_LEFT;
        if (left != WALL && right == WALL) return TILE_FLOOR_TOP_RIGHT;
        return TILE_FLOOR_TOP_(r);
    } else  if (bottom == WALL) {
        if (left == WALL && right != WALL) return TILE_FLOOR_BOTTOM_LEFT;
        if (left != WALL && right == WALL) return TILE_FLOOR_BOTTOM_RIGHT;
        return TILE_FLOOR_BOTTOM_(r);
    } else {
        if (left == WALL) return TILE_FLOOR_LEFT;
        if (right == WALL) return TILE_FLOOR_RIGHT;
        return TILE_FLOOR_(r);
    }
}

static tile_t decode_decoration(const char *m, ssize_t w, ssize_t h, int x, int y) {
    char bottom = get_cell(m, w, h, x, y + 1);
    char left = get_cell(m, w, h, x - 1, y);
    char cur = get_cell(m, w, h, x, y);
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

static void load_map_from_file(const char *file) {
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
            tilemap_set_tile(state.map, x, y, 0, decode_wall(addr, width, height, x, y));
            x++;
            break;
        case VOID: /* void */
            tilemap_set_tile(state.map, x++, y, 0, TILE_VOID);
            break;
        case PLAYER: /* player start */
            state.player.x = x*TILE_WIDTH;
            state.player.y = y*TILE_HEIGHT;
            tilemap_set_tile(state.map, x, y, 0, decode_floor(addr, width, height, x, y));
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
            tilemap_set_tile(state.map, x, y, 0, decode_floor(addr, width, height, x, y));
            tilemap_set_tile(state.map, x++, y, 1, tile);
            break;
        }
        case EXIT: /* exit */
            tilemap_set_tile(state.map, x, y, 1, TILE_EXIT);
            // fallthrough
        case FLOOR: /* floor */
            tilemap_set_tile(state.map, x, y, 0, decode_floor(addr, width, height, x, y));
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
            tilemap_set_tile(state.map, x, y, 2, decode_decoration(addr, width, height, x, y));
        }
    }

    munmap(addr, statbuf.st_size + 1);
}

static struct tilemap *create_screen(size_t width, size_t height) {
    struct tilemap *map  = create_tilemap(width, height, TILE_WIDTH, TILE_HEIGHT, state.tilesets, NTILESETS);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            tile_t tile = NOTILE;
            int r = rand();
            if (y == 0) {
                if (x == 0) tile = TILE_FLOOR_TOP_LEFT;
                else if (x == width - 1) tile = TILE_FLOOR_TOP_RIGHT;
                else tile = TILE_FLOOR_TOP_(r);
            } else if (y == height - 1) {
                if (x == 0) tile = TILE_FLOOR_BOTTOM_LEFT;
                else if (x == width - 1) tile = TILE_FLOOR_BOTTOM_RIGHT;
                else tile = TILE_FLOOR_BOTTOM_(r);
            } else {
                if (x == 0) tile = TILE_FLOOR_LEFT;
                else if (x == width - 1) tile = TILE_FLOOR_RIGHT;
                else tile = TILE_FLOOR_(r);
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
    draw_message(map, 0, 3, "Press DEL to restart");
    draw_message(map, 3, 4, "or ESC to exit");
    for (size_t y = 0; y < map->height; y++) {
        for (size_t x = 0; x < map->width; x++) {
            int r = rand();
            if (r/2 % 7 == 0) {
                tilemap_set_tile(map, x, y, 1, r & 1 ? TILE_BONES_1 : TILE_BONES_2);
            }

        }
    }
    tilemap_refresh(map);
    return map;
}

static struct tilemap *create_win_screen(void) {
    struct tilemap *map = create_screen(STATIC_SCREEN_WIDTH, STATIC_SCREEN_HEIGHT);
    tilemap_set_tile(map, 5, 2, 1, TILE_CHEST_1);
    tilemap_set_tile(map, 13, 2, 1, TILE_CHEST_1);
    tilemap_set_tile(map, 0, 0, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, STATIC_SCREEN_WIDTH - 1, 0, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, 0, STATIC_SCREEN_HEIGHT - 1, 1, TILE_FLAG_TOP);
    tilemap_set_tile(map, STATIC_SCREEN_WIDTH - 1, STATIC_SCREEN_HEIGHT - 1, 1, TILE_FLAG_TOP);
    draw_message(map, 6, 2, "YOU WON");
    draw_message(map, 2, 3, "Congratulations!");
    draw_message(map, 0, 4, "Press DEL to restart");
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
    tilemap_set_tile(map, 4, 2, 1, TILE_DOOR_LEFT);
    tilemap_set_tile(map, 15, 2, 1, TILE_DOOR_RIGHT);
    draw_message(map, 5, 2, "GREETINGS!");
    draw_message(map, 11, 4, "ESC w");
    draw_message(map, 14, 5, "asd");
    tilemap_refresh(map);
    return map;
}

static void do_load(void *varg) {
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
                .type = arg->animated ? TILE_TYPE_ANIMATED : 0,
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

inline static void set_tile_type(tile_t tileid, uint32_t type) {
    struct tileset *ts = state.tilesets[TILESET_ID(tileid)];;
    assert(TILE_ID(tileid) < ts->ntiles);
    ts->tiles[TILE_ID(tileid)].type |= type;
}

static void init_tiles(void) {
    static const struct tileset_desc tileset_descs[NTILESETS] = {
        {"data/tiles.png", 10, 10, 0, TILESET_STATIC},
        {"data/ani.png", 4, 27, 1, TILESET_ANIMATED},
        {"data/ent.png", 4, 14, 1, TILESET_ENTITIES},
        {"data/ascii.png", 16, 16, 0, TILESET_ASCII},
    };

    for (size_t i = 0; i < NTILESETS; i++)
        submit_work(do_load, tileset_descs + i, sizeof *tileset_descs);
    drain_work();

    struct {
        tile_t tile;
        uint32_t type;
    } types[] = {
        { TILE_VOID, VOID },
        { TILE_EXIT, EXIT },
        { TILE_TRAP, TRAP | TILE_TYPE_RANDOM | (83 << 16) },
        { TILE_TRAP_0, ACTIVETRAP },
        { TILE_TRAP_1, ACTIVETRAP },
        { TILE_TRAP_2, ACTIVETRAP },
        { TILE_POISON, POISON },
        { TILE_POISON_0, POISON },
        { TILE_POISON_1, POISON },
        { TILE_POISON_2, POISON },
        { TILE_IPOISON, IPOISON },
        { TILE_IPOISON_0, IPOISON },
        { TILE_IPOISON_1, IPOISON },
        { TILE_IPOISON_2, IPOISON },
        { TILE_SPOISON, SPOISON },
        { TILE_SPOISON_0, SPOISON },
        { TILE_SPOISON_1, SPOISON },
        { TILE_SPOISON_2, SPOISON },
        { TILE_SIPOISON, SIPOISON },
        { TILE_SIPOISON_0, SIPOISON },
        { TILE_SIPOISON_1, SIPOISON },
        { TILE_SIPOISON_2, SIPOISON },
        { TILE_POISON_STATIC, POISON },
        { TILE_IPOISON_STATIC, IPOISON },
        { TILE_SPOISON_STATIC, SPOISON },
        { TILE_SIPOISON_STATIC, SIPOISON },
    };

    for (size_t i = 0; i < LEN(types); i++)
        set_tile_type(types[i].tile, types[i].type);

    tile_t floor_tiles[] = {
        TILE_FLOOR_TOP_LEFT, TILE_FLOOR_TOP_RIGHT, TILE_FLOOR_BOTTOM_LEFT,
        TILE_FLOOR_TOP_(0), TILE_FLOOR_TOP_(1),
        TILE_FLOOR_BOTTOM_(0), TILE_FLOOR_BOTTOM_(1),
        TILE_FLOOR_(0), TILE_FLOOR_(1), TILE_FLOOR_(2),
        TILE_FLOOR_(3), TILE_FLOOR_(4), TILE_FLOOR_(5),
        TILE_FLOOR_(6), TILE_FLOOR_(7), TILE_FLOOR_(8),
        TILE_FLOOR_(9), TILE_FLOOR_(10), TILE_FLOOR_(11),
        TILE_FLOOR_BOTTOM_RIGHT, TILE_FLOOR_LEFT, TILE_FLOOR_RIGHT,
    };
    for (size_t i = 0; i < LEN(floor_tiles); i++)
        set_tile_type(floor_tiles[i], FLOOR);

    tile_t wall_tiles[] = {
        TILE_WALL_LEFT_(0), TILE_WALL_LEFT_(1), TILE_WALL_LEFT_(2), TILE_WALL_LEFT_(3),
        TILE_WALL_RIGHT_(0), TILE_WALL_RIGHT_(1), TILE_WALL_RIGHT_(2), TILE_WALL_RIGHT_(3),
        TILE_WALL_BOTTOM_LEFT, TILE_WALL_BOTTOM_RIGHT,
        TILE_WALL_BOTTOM_(0), TILE_WALL_BOTTOM_(1), TILE_WALL_BOTTOM_(2), TILE_WALL_BOTTOM_(3),
        TILE_WALL_TOP_(0), TILE_WALL_TOP_(1), TILE_WALL_TOP_(2), TILE_WALL_TOP_(3),
        TILE_WALL,
        TILE_WALL_BOTTOM_LEFT_EX(0), TILE_WALL_BOTTOM_LEFT_EX(1),
        TILE_WALL_BOTTOM_RIGHT_EX(0), TILE_WALL_BOTTOM_RIGHT_EX(1),
    };
    for (size_t i = 0; i < LEN(wall_tiles); i++)
        set_tile_type(wall_tiles[i], WALL);
}

void init(void) {
    init_tiles();
    reset_game();

    state.screens[s_greet] = create_greet_screen();
    state.screens[s_game_over] = create_death_screen();
    state.screens[s_win] = create_win_screen();
    state.state = s_greet;
    state.tick_n = 1;
    state.avg_delta = SEC/FPS;
    clock_gettime(CLOCK_TYPE, &state.last_frame);
    TIMEINC(state.last_frame, -(int64_t)state.avg_delta);
}

void cleanup(void) {
    free_tilemap(state.map);
    for (size_t i = 0; i < s_MAX; i++)
        if (state.screens[i]) free_tilemap(state.screens[i]);
    for (size_t i = 0; i < NTILESETS; i++)
        unref_tileset(state.tilesets[i]);
}
