/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef _TILEMAP_H
#define _TILEMAP_H 1

#define _POSIX_C_SOURCE 200809L

#include "image.h"

#include <stdint.h>

#define TILES_PER_CELL 3
#define TILESET_ID(x) ((x) >> 10)
#define TILE_ID(x) ((x) & 0x3FF)
#define MKTILE(set, id) (((set) << 10) | (id))
#define NOTILE UINT16_MAX

typedef uint16_t tile_t;

struct tileset {
    struct image img;
    size_t ntiles;
    size_t refc;
    struct tile {
        struct rect pos;
        int16_t origin_x;
        int16_t origin_y;
        /* Next animation frame */
        int32_t next_frame;
    } *tiles;
};

struct tilemap {
    struct image cbuf;
    size_t nsets;
    struct tileset **sets;
    size_t width;
    size_t height;
    int16_t tile_width;
    int16_t tile_height;
    double scale;
    tile_t tiles[];
};


struct tileset *create_tileset(const char *path, struct tile *tiles, size_t ntiles);
void unref_tileset(struct tileset *);
void ref_tileset(struct tileset *);
void tileset_draw_tile(struct image dst, struct tileset *set, tile_t tile, int16_t x, int16_t y, double scale);

struct tilemap *create_tilemap(size_t width, size_t height, int16_t tile_width, int16_t tile_height, struct tileset **sets, size_t nsets) ;
void free_tilemap(struct tilemap *map);
tile_t tilemap_add_tileset(struct tilemap *map, struct tileset *tileset);
void tilemap_draw(struct image dst, struct tilemap *map, int16_t x, int16_t y);
tile_t tilemap_set_tile(struct tilemap *map, int16_t x, int16_t y, int16_t layer, tile_t tile);
void tilemap_set_scale(struct tilemap *map, double scale);
tile_t tilemap_get_tile(struct tilemap *map, int16_t x, int16_t y, int16_t layer);
void tilemap_animation_tick(struct tilemap *map);

inline static tile_t tilemap_set_tile_unsafe(struct tilemap *map, int16_t x, int16_t y, int16_t layer, tile_t tile) {
    tile_t *tilep = &map->tiles[layer + x*TILES_PER_CELL + y*TILES_PER_CELL*map->width];
    tile_t old = *tilep;
    *tilep = tile;
    return old;
}

inline static tile_t tilemap_get_tile_unsafe(struct tilemap *map, int16_t x, int16_t y, int16_t layer) {
    return map->tiles[layer + x*TILES_PER_CELL + y*TILES_PER_CELL*map->width];
}


#endif
