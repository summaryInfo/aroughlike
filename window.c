/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#define _GNU_SOURCE

#include "util.h"
#include "image.h"


/* This is a copy of key sym definitions
 * file from Xlib is shipped with the
 * program in order to prevent X11 from being
 * a dependency (only libxcb and libxcb-shm are required) */
#define XK_MISCELLANY
#define XK_LATIN1
#include "keysymdef.h"

#include <X11/keysym.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

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

#define TRUE_COLOR_ALPHA_DEPTH 32
#define BG_COLOR 0xFFFFFFFF
#define WINDOW_X 100
#define WINDOW_Y 100
#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480
#define FPS 60
#define TPS 10

#define WINDOW_CLASS "SoftRendererExa"
#define WINDOW_TITLE "Rough-like with software rendering"

void warn(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
_Noreturn void die(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

typedef uint32_t color_t;

struct context {
    xcb_connection_t *con;
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;

    xcb_window_t wid;
    xcb_gcontext_t gc;

    bool focused;
    bool active;
    bool force_redraw;
    bool want_exit;
    bool want_redraw;
    bool has_shm;
    bool has_shm_pixmaps;

    xcb_shm_seg_t shm_seg;
    xcb_pixmap_t shm_pixmap;
    struct image im;

    struct timespec last_draw;
    /* Game logic is handled in fixed rate,
     * separate from FPS */
    struct timespec last_tick;

    double dpi;

    struct atom_ {
        xcb_atom_t _NET_WM_PID;
        xcb_atom_t _NET_WM_NAME;
        xcb_atom_t _NET_WM_ICON_NAME;
        xcb_atom_t WM_DELETE_WINDOW;
        xcb_atom_t WM_PROTOCOLS;
        xcb_atom_t UTF8_STRING;
    } atom;

    int16_t center_x, center_y;

    xcb_get_keyboard_mapping_reply_t *keymap;

    struct image image;

    struct input_state {
        bool forward : 1;
        bool backward : 1;
        bool left : 1;
        bool right : 1;
    } keys;
};

struct context ctx;

_Noreturn void die(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("[\033[31;1mFATAL\033[m] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    exit(EXIT_FAILURE);
}

void warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("[\033[33;1mWARN\033[m] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}


inline static bool check_void_cookie(xcb_void_cookie_t ck) {
    xcb_generic_error_t *err = xcb_request_check(ctx.con, ck);
    if (err) {
        warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
        return 1;
    }
    free(err);
    return 0;
}

static struct image create_image(const char *file) {
    int x, y, n;
    color_t *image = (void *)stbi_load(file, &x, &y, &n, sizeof(color_t));
    if (!image) {
        die("Can't load image: %s", stbi_failure_reason());
    }

    // We need to swap channels since we expect BGR
    // And also X11 uses premultiplied alpha channel
    // (And this is a one-time conversion, so speed does not matter)
    for (size_t yi = 0; yi < (size_t)y; yi++) {
        for (size_t xi = 0; xi < (size_t)x; xi++) {
            color_t *col = &image[xi+yi*x];
            uint8_t a = color_a(*col);
            uint8_t r = color_r(*col);
            uint8_t g = color_g(*col);
            uint8_t b = color_b(*col);
            *col = mk_color(b*a/255., g*a/255., r*a/255., a);
        }
    }

    return (struct image) { .width = x, .height = y, .shmid = -1, .data = image };
}

static struct image create_shm_image(int16_t width, int16_t height) {
    struct image im = {
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
            im.shmid = shm_open(temp, O_RDWR | O_CREAT | O_EXCL, 0600);
        } while (im.shmid < 0 && errno == EEXIST && attempts-- > 0);

        shm_unlink(temp);

        if (im.shmid < 0) return im;

        if (ftruncate(im.shmid, size) < 0) goto error;

        im.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, im.shmid, 0);
        if (im.data == MAP_FAILED) goto error;
        xcb_void_cookie_t c;
        if (!ctx.shm_seg) {
            ctx.shm_seg = xcb_generate_id(ctx.con);
        } else {
            if (ctx.has_shm_pixmaps && ctx.shm_pixmap)
                xcb_free_pixmap(ctx.con, ctx.shm_pixmap);
            c = xcb_shm_detach_checked(ctx.con, ctx.shm_seg);
            check_void_cookie(c);
        }

        c = xcb_shm_attach_fd_checked(ctx.con, ctx.shm_seg, dup(im.shmid), 0);
        if (check_void_cookie(c)) goto error;

        if (ctx.has_shm_pixmaps) {
            if (!ctx.shm_pixmap)
                ctx.shm_pixmap = xcb_generate_id(ctx.con);
            xcb_shm_create_pixmap(ctx.con, ctx.shm_pixmap,
                    ctx.wid, width, height, 32, ctx.shm_seg, 0);
        }

        return im;
    error:
        warn("Can't create image");
        if (im.data != MAP_FAILED) munmap(im.data, size);
        if (im.shmid >= 0) close(im.shmid);
        im.shmid = -1;
        im.data = NULL;
        return im;
    } else {
        im.data = malloc(size);
        return im;
    }
}

static void free_image(struct image *im) {
    if (im->shmid >= 0) {
        if (im->data) munmap(im->data, im->width * im->height * sizeof(color_t));
        if (im->shmid >= 0) close(im->shmid);
    } else {
        if (im->data) free(im->data);
    }
    im->shmid = -1;
    im->data = NULL;
}

static void renderer_update(struct rect rect) {
    if (ctx.has_shm_pixmaps) {
        xcb_copy_area(ctx.con, ctx.shm_pixmap, ctx.wid, ctx.gc, rect.x, rect.y, rect.x, rect.y, rect.width, rect.height);
    } else if (ctx.has_shm) {
        xcb_shm_put_image(ctx.con, ctx.wid, ctx.gc, ctx.im.width, ctx.im.height, rect.x, rect.y,
                          rect.width, rect.height, rect.x, rect.y, 32, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, ctx.shm_seg, 0);
    } else {
        xcb_put_image(ctx.con, XCB_IMAGE_FORMAT_Z_PIXMAP, ctx.wid, ctx.gc, ctx.im.width, rect.height,
                      0, rect.y, 0, 32, rect.height * ctx.im.width * sizeof(color_t), (const uint8_t *)(ctx.im.data+rect.y*ctx.im.width));
    }
}

static void redraw(void) {
    struct rect screen = (struct rect){0, 0, ctx.im.width, ctx.im.height};

    image_draw_rect(ctx.im, screen, BG_COLOR);

    int16_t x = ctx.im.width/2 + ctx.center_x;
    int16_t y = ctx.im.height/2 + ctx.center_y;
    image_blt(ctx.im, (struct rect){ctx.im.width/2 - 100, ctx.im.height/2 - 50, ctx.image.width*10, ctx.image.height*10}, ctx.image, (struct rect){0, ctx.image.height, ctx.image.width, -ctx.image.height}, 0);
    image_blt(ctx.im, (struct rect){x, y, ctx.image.width*10, ctx.image.height*10}, ctx.image, (struct rect){0, 0, ctx.image.width, ctx.image.height}, 0);
}

static void tick(struct timespec time) {
#define DELTA_COORD 10

    (void)time;
    if (ctx.keys.forward) { ctx.center_y -= DELTA_COORD; ctx.want_redraw = 1; }
    if (ctx.keys.backward) { ctx.center_y += DELTA_COORD; ctx.want_redraw = 1; }
    if (ctx.keys.left) { ctx.center_x -= DELTA_COORD; ctx.want_redraw = 1; }
    if (ctx.keys.right) { ctx.center_x += DELTA_COORD; ctx.want_redraw = 1; }
}

static xcb_atom_t intern_atom(const char *atom) {
    xcb_atom_t at;
    xcb_generic_error_t *err;
    xcb_intern_atom_cookie_t c = xcb_intern_atom(ctx.con, 0, strlen(atom), atom);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(ctx.con, c, &err);
    if (err) {
        warn("Can't intern atom: %s", atom);
        free(err);
    }
    at = reply->atom;
    free(reply);
    return at;
}

static void create_window(void) {
    xcb_void_cookie_t c;

    /* Create window itself */

    uint32_t ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE | XCB_EVENT_MASK_KEY_PRESS |
            XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    uint32_t mask1 =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values1[5] = { BG_COLOR, BG_COLOR, XCB_GRAVITY_NORTH_WEST, ev_mask, ctx.mid };

    ctx.wid = xcb_generate_id(ctx.con);
    c = xcb_create_window_checked(ctx.con, TRUE_COLOR_ALPHA_DEPTH, ctx.wid, ctx.screen->root,
                                  WINDOW_X, WINDOW_Y, WINDOW_WIDTH, WINDOW_HEIGHT, 0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT, ctx.vis->visual_id, mask1, values1);
    if (check_void_cookie(c)) die("Can't create window");

    /* Create graphics context used only to present
     * window contents from backing pixmap */

    ctx.gc = xcb_generate_id(ctx.con);
    uint32_t mask2 = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values2[3] = { BG_COLOR, BG_COLOR, 0 };

    c = xcb_create_gc_checked(ctx.con, ctx.gc, ctx.wid, mask2, values2);
    if (check_void_cookie(c)) die("Can't create window");

    /* Set default windwow properties */

    uint32_t pid = getpid();
    xcb_change_property(ctx.con, XCB_PROP_MODE_REPLACE, ctx.wid, ctx.atom._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(ctx.con, XCB_PROP_MODE_REPLACE, ctx.wid, ctx.atom.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 1, &ctx.atom.WM_DELETE_WINDOW);
    xcb_change_property(ctx.con, XCB_PROP_MODE_REPLACE, ctx.wid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof WINDOW_CLASS - 1, WINDOW_CLASS);
    xcb_change_property(ctx.con, XCB_PROP_MODE_REPLACE, ctx.wid, ctx.atom._NET_WM_NAME, ctx.atom.UTF8_STRING, 8, sizeof WINDOW_TITLE - 1, WINDOW_TITLE);
    xcb_change_property(ctx.con, XCB_PROP_MODE_REPLACE, ctx.wid, ctx.atom._NET_WM_ICON_NAME, ctx.atom.UTF8_STRING, 8, sizeof WINDOW_TITLE - 1, WINDOW_TITLE);

    /* Create MIT-SHM backing pixmap */

    ctx.im = create_shm_image(WINDOW_WIDTH, WINDOW_HEIGHT);
    if (!ctx.im.data) die("Can't allocate image");

    /* And clear it with background color */
    image_draw_rect(ctx.im, (struct rect){0, 0, ctx.im.width, ctx.im.height}, BG_COLOR);

    /* Finally, map window */
    xcb_map_window(ctx.con, ctx.wid);
    xcb_flush(ctx.con);
}

static void configure_keyboard(void) {
    const struct xcb_setup_t *setup = xcb_get_setup(ctx.con);

    xcb_get_keyboard_mapping_cookie_t res = xcb_get_keyboard_mapping(ctx.con, setup->min_keycode, setup->max_keycode - setup->min_keycode + 1);
    xcb_generic_error_t *err = NULL;
    ctx.keymap = xcb_get_keyboard_mapping_reply(ctx.con, res, &err);
    if (err) {
        free(err);
        die("Can't get keyboard mapping: major=%d minor=%d error=%d", err->major_code, err->minor_code, err->error_code);
    }
}


static void init_context(void) {
    int screenp;
    ctx.con = xcb_connect(NULL, &screenp);

    xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(ctx.con));
    for (; sit.rem; xcb_screen_next(&sit))
        if (!screenp--) break;
    if (screenp != -1) {
        xcb_disconnect(ctx.con);
        die("Can't find default screen");
    }
    ctx.screen = sit.data;

    /* X11 color handling is complicated so we just
     * find apperopriate 32-bit visual and use TrueColor */

    xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(ctx.screen);
    for (; dit.rem; xcb_depth_next(&dit))
        if (dit.data->depth == TRUE_COLOR_ALPHA_DEPTH) break;
    if (dit.data->depth != TRUE_COLOR_ALPHA_DEPTH) {
        xcb_disconnect(ctx.con);
        die("Can't get 32-bit visual");
    }

    xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
    for (; vit.rem; xcb_visualtype_next(&vit))
        if (vit.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR &&
            vit.data->red_mask == 0xFF0000) break;

    if (vit.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR) {
        xcb_disconnect(ctx.con);
        die("Can't get 32-bit visual");
    }

    ctx.vis = vit.data;

    ctx.mid = xcb_generate_id(ctx.con);
    xcb_void_cookie_t c = xcb_create_colormap_checked(ctx.con, XCB_COLORMAP_ALLOC_NONE,
                                       ctx.mid, ctx.screen->root, ctx.vis->visual_id);
    if (check_void_cookie(c)) {
        xcb_disconnect(ctx.con);
        die("Can't create colormap");
    }

    // That's kind of hack
    // Try guessing if DISPLAY refers to localhost
    // This is required since MIT-SHM is only supported on local displays

    char *display = getenv("DISPLAY");
    const char *local[] = { "localhost:", "127.0.0.1:", "unix:", };
    bool localhost = display[0] == ':';
    for (size_t i = 0; !localhost && i < sizeof(local)/sizeof(*local); i++)
        localhost = local[i] == strstr(display, local[i]);

    if (localhost) {
        xcb_shm_query_version_cookie_t q = xcb_shm_query_version(ctx.con);
        xcb_generic_error_t *er = NULL;
        xcb_shm_query_version_reply_t *qr = xcb_shm_query_version_reply(ctx.con, q, &er);
        if (er) free(er);

        if (qr) {
            ctx.has_shm_pixmaps = qr->shared_pixmaps &&
                    qr->pixmap_format == XCB_IMAGE_FORMAT_Z_PIXMAP;
            free(qr);
        }
        if (!(ctx.has_shm = qr && !er)) {
            warn("MIT-SHM is not available");
        }
    }

    // Intern&cache all used atoms

    ctx.atom._NET_WM_PID = intern_atom("_NET_WM_PID");
    ctx.atom._NET_WM_NAME = intern_atom("_NET_WM_NAME");
    ctx.atom._NET_WM_ICON_NAME = intern_atom("_NET_WM_ICON_NAME");
    ctx.atom.WM_DELETE_WINDOW = intern_atom("WM_DELETE_WINDOW");
    ctx.atom.WM_PROTOCOLS = intern_atom("WM_PROTOCOLS");
    ctx.atom.UTF8_STRING = intern_atom("UTF8_STRING");


    // Find and save DPI, it might become useful later

    int32_t dpi = -1;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(ctx.con));
    for (; it.rem; xcb_screen_next(&it))
        if (it.data) dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    if (dpi > 0) ctx.dpi = dpi;

    // To reduce number of dependencied
    // XKB/xkbcommon is not used is this program
    // so we need to obtain keycode-to-keysym
    // mapping manually using core X11 protocol

    configure_keyboard();

    // Finall let's create window

    create_window();
}

static void free_context(void) {
    if (ctx.wid) {
        xcb_unmap_window(ctx.con, ctx.wid);

        if (ctx.has_shm)
            xcb_shm_detach(ctx.con, ctx.shm_seg);
        if (ctx.has_shm_pixmaps)
            xcb_free_pixmap(ctx.con, ctx.shm_pixmap);
        if (ctx.im.data)
            free_image(&ctx.im);

        xcb_free_gc(ctx.con, ctx.gc);
        xcb_destroy_window(ctx.con, ctx.wid);
    }

    free(ctx.keymap);

    xcb_disconnect(ctx.con);
}

enum modifier_mask {
    mask_shift = 1 << 0,
    mask_lock = 1 << 1,
    mask_control = 1 << 2,
    mask_mod_1 = 1 << 3, /* Alt */
    mask_mod_2 = 1 << 4, /* Numlock */
    mask_mod_3 = 1 << 5,
    mask_mod_4 = 1 << 6, /* Super */
    mask_mod_5 = 1 << 7, /* Group */
};

static xcb_keysym_t get_keysym(xcb_keycode_t kc, uint16_t state) {
    // Since we are not using XKB (xkbcommon) and only
    // core keyboard, we need to translate keycodes to keysyms
    // manually (although we might just want to the first one for consistency)

    xcb_keysym_t *ksyms = xcb_get_keyboard_mapping_keysyms(ctx.keymap);
    size_t ksym_per_kc = ctx.keymap->keysyms_per_keycode;

    xcb_keysym_t *entry = &ksyms[ksym_per_kc * (kc - xcb_get_setup(ctx.con)->min_keycode)];
    bool group = ksym_per_kc >= 3 && state & mask_mod_5 && entry[2];
    bool shift = ksym_per_kc >= 2 && state & mask_shift && entry[1];

    return entry[2*group + shift];
}

static void handle_key(xcb_keycode_t kc, uint16_t state, bool pressed) {
    xcb_keysym_t ksym = get_keysym(kc, state);
    switch (ksym) {
    case XK_w: ctx.keys.forward = pressed; break;
    case XK_s: ctx.keys.backward = pressed; break;
    case XK_a: ctx.keys.left = pressed; break;
    case XK_d: ctx.keys.right = pressed; break;
    case XK_Escape: ctx.want_exit = 1; break;
    }
}

static void run(void) {
    struct pollfd pfd = {
        .fd = xcb_get_file_descriptor(ctx.con),
        .events = POLLIN | POLLHUP,
    };

    for (int64_t next_timeout = SEC; !ctx.want_exit && !xcb_connection_has_error(ctx.con);) {
        struct timespec ts = {next_timeout / SEC, next_timeout % SEC};
        if (ppoll(&pfd, 1, &ts, NULL) < 0 && errno != EINTR)
            die("Poll error: %s", strerror(errno));

        if (pfd.revents & POLLIN) {
            for (xcb_generic_event_t *event; (event = xcb_poll_for_event(ctx.con)); free(event)) {
                switch (event->response_type &= 0x7f) {
                case XCB_EXPOSE:{
                    ctx.force_redraw = 1;
                    break;
                }
                case XCB_CONFIGURE_NOTIFY:{
                    xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
                    if (ev->width != ctx.im.width || ev->height != ctx.im.height) {
                        struct image new = create_shm_image(ev->width, ev->height);
                        SWAP(ctx.im, new);
                        free_image(&new);
                        ctx.force_redraw = 1;
                    }
                    break;
                }
                case XCB_KEY_PRESS:
                case XCB_KEY_RELEASE: {
                    xcb_key_release_event_t *ev = (xcb_key_release_event_t*)event;
                    handle_key(ev->detail, ev->state, ev->response_type == XCB_KEY_PRESS);
                    break;
                }
                case XCB_FOCUS_IN:
                case XCB_FOCUS_OUT:{
                    ctx.focused = event->response_type == XCB_FOCUS_IN;
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)event;
                    ctx.want_exit |= ev->format == 32 && ev->data.data32[0] == ctx.atom.WM_DELETE_WINDOW;
                    break;
                }
                case XCB_UNMAP_NOTIFY:
                case XCB_MAP_NOTIFY:
                    ctx.active = event->response_type == XCB_MAP_NOTIFY;
                    break;
                case XCB_VISIBILITY_NOTIFY: {
                    xcb_visibility_notify_event_t *ev = (xcb_visibility_notify_event_t*)event;
                    ctx.active = ev->state != XCB_VISIBILITY_FULLY_OBSCURED;
                    break;
                }
                case XCB_DESTROY_NOTIFY:
                case XCB_REPARENT_NOTIFY:
                   /* ignore */
                   break;
                case 0: {
                    xcb_generic_error_t *err = (xcb_generic_error_t*)event;
                    warn("[X11 Error] major=%"PRIu8", minor=%"PRIu16", error=%"PRIu8, err->major_code, err->minor_code, err->error_code);
                    break;
                }
                default:
                    warn("Unknown xcb event type: %02"PRIu8, event->response_type);
                }
            }
        }

        struct timespec cur;
        clock_gettime(CLOCK_TYPE, &cur);

        int64_t next_tick = (SEC / TPS) - TIMEDIFF(ctx.last_tick, cur);
        int64_t next_draw = (SEC / FPS) - TIMEDIFF(ctx.last_draw, cur);

        if (next_tick <= 10000LL) {
            tick(cur);
            next_tick = (SEC / TPS);
            ctx.last_tick = cur;
        }

        if (((next_draw <= 10000LL && ctx.want_redraw) || ctx.force_redraw) && ctx.active) {
            redraw();
            renderer_update((struct rect){0,0,ctx.im.width,ctx.im.height});
            next_draw = (SEC / FPS);
            ctx.last_draw = cur;
            ctx.want_redraw = 0;
            ctx.force_redraw = 0;
        }

        next_timeout = ctx.active ? MIN(next_tick, next_timeout) : next_tick;
        xcb_flush(ctx.con);
    }

    free_context();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Load locale from environment
    // (only CTYPE aspect to not ruin numbers
    // parsing of stdio functions...)
    setlocale(LC_CTYPE, "");

    init_context();

	ctx.image = create_image("test.png");

    run();

    free_image(&ctx.image);

    return EXIT_SUCCESS;
}
