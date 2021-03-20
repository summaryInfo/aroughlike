/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "image.h"
#include "tilemap.h"
#include "worker.h"

#include <assert.h>
#include <string.h>

#define BG_COLOR 0xFF25131A

inline static bool get_dirty(struct tilemap *map, size_t x, size_t y) {
    return map->dirty[y*((map->width + 31) >> 5) + (x >> 5)] & (1U << (x & 31));
}

inline static bool get_visited(struct tilemap *map, size_t x, size_t y) {
    return map->visited[y*((map->width + 31) >> 5) + (x >> 5)] & (1U << (x & 31));
}

inline static void mark_dirty(struct tilemap *map, size_t x, size_t y) {
    map->dirty[y*((map->width + 31) >> 5) + (x >> 5)] |= 1U << (x & 31);
    map->has_dirty |= 1;
}

void tilemap_visit(struct tilemap *map, int32_t x, int32_t y) {
    if (x < 0 || x >= (int32_t)map->width) return;
    if (y < 0 || y >= (int32_t)map->height) return;
    bool already =  map->visited[y*((map->width + 31) >> 5) + (x >> 5)] & (1U << (x & 31));
    map->visited[y*((map->width + 31) >> 5) + (x >> 5)] |= 1U << (x & 31);
    if (!already) mark_dirty(map, x, y);
}

inline static tile_t tilemap_set_tile_unsafe(struct tilemap *map, int32_t x, int32_t y, int32_t layer, tile_t tile) {
    mark_dirty(map, x, y);
    tile_t *tilep = &map->tiles[layer + x*TILEMAP_LAYERS + y*TILEMAP_LAYERS*map->width];
    tile_t old = *tilep;
    *tilep = tile;
    return old;
}

inline static tile_t tilemap_get_tile_unsafe(struct tilemap *map, int32_t x, int32_t y, int32_t layer) {
    return map->tiles[layer + x*TILEMAP_LAYERS + y*TILEMAP_LAYERS*map->width];
}

struct tileset *create_tileset(const char *path, struct tile *tiles, size_t ntiles) {
    struct tileset *set = calloc(1, sizeof(*set));
    assert(set);
    set->img = load_image(path);
    assert(set->img.data);
    set->ntiles = ntiles;
    set->tiles = tiles;
    set->refc = 1;

    for (size_t i = 0; i < ntiles; i++) {
        if (set->tiles[i].pos.width > 0) {
            assert(set->tiles[i].pos.x >= 0);
            assert(set->tiles[i].pos.x + set->tiles[i].pos.width <= set->img.width);
        } else {
            assert(set->tiles[i].pos.x < set->img.width);
            assert(set->tiles[i].pos.x + set->tiles[i].pos.width >= 0);
        }
        if (set->tiles[i].pos.height > 0) {
            assert(set->tiles[i].pos.y >= 0);
            assert(set->tiles[i].pos.y + set->tiles[i].pos.height <= set->img.height);
        } else {
            assert(set->tiles[i].pos.y < set->img.height);
            assert(set->tiles[i].pos.y + set->tiles[i].pos.height >= 0);
        }
    }

    return set;
}

void unref_tileset(struct tileset *set) {
    assert(set->refc);
    if (!--set->refc) {
        free_image(&set->img);
        free(set->tiles);
        free(set);
    }
}

void ref_tileset(struct tileset *set) {
    set->refc++;
}

void tileset_queue_tile(struct image dst, struct tileset *set, tile_t tile, int32_t x, int32_t y, double scale) {
    assert(tile < set->ntiles);
    assert(dst.data);

    struct tile *tl = &set->tiles[tile];
    struct rect drect = {
        x, y, tl->pos.width*scale,
        tl->pos.height*scale
    };
    struct rect srect = {
        tl->pos.x,
        tl->pos.y,
        tl->pos.width,
        tl->pos.height
    };
    image_queue_blt(dst, drect, set->img, srect, 0);
}

tile_t tileset_next_tile(struct tileset *set, tile_t tileid) {
    struct tile *tile = &set->tiles[TILE_ID(tileid)];
    if ((tile->type & (TILE_TYPE_ANIMATED |
            TILE_TYPE_RANDOM)) != TILE_TYPE_ANIMATED) return tileid;
    return MKTILE(TILESET_ID(tileid), tile->next_frame);
}

struct tilemap *create_tilemap(size_t width, size_t height, int32_t tile_width, int32_t tile_height, struct tileset **sets, size_t nsets) {
    assert(tile_width > 0);
    assert(tile_height > 0);

    size_t dirty_size = ((width + 31) >> 5)*height*sizeof(uint32_t);
    size_t ticked_size = width * height * sizeof(uint32_t);
    size_t tiles_size = (width*height*TILEMAP_LAYERS*sizeof(tile_t) + 3) & ~3;
    struct tilemap *map = calloc(1, sizeof(*map) + 2*dirty_size + tiles_size + ticked_size);
    assert(map);

    map->dirty = (uint32_t *)((uint8_t *)map->tiles + tiles_size);
    map->visited = (uint32_t *)((uint8_t *)map->dirty + dirty_size);
    map->ticked = (uint32_t *)((uint8_t *)map->visited + dirty_size);

    if (nsets) {
        assert(sets);
        map->nsets = nsets;
        map->sets = malloc(nsets*sizeof(*sets));
        for (size_t i = 0; i < nsets; i++) {
            ref_tileset(sets[i]);
            map->sets[i] = sets[i];
        }
    }

    map->width = width;
    map->height = height;
    map->tile_width = tile_width;
    map->tile_height = tile_height;
    map->cbuf = create_image(width*tile_width, height*tile_height);

    image_queue_fill(map->cbuf, (struct rect){0, 0, width*tile_width, height*tile_height}, BG_COLOR);
    drain_work();

    /* Set every tile to NOTILE */
    memset(map->tiles, 0xFF, width*height*TILEMAP_LAYERS*sizeof(tile_t));

    return map;
}

void free_tilemap(struct tilemap *map) {
    for (size_t i = 0; i < map->nsets; i++) {
        unref_tileset(map->sets[i]);
    }
    free(map->sets);
    free_image(&map->cbuf);
    free(map);
}

tile_t tilemap_add_tileset(struct tilemap *map, struct tileset *newset) {
    struct tileset **new = realloc(map->sets, (map->nsets + 1)*sizeof(map->sets[0]));
    if (!new) return 0;

    map->sets[map->nsets] = newset;
    return map->nsets++;
}

tile_t tilemap_get_tile(struct tilemap *map, int32_t x, int32_t y, int32_t layer) {
    if (x < 0 || x >= (ssize_t)map->width) return NOTILE;
    if (y < 0 || y >= (ssize_t)map->height) return NOTILE;
    if (layer < 0 || layer >= TILEMAP_LAYERS) return NOTILE;
    return tilemap_get_tile_unsafe(map, x, y, layer);
}

void tilemap_queue_draw(struct image dst, struct tilemap *map, int32_t x, int32_t y) {
    image_queue_blt(dst, (struct rect){x, y, map->tile_width*map->width*map->scale, map->tile_height*map->height*map->scale},
              map->cbuf, (struct rect){0, 0, map->tile_width*map->width, map->tile_height*map->height}, 0);
}

tile_t tilemap_set_tile(struct tilemap *map, int32_t x, int32_t y, int32_t layer, tile_t tile) {
    assert(x >= 0 && x < (ssize_t)map->width);
    assert(y >= 0 && y < (ssize_t)map->height);
    assert(layer >= 0 && layer < TILEMAP_LAYERS);
    if (tile != NOTILE) {
        assert(TILESET_ID(tile) < map->nsets);
        assert(TILE_ID(tile) < map->sets[TILESET_ID(tile)]->ntiles);
    }

    return tilemap_set_tile_unsafe(map, x, y, layer, tile);
}

void tilemap_set_scale(struct tilemap *map, double scale) {
    map->scale = scale;
}

bool tilemap_refresh(struct tilemap *map) {
    if (!map->has_dirty) return 0;

    if (map->fade > 0.001) {
        image_queue_fill(map->cbuf, (struct rect){0, 0, map->width*map->tile_width,
            map->height*map->tile_height}, BG_COLOR);
        drain_work();
    }
    for (size_t i = 0; i < TILEMAP_LAYERS; i++) {
        for (size_t yi = 0; yi < map->height; yi++) {
            for (size_t xi = 0; xi < map->width; xi++) {
                if (get_dirty(map, xi, yi) && get_visited(map, xi, yi)) {
                    tile_t tile = tilemap_get_tile_unsafe(map, xi, yi, i);
                    if (tile == NOTILE) continue;
                    tileset_queue_tile(map->cbuf, map->sets[TILESET_ID(tile)],
                                       TILE_ID(tile), xi*map->tile_width, yi*map->tile_height, 1);
                }
            }
        }
        drain_work();
    }
    if (map->fade > 0.001) {
        image_queue_fill(map->cbuf, (struct rect){0, 0, map->width*map->tile_width,
            map->height*map->tile_height}, color_apply_a(BG_COLOR, map->fade));
        drain_work();
    }
    map->has_dirty = 0;
    size_t dirty_size = ((map->width + 31) >> 5)*map->height*sizeof(uint32_t);
    memset(map->dirty, 0, dirty_size);
    return 1;
}

uint32_t tilemap_get_tiletype(struct tilemap *map, int32_t x, int32_t y, int32_t layer) {
    uint32_t tileid = tilemap_get_tile(map, x, y, layer);
    if (tileid == NOTILE) return VOID;

    struct tile *tile = &map->sets[TILESET_ID(tileid)]->tiles[TILE_ID(tileid)];
    return tile->type;
}

void tilemap_animation_tick(struct tilemap *map) {
    for (size_t yi = 0; yi < map->height; yi++) {
        for (size_t xi = 0; xi < map->width; xi++) {
            for (size_t zi = 0; zi < TILEMAP_LAYERS; zi++) {
                tile_t tileid = tilemap_get_tile_unsafe(map, xi, yi, zi);
                if (tileid == NOTILE) continue;
                tile_t next = tileset_next_tile(map->sets[TILESET_ID(tileid)], tileid);
                if (next != tileid) tilemap_set_tile_unsafe(map, xi, yi, zi, next);
            }
        }
    }
}

void tilemap_fade(struct tilemap *map, double val) {
    double old_fade = map->fade;

    if ((map->fade = val) <= 0.001 && old_fade > 0.001) {
        image_queue_fill(map->cbuf, (struct rect){0, 0, map->width*map->tile_width,
            map->height*map->tile_height}, BG_COLOR);
        drain_work();
    }

    map->has_dirty = 1;
    size_t dirty_size = ((map->width + 31) >> 5)*map->height*sizeof(uint32_t);
    memset(map->dirty, 0xFF, dirty_size);
}

void tilemap_random_tick(struct tilemap *map, unsigned *seed) {
    for (size_t yi = 0; yi < map->height; yi++) {
        for (size_t xi = 0; xi < map->width; xi++) {
            tile_t tileid = tilemap_get_tile_unsafe(map, xi, yi, 0);
            if (tileid == NOTILE) continue;

            // Handle random tick delay
            if (map->ticked[map->width*yi + xi]) {
                map->ticked[map->width*yi + xi]--;
                continue;
            }

            struct tileset *set = map->sets[TILESET_ID(tileid)];
            struct tile *tile = &set->tiles[TILE_ID(tileid)];
            if (!(tile->type & TILE_TYPE_RANDOM)) continue;
            if (uniform_r(seed, 0, TILE_TYPE_RDIV(tile->type))) continue;

            tile_t next = MKTILE(TILESET_ID(tileid), tile->next_frame);
            if (next != tileid) {
                map->ticked[map->width*yi + xi] = TILE_TYPE_RREST(TILE_TYPE_CHAR(tile->type));
                tilemap_set_tile_unsafe(map, xi, yi, 0, next);
            }
        }
    }
}
