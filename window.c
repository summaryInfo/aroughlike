/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _GNU_SOURCE

#include "context.h"
#include "image.h"
#include "keys.h"
#include "util.h"
#include "worker.h"

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
#include <sys/un.h>
#include <unistd.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

struct context {
    xcb_connection_t *con;
    xcb_screen_t *screen;
    xcb_colormap_t mid;
    xcb_visualtype_t *vis;

    xcb_window_t wid;
    xcb_gcontext_t gc;

    xcb_shm_seg_t shm_seg;
    xcb_pixmap_t shm_pixmap;

    bool focused : 1;
    bool active : 1;
    bool force_redraw : 1;
    bool has_shm : 1;
    bool has_shm_pixmaps : 1;

    struct atom_ {
        xcb_atom_t _NET_WM_PID;
        xcb_atom_t _NET_WM_NAME;
        xcb_atom_t _NET_WM_ICON_NAME;
        xcb_atom_t WM_DELETE_WINDOW;
        xcb_atom_t WM_PROTOCOLS;
        xcb_atom_t UTF8_STRING;
    } atom;

    xcb_get_keyboard_mapping_reply_t *keymap;
    bool en_group;
};

static struct context ctx;

struct scale scale;
struct image backbuf;
bool want_exit;

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

static void renderer_update(struct rect rect) {
    /* Copy back buffer to front buffer using
     * fastest method available.
     *     - MIT-SHM shared pixmap
     *     - MIT-SHM shared buffer
     *     - Core X11 PutImage request
     *
     * Double buffering in X11 is weird, so
     * like everyone normally do, we just draw to
     * one pixmap and then copy that pixmap
     * to the game window
     */

    size_t stride = (backbuf.width + 3) & ~3;

    if (ctx.has_shm_pixmaps) {
        xcb_copy_area(ctx.con, ctx.shm_pixmap, ctx.wid, ctx.gc, rect.x, rect.y, rect.x, rect.y, rect.width, rect.height);
    } else if (ctx.has_shm) {
        xcb_shm_put_image(ctx.con, ctx.wid, ctx.gc, stride, backbuf.height, rect.x, rect.y,
                          rect.width, rect.height, rect.x, rect.y, 32, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, ctx.shm_seg, 0);
    } else {
        xcb_put_image(ctx.con, XCB_IMAGE_FORMAT_Z_PIXMAP, ctx.wid, ctx.gc, stride, rect.height,
                      0, rect.y, 0, 32, rect.height * stride * sizeof(color_t), (const uint8_t *)(backbuf.data+rect.y*stride));
    }
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

static void resize_mitshm_image(int32_t width, int32_t height) {
    free_image(&backbuf);

    backbuf = ctx.has_shm ? create_shm_image(width, height) : create_image(width, height);
    if (!backbuf.data)
        die("Can't create MIT-SHM image of size %dx%d", width, height);

    size_t stride = (width + 3) & ~3;

    xcb_void_cookie_t c;
    if (!ctx.shm_seg) {
        ctx.shm_seg = xcb_generate_id(ctx.con);
    } else {
        if (ctx.has_shm_pixmaps && ctx.shm_pixmap)
            xcb_free_pixmap(ctx.con, ctx.shm_pixmap);
        xcb_shm_detach(ctx.con, ctx.shm_seg);
    }

    c = xcb_shm_attach_fd_checked(ctx.con, ctx.shm_seg, dup(backbuf.shmid), 0);
    if (check_void_cookie(c))
        die("Can't attach MIT-SHM image of size %dx%d", width, height);

    if (ctx.has_shm_pixmaps) {
        if (!ctx.shm_pixmap)
            ctx.shm_pixmap = xcb_generate_id(ctx.con);
        c = xcb_shm_create_pixmap(ctx.con, ctx.shm_pixmap,
                ctx.wid, stride, height, 32, ctx.shm_seg, 0);
        if (check_void_cookie(c))
            die("Can't attach MIT-SHM shared pixmap of size %dx%d", width, height);
    }
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
    resize_mitshm_image(WINDOW_WIDTH, WINDOW_HEIGHT);

    /* And clear it with background color */
    image_queue_fill(backbuf, (struct rect){0, 0, backbuf.width, backbuf.height}, BG_COLOR);
    drain_work();

    /* Finally, map window */
    xcb_map_window(ctx.con, ctx.wid);
}

static void configure_keyboard(void) {
    if (ctx.keymap) free(ctx.keymap);
    const struct xcb_setup_t *setup = xcb_get_setup(ctx.con);

    xcb_get_keyboard_mapping_cookie_t res = xcb_get_keyboard_mapping(ctx.con, setup->min_keycode, setup->max_keycode - setup->min_keycode + 1);
    xcb_generic_error_t *err = NULL;
    ctx.keymap = xcb_get_keyboard_mapping_reply(ctx.con, res, &err);
    if (err) {
        free(err);
        die("Can't get keyboard mapping: major=%d minor=%d error=%d", err->major_code, err->minor_code, err->error_code);
    }

    /* We need to find ASCII layout in order to implement
     * layout independent controls */

    xcb_keysym_t *ksyms = xcb_get_keyboard_mapping_keysyms(ctx.keymap);
    size_t ksym_per_kc = ctx.keymap->keysyms_per_keycode;
    size_t minkc = xcb_get_setup(ctx.con)->max_keycode;
    size_t maxkc = xcb_get_setup(ctx.con)->min_keycode;
    for (size_t i = minkc; i < maxkc; i++) {
        for (size_t j = 0; j < MIN(ksym_per_kc, 4); j++) {
            uint32_t ks = ksyms[i*ksym_per_kc+j];
            if ((ks >= 'A' && ks <= 'Z') || (ks >= 'a' && ks <= 'z')) {
                ctx.en_group = j/2;
                return;
            }
        }
    }

}

xcb_keysym_t get_keysym(xcb_keycode_t kc, uint32_t state) {
    /* Since we are not using XKB (xkbcommon) and only
     * core keyboard, we need to translate keycodes to keysyms
     * manually (although we might just want to the first one for consistency) */

    xcb_keysym_t *ksyms = xcb_get_keyboard_mapping_keysyms(ctx.keymap);
    size_t ksym_per_kc = ctx.keymap->keysyms_per_keycode;

    xcb_keysym_t *entry = &ksyms[ksym_per_kc * (kc - xcb_get_setup(ctx.con)->min_keycode)];

    // Use ASCII layout for decoding
    bool group = ctx.en_group; //ksym_per_kc >= 3 && state & mask_mod_5 && entry[2];
    bool shift = ksym_per_kc >= 2 && state & mask_shift && entry[1];

    return entry[2*group + shift];
}

static void init_scale(void) {
    int32_t dpi = -1;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(ctx.con));
    for (; it.rem; xcb_screen_next(&it))
        if (it.data) dpi = MAX(dpi, (it.data->width_in_pixels * 25.4)/it.data->width_in_millimeters);
    scale.dpi = dpi > 0 ? dpi : 96;
    scale.map = MAX(1, dpi/24);
    scale.interface = MAX(1, dpi/32);
}

static void init_context(void) {
    int screenp;
    ctx.con = xcb_connect(NULL, &screenp);
    backbuf.shmid = -1;

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

    /* That's kind of hack
     * Try guessing if DISPLAY refers to localhost
     * This is required since MIT-SHM is only supported on local displays */

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

    /* Intern&cache all used atoms */

    ctx.atom._NET_WM_PID = intern_atom("_NET_WM_PID");
    ctx.atom._NET_WM_NAME = intern_atom("_NET_WM_NAME");
    ctx.atom._NET_WM_ICON_NAME = intern_atom("_NET_WM_ICON_NAME");
    ctx.atom.WM_DELETE_WINDOW = intern_atom("WM_DELETE_WINDOW");
    ctx.atom.WM_PROTOCOLS = intern_atom("WM_PROTOCOLS");
    ctx.atom.UTF8_STRING = intern_atom("UTF8_STRING");


    init_scale();

    /* To reduce number of dependencied
     * XKB/xkbcommon is not used is this program
     * so we need to obtain keycode-to-keysym
     * mapping manually using core X11 protocol */

    configure_keyboard();

    /* Finall let's create window */

    create_window();
}

static void free_context(void) {
    if (ctx.wid) {
        xcb_unmap_window(ctx.con, ctx.wid);

        if (ctx.has_shm)
            xcb_shm_detach(ctx.con, ctx.shm_seg);
        if (ctx.has_shm_pixmaps)
            xcb_free_pixmap(ctx.con, ctx.shm_pixmap);
        if (backbuf.data)
            free_image(&backbuf);

        xcb_free_gc(ctx.con, ctx.gc);
        xcb_destroy_window(ctx.con, ctx.wid);
    }

    free(ctx.keymap);

    xcb_disconnect(ctx.con);
}

static void run(void) {
    struct pollfd pfd = {
        .fd = xcb_get_file_descriptor(ctx.con),
        .events = POLLIN | POLLHUP,
    };

    for (int64_t next_timeout = 0; !want_exit && !xcb_connection_has_error(ctx.con);) {
        struct timespec ts = {next_timeout / SEC, next_timeout % SEC};
        if (ppoll(&pfd, 1, &ts, NULL) < 0 && errno != EINTR)
            die("Poll error: %s", strerror(errno));

        for (xcb_generic_event_t *event, *nextev = NULL; nextev || (event = xcb_poll_for_event(ctx.con)); free(event)) {
            if (nextev) event = nextev, nextev = NULL;
            switch (event->response_type &= 0x7F) {
            case XCB_EXPOSE:
                ctx.force_redraw = 1;
                break;
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
                if (ev->width != backbuf.width || ev->height != backbuf.height) {
                    resize_mitshm_image(ev->width, ev->height);
                    ctx.force_redraw = 1;
                }
                break;
            }
            case XCB_KEY_RELEASE:
                /* Skip key repeats */
                if ((nextev = xcb_poll_for_queued_event(ctx.con)) &&
                        (nextev->response_type &= 0x7F) == XCB_KEY_PRESS &&
                        event->full_sequence == nextev->full_sequence) {
                    free(nextev);
                    nextev = NULL;
                    continue;
                }
                // fallthrough
            case XCB_KEY_PRESS: {
                xcb_key_release_event_t *ev = (xcb_key_release_event_t*)event;
                handle_key(ev->detail, ev->state, ev->response_type == XCB_KEY_PRESS);
                break;
            }
            case XCB_FOCUS_IN:
            case XCB_FOCUS_OUT: {
                ctx.focused = event->response_type == XCB_FOCUS_IN;
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                xcb_client_message_event_t *ev = (xcb_client_message_event_t*)event;
                want_exit |= ev->format == 32 && ev->data.data32[0] == ctx.atom.WM_DELETE_WINDOW;
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
            case XCB_MAPPING_NOTIFY: {
                xcb_mapping_notify_event_t *ev = (xcb_mapping_notify_event_t*)event;
                if (ev->request == XCB_MAPPING_KEYBOARD) configure_keyboard();
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

        struct timespec cur;
        clock_gettime(CLOCK_TYPE, &cur);
        next_timeout = tick(cur);

        if (redraw(cur, ctx.force_redraw)) {
            renderer_update((struct rect){0, 0, backbuf.width, backbuf.height});
            ctx.force_redraw = 0;
        }

        xcb_flush(ctx.con);
    }

}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Load locale from environment
     * (only CTYPE aspect to not ruin numbers
     * parsing of stdio functions...) */
    setlocale(LC_CTYPE, "");

    init_workers();
    init_context();
    init();

    run();

    cleanup();
    free_context();
    fini_workers(1);

    return EXIT_SUCCESS;
}
