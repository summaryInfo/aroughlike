/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef UTIL_H_
#define UTIL_H_ 1

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define CACHE_LINE 64

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define CLAMP(low, x, high) (MAX(MIN(x, high), low))
#define SWAP(a, b) do{__typeof__(a) t__ = (a); (a) = (b); (b) = t__;}while(0)
#define SEC 1000000000LL
#define TIMEDIFF(t, d)  ((((d).tv_sec - (t).tv_sec) * SEC + ((d).tv_nsec - (t).tv_nsec)))
#define TIMEINC(t, in) ((t).tv_sec += (in)/SEC), ((t).tv_nsec += (in)%SEC)
#define LEN(x) (sizeof(x)/sizeof(*(x)))

#define LIKELY(x) (__builtin_expect(!!(x), 1))
#define UNLIKELY(x) (__builtin_expect((x), 0))
#define HOT __attribute__((hot))
#define FORCEINLINE __attribute__((always_inline))
#define ASSUMEALIGNED(p, al) __builtin_assume_aligned((p), (al))

#ifdef CLOCK_MONOTONIC_RAW
#   define CLOCK_TYPE CLOCK_MONOTONIC_RAW
#else
#   define CLOCK_TYPE CLOCK_MONOTONIC
#endif

typedef uint32_t color_t;
struct rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

inline static struct rect rect_scale_up(struct rect rect, int32_t x_factor, int32_t y_factor) {
    rect.x *= x_factor;
    rect.y *= y_factor;
    rect.width *= x_factor;
    rect.height *= y_factor;
    return rect;
}
inline static struct rect rect_scale_down(struct rect rect, int32_t x_factor, int32_t y_factor) {
    rect.x /= x_factor;
    rect.y /= y_factor;
    rect.width /= x_factor;
    rect.height /= y_factor;
    return rect;
}
inline static struct rect rect_shift(struct rect rect, int32_t x_off, int32_t y_off) {
    rect.x += x_off;
    rect.y += y_off;
    return rect;
}
inline static struct rect rect_resize(struct rect rect, int32_t x_off, int32_t y_off) {
    rect.width += x_off;
    rect.height += y_off;
    return rect;
}
inline static struct rect rect_union(struct rect rect, struct rect other) {
    rect.width = MAX(rect.width + rect.x, other.width + other.x);
    rect.height = MAX(rect.height + rect.y, other.height + other.y);
    rect.width -= rect.x = MIN(rect.x, other.x);
    rect.height -= rect.y = MIN(rect.y, other.y);
    return rect;
}

inline static bool intersect_with(struct rect *src, struct rect *dst) {
        struct rect inters = { .x = MAX(src->x, dst->x), .y = MAX(src->y, dst->y) };

        int32_t x1 = MIN(src->x + (int32_t)src->width, dst->x + (int32_t)dst->width);
        int32_t y1 = MIN(src->y + (int32_t)src->height, dst->y + (int32_t)dst->height);

        if (x1 <= inters.x || y1 <= inters.y) {
            *src = (struct rect) {0, 0, 0, 0};
            return 0;
        } else {
            inters.width = x1 - inters.x;
            inters.height = y1 - inters.y;
            *src = inters;
            return 1;
        }
}

inline static int32_t uniform_r(unsigned *seed, int32_t minn, int32_t maxn) {
    /* Small helper to generate good random dirtibution */
    return minn + (maxn-minn+1)*(int64_t)rand_r(seed)/RAND_MAX;
}

void warn(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
_Noreturn void die(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

#endif
