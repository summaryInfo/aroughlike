/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef IMAGE_H_
#define IMAGE_H_ 1

#include "util.h"

#include <math.h>
#include <stdint.h>

struct image {
    int16_t width;
    int16_t height;
    int shmid;
    color_t *data;
};

enum sampler_mode {
    sample_nearest = 0,
    sample_linear = 1,
};

inline static uint8_t color_r(color_t c) { return (c >> 16) & 0xFF; }
inline static uint8_t color_g(color_t c) { return (c >> 8) & 0xFF; }
inline static uint8_t color_b(color_t c) { return c & 0xFF; }
inline static uint8_t color_a(color_t c) { return c >> 24; }
inline static color_t mk_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((color_t)a << 24U) | (r << 16U) | (g << 8U) | b;
}

inline static color_t color_blend(color_t dstc, color_t srcc) {
    double alpha = 1 - color_a(srcc)/255.;

    return mk_color(
            color_r(dstc)*alpha + color_r(srcc),
            color_g(dstc)*alpha + color_g(srcc),
            color_b(dstc)*alpha + color_b(srcc),
            color_a(dstc)*alpha + color_a(srcc));
}

inline static color_t color_mix(color_t dstc, color_t srcc, double alpha) {
    return mk_color(
            color_r(dstc)*(1. - alpha) + color_r(srcc)*alpha,
            color_g(dstc)*(1. - alpha) + color_g(srcc)*alpha,
            color_b(dstc)*(1. - alpha) + color_b(srcc)*alpha,
            color_a(dstc)*(1. - alpha) + color_a(srcc)*alpha);
}

inline static color_t image_sample(struct image src, double x, double y, enum sampler_mode mode) {
    // Always clamp to border
    x = MAX(0, x), y = MAX(0, y);

    ssize_t x0 = floor(x), y0 = floor(y);
    x0 = MIN(x0, src.width - 1);
    y0 = MIN(y0, src.height - 1);

    if (UNLIKELY(mode == sample_linear)) {
        // IDK why did i implement this...
        ssize_t x1 = ceil(x), y1 = ceil(y)*src.width;

        x1 = MIN(x1, src.width - 1);
        y1 = MIN(y1, src.height - 1);

        double valpha = y - y0, halpha = x - x0;
        y0 *= src.width;

        color_t v0 = color_mix(src.data[x0+y0], src.data[x1+y0], halpha);
        color_t v1 = color_mix(src.data[x0+y1], src.data[x1+y1], halpha);

        return color_mix(v0, v1, valpha);
    } else {
        return src.data[x0 + y0*src.width];
    }
}

void image_draw_rect(struct image im, struct rect rect, color_t fg);
void image_blt(struct image dst, struct rect drect, struct image src, struct rect srect, enum sampler_mode mode);

#endif

