/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "context.h"
#include "image.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xcb/shm.h>
#include <smmintrin.h>

#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
/* Well, STB library does not have
 * best code ever, so disable some
 * warnings just for this header */
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wduplicated-branches"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

struct image create_empty_image(int16_t width, int16_t height) {
    size_t stride = (width + 3) & ~3;
    color_t *restrict data = aligned_alloc(CACHE_LINE, stride*height*sizeof(color_t));
    memset(data, 0, stride*height*sizeof(color_t));
    return (struct image) { .width = width, .height = height, .shmid = -1, .data = data };
}

struct image create_image(const char *file) {
    int x, y, n;
    color_t *image = (void *)stbi_load(file, &x, &y, &n, sizeof(color_t));
    size_t stride = (x + 3) & ~3;
    if (!image) {
        die("Can't load image: %s", stbi_failure_reason());
    }

    color_t *restrict data = aligned_alloc(CACHE_LINE, x*y*sizeof(color_t));

    // We need to swap channels since we expect BGR
    // And also X11 uses premultiplied alpha channel
    // (And this is a one-time conversion, so speed does not matter)
    for (size_t yi = 0; yi < (size_t)y; yi++) {
        for (size_t xi = 0; xi < (size_t)x; xi++) {
            color_t col = image[xi+yi*x];
            uint8_t a = color_a(col);
            uint8_t r = color_r(col);
            uint8_t g = color_g(col);
            uint8_t b = color_b(col);
            data[yi*stride+xi] = mk_color(b*a/255., g*a/255., r*a/255., a);
        }
    }

    free(image);

    return (struct image) { .width = x, .height = y, .shmid = -1, .data = data };
}

struct image create_shm_image(int16_t width, int16_t height) {
    struct image backbuf = {
        .width = width,
        .height = height,
        .shmid = -1,
    };
    size_t stride = (width + 3) & ~3;
    size_t size = stride * height * sizeof(color_t);

    if (ctx.has_shm) {
        char temp[] = "/renderer-XXXXXX";
        int32_t attempts = 16;

        do {
            struct timespec cur;
            clock_gettime(CLOCK_REALTIME, &cur);
            uint64_t r = cur.tv_nsec;
            for (int i = 0; i < 6; ++i, r >>= 5)
                temp[6+i] = 'A' + (r & 15) + (r & 16) * 2;
            backbuf.shmid = shm_open(temp, O_RDWR | O_CREAT | O_EXCL, 0600);
        } while (backbuf.shmid < 0 && errno == EEXIST && attempts-- > 0);

        shm_unlink(temp);

        if (backbuf.shmid < 0) return backbuf;

        if (ftruncate(backbuf.shmid, size) < 0) goto error;

        backbuf.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, backbuf.shmid, 0);
        if (backbuf.data == MAP_FAILED) goto error;
        xcb_void_cookie_t c;
        if (!ctx.shm_seg) {
            ctx.shm_seg = xcb_generate_id(ctx.con);
        } else {
            if (ctx.has_shm_pixmaps && ctx.shm_pixmap)
                xcb_free_pixmap(ctx.con, ctx.shm_pixmap);
            c = xcb_shm_detach_checked(ctx.con, ctx.shm_seg);
            check_void_cookie(c);
        }

        c = xcb_shm_attach_fd_checked(ctx.con, ctx.shm_seg, dup(backbuf.shmid), 0);
        if (check_void_cookie(c)) goto error;

        if (ctx.has_shm_pixmaps) {
            if (!ctx.shm_pixmap)
                ctx.shm_pixmap = xcb_generate_id(ctx.con);
            xcb_shm_create_pixmap(ctx.con, ctx.shm_pixmap,
                    ctx.wid, stride, height, 32, ctx.shm_seg, 0);
        }

        return backbuf;
    error:
        warn("Can't create image");
        if (backbuf.data != MAP_FAILED) munmap(backbuf.data, size);
        if (backbuf.shmid >= 0) close(backbuf.shmid);
        backbuf.shmid = -1;
        backbuf.data = NULL;
        return backbuf;
    } else {
        backbuf.data = aligned_alloc(CACHE_LINE, size);
        return backbuf;
    }
}

void free_image(struct image *backbuf) {
    if (backbuf->shmid >= 0) {
        size_t stride = (backbuf->width + 3) & ~3;
        if (backbuf->data) munmap(backbuf->data, stride * backbuf->height * sizeof(color_t));
        if (backbuf->shmid >= 0) close(backbuf->shmid);
    } else {
        if (backbuf->data) free(backbuf->data);
    }
    backbuf->shmid = -1;
    backbuf->data = NULL;
}

void image_draw_rect(struct image im, struct rect rect, color_t fg) {
    color_t *data = __builtin_assume_aligned(im.data, CACHE_LINE);
    size_t stride = (im.width + 3) & ~3;
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height})) {
        if (rect.x & 3 && rect.width > 4) {
            color_t *ptr = &data[rect.y * stride + rect.x];
            for (size_t j = 0; j < (size_t)rect.height; j++, ptr += stride) {
                switch (rect.x & 3) {
                case 1: ptr[2] = fg; //fallthrough
                case 2: ptr[1] = fg; //fallthrough
                case 3: ptr[0] = fg; //fallthrough
                default:;
                }
            }
            rect.width -= 4 - (rect.x & 3);
            rect.x += 4 - (rect.x & 3);
        }

        __m128i val = _mm_set1_epi32(fg);
        for (size_t j = 0; j < (size_t)rect.height; j++) {
            for (size_t i = 0; i < (size_t)(rect.width & ~3); i += 4) {
                void *ptr = &data[(rect.y + j) * stride + (rect.x + i)];
                _mm_store_si128(ptr, val);
            }
        }
        if (rect.width & 3) {
            color_t *ptr = &data[rect.y * stride + (rect.x + (size_t)(rect.width & ~3))];
            for (size_t j = 0; j < (size_t)rect.height; j++, ptr += stride) {
                switch (rect.width & 3) {
                case 3: ptr[2] = fg; //fallthrough
                case 2: ptr[1] = fg; //fallthrough
                case 1: ptr[0] = fg; //fallthrough
                case 0:;
                }
            }
        }
    }
}

__attribute__((always_inline))
inline static __m128i blend4(__m128i under, __m128i over) {
    const __m128i zero = (__m128i)_mm_setr_epi32(0x00000000, 0x00000000, 0x00000000, 0x00000000);
    const __m128i allo = (__m128i)_mm_setr_epi32(0x03FF03FF, 0x03FF03FF, 0x07FF07FF, 0x07FF07FF);
    const __m128i alhi = (__m128i)_mm_setr_epi32(0x0BFF0BFF, 0x0BFF0BFF, 0x0FFF0FFF, 0x0FFF0FFF);
    const __m128  m255 = (__m128)_mm_setr_epi32(0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);

    __m128i u16_0 = _mm_cvtepu8_epi16(under);
    __m128i u16_1 = _mm_unpackhi_epi8(under, zero);
    __m128i al8_0 = _mm_shuffle_epi8(over, allo);
    __m128i al8_1 = _mm_shuffle_epi8(over, alhi);
    __m128i mal_0 = (__m128i)_mm_xor_ps(m255, (__m128)al8_0);
    __m128i mal_1 = (__m128i)_mm_xor_ps(m255, (__m128)al8_1);
    __m128i mul_0 = _mm_mulhi_epu16(u16_0, mal_0);
    __m128i mul_1 = _mm_mulhi_epu16(u16_1, mal_1);
    __m128i pixel = _mm_packus_epi16(mul_0, mul_1);

    return _mm_adds_epi8(over, pixel);
}

__attribute__((always_inline))
inline static color_t image_sample(struct image src, double x, double y) {
    // Always clamp to border
    // IDK why I have implemented this
    x = MAX(0, x), y = MAX(0, y);

    ssize_t x0 = floor(x), y0 = floor(y);
    size_t sstride = (src.width + 3) & ~3;
    x0 = MIN(x0, src.width - 1);
    y0 = MIN(y0, src.height - 1);

    color_t *data = __builtin_assume_aligned(src.data, CACHE_LINE);

    // IDK why did i implement this...
    ssize_t x1 = ceil(x), y1 = ceil(y)*sstride;

    x1 = MIN(x1, src.width - 1);
    y1 = MIN(y1, src.width*(src.height - 1));

    double valpha = y - y0, halpha = x - x0;
    y0 *= sstride;

    color_t v0 = color_mix(data[x0+y0], data[x1+y0], halpha);
    color_t v1 = color_mix(data[x0+y1], data[x1+y1], halpha);

    return color_mix(v0, v1, valpha);
}

__attribute__((always_inline))
inline static color_t image_sample_nearest(struct image src, double x, double y) {
    size_t stride = (src.width + 3) & ~3;
    size_t ix = MIN(MAX(0, x), src.width - 1);
    size_t iy = MIN(MAX(0, y), src.height - 1);
    return src.data[iy*stride+ix];
}


void image_blt(struct image dst, struct rect drect, struct image src, struct rect srect, enum sampler_mode mode) {
    bool fastpath = srect.width == drect.width && srect.height == drect.height;
    double xscale = srect.width/(double)drect.width;
    double yscale = srect.height/(double)drect.height;

    drect.width = MIN(dst.width - drect.x, drect.width);
    drect.height = MIN(dst.height - drect.y, drect.height);

    color_t *sdata = __builtin_assume_aligned(src.data, CACHE_LINE);
    color_t *ddata = __builtin_assume_aligned(dst.data, CACHE_LINE);
    size_t sstride = (src.width + 3) & ~3;
    size_t dstride = (dst.width + 3) & ~3;

    if (fastpath) {
        if (drect.x < 0) drect.width -= drect.x, srect.x += drect.x, drect.x = 0;
        if (drect.y < 0) drect.height -= drect.y, srect.y += drect.y, drect.y = 0;
        drect.height = MIN(drect.height, src.height - srect.y);
        drect.width = MIN(drect.width, src.width - srect.x);
        if (LIKELY(drect.width > 0 && drect.height > 0)) {
            if (drect.x & 3) {
                for (size_t j = 0; j < (size_t)drect.height; j++) {
                    for (size_t i = 0; i < (size_t)(drect.x & 3); i++) {
                        color_t srcc = sdata[srect.x + i + (srect.y + j)*sstride];
                        color_t *pdstc = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                        *pdstc = color_blend(*pdstc, srcc);
                    }
                }
                drect.width -= 4 - (drect.x & 3);
                srect.x += 4 - (drect.x & 3);
                drect.x += 4 - (drect.x & 3);
            }
            /* Fast path for aligned non-resizing blits */
            for (size_t j = 0; j < (size_t)drect.height; j++) {
                for (size_t i = 0; i < (size_t)(drect.width & ~3); i += 4) {
                    void *ptr = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                    const __m128i d = _mm_load_si128((const void *)ptr);
                    const __m128i s = _mm_load_si128((const void *)&sdata[(srect.y + j) * sstride + (srect.x + i)]);
                    _mm_store_si128(ptr, blend4(d, s));
                }
            }
            if (drect.width & 3) {
                for (size_t j = 0; j < (size_t)drect.height; j++) {
                    for (size_t i = (size_t)(drect.width & ~3); i < (size_t)drect.width; i++) {
                        color_t srcc = sdata[srect.x + i + (srect.y + j)*sstride];
                        color_t *pdstc = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                        *pdstc = color_blend(*pdstc, srcc);
                    }
                }
            }
        }
    } else {
        if (LIKELY(drect.width > 0 && drect.height > 0)) {
            // Separate branches for better inlining...
            if (LIKELY(mode == sample_nearest)) {
                double sx0 = srect.x;
                if (drect.x & 3) {
                    for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                        for (size_t i = 0; i < (size_t)(drect.x & 3); i++) {
                            color_t srcc = image_sample_nearest(src, sx0 + i*xscale, srect.y + j*yscale);
                            color_t *pdstc = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                            *pdstc = color_blend(*pdstc, srcc);
                        }
                    }
                    drect.width -= 4 - (drect.x & 3);
                    sx0 += (4 - ((drect.x & 3)))*xscale;
                    drect.x += 4 - (drect.x & 3);
                }
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = MAX(-drect.x, 0); i < (size_t)drect.width; i += 4) {
                        void *ptr = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                        const __m128i d = _mm_load_si128((const void *)ptr);
                        const __m128i s = _mm_set_epi32(
                            image_sample_nearest(src, sx0 + (i + 3)*xscale, srect.y + j*yscale),
                            image_sample_nearest(src, sx0 + (i + 2)*xscale, srect.y + j*yscale),
                            image_sample_nearest(src, sx0 + (i + 1)*xscale, srect.y + j*yscale),
                            image_sample_nearest(src, sx0 + (i + 0)*xscale, srect.y + j*yscale));
                        _mm_store_si128(ptr, blend4(d, s));
                    }
                }
                if (drect.width & 3) {
                    for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                        for (size_t i = (size_t)(drect.width & ~3); i < (size_t)drect.width; i++) {
                            color_t srcc = image_sample_nearest(src, sx0 + i*xscale, srect.y + j*yscale);
                            color_t *pdstc = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                            *pdstc = color_blend(*pdstc, srcc);
                        }
                    }
                }
            } else {
                double sx0 = srect.x;
                if (drect.x & 3) {
                    for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                        for (size_t i = 0; i < (size_t)(drect.x & 3); i++) {
                            color_t srcc = image_sample(src, sx0 + i*xscale, srect.y + j*yscale);
                            color_t *pdstc = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                            *pdstc = color_blend(*pdstc, srcc);
                        }
                    }
                    drect.width -= 4 - (drect.x & 3);
                    sx0 += (4 - ((drect.x & 3)))*xscale;
                    drect.x += 4 - (drect.x & 3);
                }
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = MAX(-drect.x, 0); i < (size_t)drect.width; i += 4) {
                        void *ptr = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                        const __m128i d = _mm_load_si128((const void *)ptr);
                        const __m128i s = _mm_set_epi32(
                            image_sample(src, sx0 + (i + 3)*xscale, srect.y + j*yscale),
                            image_sample(src, sx0 + (i + 2)*xscale, srect.y + j*yscale),
                            image_sample(src, sx0 + (i + 1)*xscale, srect.y + j*yscale),
                            image_sample(src, sx0 + (i + 0)*xscale, srect.y + j*yscale));
                        _mm_store_si128(ptr, blend4(d, s));
                    }
                }
                if (drect.width & 3) {
                    for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                        for (size_t i = (size_t)(drect.width & ~3); i < (size_t)drect.width; i++) {
                            color_t srcc = image_sample(src, sx0 + i*xscale, srect.y + j*yscale);
                            color_t *pdstc = &ddata[(drect.y + j) * dstride + (drect.x + i)];
                            *pdstc = color_blend(*pdstc, srcc);
                        }
                    }
                }
            }
        }
    }
}
