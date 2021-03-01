/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef IMAGE_H_
#define IMAGE_H_ 1

#include "util.h"

#include <math.h>
#include <stdint.h>

#define FIXPREC 16

struct image {
    int32_t width;
    int32_t height;
    int shmid;
    color_t *data;
};

enum sample_mode {
    sample_nearest = 0,
    sample_linear = 1,
};

FORCEINLINE inline static uint8_t color_r(color_t c) { return (c >> 16) & 0xFF; }

FORCEINLINE inline static uint8_t color_g(color_t c) { return (c >> 8) & 0xFF; }

FORCEINLINE inline static uint8_t color_b(color_t c) { return c & 0xFF; }

FORCEINLINE inline static uint8_t color_a(color_t c) { return c >> 24; }

FORCEINLINE inline static color_t mk_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((color_t)a << 24U) | (r << 16U) | (g << 8U) | b;
}

FORCEINLINE inline static color_t color_blend(color_t dstc, color_t srcc) {
    ssize_t alpha = 255 - color_a(srcc);
    return mk_color(
            alpha*color_r(dstc)/255 + color_r(srcc),
            alpha*color_g(dstc)/255 + color_g(srcc),
            alpha*color_b(dstc)/255 + color_b(srcc),
            alpha*color_a(dstc)/255 + color_a(srcc));
}

FORCEINLINE inline static color_t color_mix(color_t dstc, color_t srcc, ssize_t fixalpha) {
    return mk_color(
            (color_r(dstc)*((1LL << FIXPREC) - 1 - fixalpha) + color_r(srcc)*fixalpha) >> FIXPREC,
            (color_g(dstc)*((1LL << FIXPREC) - 1 - fixalpha) + color_g(srcc)*fixalpha) >> FIXPREC,
            (color_b(dstc)*((1LL << FIXPREC) - 1 - fixalpha) + color_b(srcc)*fixalpha) >> FIXPREC,
            (color_a(dstc)*((1LL << FIXPREC) - 1 - fixalpha) + color_a(srcc)*fixalpha) >> FIXPREC);
}

void image_queue_fill(struct image im, struct rect rect, color_t fg);
void image_queue_blt(struct image dst, struct rect drect, struct image src, struct rect srect, enum sample_mode mode);
struct image load_image(const char *file);
struct image create_image(int32_t width, int16_t height);
struct image create_shm_image(int32_t width, int16_t height);
void free_image(struct image *backbuf);

#endif

