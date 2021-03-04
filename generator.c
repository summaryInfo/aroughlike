/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "util.h"
#include "context.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct rectvec {
    struct rect *data;
    ssize_t size;
    ssize_t caps;
};

struct genstate {
    int32_t width;
    int32_t height;
    unsigned seed;
    struct rectvec rooms;
    struct rectvec rects;
    char *map;
    bool *adjacentcy;
};

static void push_rect(struct rectvec *vec, const struct rect rec) {
    if (vec->size + 1 > vec->caps) {
        size_t newcaps = 3*vec->caps/2 + 1;
        struct rect *new = realloc(vec->data, newcaps * sizeof *new);
        assert(new);
        vec->data = new;
        vec->caps = newcaps;
    }
    vec->data[vec->size++] = rec;
}

inline static int32_t uniform(struct genstate *state, int32_t minn, int32_t maxn) {
    /* Small helper to generate good random dirtibution */
    int r = uniform_r(&state->seed, minn, maxn);
    return r;
}

static void subdivide(struct genstate *state, struct rect r) {
    if (r.width < 5 || r.height < 5) return;
    push_rect(&state->rects, (struct rect) {r.x, r.y, r.width/2, r.height/2});
    push_rect(&state->rects, (struct rect) {r.x, r.y + r.height/2, r.width/2, r.height/2});
    push_rect(&state->rects, (struct rect) {r.x + r.width/2, r.y, r.width/2, r.height/2});
    push_rect(&state->rects, (struct rect) {r.x + r.width/2, r.y + r.height/2, r.width/2, r.height/2});
}

static struct rect get_rand_subrect(struct genstate *state, struct rect rec) {
    int32_t oldw = rec.width, oldh = rec.height;

    if (oldw <= 2 || oldh <= 2) return rec;

    int32_t basew = uniform(state, 2, MIN(MIN(oldw, oldh) - 1, 12));
    rec.width = uniform(state, basew, MIN(oldw, 12 + basew));
    rec.height = uniform(state, basew, MIN(oldh, 12 + basew));

    if (oldw != rec.width) rec.x += uniform(state, 0, oldw - rec.width);
    if (oldh != rec.height) rec.y += uniform(state, 0, oldh - rec.height);
    return rec;
}

inline static char c_get(struct genstate *state, int32_t x, int32_t y) {
    if (y < 0 || x < 0 || x >= state->width || y >= state->height) return VOID;
    return state->map[(state->width + 1)*y + x];
}

inline static void c_set(struct genstate *state, int32_t x, int32_t y, char c) {
    assert(y >= 0);
    assert(x >= 0);
    assert(x < state->width);
    assert(y < state->height);
    state->map[(state->width + 1)*y + x] = c;
}

static struct rect pad_rect(struct rect r, int32_t pad) {
    r.x += pad;
    r.y += pad;
    r.width -= MIN(2*pad, r.width - 1);
    r.height -= MIN(2*pad, r.height - 1);
    return r;
}

static bool is_possible(struct genstate *state, struct rect rec) {
    if (rec.width < 3) return 0;
    if (rec.height < 3) return 0;

    rec = pad_rect(rec, -2);

    if (rec.x + rec.width > state->width - 2) return 0;
    if (rec.y + rec.height > state->height - 2) return 0;
    if (rec.x < 1) return 0;
    if (rec.y < 1) return 0;

    for (ssize_t y = rec.y; y < rec.y + rec.height; y++)
        for (ssize_t x = rec.x; x < rec.x + rec.width; x++)
            if (c_get(state, x, y) != VOID) return 0;
    return 1;
}

static void draw_horizontal_line(struct genstate *state, int32_t x0, int32_t x1, int32_t y, char c) {
    if (x0 > x1) SWAP(x0, x1);
    for (ssize_t x = x0; x <= x1; x++)
        state->map[(state->width + 1)*y + x] = c;
}

static void draw_vertical_line(struct genstate *state, int32_t y0, int32_t y1, int32_t x, char c) {
    if (y0 > y1) SWAP(y0, y1);
    for (ssize_t y = y0; y <= y1; y++)
        state->map[(state->width + 1)*y + x] = c;
}

static struct rect *find_closest_to(struct genstate *state, struct rect *r1) {
    struct rect *closest = NULL;
    int32_t closest_dist = INT32_MAX;
    for (struct rect *r2 = state->rooms.data; r2 < state->rooms.data + state->rooms.size; r2++) {
        if (r1 == r2 || r2->width < 0) continue;
        int32_t dx = (r1->x + r1->width/2) - (r2->x + r2->width/2);
        int32_t dy = (r1->y + r1->height/2) - (r2->y + r2->height/2);
        int32_t dist = dx*dx + dy*dy;
        if (dist < closest_dist) {
            closest_dist = dist;
            closest = r2;
        }
    }
    return closest;
}

static void draw_rect(struct genstate *state, struct rect room, char c) {
    for (ssize_t y = 0; y < room.height; y++) {
        for (ssize_t x = 0; x < room.width; x++) {
            c_set(state, x + room.x, y + room.y, c);
        }
    }
}

static void draw_checkerboard(struct genstate *state, struct rect room, char c) {
    for (ssize_t y = 0; y < room.height; y++) {
        bool checker = !(y & 1);
        for (ssize_t x = 0; x < room.width; x++) {
            if (checker) c_set(state, x + room.x, y + room.y, c);
            checker = !checker;
        }
    }
}

static void draw_line(struct genstate *state, int32_t x0, int32_t y0, int32_t x1, int32_t y1, char c) {
    bool step = abs(y1 - y0) > abs(x1 - x0);
    if (step) { SWAP(x0, y0); SWAP(x1, y1); }
    if (x0 > x1) { SWAP(x0, x1); SWAP(y0, y1); }

    int dx = x1 - x0, dy = abs(y1 - y0);
    int er = 0, ystep = (y0 < y1) - (y0 > y1);
    for (int y = y0, x = x0; x <= x1; x++) {
        if (step) c_set(state, y, x, c);
        else c_set(state, x, y, c);
        er += dy;
        if (2 * er >= dx) {
            y += ystep;
            er -= dx;
        }
    }
}

static void draw_random_line(struct genstate *state, struct rect room, char c) {
    if (uniform(state, 0, 2)) {
        int32_t x1 = uniform(state, room.x, room.x + room.width - 1);
        int32_t x2 = uniform(state, room.x, room.x + room.width - 1);
        int32_t y = uniform(state, room.y, room.y + room.height - 1);
        draw_horizontal_line(state, x1, x2, y, c);
    } else {
        int32_t x = uniform(state, room.x, room.x + room.width - 1);
        int32_t y1 = uniform(state, room.y, room.y + room.height - 1);
        int32_t y2 = uniform(state, room.y, room.y + room.height - 1);
        draw_vertical_line(state, y1, y2, x, c);
    }
}

static void generate_walls(struct genstate *state, struct rect room) {
    if (room.width < 7 || room.height < 7) return;

    /* Draw vertical walls */
    int32_t v_wall_x = uniform(state, room.x + 2, room.x + room.width - 4);
    int32_t v_wall_type = uniform(state, 0, 3);
    int32_t v_wall_y0 = room.y, v_wall_y1 = room.y + room.height - 1;
    if (!(v_wall_type & 1))
        v_wall_y0 += uniform(state, 1, (room.height - 4)/2);
    if (!(v_wall_type & 2))
        v_wall_y1 -= uniform(state, 1, (room.height - 4)/2);
    if (v_wall_type)
        draw_vertical_line(state, v_wall_y0, v_wall_y1, v_wall_x, WALL);


    /* Draw horizontal walls */
    int32_t h_wall_y = uniform(state, room.y + 1, room.y + room.height - 2);
    int32_t h_wall_type = uniform(state, 0, 3);
    int32_t h_wall_x0 = room.x, h_wall_x1 = room.x + room.width - 1;
    if (!(h_wall_type & 1))
        h_wall_x0 += uniform(state, 1, (room.width - 4)/2);
    if (!(h_wall_type & 2))
        h_wall_x1 -= uniform(state, 1, (room.width - 4)/2);
    if (h_wall_type)
        draw_horizontal_line(state, h_wall_x0, h_wall_x1, h_wall_y, WALL);

    /* Clear out exits */
    if (v_wall_type & 1 && c_get(state, v_wall_x, room.y - 1) != WALL)
            c_set(state, v_wall_x, room.y, FLOOR);
    if (v_wall_type & 2 && c_get(state, v_wall_x, room.y + room.height) != WALL)
            c_set(state, v_wall_x, room.y + room.height - 1, FLOOR);
    if (h_wall_type & 1 && c_get(state, room.x - 1, h_wall_y) != WALL)
            c_set(state, room.x, h_wall_y, FLOOR);
    if (h_wall_type & 2 && c_get(state, room.x + room.width, h_wall_y) != WALL)
            c_set(state, room.x + room.width - 1, h_wall_y, FLOOR);

    /* Create walk-throughs */
    if (h_wall_x1 - h_wall_x0 > 2 && h_wall_type) {
        bool vsplit = v_wall_type && h_wall_x0 <= v_wall_x && v_wall_x <= h_wall_x1;
        if (h_wall_type & 1 && (!vsplit || h_wall_x0 + 1 <= v_wall_x - 1))
            c_set(state, uniform(state, h_wall_x0 + 1, (vsplit ? v_wall_x : h_wall_x1) - 1), h_wall_y, FLOOR);
        if (h_wall_type & 2 && (!vsplit || v_wall_x + 1 <= h_wall_x1 - 1))
            c_set(state, uniform(state, (vsplit ? v_wall_x : h_wall_x0) + 1, h_wall_x1 - 1), h_wall_y, FLOOR);
    }
    if (v_wall_y1 - v_wall_y0 > 2 && v_wall_type) {
        bool hsplit = h_wall_type && v_wall_y0 <= h_wall_y && h_wall_y <= v_wall_y1;
        if (v_wall_type & 1 && (!hsplit || v_wall_y0 + 1 <= h_wall_y - 1))
            c_set(state, v_wall_x, uniform(state, v_wall_y0 + 1, (hsplit ? h_wall_y : v_wall_y1) - 1), FLOOR);
        if (v_wall_type & 2 && (!hsplit || h_wall_y + 1 >= v_wall_y1 - 1))
            c_set(state, v_wall_x, uniform(state, (hsplit ? h_wall_y : v_wall_y0) + 1, v_wall_y1 - 1), FLOOR);
    }
}

enum poison_pos {
    pp_none,
    pp_center,
    pp_corner,
};

static enum poison_pos generate_poisons(struct genstate *state, struct rect room) {
    int32_t center_x = room.x + room.width/2;
    int32_t center_y = room.y + room.height/2;

    /* Poisons (4 types)
     *     Type
     *         1 big healing
     *         5 small healing
     *         2 big invincibility
     *         3 small invincibility
     *     Group
     *         Left
     *         Top
     *         Right
     *         Bottom
     *         Corners
     *         Center
     *      Count
     *         5 1
     *         3 2
     *         2 3
     *         1 4
     */

    char poison_char = 0;
    int poison_count = 0;
    enum poison_pos position = pp_none;

    int r = uniform(state, 0, 10);
    if (r < 1) poison_char = POISON;
    else if (r < 3) poison_char = IPOISON;
    else if (r < 6) poison_char = SIPOISON;
    else poison_char = SPOISON;

    poison_count = log(uniform(state, 2, 33));
    poison_count = MIN(poison_count, MIN(room.width & ~1, room.height & ~1));
    int32_t phigh = (poison_count + 1)/2,  plow = poison_count/2;

    switch (uniform(state, 0, 9)) {
    case 8: /* Sides */
        position = pp_corner;
        draw_horizontal_line(state, center_x - phigh, center_x + plow, room.y, poison_char);
        draw_horizontal_line(state, center_x - phigh,  center_x + plow, room.y + room.height - 1, poison_char);
        /* fallthrough */
    case 1: /* Left-Right */
        draw_vertical_line(state, center_y - phigh,  center_y + plow, room.x + room.width - 1, poison_char);
        /* fallthrough */
    case 0: /* Left */
        position = pp_corner;
        draw_vertical_line(state, center_y - phigh,  center_y + plow, room.x, poison_char);
        break;
    case 2: /* Top */
        position = pp_corner;
        draw_horizontal_line(state, center_x - phigh,  center_x + plow, room.y, poison_char);
        break;
    case 3: /* Top-Bottom */
        draw_horizontal_line(state, center_x - phigh,  center_x + plow, room.y, poison_char);
        /* fallthrough */
    case 5: /* Bottom */
        position = pp_corner;
        draw_horizontal_line(state, center_x - phigh,  center_x + plow, room.y + room.height - 1, poison_char);
        break;
    case 4: /* Right */
        position = pp_corner;
        draw_vertical_line(state, center_y - phigh,  center_y + plow, room.x + room.width - 1, poison_char);
        break;
    case 6: /* Corners */
        position = pp_corner;
        poison_count = MAX(poison_count - 1, 1);
        draw_horizontal_line(state, room.x, room.x + phigh, room.y, poison_char);
        draw_horizontal_line(state, room.x + room.width - 1, room.x + room.width - 1 - phigh, room.y, poison_char);
        draw_vertical_line(state, room.y, room.y + phigh, room.x, poison_char);
        draw_vertical_line(state, room.y + room.height - 1, room.y + room.height - 1 - phigh, room.x, poison_char);
        break;
    case 7: /* Center */
        position = pp_center;
        draw_rect(state, (struct rect){center_x - phigh, center_y - phigh, poison_count, poison_count}, poison_char);
        break;
    case 9: /* Checkerboard in center */
        position = pp_center;
        poison_count = MIN(poison_count + 1, MIN(room.width & ~1, room.height & ~1));
        phigh = (poison_count + 1)/2;
        draw_checkerboard(state, (struct rect){center_x - phigh, center_y - phigh, poison_count, poison_count}, poison_char);
        break;
    }
    return position;
}

static void generate_traps(struct genstate *state, struct rect room, enum poison_pos pp) {
    int32_t center_x = room.x + room.width/2;
    int32_t center_y = room.y + room.height/2;

    /* Traps
     *     X-cross
     *     +-cross
     *     Chess pattern
     *     Half chess pattern
     *     Frame
     *     Center
     *     Random line (1-3)
     */

    switch (uniform(state, 0, 9)) {
    case 0: /* X-cross */
        // TODO poisons_at_center/poisons_at_corners
        draw_line(state, room.x, room.y, room.x + room.width - 1, room.y + room.height - 1, TRAP);
        draw_line(state, room.x + room.width - 1, room.y, room.x, room.y + room.height - 1, TRAP);
        break;
    case 1: /* +-cross */
        if (pp == pp_center) {
            draw_vertical_line(state, room.y, room.y + room.height - 1, center_x - 1, TRAP);
            draw_vertical_line(state, room.y, room.y + room.height - 1, center_x + 1, TRAP);
            draw_horizontal_line(state, room.x, room.x + room.width - 1, center_y - 1, TRAP);
            draw_horizontal_line(state, room.x, room.x + room.width - 1, center_y + 1, TRAP);
        } else {
            draw_vertical_line(state, room.y, room.y + room.height - 1, center_x, TRAP);
            draw_horizontal_line(state, room.x, room.x + room.width - 1, center_y, TRAP);
        }
        break;
    case 2: /* Chess pattern */
        draw_checkerboard(state, room, TRAP);
        break;
    case 3: /* Half chess pattern (left) */ {
        struct rect half = pad_rect(room, pp == pp_corner);
        switch(uniform(state, 0, 3)) {
        case 1: half.x += half.width / 2; /* fallthrough */
        case 0: half.width /= 2; break;
        case 2: half.y += half.height / 2; /* fallthrough */
        case 3: half.height /= 2; break;
        }
        draw_checkerboard(state, half, TRAP);
        break;
    }
    case 5: /* Center */
        if (pp != pp_center) {
            int32_t pad = (pp == pp_corner) + uniform(state, 0, MIN(room.width - 2, room.height - 2)/2);
            struct rect half = pad_rect(room, pad);
            draw_rect(state, half, TRAP);
            break;
        }
        // fallthrough
    case 4: /* Frame */ {
        struct rect half = pad_rect(room, pp == pp_corner);
        draw_horizontal_line(state, half.x, half.x + half.width - 1, half.y, TRAP);
        draw_horizontal_line(state, half.x, half.x + half.width - 1, half.y + half.height - 1, TRAP);
        draw_vertical_line(state, half.y, half.y + half.height - 1, half.x, TRAP);
        draw_vertical_line(state, half.y, half.y + half.height - 1, half.x + half.width - 1, TRAP);
        break;
    }
    case 9: /* 4 random lines */
        draw_random_line(state, room, TRAP);
        // fallthrough
    case 8: /* 3 random lines */
        draw_random_line(state, room, TRAP);
        // fallthrough
    case 7: /* 2 random lines */
        draw_random_line(state, room, TRAP);
        // fallthrough
    case 6: /* 1 random line */
        draw_random_line(state, room, TRAP);
        break;
    }
}

char *generate_map(int32_t width, int32_t height, unsigned seed) {
    struct genstate state = {
        .map = malloc((width + 1) * height + 1),
        .width = width,
        .height = height,
        .seed = seed,
    };

    // Lets generate map in same format as stored in text file
    memset(state.map, VOID, (width + 1)*height);
    state.map[(state.width + 1)*height] = 0;
    for (ssize_t i = 0; i < height; i++)
        state.map[(state.width + 1)*i + width] = '\n';

    push_rect(&state.rects, (struct rect){3, 3, width - 6, height - 6});
    subdivide(&state, state.rects.data[0]);
    for (ssize_t i = 0; i < MIN(width, height) - 32; i++) {
        struct rect rec = state.rects.data[uniform(&state, 0, state.rects.size - 1)];
        struct rect sub = get_rand_subrect(&state, rec);
        if (is_possible(&state, sub)) {
            push_rect(&state.rooms, sub);
            draw_rect(&state, sub, FLOOR);
            subdivide(&state, rec);
        }
    }

    /* Draw tunnels */

    struct rect *first = &state.rooms.data[uniform(&state, 0, state.rooms.size - 1)];
    struct rect *prev = first, *next = NULL;
    for (ssize_t i = 0; i < state.rooms.size; i++) {
        int ncorridors = sqrt(uniform(&state, 1, 16));
        for (int r = 0; r < ncorridors; r++) {
            next = find_closest_to(&state, prev);
            if (!next) goto finish_tunnels;
            int32_t px = prev->x + prev->width/2, py = prev->y + prev->height/2;
            int32_t nx = next->x + next->width/2, ny = next->y + next->height/2;

            if (rand() % 3 == 1) {
                draw_horizontal_line(&state, px, nx, py, FLOOR);
                draw_vertical_line(&state, py, ny, nx, FLOOR);
            } else {
                draw_vertical_line(&state, py, ny, px, FLOOR);
                draw_horizontal_line(&state, px, nx, ny, FLOOR);
            }
        }
        prev->width = -prev->width;
        prev = next;
    }
finish_tunnels:

    /* Draw walls */

    for (ssize_t y = 0; y < height; y++) {
        for (ssize_t x = 0; x < width; x++) {
            if (c_get(&state, x, y) == VOID) {
                ssize_t nfloor = 0;
                for (ssize_t yi = y - 1; yi <= y + 1; yi++) {
                    for (ssize_t xi = x - 1; xi <= x + 1; xi++) {
                        char ch = c_get(&state, xi, yi);
                        nfloor += ch == FLOOR;
                    }
                }
                if (nfloor) {
                    if (nfloor < 6) c_set(&state, x, y, WALL);
                    else c_set(&state, x, y, FLOOR);
                }
            }
        }
    }

    // Decorate rooms
    for (ssize_t i = 0; i < state.rooms.size; i++) {
        bool spawn = state.rooms.data + i == first;
        bool has_walls = !uniform(&state, 0, 3);
        bool has_poisons = !uniform(&state, 0, 3) || spawn;
        bool has_traps = uniform(&state, 0, 3) && !spawn;
        enum poison_pos pp = pp_none;

        // room width is set no negative value by tunnel making algorithm
        // as an indication that room is already connected, so fix it
        state.rooms.data[i].width = abs(state.rooms.data[i].width);

        if (has_poisons) pp = generate_poisons(&state, state.rooms.data[i]);
        if (has_traps) generate_traps(&state, state.rooms. data[i], pp);
        if (has_walls) generate_walls(&state, state.rooms.data[i]);
    }


    // Spawn point, exit, key
    {
        int x, y, attempt = 0;
        do {
            x = uniform(&state, first->x + 1, first->x + first->width - 2);
            y = uniform(&state, first->y + 1, first->x + first->height - 2);
            attempt++;
        } while (c_get(&state, x, y) != FLOOR && attempt < 1000);
        if (attempt == 1000) do {
            x = uniform(&state, first->x, first->x + first->width - 1);
            y = uniform(&state, first->y, first->x + first->height - 1);
        } while (c_get(&state, x, y) == WALL);
        c_set(&state, x, y, PLAYER);

        bool has_key = uniform(&state, 0, 5);
        attempt = 0;

        do {
            x = uniform(&state, prev->x + 1, prev->x + prev->width - 2);
            y = uniform(&state, prev->y + 1, prev->x + prev->height - 2);
            attempt++;
        } while (c_get(&state, x, y) != FLOOR &&
                 c_get(&state, x, y) != TRAP && attempt < 1000);
        if (attempt == 1000) do {
            x = uniform(&state, prev->x, prev->x + prev->width - 1);
            y = uniform(&state, prev->y, prev->x + prev->height - 1);
        } while (c_get(&state, x, y) == WALL);
        c_set(&state, x, y, has_key ? CEXIT : EXIT);

        if (has_key) {
            attempt = 0;
            prev = &state.rooms.data[uniform(&state, 0, state.rooms.size - 1)];
            do {
                x = uniform(&state, prev->x, prev->x + prev->width - 1);
                y = uniform(&state, prev->y, prev->x + prev->height - 1);
                attempt++;
            } while (c_get(&state, x, y) != FLOOR &&
                     c_get(&state, x, y) != TRAP && attempt < 1000);
            if (attempt == 1000) do {
                x = uniform(&state, prev->x, prev->x + prev->width - 1);
                y = uniform(&state, prev->y, prev->x + prev->height - 1);
            } while (c_get(&state, x, y) == WALL);
            c_set(&state, x, y, KEY1);
        }
    }

    // Randomly placed objects
    for (ssize_t y = 0; y < height; y++) {
        for (ssize_t x = 0; x < width; x++) {
            if (c_get(&state, x, y) == FLOOR) {
                int r = uniform(&state, 0, 3000), tile = FLOOR;
                if (r < 1) tile = POISON;
                else if (r < 6) tile = SPOISON;
                else if (r < 8) tile = IPOISON;
                else if (r < 11) tile = SIPOISON;
                else if (r < 50) tile = TRAP;
                c_set(&state, x, y, tile);
            }
        }
    }

    free(state.rects.data);
    free(state.rooms.data);
    return state.map;
}

