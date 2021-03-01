/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef _TILEMAP_H
#define _TILEMAP_H 1

#define _POSIX_C_SOURCE 200809L

#include "image.h"

#include <stdint.h>

#define TILEMAP_LAYERS 3
#define TILESET_ID(x) ((x) >> 10)
#define TILE_ID(x) ((x) & 0x3FF)
#define MKTILE(set, id) (((set) << 10) | (id))
#define NOTILE UINT32_MAX

typedef uint32_t tile_t;

struct tileset {
    struct image img;
    size_t ntiles;
    size_t refc;
    struct tile {
        struct rect pos;
        int32_t origin_x;
        int32_t origin_y;
        /* Next animation frame */
        uint32_t next_frame;
    } *tiles;
};

struct tilemap {
    struct image cbuf;
    size_t nsets;
    struct tileset **sets;
    size_t width;
    size_t height;
    int32_t tile_width;
    int32_t tile_height;
    uint32_t *dirty;
    double scale;
    tile_t tiles[];
};


struct tileset *create_tileset(const char *path, struct tile *tiles, size_t ntiles);
void unref_tileset(struct tileset *);
void ref_tileset(struct tileset *);
void tileset_queue_tile(struct image dst, struct tileset *set, tile_t tile, int32_t x, int16_t y, double scale);

struct tilemap *create_tilemap(size_t width, size_t height, int32_t tile_width, int16_t tile_height, struct tileset **sets, size_t nsets) ;
void free_tilemap(struct tilemap *map);
tile_t tilemap_add_tileset(struct tilemap *map, struct tileset *tileset);
void tilemap_queue_draw(struct image dst, struct tilemap *map, int32_t x, int16_t y);
tile_t tilemap_set_tile(struct tilemap *map, int32_t x, int16_t y, int16_t layer, tile_t tile);
void tilemap_set_scale(struct tilemap *map, double scale);
tile_t tilemap_get_tile(struct tilemap *map, int32_t x, int16_t y, int16_t layer);
void tilemap_animation_tick(struct tilemap *map);
void tilemap_random_tick(struct tilemap *map);
void tilemap_refresh(struct tilemap *map);

#endif
