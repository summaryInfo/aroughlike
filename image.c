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
#include "stb_image.h"
#pragma GCC diagnostic pop

struct image create_empty_image(int16_t width, int16_t height) {
    color_t *restrict data = aligned_alloc(CACHE_LINE, width*height*sizeof(color_t));
    memset(data, 0, width*height*sizeof(color_t));
    return (struct image) { .width = width, .height = height, .shmid = -1, .data = data };
}

struct image create_image(const char *file) {
    int x, y, n;
    color_t *image = (void *)stbi_load(file, &x, &y, &n, sizeof(color_t));
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
            data[yi*x+xi] = mk_color(b*a/255., g*a/255., r*a/255., a);
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
    size_t size = width * height * sizeof(color_t);

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
                    ctx.wid, width, height, 32, ctx.shm_seg, 0);
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
        if (backbuf->data) munmap(backbuf->data, backbuf->width * backbuf->height * sizeof(color_t));
        if (backbuf->shmid >= 0) close(backbuf->shmid);
    } else {
        if (backbuf->data) free(backbuf->data);
    }
    backbuf->shmid = -1;
    backbuf->data = NULL;
}

void image_draw_rect(struct image im, struct rect rect, color_t fg) {
    color_t *data = __builtin_assume_aligned(im.data, CACHE_LINE);
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height})) {
        for (size_t j = 0; j < (size_t)rect.height; j++) {
            for (size_t i = 0; i < (size_t)rect.width; i++) {
                data[(rect.y + j) * im.width + (rect.x + i)] = fg;
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
    __m128i u16_1 = _mm_unpackhi_epi8(under,zero);
    __m128i al8_0 = _mm_shuffle_epi8 (over,allo);
    __m128i al8_1 = _mm_shuffle_epi8 (over,alhi);
    __m128i mal_0 = (__m128i)_mm_xor_ps(m255,(__m128)al8_0);
    __m128i mal_1 = (__m128i)_mm_xor_ps(m255,(__m128)al8_1);
    __m128i mul_0 = _mm_mulhi_epu16(u16_0, mal_0);
    __m128i mul_1 = _mm_mulhi_epu16(u16_1, mal_1);
    __m128i pixel = _mm_packus_epi16(mul_0, mul_1);

    return _mm_adds_epi8(over, pixel);
}

void image_blt(struct image dst, struct rect drect, struct image src, struct rect srect, enum sampler_mode mode) {
    bool fastpath = srect.width == drect.width && srect.height == drect.height;
    double xscale = srect.width/(double)drect.width;
    double yscale = srect.height/(double)drect.height;

    drect.width = MIN(dst.width - drect.x, drect.width);
    drect.height = MIN(dst.height - drect.y, drect.height);

    color_t *sdata = __builtin_assume_aligned(src.data, CACHE_LINE);
    color_t *ddata = __builtin_assume_aligned(dst.data, CACHE_LINE);

    if (drect.width > 0 && drect.height > 0) {
        if (fastpath) {
            if (!(MAX(0, drect.x) & 3) && !(dst.width & 3) &&
                !(MAX(0, srect.x) & 3) && !(src.width & 3)) {
                /* Fast path for aligned non-resizing blits */
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = MAX(-drect.x, 0); i < (size_t)(drect.width & ~3); i += 4) {
                        void *ptr = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                        const __m128i d = _mm_load_si128((const void *)ptr);
                        const __m128i s = _mm_load_si128((const void *)&sdata[(srect.y + j) * src.width + (srect.x + i)]);
                        _mm_storeu_si128(ptr, blend4(d, s));
                    }
                }
            } else {
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = MAX(-drect.x, 0); i < (size_t)(drect.width & ~3); i += 4) {
                        void *ptr = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                        const __m128i d = _mm_loadu_si128((const void *)ptr);
                        const __m128i s = _mm_loadu_si128((const void *)&sdata[(srect.y + j) * src.width + (srect.x + i)]);
                        _mm_storeu_si128(ptr, blend4(d, s));
                    }
                }
            }
            for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                for (size_t i = (size_t)(drect.width & ~3); i < (size_t)drect.width; i ++) {
                    color_t srcc = image_sample(src, srect.x + i*xscale, srect.y + j*yscale, sample_nearest);
                    color_t *pdstc = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                    *pdstc = color_blend(*pdstc, srcc);
                }
            }
        } else {
            // Separate branches for better inlining...
            if (mode == sample_nearest) {
                if (!(MAX(0, drect.x) & 3) && !(dst.width & 3)) {
                    for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                        for (size_t i = MAX(-drect.x, 0); i < (size_t)drect.width; i += 4) {
                            void *ptr = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                            const __m128i d = _mm_load_si128((const void *)ptr);
                            const __m128i s = _mm_set_epi32(
                                image_sample(src, srect.x + (i + 3)*xscale, srect.y + j*yscale, sample_nearest),
                                image_sample(src, srect.x + (i + 2)*xscale, srect.y + j*yscale, sample_nearest),
                                image_sample(src, srect.x + (i + 1)*xscale, srect.y + j*yscale, sample_nearest),
                                image_sample(src, srect.x + (i + 0)*xscale, srect.y + j*yscale, sample_nearest));
                            _mm_storeu_si128(ptr, blend4(d, s));
                        }
                    }
                } else {
                    for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                        for (size_t i = MAX(-drect.x, 0); i < (size_t)(drect.width & ~3); i += 4) {
                            void *ptr = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                            const __m128i d = _mm_loadu_si128((const void *)ptr);
                            const __m128i s = _mm_set_epi32(
                                image_sample(src, srect.x + (i + 3)*xscale, srect.y + j*yscale, sample_nearest),
                                image_sample(src, srect.x + (i + 2)*xscale, srect.y + j*yscale, sample_nearest),
                                image_sample(src, srect.x + (i + 1)*xscale, srect.y + j*yscale, sample_nearest),
                                image_sample(src, srect.x + (i + 0)*xscale, srect.y + j*yscale, sample_nearest));
                            _mm_storeu_si128(ptr, blend4(d, s));
                        }
                    }
                }
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = (size_t)(drect.width & ~3); i < (size_t)drect.width; i++) {
                        color_t srcc = image_sample(src, srect.x + i*xscale, srect.y + j*yscale, sample_nearest);
                        color_t *pdstc = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                        *pdstc = color_blend(*pdstc, srcc);
                    }
                }
            } else {
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = MAX(-drect.x, 0); i < (size_t)(drect.width & ~3); i += 4) {
                        void *ptr = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                        const __m128i d = _mm_loadu_si128((const void *)ptr);
                        const __m128i s = _mm_set_epi32(
                            image_sample(src, srect.x + (i + 3)*xscale, srect.y + j*yscale, sample_nearest),
                            image_sample(src, srect.x + (i + 2)*xscale, srect.y + j*yscale, sample_nearest),
                            image_sample(src, srect.x + (i + 1)*xscale, srect.y + j*yscale, sample_nearest),
                            image_sample(src, srect.x + (i + 0)*xscale, srect.y + j*yscale, sample_nearest));
                        _mm_storeu_si128(ptr, blend4(d, s));
                    }
                }
                for (size_t j = MAX(-drect.y, 0); j < (size_t)drect.height; j++) {
                    for (size_t i = (size_t)(drect.width & ~3); i < (size_t)drect.width; i++) {
                        color_t srcc = image_sample(src, srect.x + i*xscale, srect.y + j*yscale, sample_linear);
                        color_t *pdstc = &ddata[(drect.y + j) * dst.width + (drect.x + i)];
                        *pdstc = color_blend(*pdstc, srcc);
                    }
                }
            }
        }
    }
}
