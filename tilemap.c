
#include "tilemap.h"
#include "image.h"

#include <assert.h>
#include <string.h>

struct tileset *create_tileset(const char *path, struct tile *tiles, size_t ntiles) {
    struct tileset *set = malloc(sizeof(*set) + sizeof(*tiles)*ntiles);
    assert(set);
    set->img = create_image(path);
    assert(set->img.data);
    set->ntiles = ntiles;
    set->refc = 1;
    memcpy(set->tiles, tiles, ntiles*sizeof(*tiles));

    for (size_t i = 0; i < ntiles; i++) {
        if (set->tiles[i].pos.width > 0) {
            assert(set->tiles[i].pos.x >= 0);
            assert(set->tiles[i].pos.x + set->tiles[i].pos.width < set->img.width);
        } else {
            assert(set->tiles[i].pos.x < set->img.width);
            assert(set->tiles[i].pos.x + set->tiles[i].pos.width >= 0);
        }
        if (set->tiles[i].pos.height > 0) {
            assert(set->tiles[i].pos.y >= 0);
            assert(set->tiles[i].pos.y + set->tiles[i].pos.height < set->img.height);
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
        free(set);
    }
}

void ref_tileset(struct tileset *set) {
    set->refc++;
}

void tileset_draw_tile(struct image dst, struct tileset *set, tile_t tile, int16_t x, int16_t y) {
    assert(tile < set->ntiles);
    assert(dst.data);

    struct tile *tl = &set->tiles[tile];
    struct rect drect = {
        x + tl->origin_x*set->scale,
        y + tl->origin_y*set->scale,
        tl->pos.width*set->scale,
        tl->pos.height*set->scale
    };
    struct rect srect = {
        tl->pos.x,
        tl->pos.y,
        tl->pos.width,
        tl->pos.height
    };
    image_blt(dst, drect, set->img, srect, 0);
}

void tileset_set_scale(struct tileset *set, double scale) {
    set->scale = scale;
}

struct tilemap *create_tilemap(size_t width, size_t height, int16_t tile_width, int16_t tile_height, struct tileset **sets, size_t nsets) {
    assert(tile_width > 0);
    assert(tile_height > 0);

    struct tilemap *map = malloc(sizeof(*map) + width*height*TILES_PER_CELL*sizeof(tile_t));
    assert(map);

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

    /* Set every tile to NOTILE */
    memset(map->tiles, 0xFF, width*height*TILES_PER_CELL*sizeof(tile_t));

    return map;
}

void free_tilemap(struct tilemap *map) {
    for (size_t i = 0; i < map->nsets; i++) {
        unref_tileset(map->sets[i]);
    }
    free(map->sets);
    free(map);
}

tile_t tilemap_add_tileset(struct tilemap *map, struct tileset *newset) {
    struct tileset **new = realloc(map->sets, (map->nsets + 1)*sizeof(map->sets[0]));
    if (!new) return 0;

    map->sets[map->nsets] = newset;
    return map->nsets++;
}

tile_t tilemap_get_tile(struct tilemap *map, int16_t x, int16_t y, int16_t layer) {
    assert(x >= 0 && x < (ssize_t)map->width);
    assert(y >= 0 && y < (ssize_t)map->height);
    assert(layer >= 0 && layer < TILES_PER_CELL);
    return tilemap_get_tile_unsafe(map, x, y, layer);
}

void tilemap_draw(struct image dst, struct tilemap *map, int16_t x, int16_t y) {
    for (size_t yi = 0; yi < map->height; yi++) {
        for (size_t xi = 0; xi < map->width; xi++) {
            for (size_t zi = 0; zi < TILES_PER_CELL; zi++) {
                tile_t tile = tilemap_get_tile_unsafe(map, xi, yi, zi);
                tileset_draw_tile(dst, map->sets[TILESET_ID(tile)], TILE_ID(tile),
                                  x + xi*map->tile_width, y + yi*map->tile_height);
            }
        }
    }

}

void tilemap_set_tile(struct tilemap *map, int16_t x, int16_t y, int16_t layer, tile_t tile) {
    assert(x >= 0 && x < (ssize_t)map->width);
    assert(y >= 0 && y < (ssize_t)map->height);
    assert(layer >= 0 && layer < TILES_PER_CELL);
    assert(TILESET_ID(tile) < map->nsets);
    assert(TILE_ID(tile) < map->sets[TILESET_ID(tile)]->ntiles);

    tilemap_set_tile_unsafe(map, x, y, layer, tile);
}

void tilemap_set_scale(struct tilemap *map, double scale) {
    for (size_t i = 0; i < map->nsets; i++) {
        tileset_set_scale(map->sets[i], scale);
    }
}
