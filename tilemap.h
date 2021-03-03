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

#define TILE_TYPE_ANIMATED 0x1000
#define TILE_TYPE_RANDOM 0x2000
#define TILE_TYPE_RDIV(x) (((x) >> 16) & 0xFF)
#define TILE_TYPE_RREST(x) (((x) >> 24) & 0xFF)
#define TILE_TYPE_CHAR(x) ((x) & 0xFF)

#define VOID ' '

typedef uint32_t tile_t;

struct tileset {
    struct image img;
    size_t ntiles;
    size_t refc;
    struct tile {
        struct rect pos;
        /* Next animation frame */
        tile_t next_frame;
        uint8_t rest;
        uint32_t type;
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
    uint32_t *ticked;
    bool has_dirty;
    double scale;
    tile_t tiles[];
};


struct tileset *create_tileset(const char *path, struct tile *tiles, size_t ntiles);
void unref_tileset(struct tileset *);
void ref_tileset(struct tileset *);
void tileset_queue_tile(struct image dst, struct tileset *set, tile_t tile, int32_t x, int32_t y, double scale);
tile_t tileset_next_tile(struct tileset *set, tile_t tileid, bool random);

struct tilemap *create_tilemap(size_t width, size_t height, int32_t tile_width, int32_t tile_height, struct tileset **sets, size_t nsets) ;
void free_tilemap(struct tilemap *map);
tile_t tilemap_add_tileset(struct tilemap *map, struct tileset *tileset);
void tilemap_queue_draw(struct image dst, struct tilemap *map, int32_t x, int32_t y);
tile_t tilemap_set_tile(struct tilemap *map, int32_t x, int32_t y, int32_t layer, tile_t tile);
void tilemap_set_scale(struct tilemap *map, double scale);
tile_t tilemap_get_tile(struct tilemap *map, int32_t x, int32_t y, int32_t layer);
uint32_t tilemap_get_tiletype(struct tilemap *map, int32_t x, int32_t y, int32_t layer);
void tilemap_animation_tick(struct tilemap *map);
void tilemap_random_tick(struct tilemap *map);
bool tilemap_refresh(struct tilemap *map);

#endif
