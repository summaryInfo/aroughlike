/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"

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

static void subdivide(struct genstate *state, struct rect r) {
    if (r.width < 5 || r.height < 5) return;
    push_rect(&state->rects, (struct rect) {r.x, r.y, r.width/2, r.height/2});
    push_rect(&state->rects, (struct rect) {r.x, r.y + r.height/2, r.width/2, r.height/2});
    push_rect(&state->rects, (struct rect) {r.x + r.width/2, r.y, r.width/2, r.height/2});
    push_rect(&state->rects, (struct rect) {r.x + r.width/2, r.y + r.height/2, r.width/2, r.height/2});
}

struct rect get_rand_subrect(struct rect rec) {
    int32_t oldw = rec.width, oldh = rec.height;

    if (oldw <= 2 || oldh <= 2) return rec;

    int32_t basew = rand() % MIN(MIN(oldw - 2, oldh - 2), 12) + 2;

    rec.width = basew + rand() % MIN(rec.width - basew, 12);
    rec.height = basew + rand() % MIN(rec.height - basew, 12);

    assert(oldw >= rec.width);
    assert(oldh >= rec.height);

    if (oldw != rec.width) rec.x += (rand() + 1) % (oldw - rec.width);
    if (oldh != rec.height) rec.y += (rand() + 1) % (oldh - rec.height);
    return rec;
}

inline static char c_get(struct genstate *state, int32_t x, int32_t y) {
    if (y < 0 || x < 0 || x >= state->width || y >= state->height) return ' ';
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
            if (c_get(state, x, y) != ' ') return 0;
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

void draw_rect(struct genstate *state, struct rect room, char c) {
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
    if (x0 == x1) draw_vertical_line(state, y0, y1, x0, c);
    if (y0 == y1) draw_horizontal_line(state, x0, x1, y0, c);
    if (abs(x1 - x0) < abs(y1 - y0)) {
        if (y0 > y1) { SWAP(y0, y1); SWAP(x0, x1); }
        if (x0 > x1) {
            for (int32_t y = y0; y <= y1; y++)
                c_set(state, x0 - ((x0 - x1)*(y - y0)/(y1 - y0)), y, c);
        } else {
            for (int32_t y = y0; y <= y1; y++)
                c_set(state, x0 + ((x1 - x0)*(y - y0)/(y1 - y0)), y, c);
        }
    } else {
        if (x0 > x1) { SWAP(y0, y1); SWAP(x0, x1); }
        if (y0 > y1) {
            for (int32_t x = x0; x <= x1; x++)
                c_set(state, x, y0 - ((y0 - y1)*(x - x0)/(x1 - x0)), c);
        } else {
            for (int32_t x = x0; x <= x1; x++)
                c_set(state, x, y0 + (y1 - y0)*(x - x0)/(x1 - x0), c);
        }
    }
}

static void draw_random_line(struct genstate *state, struct rect room, char c, int *random) {
    int32_t x = *random % (room.width - 2);
    *random /= room.width - 1;
    int32_t y = *random % (room.height - 2);

    bool horiz = *random & 1;
    *random /= 2;

    if (horiz) {
        int32_t len = *random % (room.width - x);
        *random %= room.width - x;
        draw_horizontal_line(state, room.x + x, room.x + x + len, room.y + y, c);
    } else {
        int32_t len = *random % (room.height - y);
        *random %= room.height - y;
        draw_vertical_line(state, room.y + y, room.y + y + len, room.x + x, c);
    }
}

static void generate_walls(struct genstate *state, struct rect room) {
    if (room.width < 7 || room.height < 7) return;

    int r1 = rand(), r3 = rand();

    /*
     * Walls
     *    Vertical wall
     *    Vertical top half wall
     *    Vertical bottom half wall
     *    Horizontal wall
     *    Horizontal top half wall
     *    Horizontal bottom half wall
     */


    bool full = 0;
    int32_t full_x = 0;
    int32_t full_y1 = 0, full_y2 = 0;
    switch (r1 % 4) {
    case 0: /* Vertical wall */ {
        int32_t rx = room.x + 1 + (r3 % (room.width - 2));
        draw_vertical_line(state, room.y, room.y + room.height - 1, rx, '#');
        r3 /= room.width - 2;
        c_set(state, rx, full_y1 = (room.y + r3 % room.height), '.');
        r3 /= room.height;
        c_set(state, rx, full_y2 = (room.y + r3 % room.height), '.');
        r3 /= room.height;
        if (c_get(state, rx, room.y - 1) == '.') c_set(state, rx, room.y, '.');
        if (c_get(state, rx, room.y + room.height) == '.') c_set(state, rx, room.y + room.height - 1, '.');
        full = 1;
        full_x = rx;
        break;
     }
    case 1: /* Vertical top half */ {
        int32_t rx = room.x + 1 + (r3 % (room.width - 2));
        r3 /= room.width - 2;
        int32_t ry = room.y + 1 + (r3 % ((room.height - 3)/ 2));
        r3 /= room.height - 1;
        draw_vertical_line(state, room.y, ry, rx, '#');
        if (c_get(state, rx, room.y - 1) == '.') c_set(state, rx, room.y, '.');
        break;
    }
    case 2: /* Vertical bottom half */ {
        int32_t rx = room.x + 1 + (r3 % (room.width - 2));
        r3 /= room.width - 2;
        int32_t ry = room.y + room.height - 1 - (r3 % ((room.height - 3)/ 2));
        r3 /= room.height - 1;
        draw_vertical_line(state, room.y + room.height - 1, ry, rx, '#');
        if (c_get(state, rx, room.y + room.height) == '.') c_set(state, rx, room.y + room.height - 1, '.');
        break;
    }
    case 3:
        // NONE
        break;
    }
    r1 /= 4;

    int mod = r1 % 4;
    if (full && (mod == 1 || mod == 2)) mod = 0;
    switch (mod) {
    case 0: /* Horizontal wall */ {
        int32_t ry = room.y + 1 + (r3 % (room.height - 2));
        if (ry == full_y1 || ry == full_y2) {
            if (ry == room.y + 1) ry++;
            else ry--;
        }
        draw_horizontal_line(state, room.x, room.x + room.width - 1, ry, '#');
        r3 /= room.height - 2;
        if (full) {
            int32_t x = room.x + r3 % (full_x - room.x);
            c_set(state, x, ry, '.');
            r3 /= (full_x - room.x);
            x = full_x + r3 % (room.width - (full_x - room.x));
            c_set(state, x, ry, '.');
            r3 /= (room.width - (full_x - room.x));

        } else {
            int32_t x = full_x + r3 % room.width;
            c_set(state, x, ry, '.');
            r3 /= room.width;
            x = room.x + r3 % room.width;
            c_set(state, x, ry, '.');
            r3 /= room.width;
        }
        if (c_get(state, room.x - 1, ry) == '.') c_set(state, room.x, ry, '.');
        if (c_get(state, room.x + room.width, ry) == '.') c_set(state, room.x + room.width - 1, ry, '.');
        break;
     }
    case 1: /* Horizontal left half */ {
        int32_t ry = room.y + 1 + (r3 % (room.height - 3));
        r3 /= room.height - 2;
        int32_t rx = room.x + (r3 % ((room.width - 3)/ 2));
        r3 /= (room.width - 1)/ 2;
        draw_horizontal_line(state, room.x, rx, ry, '#');
        if (c_get(state, room.x - 1, ry) == '.') c_set(state, room.x, ry, '.');
        break;
    }
    case 2: /* Horizontal right half */ {
        int32_t ry = room.y + 1 + (r3 % (room.height - 3));
        r3 /= room.height - 2;
        int32_t rx = room.x + room.width - 1 - (r3 % ((room.width - 3)/ 2));
        r3 /= (room.width - 1)/ 2;
        draw_horizontal_line(state, room.x + room.width - 1, rx, ry, '#');
        if (c_get(state, room.x + room.width, ry) == '.') c_set(state, room.x + room.width - 1, ry, '.');
        break;
    }
    case 3:
        // NONE
        break;
    }
    r1 /= 4;
}

enum poison_pos {
    pp_none,
    pp_center,
    pp_corner,
};

static enum poison_pos generate_poisons(struct genstate *state, struct rect room) {
    int32_t center_x = room.x + room.width/2;
    int32_t center_y = room.y + room.height/2;
    int r1 = rand();

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

    switch (r1 % 11) {
    case 0: poison_char = 'P'; break;
    case 1: case 2: poison_char = 'I'; break;
    case 3: case 4: case 5: poison_char = 'i'; break;
    default: poison_char = 'p'; break;
    }
    r1 /= 11;
    switch (r1 % 32) {
    case 0: poison_count = 5; break;
    case 1: case 2: poison_count = 4; break;
    case 3: case 4: case 5: case 6: case 15: poison_count = 3; break;
    case 7: case 8: case 9: case 10: case 16:
    case 11: case 12: case 13: case 14: poison_count = 2; break;
    default: poison_count = 1; break;
    }
    r1 /= 32;

    poison_count = MIN(poison_count, MIN(room.width & ~1, room.height & ~1));
    int32_t phigh = (poison_count + 1)/2;
    int32_t plow = poison_count/2;
    switch (r1 % 10) {
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
    r1 /= 10;
    return position;
}

static void generate_traps(struct genstate *state, struct rect room, enum poison_pos pp) {
    int32_t center_x = room.x + room.width/2;
    int32_t center_y = room.y + room.height/2;
    int r1 = rand(), r2 = rand();

    /* Traps
     *     X-cross
     *     +-cross
     *     Chess pattern
     *     Half chess pattern
     *     Frame
     *     Center
     *     Random line (1-3)
     */

    switch (r1 % 10) {
    case 0: /* X-cross */
        // TODO poisons_at_center/poisons_at_corners
        draw_line(state, room.x, room.y, room.x + room.width - 1, room.y + room.height - 1, 'T');
        draw_line(state, room.x + room.width - 1, room.y, room.x, room.y + room.height - 1, 'T');
        break;
    case 1: /* +-cross */
        if (pp == pp_center) {
            draw_vertical_line(state, room.y, room.y + room.height - 1, center_x - 1, 'T');
            draw_vertical_line(state, room.y, room.y + room.height - 1, center_x + 1, 'T');
            draw_horizontal_line(state, room.x, room.x + room.width - 1, center_y - 1, 'T');
            draw_horizontal_line(state, room.x, room.x + room.width - 1, center_y + 1, 'T');
        } else {
            draw_vertical_line(state, room.y, room.y + room.height - 1, center_x, 'T');
            draw_horizontal_line(state, room.x, room.x + room.width - 1, center_y, 'T');
        }
        break;
    case 2: /* Chess pattern */
        draw_checkerboard(state, room, 'T');
        break;
    case 3: /* Half chess pattern (left) */ {
        struct rect half = pad_rect(room, pp == pp_corner);
        switch(r2 % 4) {
        case 1: half.x += half.width / 2; /* fallthrough */
        case 0: half.width /= 2; break;
        case 2: half.y += half.height / 2; /* fallthrough */
        case 3: half.height /= 2; break;
        }
        draw_checkerboard(state, half, 'T');
        break;
    }
    case 5: /* Center */
        if (pp != pp_center) {
            int32_t pad = (pp == pp_corner) + (r2 % MIN(room.width - 1, room.height - 1))/2;
            struct rect half = pad_rect(room, pad);
            draw_rect(state, half, 'T');
            break;
        }
        // fallthrough
    case 4: /* Frame */ {
        struct rect half = pad_rect(room, pp == pp_corner);
        draw_horizontal_line(state, half.x, half.x + half.width - 1, half.y, 'T');
        draw_horizontal_line(state, half.x, half.x + half.width - 1, half.y + half.height - 1, 'T');
        draw_vertical_line(state, half.y, half.y + half.height - 1, half.x, 'T');
        draw_vertical_line(state, half.y, half.y + half.height - 1, half.x + half.width - 1, 'T');
        break;
    }
    case 9: /* 4 random lines */
        draw_random_line(state, room, 'T', &r2);
        // fallthrough
    case 8: /* 3 random lines */
        draw_random_line(state, room, 'T', &r2);
        // fallthrough
    case 7: /* 2 random lines */
        draw_random_line(state, room, 'T', &r2);
        // fallthrough
    case 6: /* 1 random line */
        draw_random_line(state, room, 'T', &r2);
        break;
    }
    r1 /= 10;
}

char *generate_map(int32_t width, int32_t height) {
    struct genstate state = {
        .map = malloc((width + 1) * height + 1),
        .width = width,
        .height = height
    };

    // Lets generate map in same format as stored in text file
    memset(state.map, ' ', (width + 1)*height);
    state.map[(state.width + 1)*height] = 0;
    for (ssize_t i = 0; i < height; i++)
        state.map[(state.width + 1)*i + width] = '\n';

    push_rect(&state.rects, (struct rect){3, 3, width - 6, height - 6});
    subdivide(&state, state.rects.data[0]);
    for (ssize_t i = 0; i < MIN(width, height) - 32; i++) {
        struct rect rec = state.rects.data[rand() % state.rects.size];
        struct rect sub = get_rand_subrect(rec);
        if (is_possible(&state, sub)) {
            push_rect(&state.rooms, sub);
            draw_rect(&state, sub, '.');
            subdivide(&state, rec);
        }
    }

    /* Draw tunnels */

    struct rect *first = &state.rooms.data[rand() % state.rooms.size];
    struct rect *prev = first, *next = NULL;
    for (ssize_t i = 0; i < state.rooms.size; i++) {
        int ncorridors = sqrt((rand() % 15) + 1);
        for (int r = 0; r < ncorridors; r++) {
            next = find_closest_to(&state, prev);
            if (!next) goto finish_tunnels;
            int32_t px = prev->x + prev->width/2, py = prev->y + prev->height/2;
            int32_t nx = next->x + next->width/2, ny = next->y + next->height/2;

            if (rand() % 3 == 1) {
                draw_horizontal_line(&state, px, nx, py, '.');
                draw_vertical_line(&state, py, ny, nx, '.');
            } else {
                draw_vertical_line(&state, py, ny, px, '.');
                draw_horizontal_line(&state, px, nx, ny, '.');
            }
        }
        prev->width = -prev->width;
        prev = next;
    }
finish_tunnels:

    /* Draw walls */

    for (size_t i = 0; i < 20; i++) {
        for (ssize_t y = 0; y < height; y++) {
            for (ssize_t x = 0; x < width; x++) {
                if (c_get(&state, x, y) == ' ') {
                    ssize_t nfloor = 0;
                    for (ssize_t yi = y - 1; yi <= y + 1; yi++) {
                        for (ssize_t xi = x - 1; xi <= x + 1; xi++) {
                            char ch = c_get(&state, xi, yi);
                            nfloor += ch == '.';
                        }
                    }
                    if (nfloor) {
                        if (nfloor < 6) c_set(&state, x, y, '#');
                        else c_set(&state, x, y, '.');
                    }
                }
            }
        }
    }

    // Decorate rooms
    for (ssize_t i = 0; i < state.rooms.size; i++) {
        int r1 = rand();

        bool spawn = state.rooms.data + i == first;
        bool has_walls = r1 % 2 == 0; r1 /= 2;
        bool has_poisons = r1 % 3 == 0 || spawn; r1 /= 3;
        bool has_traps = r1 % 3 != 0 && !spawn; r1 /= 3;
        enum poison_pos pp = pp_none;

        // room width is set no negative value by tunnel making algorithm
        // as an indication that room is already connected, so fix it
        state.rooms.data[i].width = abs(state.rooms.data[i].width);

        if (has_poisons) pp = generate_poisons(&state, state.rooms.data[i]);
        if (has_traps) generate_traps(&state, state.rooms. data[i], pp);
        if (has_walls) generate_walls(&state, state.rooms.data[i]);
    }


    // Spawn point and exit
    {
        int x, y;
        do {
            x = first->x + rand() % first->width;
            y = first->y + rand() % first->height;
         } while (c_get(&state, x, y) != '.');
         c_set(&state, x, y, '@');

        do {
            x = prev->x + rand() % prev->width;
            y = prev->y + rand() % prev->height;
         } while (c_get(&state, x, y) != '.' &&
                  c_get(&state, x, y) != 'T');
         c_set(&state, x, y, 'x');
    }

    // Randomly placed objects
    for (ssize_t y = 0; y < height; y++) {
        for (ssize_t x = 0; x < width; x++) {
            if (c_get(&state, x, y) == '.') {
                int r = rand() % 3000;
                switch (r) {
                case 0:
                    c_set(&state, x, y, 'P');
                    break;
                case 3: case 4: case 5: case 6:  case 2:
                    c_set(&state, x, y, 'p');
                    break;
                case 8: case 9:
                    c_set(&state, x, y, 'I');
                    break;
                case 10: case 11: case 12:
                    c_set(&state, x, y, 'i');
                    break;
                default:
                    if (r > 2900) c_set(&state, x, y, 'T');
                }
            }
        }
    }

    free(state.rects.data);
    free(state.rooms.data);
    return state.map;
}

