/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "image.h"

#include <math.h>
#include <stdint.h>

void image_draw_rect(struct image im, struct rect rect, color_t fg) {
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height})) {
        for (size_t j = 0; j < (size_t)rect.height; j++) {
            for (size_t i = 0; i < (size_t)rect.width; i++) {
                im.data[(rect.y + j) * im.width + (rect.x + i)] = fg;
            }
        }
    }
}

void image_blt(struct image dst, struct rect drect, struct image src, struct rect srect, enum sampler_mode mode) {
    bool fastpath = srect.width == drect.width && srect.height == drect.height;
    double xscale = srect.width/(double)drect.width;
    double yscale = srect.height/(double)drect.height;

    drect.width = MIN(dst.width - drect.x, drect.width);
    drect.height = MIN(dst.height - drect.y, drect.height);

    if (drect.width > 0 && drect.height > 0) {
        if (fastpath) {
            for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                for (size_t i = MAX(-drect.x, 0); i < (size_t)drect.width; i++) {
                    // TODO SIMD this
                    color_t srcc = src.data[srect.x + i + src.width*(srect.y + j)];
                    color_t *pdstc = &dst.data[(drect.y + j) * dst.width + (drect.x + i)];
                    *pdstc = color_blend(*pdstc, srcc);
                }
            }
        } else {
            // Separate branches for better inlining...
            if (mode == sample_nearest) {
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = MAX(-drect.x, 0); i < (size_t)drect.width; i++) {
                        // TODO SIMD this
                        color_t srcc = image_sample(src, srect.x + i*xscale, srect.y + j*yscale, sample_nearest);
                        color_t *pdstc = &dst.data[(drect.y + j) * dst.width + (drect.x + i)];
                        *pdstc = color_blend(*pdstc, srcc);
                    }
                }
            } else {
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                        for (size_t i = MAX(-drect.x, 0); i < (size_t)drect.width; i++) {
                            // TODO SIMD this
                            color_t srcc = image_sample(src, srect.x + i*xscale, srect.y + j*yscale, sample_linear);
                            color_t *pdstc = &dst.data[(drect.y + j) * dst.width + (drect.x + i)];
                            *pdstc = color_blend(*pdstc, srcc);
                        }
                    }
            }
        }
    }
}
