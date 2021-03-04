/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "image.h"
#include "util.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <smmintrin.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

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


struct image create_shm_image(int32_t width, int32_t height) {
    struct image im = {
        .width = width,
        .height = height,
        .shmid = -1,
    };
    size_t stride = (width + 3) & ~3;
    size_t size = stride * height * sizeof(color_t);

    char temp[] = "/renderer-XXXXXX";
    int32_t attempts = 16;

    do {
        struct timespec cur;
        clock_gettime(CLOCK_REALTIME, &cur);
        uint64_t r = cur.tv_nsec;
        for (int i = 0; i < 6; ++i, r >>= 5)
            temp[6+i] = 'A' + (r & 15) + (r & 16) * 2;
        im.shmid = shm_open(temp, O_RDWR | O_CREAT | O_EXCL, 0600);
    } while (im.shmid < 0 && errno == EEXIST && attempts-- > 0);

    shm_unlink(temp);

    if (im.shmid < 0) return im;

    if (ftruncate(im.shmid, size) < 0) goto error;

    im.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, im.shmid, 0);
    if (im.data == MAP_FAILED) goto error;

    return im;

error:
    warn("Can't create image");
    free_image(&im);
    return im;
}

struct image create_image(int32_t width, int32_t height) {
    struct image im = {
        .width = width,
        .height = height,
        .shmid = -1,
    };
    size_t stride = (width + 3) & ~3;
    size_t size = stride * height * sizeof(color_t);

    im.data = aligned_alloc(CACHE_LINE, size);
    memset(im.data, 0, stride*height*sizeof(color_t));

    return im;
}

struct image load_image(const char *file) {
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


void free_image(struct image *im) {
    if (im->shmid >= 0) {
        size_t stride = (im->width + 3) & ~3;
        if (im->data && im->data != MAP_FAILED)
            munmap(im->data, stride * im->height * sizeof(color_t));
        close(im->shmid);
    } else {
        if (im->data && im->data != MAP_FAILED)
            free(im->data);
    }
    im->shmid = -1;
    im->data = NULL;
}

struct do_fill_arg {
    color_t *ptr;
    color_t fg;
    ssize_t h;
    ssize_t w;
    ssize_t stride;
};

static HOT void do_fill_unaligned(void *varg) {
    struct do_fill_arg *arg = varg;

    for (ssize_t j = 0; j < arg->h; j++, arg->ptr += arg->stride) {
        switch (arg->w) {
        case 3: arg->ptr[2] = color_blend(arg->ptr[2], arg->fg); //fallthrough
        case 2: arg->ptr[1] = color_blend(arg->ptr[1], arg->fg); //fallthrough
        case 1: arg->ptr[0] = color_blend(arg->ptr[0], arg->fg); //fallthrough
        default:;
        }
    }
}

static FORCEINLINE inline __m128i blend4(__m128i under, __m128i over) {
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

    return _mm_adds_epu8(over, pixel);
}

static HOT void do_fill_aligned(void *varg) {
    struct do_fill_arg *arg = varg;

    const __m128i val = _mm_set1_epi32(arg->fg);
    for (ssize_t j = 0; j < arg->h; j++) {
        for (ssize_t i = 0; i < arg->w; i += 4) {
            color_t *ptr = arg->ptr + arg->stride*j + i;
            const __m128i dst = _mm_load_si128((void *)ptr);
            _mm_store_si128((void *)ptr, blend4(dst, val));
        }
    }
}

void image_queue_fill(struct image im, struct rect rect, color_t fg) {
    color_t *data = ASSUMEALIGNED(im.data, CACHE_LINE);
    ssize_t stride = (im.width + 3) & ~3;
    if (intersect_with(&rect, &(struct rect){0, 0, im.width, im.height})) {
        if (rect.x & 3) {
            ssize_t pref = MIN(4 - (rect.x & 3), rect.width);
            struct do_fill_arg arg = {
                &data[rect.y * stride + rect.x],
                fg, rect.height, pref, stride
            };
            submit_work(do_fill_unaligned, &arg, sizeof arg);
            rect.width -= pref;
            rect.x += pref;
        }

        if (rect.width & ~3) {
            if (rect.height < 2*(int)nproc) {
                struct do_fill_arg arg = {
                    &data[rect.y * stride + rect.x],
                    fg, rect.height, rect.width & ~3, stride
                };
                submit_work(do_fill_aligned, &arg, sizeof arg);
            } else {
                ssize_t block = rect.height/nproc;
                for (ssize_t i = 0; i < nproc; i++) {
                    struct do_fill_arg arg = {
                        &data[(rect.y + i*block) * stride + rect.x],
                        fg, block, rect.width & ~3, stride
                    };
                    submit_work(do_fill_aligned, &arg, sizeof arg);
                }

                if (nproc*block != (ssize_t)rect.height) {
                    struct do_fill_arg arg = {
                        &data[(rect.y + nproc*block) * stride + rect.x],
                        fg, rect.height - nproc*block, rect.width & ~3, stride
                    };
                    submit_work(do_fill_aligned, &arg, sizeof arg);
                }
            }
        }

        if (rect.width & 3) {
            struct do_fill_arg arg = {
                &data[rect.y * stride + rect.x + (rect.width & ~3)],
                fg, rect.height, rect.width & 3, stride
            };
            submit_work(do_fill_unaligned, &arg, sizeof arg);
        }
    }
}

static FORCEINLINE inline color_t image_sample(struct image src, ssize_t x, ssize_t y) {
    // Always clamp to border
    // IDK why I have implemented this...

    ssize_t sstride = (src.width + 3) & ~3;
    ssize_t x0 = MIN(x >> FIXPREC, src.width - 1);
    ssize_t y0 = MIN(y >> FIXPREC, src.height - 1);

    color_t *data = ASSUMEALIGNED(src.data, CACHE_LINE);

    ssize_t x1 = MIN((x + (1LL << FIXPREC) - 1) >> FIXPREC, src.width - 1);
    ssize_t y1 = MIN((y + (1LL << FIXPREC) - 1) >> FIXPREC, src.height - 1);
    ssize_t valpha = y & ((1LL << FIXPREC) - 1);
    ssize_t halpha = x & ((1LL << FIXPREC) - 1);

    y0 *= sstride;
    y1 *= sstride;

    color_t v0 = color_mix(data[x0+y0], data[x1+y0], halpha);
    color_t v1 = color_mix(data[x0+y1], data[x1+y1], halpha);

    return color_mix(v0, v1, valpha);
}

struct do_blt_arg {
    size_t h;
    size_t w;
    size_t dstride;
    size_t sstride;
    color_t *dst;
    color_t *src;
};

static HOT void do_blt_aligned(void *varg) {
    struct do_blt_arg *arg = varg;

    for (size_t j = 0; j < arg->h; j++) {
        for (size_t i = 0; i < arg->w; i += 4) {
            void *ptr = arg->dst + j*arg->dstride + i;
            const __m128i d = _mm_load_si128((const void *)ptr);
            const __m128i s = _mm_loadu_si128((const void *)(arg->src + j*arg->sstride + i));
            _mm_store_si128(ptr, blend4(d, s));
        }
    }
}

static HOT void do_blt_aligned2(void *varg) {
    struct do_blt_arg *arg = varg;

    for (size_t j = 0; j < arg->h; j++) {
        for (size_t i = 0; i < arg->w; i += 4) {
            void *ptr = arg->dst + j*arg->dstride + i;
            const __m128i d = _mm_load_si128((void *)ptr);
            const __m128i s = _mm_load_si128((void *)(arg->src + j*arg->sstride + i));
            _mm_store_si128(ptr, blend4(d, s));
        }
    }
}

static HOT void do_blt_unaligned(void *varg) {
    struct do_blt_arg *arg = varg;

    for (size_t j = 0; j < arg->h; j++) {
        for (size_t i = 0; i < arg->w; i++) {
            color_t srcc = arg->src[i + j*arg->sstride];
            color_t *pdstc = &arg->dst[j * arg->dstride + i];
            *pdstc = color_blend(*pdstc, srcc);
        }
    }
}

struct do_blt_scale_arg {
    ssize_t h;
    ssize_t w;
    ssize_t dstride;
    ssize_t sstride;
    ssize_t x0;
    ssize_t y0;
    ssize_t xscale;
    ssize_t yscale;
    color_t *dst;
    struct image src;
};

static HOT void do_blt_unaligned_scaling_nearest(void *varg) {
    struct do_blt_scale_arg *arg = varg;

    for (ssize_t j = 0; j < arg->h; j++) {
        ssize_t iy = MIN(MAX(0, (arg->y0 + j*arg->yscale) >> FIXPREC), arg->src.height - 1);
        for (ssize_t i = 0; i < arg->w; i++) {
            ssize_t ix = MIN(MAX(0, (arg->x0 + i*arg->xscale) >> FIXPREC), arg->src.width - 1);
            color_t srcc = arg->src.data[iy*arg->sstride+ix];
            color_t *pdstc = &arg->dst[j * arg->dstride + i];
            *pdstc = color_blend(*pdstc, srcc);
        }
    }
}

static HOT void do_blt_unaligned_scaling_linear(void *varg) {
    struct do_blt_scale_arg *arg = varg;

    for (ssize_t j = 0; j < arg->h; j++) {
        for (ssize_t i = 0; i < arg->w; i++) {
            color_t srcc = image_sample(arg->src, (arg->x0 + i*arg->xscale), (arg->y0 + j*arg->yscale));
            color_t *pdstc = &arg->dst[j * arg->dstride + i];
            *pdstc = color_blend(*pdstc, srcc);
        }
    }
}

static HOT void do_blt_aligned_scaling_nearest(void *varg) {
    struct do_blt_scale_arg *arg = varg;

    if (arg->xscale > 0 && arg->x0 >= 0 && ((arg->x0 + arg->w*arg->xscale) >> FIXPREC) <= arg->src.width - 1) {
        for (ssize_t j = 0; j < arg->h; j++) {
            color_t *sptr = arg->src.data + MIN(MAX(0, (arg->y0 + j*arg->yscale) >> FIXPREC), arg->src.height - 1)*arg->sstride;
            for (ssize_t i = 0; i < arg->w; i += 4) {
                void *ptr = (char *)&arg->dst[j * arg->dstride + i];
                ssize_t ix0 = (arg->x0 + (i + 0)*arg->xscale) >> FIXPREC;
                ssize_t ix1 = (arg->x0 + (i + 1)*arg->xscale) >> FIXPREC;
                ssize_t ix2 = (arg->x0 + (i + 2)*arg->xscale) >> FIXPREC;
                ssize_t ix3 = (arg->x0 + (i + 3)*arg->xscale) >> FIXPREC;
                const __m128i s = _mm_set_epi32(sptr[ix3], sptr[ix2], sptr[ix1], sptr[ix0]);
                const __m128i d = _mm_load_si128(ptr);
                _mm_store_si128(ptr, blend4(d, s));
            }
        }
    } else {
        for (ssize_t j = 0; j < arg->h; j++) {
            color_t *sptr = arg->src.data + MIN(MAX(0, (arg->y0 + j*arg->yscale) >> FIXPREC), arg->src.height - 1)*arg->sstride;
            for (ssize_t i = 0; i < arg->w; i += 4) {
                void *ptr = &arg->dst[j * arg->dstride + i];
                ssize_t ix0 = MAX(0, MIN((arg->x0 + (i + 0)*arg->xscale) >> FIXPREC, arg->src.width - 1));
                ssize_t ix1 = MAX(0, MIN((arg->x0 + (i + 1)*arg->xscale) >> FIXPREC, arg->src.width - 1));
                ssize_t ix2 = MAX(0, MIN((arg->x0 + (i + 2)*arg->xscale) >> FIXPREC, arg->src.width - 1));
                ssize_t ix3 = MAX(0, MIN((arg->x0 + (i + 3)*arg->xscale) >> FIXPREC, arg->src.width - 1));
                const __m128i s = _mm_set_epi32(sptr[ix3], sptr[ix2], sptr[ix1], sptr[ix0]);
                const __m128i d = _mm_load_si128(ptr);
                _mm_store_si128(ptr, blend4(d, s));
            }
        }
    }
}

static HOT void do_blt_aligned_scaling_linear(void *varg) {
    struct do_blt_scale_arg *arg = varg;

    for (ssize_t j = 0; j < arg->h; j++) {
        for (ssize_t i = 0; i < arg->w; i += 4) {
            void *ptr = &arg->dst[j * arg->dstride + i];
            const __m128i d = _mm_load_si128(ptr);
            const __m128i s = _mm_set_epi32(
                image_sample(arg->src, (arg->x0 + (i + 3)*arg->xscale), (arg->y0 + j*arg->yscale)),
                image_sample(arg->src, (arg->x0 + (i + 2)*arg->xscale), (arg->y0 + j*arg->yscale)),
                image_sample(arg->src, (arg->x0 + (i + 1)*arg->xscale), (arg->y0 + j*arg->yscale)),
                image_sample(arg->src, (arg->x0 + (i + 0)*arg->xscale), (arg->y0 + j*arg->yscale)));
            _mm_store_si128(ptr, blend4(d, s));
        }
    }
}

void image_queue_blt(struct image dst, struct rect drect, struct image src, struct rect srect, enum sample_mode mode) {
    bool fastpath = srect.width == drect.width && srect.height == drect.height;

    ssize_t xscale = ((ssize_t)srect.width << FIXPREC)/drect.width;
    ssize_t yscale = ((ssize_t)srect.height << FIXPREC)/drect.height;

    drect.width = MIN(dst.width - drect.x, drect.width);
    drect.height = MIN(dst.height - drect.y, drect.height);

    color_t *sdata = ASSUMEALIGNED(src.data, CACHE_LINE);
    color_t *ddata = ASSUMEALIGNED(dst.data, CACHE_LINE);
    ssize_t sstride = (src.width + 3) & ~3;
    ssize_t dstride = (dst.width + 3) & ~3;

    if (fastpath) {
        /* Fast path for aligned non-resizing blits */
        if (drect.x < 0) drect.width += drect.x, srect.x -= drect.x, drect.x = 0;
        if (drect.y < 0) drect.height += drect.y, srect.y -= drect.y, drect.y = 0;
        drect.height = MIN(drect.height, src.height - srect.y);
        drect.width = MIN(drect.width, src.width - srect.x);
        if (LIKELY(drect.width > 0 && drect.height > 0)) {
            if (drect.x & 3) {
                struct do_blt_arg arg = {
                    drect.height, 4 - (drect.x & 3), dstride, sstride,
                    &ddata[drect.y*dstride+drect.x],
                    &sdata[srect.y*sstride+srect.x]
                };
                submit_work(do_blt_unaligned, &arg, sizeof arg);
                drect.width -= 4 - (drect.x & 3);
                srect.x += 4 - (drect.x & 3);
                drect.x += 4 - (drect.x & 3);
            }

            if (drect.width & ~3) {
                if (drect.height*drect.width < 256*(int)nproc) {
                    struct do_blt_arg arg = {
                        drect.height, drect.width & ~3, dstride, sstride,
                        &ddata[drect.y*dstride+drect.x],
                        &sdata[srect.y*sstride+srect.x]
                    };
                    submit_work((uintptr_t)arg.src & 15 ? do_blt_aligned : do_blt_aligned2, &arg, sizeof arg);
                } else {
                    ssize_t block = drect.height/nproc;
                    for (ssize_t i = 0; i < nproc; i++) {
                        struct do_blt_arg arg = {
                            block, drect.width & ~3, dstride, sstride,
                            &ddata[(drect.y + block*i)*dstride+drect.x],
                            &sdata[(srect.y + block*i)*sstride+srect.x]
                        };
                        submit_work((uintptr_t)arg.src & 15 ? do_blt_aligned : do_blt_aligned2, &arg, sizeof arg);
                    }

                    if (nproc*block != drect.height) {
                        struct do_blt_arg arg = {
                            drect.height - nproc*block, drect.width & ~3, dstride, sstride,
                            &ddata[(drect.y + block*nproc)*dstride+drect.x],
                            &sdata[(srect.y + block*nproc)*sstride+srect.x]
                        };
                        submit_work((uintptr_t)arg.src & 15 ? do_blt_aligned : do_blt_aligned2, &arg, sizeof arg);
                    }
                }
            }

            if (drect.width & 3) {
                struct do_blt_arg arg = {
                    drect.height, drect.width - (drect.width & ~3), dstride, sstride,
                    &ddata[drect.y*dstride+(drect.width & ~3)+drect.x],
                    &sdata[srect.y*sstride+(drect.width & ~3)+srect.x]
                };
                submit_work(do_blt_unaligned, &arg, sizeof arg);
            }
        }
    } else {
        if (LIKELY(drect.width + MIN(drect.x, 0) > 0 && drect.height + MIN(drect.y, 0) > 0)) {
            ssize_t sx0 = srect.x << FIXPREC;
            if (drect.x & 3 && drect.x > 0) {
                struct do_blt_scale_arg arg = {
                    drect.height + MIN(drect.y, 0), 4 - (drect.x & 3), dstride, sstride,
                    sx0, (srect.y << FIXPREC) - MIN(drect.y, 0)*yscale, xscale, yscale,
                    &ddata[MAX(drect.y, 0) * dstride + drect.x], src,
                };
                submit_work(mode == sample_nearest ? do_blt_unaligned_scaling_nearest :
                            do_blt_unaligned_scaling_linear, &arg, sizeof arg);
                drect.width -= 4 - (drect.x & 3);
                sx0 += (4 - ((drect.x & 3)))*xscale;
                drect.x += 4 - (drect.x & 3);
            } else if (drect.x < 0) {
                drect.width += drect.x;
                sx0 -= drect.x*xscale;
                drect.x = 0;
            }

            if (drect.width & ~3) {
                if (drect.height*drect.width < 256*nproc) {
                        struct do_blt_scale_arg arg = {
                            drect.height + MIN(drect.y, 0), drect.width & ~3, dstride, sstride,
                            sx0, (srect.y << FIXPREC) - MIN(drect.y, 0)*yscale, xscale, yscale,
                            &ddata[MAX(drect.y, 0) * dstride + drect.x], src,
                        };
                        submit_work(do_blt_aligned_scaling_nearest, &arg, sizeof arg);
                    submit_work(mode == sample_nearest ? do_blt_aligned_scaling_nearest :
                                do_blt_aligned_scaling_linear, &arg, sizeof arg);
                } else {
                    ssize_t block = (drect.height - MAX(-drect.y, 0))/nproc;
                    for (ssize_t i = 0; i < nproc; i++) {
                        struct do_blt_scale_arg arg = {
                            block, drect.width & ~3, dstride, sstride,
                            sx0, (srect.y << FIXPREC) + (i*block + MAX(-drect.y, 0))*yscale, xscale, yscale,
                            &ddata[(MAX(drect.y, 0) + i*block) * dstride + drect.x], src,
                        };
                        submit_work(mode == sample_nearest ? do_blt_aligned_scaling_nearest :
                                    do_blt_aligned_scaling_linear, &arg, sizeof arg);
                    }

                    if (nproc*block != drect.height) {
                        struct do_blt_scale_arg arg = {
                            drect.height + MIN(drect.y, 0) - block*nproc, drect.width & ~3, dstride, sstride,
                            sx0, (srect.y << FIXPREC) + (nproc*block+MAX(-drect.y, 0))*yscale, xscale, yscale,
                            &ddata[(MAX(drect.y, 0) + nproc*block) * dstride + drect.x], src,
                        };
                        submit_work(mode == sample_nearest ? do_blt_aligned_scaling_nearest :
                                    do_blt_aligned_scaling_linear, &arg, sizeof arg);
                    }
                }
            }

            if (drect.width & 3) {
                struct do_blt_scale_arg arg = {
                    drect.height + MIN(drect.y, 0), drect.width - (drect.width & ~3), dstride, sstride,
                    sx0 + xscale*(drect.width & ~3), (srect.y << FIXPREC) - MIN(drect.y, 0)*yscale, xscale, yscale,
                    &ddata[MAX(drect.y, 0) * dstride + drect.x + (drect.width & ~3)], src,
                };
                submit_work(mode == sample_nearest ? do_blt_unaligned_scaling_nearest :
                            do_blt_unaligned_scaling_linear, &arg, sizeof arg);
            }
        }
    }
}
