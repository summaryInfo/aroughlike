/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef IMAGE_H_
#define IMAGE_H_ 1

#include "util.h"

#include <stdint.h>

struct image {
    int16_t width;
    int16_t height;
    int shmid;
    color_t *data;
};

void image_draw_rect(struct image im, struct rect rect, color_t fg);
void image_copy(struct image im, struct rect rect, struct image src, int16_t sx, int16_t sy);

#endif

