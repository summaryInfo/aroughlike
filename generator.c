/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"

#include <assert.h>
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

static int rect_cmp(const void *vr1, const void *vr2) {
    const struct rect *r1 = vr1, *r2 = vr2;
    int32_t d1 = (r1->y + r1->height/2)*(r1->y + r1->height/2) +
            (r1->x + r1->width/2)*(r1->x + r1->width/2);
    int32_t d2 = (r2->y + r2->height/2)*(r2->y + r2->height/2) +
            (r2->x + r2->width/2)*(r2->x + r2->width/2);
    if (d1 < d2) return -1;
    if (d1 > d2) return 1;
    if (r1->x < r2->x) return 1;
    if (r1->x > r2->x) return 1;
    return 0;
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

bool is_possible(struct genstate *state, struct rect rec) {
    if (rec.width < 3) return 0;
    if (rec.height < 3) return 0;

    rec.x -= 3, rec.y -= 3, rec.width += 6, rec.height += 6;

    if (rec.x + rec.width > state->width - 2) return 0;
    if (rec.y + rec.height > state->height - 2) return 0;
    if (rec.x < 1) return 0;
    if (rec.y < 1) return 0;

    for (ssize_t y = rec.y; y < rec.y + rec.height; y++)
        for (ssize_t x = rec.x; x < rec.x + rec.width; x++)
            if (c_get(state, x, y) != ' ') return 0;
    return 1;
}

static void draw_horizontal_tunnel(struct genstate *state, int32_t x0, int32_t x1, int32_t y) {
    if (x0 > x1) SWAP(x0, x1);
    for (ssize_t x = x0; x <= x1; x++)
        state->map[(state->width + 1)*y + x] = '.';
}

static void draw_vertical_tunnel(struct genstate *state, int32_t y0, int32_t y1, int32_t x) {
    if (y0 > y1) SWAP(y0, y1);
    for (ssize_t y = y0; y <= y1; y++)
        state->map[(state->width + 1)*y + x] = '.';
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
            subdivide(&state, rec);
        }
    }

    qsort(state.rooms.data, state.rooms.size, sizeof(*state.rooms.data), rect_cmp);
    for (ssize_t i = 0; i < state.rooms.size; i++) {
        struct rect room = state.rooms.data[i];
        for (ssize_t y = 0; y < room.height; y++) {
            for (ssize_t x = 0; x < room.width; x++) {
                c_set(&state, x + room.x, y + room.y, '.');
            }
        }
    }

    for (ssize_t i = 1; i < state.rooms.size; i++) {
        struct rect *prev = &state.rooms.data[i - 1];
        struct rect *new = &state.rooms.data[i];
        int32_t px = prev->x + prev->width/2, py = prev->y + prev->height/2;
        int32_t nx = new->x + new->width/2, ny = new->y + new->height/2;

        if (rand() % 3 == 1) {
            draw_horizontal_tunnel(&state, px, nx, py);
            draw_vertical_tunnel(&state, py, ny, nx);
        } else {
            draw_vertical_tunnel(&state, py, ny, px);
            draw_horizontal_tunnel(&state, px, nx, ny);
        }
    }

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

    {
        int x, y;
        do {
            x = rand() % width;
            y = rand() % height;
         } while (c_get(&state, x, y) != '.');
         c_set(&state, x, y, '@');

        do {
            x = rand() % width;
            y = rand() % height;
         } while (c_get(&state, x, y) != '.');
         c_set(&state, x, y, 'x');
    }

    for (ssize_t y = 0; y < height; y++) {
        for (ssize_t x = 0; x < width; x++) {
            if (c_get(&state, x, y) == '.') {
                int r = rand() % 345;
                switch (r) {
                case 4:
                    c_set(&state, x, y, 'P');
                    break;
                case 5:
                    c_set(&state, x, y, 'p');
                    break;
                case 6:
                    c_set(&state, x, y, 'I');
                    break;
                case 7:
                    c_set(&state, x, y, 'i');
                    break;
                default:
                    if (r > 300) c_set(&state, x, y, 'T');
                }
            }
        }
    }

    free(state.rects.data);
    free(state.rooms.data);
    return state.map;
}

