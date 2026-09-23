#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include "linux-dmabuf-client.h"
#include "xdg-shell-client.h"

static int failures;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "ASSERT %s\n", msg);
        failures++;
    } else {
        fprintf(stderr, "OK %s\n", msg);
    }
}

static void fill(uint8_t *base, uint32_t offset, int w, int h, int stride, uint32_t format, int tag) {
    for (int y = 0; y < h; y++) {
        uint8_t *row = base + offset + (uint32_t)y * (uint32_t)stride;
        for (int x = 0; x < w; x++) {
            row[x * 4 + 0] = (uint8_t)((x + tag) & 255);
            row[x * 4 + 1] = (uint8_t)(y & 255);
            row[x * 4 + 2] = (uint8_t)((x * 3 + y) & 255);
            row[x * 4 + 3] = format == DRM_FORMAT_ARGB8888 ? (uint8_t)((x + y * 2) & 255) : 0;
        }
    }
}

static int row_matches(const uint8_t *got, int w, int y, uint32_t format, int tag) {
    for (int x = 0; x < w; x++) {
        uint8_t exp[4] = {
            (uint8_t)((x + tag) & 255),
            (uint8_t)(y & 255),
            (uint8_t)((x * 3 + y) & 255),
            format == DRM_FORMAT_ARGB8888 ? (uint8_t)((x + y * 2) & 255) : 0,
        };
        int n = format == DRM_FORMAT_ARGB8888 ? 4 : 3;
        if (memcmp(got + x * 4, exp, (size_t)n) != 0)
            return 0;
    }
    return 1;
}

struct got_frame {
    int w, h, stride;
    uint32_t format;
    uint8_t *data;
};

static int read_frame(int fd, struct got_frame *out) {
    out->data = NULL;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 2000) <= 0)
        return -1;
    char hdr[128];
    size_t n = 0;
    while (n < sizeof hdr - 1) {
        char c;
        if (read(fd, &c, 1) != 1)
            return -1;
        hdr[n++] = c;
        if (c == '\n')
            break;
    }
    hdr[n] = 0;
    if (sscanf(hdr, "FRAME %d %d %d %u", &out->w, &out->h, &out->stride, &out->format) != 4)
        return -1;
    size_t bytes = (size_t)out->stride * (size_t)out->h;
    out->data = malloc(bytes);
    size_t got = 0;
    while (got < bytes) {
        ssize_t r = read(fd, out->data + got, bytes - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

struct client {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *xdg;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int32_t conf_w, conf_h;
    int configured;
    int entered;
    int motion_x, motion_y;
    int button, button_state;
    int key, key_state;
    int mods;
    int saw_super;
    int saw_return;
    int saw_mod4;
    int axis120;
    int closed;
    int releases;
    int protocol_error;
    struct gbm_device *gbm;
    int drm;
};

static void xdg_ping(void *d, struct xdg_wm_base *x, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(x, serial);
}
static const struct xdg_wm_base_listener xdg_listener = {.ping = xdg_ping};
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void)t;
    (void)states;
    struct client *c = d;
    c->conf_w = w;
    c->conf_h = h;
}
static void top_close(void *d, struct xdg_toplevel *t) {
    (void)t;
    ((struct client *)d)->closed = 1;
}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {
    (void)d;
    (void)t;
    (void)w;
    (void)h;
}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *caps) {
    (void)d;
    (void)t;
    (void)caps;
}
static const struct xdg_toplevel_listener top_listener = {
    .configure = top_conf,
    .close = top_close,
    .configure_bounds = top_bounds,
    .wm_capabilities = top_caps,
};
static void xs_conf(void *d, struct xdg_surface *s, uint32_t serial) {
    struct client *c = d;
    xdg_surface_ack_configure(s, serial);
    c->configured = 1;
}
static const struct xdg_surface_listener xs_listener = {.configure = xs_conf};
static void pent(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    (void)serial;
    (void)s;
    (void)x;
    (void)y;
    ((struct client *)d)->entered = 1;
}
static void pleave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void)d;
    (void)p;
    (void)serial;
    (void)s;
}
static void pmove(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    (void)t;
    struct client *c = d;
    c->motion_x = wl_fixed_to_int(x);
    c->motion_y = wl_fixed_to_int(y);
}
static void pbutton(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state) {
    (void)p;
    (void)serial;
    (void)t;
    struct client *c = d;
    c->button = (int)b;
    c->button_state = (int)state;
}
static void paxis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
    (void)d;
    (void)p;
    (void)t;
    (void)axis;
    (void)v;
}
static void pframe(void *d, struct wl_pointer *p) {
    (void)d;
    (void)p;
}
static void psource(void *d, struct wl_pointer *p, uint32_t source) {
    (void)d;
    (void)p;
    (void)source;
}
static void pstop(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis) {
    (void)d;
    (void)p;
    (void)t;
    (void)axis;
}
static void pdisc(void *d, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
    (void)d;
    (void)p;
    (void)axis;
    (void)discrete;
    failures++;
    fprintf(stderr, "ASSERT unexpected axis_discrete\n");
}
static void p120(void *d, struct wl_pointer *p, uint32_t axis, int32_t value) {
    (void)p;
    (void)axis;
    ((struct client *)d)->axis120 = value;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pent,
    .leave = pleave,
    .motion = pmove,
    .button = pbutton,
    .axis = paxis,
    .frame = pframe,
    .axis_source = psource,
    .axis_stop = pstop,
    .axis_discrete = pdisc,
    .axis_value120 = p120,
};
static void kkmap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t size) {
    (void)d;
    (void)k;
    (void)f;
    (void)size;
    close(fd);
}
static void kenter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)d;
    (void)k;
    (void)serial;
    (void)s;
    (void)keys;
}
static void kleave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    (void)d;
    (void)k;
    (void)serial;
    (void)s;
}
static void kkey(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) {
    (void)k;
    (void)serial;
    (void)t;
    struct client *c = d;
    c->key = (int)key;
    c->key_state = (int)state;
    if (key == 125 && state == 1)
        c->saw_super = 1;
    if (key == 28 && state == 1)
        c->saw_return = 1;
}
static void kmods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    (void)k;
    (void)serial;
    (void)latched;
    (void)locked;
    (void)group;
    struct client *c = d;
    c->mods = (int)depressed;
    if (depressed & 64)
        c->saw_mod4 = 1;
}
static void krep(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d;
    (void)k;
    (void)rate;
    (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kkmap,
    .enter = kenter,
    .leave = kleave,
    .key = kkey,
    .modifiers = kmods,
    .repeat_info = krep,
};
static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    struct client *c = d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !c->pointer) {
        c->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(c->pointer, &pointer_listener, c);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !c->keyboard) {
        c->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(c->keyboard, &keyboard_listener, c);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {
    (void)d;
    (void)s;
    (void)name;
}
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

static void fb_main(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev) {
    (void)d;
    (void)f;
    (void)dev;
}
static void fb_table(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, int32_t fd, uint32_t size) {
    (void)d;
    (void)f;
    (void)size;
    close(fd);
}
static void fb_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) {
    (void)d;
    (void)f;
}
static void fb_tdone(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) {
    (void)d;
    (void)f;
}
static void fb_tdev(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev) {
    (void)d;
    (void)f;
    (void)dev;
}
static void fb_flags(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, uint32_t flags) {
    (void)d;
    (void)f;
    (void)flags;
}
static void fb_fmts(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *idx) {
    (void)d;
    (void)f;
    (void)idx;
}
static const struct zwp_linux_dmabuf_feedback_v1_listener fb_listener = {
    .done = fb_done,
    .format_table = fb_table,
    .main_device = fb_main,
    .tranche_done = fb_tdone,
    .tranche_target_device = fb_tdev,
    .tranche_formats = fb_fmts,
    .tranche_flags = fb_flags,
};

static void registry_global(void *d, struct wl_registry *reg, uint32_t id, const char *iface, uint32_t ver) {
    struct client *c = d;
    if (strcmp(iface, "wl_compositor") == 0)
        c->comp = wl_registry_bind(reg, id, &wl_compositor_interface, 6);
    else if (strcmp(iface, "wl_shm") == 0)
        c->shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    else if (strcmp(iface, "wl_seat") == 0) {
        c->seat = wl_registry_bind(reg, id, &wl_seat_interface, 9);
        wl_seat_add_listener(c->seat, &seat_listener, c);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        c->xdg = wl_registry_bind(reg, id, &xdg_wm_base_interface, 6);
        xdg_wm_base_add_listener(c->xdg, &xdg_listener, c);
    } else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0)
        c->dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, 4);
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void)d;
    (void)r;
    (void)id;
}
static const struct wl_registry_listener reg_listener = {.global = registry_global, .global_remove = registry_remove};

static int open_vkms(void) {
    drmDevicePtr devices[16];
    int count = drmGetDevices2(0, devices, 16);
    if (count < 0)
        return -1;
    for (int i = 0; i < count; i++) {
        if (!(devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;
        const char *primary = devices[i]->nodes[DRM_NODE_PRIMARY];
        const char *base = strrchr(primary, '/');
        char sys[256];
        snprintf(sys, sizeof sys, "/sys/class/drm/%s", base + 1);
        char *resolved = realpath(sys, NULL);
        int match = resolved && strstr(resolved, "/vkms/");
        free(resolved);
        if (!match)
            continue;
        int fd = open(primary, O_RDWR | O_CLOEXEC);
        drmFreeDevices(devices, count);
        return fd;
    }
    drmFreeDevices(devices, count);
    return -1;
}

static void buf_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    ((struct client *)data)->releases++;
}
static const struct wl_buffer_listener buf_listener = {.release = buf_release};

static struct wl_buffer *make_buffer(struct client *c, int w, int h, uint32_t format, uint64_t modifier, uint32_t plane, uint32_t use_offset, int tag, int *out_stride) {
    if (!c->gbm)
        return NULL;
    int alloc_h = use_offset ? h + 1 : h;
    struct gbm_bo *bo = gbm_bo_create(c->gbm, w, alloc_h, format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo)
        return NULL;
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t map_stride = 0;
    void *map_data = NULL;
    void *map = gbm_bo_map(bo, 0, 0, w, alloc_h, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
    if (!map)
        return NULL;
    uint32_t offset = use_offset ? map_stride : 0;
    fill(map, offset, w, h, (int)map_stride, format, tag);
    gbm_bo_unmap(bo, map_data);
    int fd = gbm_bo_get_fd(bo);
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(c->dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, plane, offset, map_stride, (uint32_t)(modifier >> 32), (uint32_t)modifier);
    struct wl_buffer *buffer = zwp_linux_buffer_params_v1_create_immed(params, w, h, format, 0);
    if (buffer)
        wl_buffer_add_listener(buffer, &buf_listener, c);
    if (out_stride)
        *out_stride = (int)map_stride;
    close(fd);
    return buffer;
}

static int connect_client(struct client *c, const char *name) {
    memset(c, 0, sizeof *c);
    setenv("WAYLAND_DISPLAY", name, 1);
    unsetenv("WAYLAND_SOCKET");
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy)
        return -1;
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &reg_listener, c);
    wl_display_roundtrip(c->dpy);
    c->drm = open_vkms();
    c->gbm = c->drm >= 0 ? gbm_create_device(c->drm) : NULL;
    struct zwp_linux_dmabuf_feedback_v1 *fb = zwp_linux_dmabuf_v1_get_default_feedback(c->dmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(fb, &fb_listener, c);
    wl_display_roundtrip(c->dpy);
    c->surface = wl_compositor_create_surface(c->comp);
    c->xdg_surface = xdg_wm_base_get_xdg_surface(c->xdg, c->surface);
    xdg_surface_add_listener(c->xdg_surface, &xs_listener, c);
    c->toplevel = xdg_surface_get_toplevel(c->xdg_surface);
    xdg_toplevel_add_listener(c->toplevel, &top_listener, c);
    xdg_toplevel_set_title(c->toplevel, "suite");
    wl_surface_commit(c->surface);
    wl_display_roundtrip(c->dpy);
    return c->configured ? 0 : -1;
}

static int submit(struct client *c, struct wl_buffer *buffer) {
    wl_surface_attach(c->surface, buffer, 0, 0);
    wl_surface_damage_buffer(c->surface, 0, 0, c->conf_w > 0 ? c->conf_w : 64, c->conf_h > 0 ? c->conf_h : 64);
    wl_surface_set_input_region(c->surface, NULL);
    struct wl_callback *cb = wl_surface_frame(c->surface);
    (void)cb;
    wl_surface_commit(c->surface);
    return wl_display_roundtrip(c->dpy);
}

static int spawn(const char *bin, char *const av[], int *in_fd, int *out_fd) {
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        close(in[1]);
        close(out[0]);
        execv(bin, av);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (in_fd)
        *in_fd = in[1];
    if (out_fd)
        *out_fd = out[0];
    return pid;
}

static int check_frame(struct got_frame *f, int w, int h, uint32_t drm_format, int tag, const char *name) {
    int ok = f->w == w && f->h == h;
    uint32_t shm = drm_format == DRM_FORMAT_ARGB8888 ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
    ok = ok && f->format == shm;
    for (int y = 0; ok && y < h; y++)
        ok = row_matches(f->data + (size_t)y * (size_t)f->stride, w, y, drm_format, tag);
    expect(ok, name);
    free(f->data);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: bridge-suite mock-parent wslg-bridge\n");
        return 2;
    }
    char parent_name[64], bridge_name[64];
    snprintf(parent_name, sizeof parent_name, "mockp-%d", getpid());
    snprintf(bridge_name, sizeof bridge_name, "br-%d", getpid());
    char *p_av[] = {argv[1], parent_name, NULL};
    int p_in = -1, p_out = -1;
    pid_t parent = spawn(argv[1], p_av, &p_in, &p_out);
    usleep(200000);
    setenv("WAYLAND_DISPLAY", parent_name, 1);
    char *b_av[] = {argv[2], "--listen", bridge_name, "--no-child", NULL};
    int b_in = -1, b_out = -1;
    pid_t bridge = spawn(argv[2], b_av, &b_in, &b_out);
    char listen[128] = {0};
    struct pollfd bp = {.fd = b_out, .events = POLLIN};
    expect(poll(&bp, 1, 2000) > 0, "bridge started");
    read(b_out, listen, sizeof listen - 1);
    expect(strncmp(listen, "LISTEN ", 7) == 0, "listen line");

    struct client c;
    expect(connect_client(&c, bridge_name) == 0, "client configured");
    int stride = 0;
    struct wl_buffer *buf = make_buffer(&c, 64, 64, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 1, &stride);
    expect(buf != NULL, "xrgb buffer");
    expect(stride >= 64 * 4, "gbm stride covers the row");
    submit(&c, buf);
    struct got_frame frame;
    expect(read_frame(p_out, &frame) == 0, "frame arrived");
    if (frame.data)
        check_frame(&frame, 64, 64, DRM_FORMAT_XRGB8888, 1, "xrgb pixels");

    struct wl_buffer *buf2 = make_buffer(&c, 64, 64, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 7, NULL);
    submit(&c, buf2);
    expect(read_frame(p_out, &frame) == 0, "second frame");
    if (frame.data)
        check_frame(&frame, 64, 64, DRM_FORMAT_XRGB8888, 7, "consecutive pixels");

    struct wl_buffer *abuf = make_buffer(&c, 32, 20, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, 0, 1, 3, NULL);
    submit(&c, abuf);
    expect(read_frame(p_out, &frame) == 0, "argb offset frame");
    if (frame.data)
        check_frame(&frame, 32, 20, DRM_FORMAT_ARGB8888, 3, "alpha and offset pixels");

    dprintf(p_in, "RESIZE 100 80\n");
    for (int i = 0; i < 10 && !(c.conf_w == 100 && c.conf_h == 80); i++) {
        usleep(50000);
        wl_display_roundtrip(c.dpy);
    }
    expect(c.conf_w == 100 && c.conf_h == 80, "resize configure");
    struct wl_buffer *rbuf = make_buffer(&c, 100, 80, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 4, NULL);
    submit(&c, rbuf);
    expect(read_frame(p_out, &frame) == 0, "resized frame");
    if (frame.data)
        check_frame(&frame, 100, 80, DRM_FORMAT_XRGB8888, 4, "resized pixels");

    dprintf(p_in, "PENTER\nMOTION 12 34\nBUTTON 272 1\nKENTER\nKEY 30 1\nMODS 4\nKEY 42 1\nMODS 1\nAXIS 0 2\n");
    fsync(p_in);
    usleep(100000);
    for (int i = 0; i < 8; i++)
        wl_display_roundtrip(c.dpy);
    expect(c.entered, "pointer enter");
    expect(c.motion_x == 12 && c.motion_y == 34, "pointer motion");
    expect(c.button == 272 && c.button_state == WL_POINTER_BUTTON_STATE_PRESSED, "button");
    expect(c.key == 42 && c.key_state == 1, "modifier key delivered");
    expect(c.mods == 1, "modifier after modifier key");
    expect(c.axis120 == 240, "axis_value120");

    kill(bridge, SIGUSR1);
    for (int i = 0; i < 20 && !(c.saw_super && c.saw_return && c.saw_mod4); i++) {
        usleep(50000);
        if (wl_display_get_error(c.dpy) == 0)
            wl_display_roundtrip(c.dpy);
    }
    expect(c.saw_super && c.saw_return && c.saw_mod4 && wl_display_get_error(c.dpy) == 0, "super+return on the bridge seat");

    if (c.pointer)
        wl_pointer_set_cursor(c.pointer, 0, NULL, 0, 0);
    struct wl_region *region = wl_compositor_create_region(c.comp);
    wl_region_add(region, 0, 0, 10, 10);
    wl_region_subtract(region, 1, 1, 2, 2);
    wl_surface_set_input_region(c.surface, region);
    wl_display_roundtrip(c.dpy);
    expect(wl_display_get_error(c.dpy) == 0, "cursor and region requests");
    wl_region_destroy(region);

    struct client extra;
    expect(connect_client(&extra, bridge_name) == 0, "diagnostic client");
    wl_display_disconnect(extra.dpy);
    usleep(100000);
    if (wl_display_get_error(c.dpy) == 0)
        wl_display_roundtrip(c.dpy);
    int extra_st = 0;
    expect(waitpid(bridge, &extra_st, WNOHANG) == 0, "diagnostic disconnect keeps the bridge");

    dprintf(p_in, "HOLD 1\n");
    fsync(p_in);
    int rel0 = c.releases;
    struct wl_buffer *held = make_buffer(&c, 16, 16, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 9, NULL);
    submit(&c, held);
    expect(read_frame(p_out, &frame) == 0, "held frame copied before parent release");
    if (frame.data)
        check_frame(&frame, 16, 16, DRM_FORMAT_XRGB8888, 9, "held pixels");
    expect(wl_display_get_error(c.dpy) == 0, "copy does not error the client");
    expect(c.releases == rel0 + 1, "dma-buf released after copy while parent holds");

    int rel1 = c.releases;
    submit(&c, held);
    expect(wl_display_get_error(c.dpy) == 0, "reuse after release is legal");
    expect(read_frame(p_out, &frame) == 0, "reused buffer frame");
    if (frame.data)
        check_frame(&frame, 16, 16, DRM_FORMAT_XRGB8888, 9, "reused pixels");
    expect(c.releases == rel1 + 1, "reused buffer released again");

    struct wl_buffer *mid = make_buffer(&c, 16, 16, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 10, NULL);
    submit(&c, mid);
    expect(read_frame(p_out, &frame) == 0, "third staging slot");
    if (frame.data)
        check_frame(&frame, 16, 16, DRM_FORMAT_XRGB8888, 10, "third slot pixels");

    int relq = c.releases;
    struct wl_buffer *queued = make_buffer(&c, 16, 16, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 11, NULL);
    submit(&c, queued);
    expect(wl_display_get_error(c.dpy) == 0, "backpressure is not a protocol error");
    expect(c.releases == relq, "queued buffer stays referenced");
    wl_surface_attach(c.surface, queued, 0, 0);
    wl_surface_commit(c.surface);
    wl_display_roundtrip(c.dpy);
    expect(wl_display_get_error(c.dpy) != 0, "premature reuse errors");

    wl_display_disconnect(c.dpy);
    usleep(200000);
    int st = 0;
    expect(waitpid(bridge, &st, WNOHANG) == bridge, "client disconnect stops bridge");
    bridge = -1;

    dprintf(p_in, "CLOSE\n");
    kill(parent, SIGTERM);
    parent = -1;

    char parent2[64], bridge2[64];
    snprintf(parent2, sizeof parent2, "mockp2-%d", getpid());
    snprintf(bridge2, sizeof bridge2, "br2-%d", getpid());
    char *p2[] = {argv[1], parent2, NULL};
    int p2_in = -1, p2_out = -1;
    pid_t parent_b = spawn(argv[1], p2, &p2_in, &p2_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent2, 1);
    char *b2[] = {argv[2], "--listen", bridge2, "--no-child", NULL};
    int b2_out = -1;
    pid_t bridge_b = spawn(argv[2], b2, NULL, &b2_out);
    char line2[128] = {0};
    struct pollfd bp2 = {.fd = b2_out, .events = POLLIN};
    if (poll(&bp2, 1, 2000) > 0)
        read(b2_out, line2, sizeof line2 - 1);
    struct client c2;
    if (connect_client(&c2, bridge2) == 0) {
        struct wl_buffer *bad = make_buffer(&c2, 16, 16, DRM_FORMAT_XRGB8888, 2, 0, 0, 1, NULL);
        if (bad) {
            wl_surface_attach(c2.surface, bad, 0, 0);
            wl_surface_commit(c2.surface);
            wl_display_roundtrip(c2.dpy);
        }
        expect(wl_display_get_error(c2.dpy) != 0, "bad modifier errors");
        struct client c3;
        /* plane error needs a live display; the connection above is now dead. */
        (void)c3;
    }
    dprintf(p2_in, "CLOSE\n");
    usleep(200000);
    for (int i = 0; i < 10 && !c2.closed; i++) {
        if (wl_display_get_error(c2.dpy) == 0)
            wl_display_roundtrip(c2.dpy);
        else
            break;
        usleep(50000);
    }
    int bst = 0;
    pid_t gotb = waitpid(bridge_b, &bst, WNOHANG);
    expect(c2.closed || gotb == bridge_b, "parent close stops the session");
    kill(bridge_b, SIGTERM);
    kill(parent_b, SIGTERM);
    waitpid(bridge_b, NULL, 0);
    waitpid(parent_b, NULL, 0);

    char parent3[64], bridge3[64];
    snprintf(parent3, sizeof parent3, "mockp3-%d", getpid());
    snprintf(bridge3, sizeof bridge3, "br3-%d", getpid());
    char *p3[] = {argv[1], parent3, NULL};
    int p3_in = -1, p3_out = -1;
    pid_t parent_c = spawn(argv[1], p3, &p3_in, &p3_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent3, 1);
    char *b3[] = {argv[2], "--listen", bridge3, "--no-child", NULL};
    int b3_out = -1;
    pid_t bridge_c = spawn(argv[2], b3, NULL, &b3_out);
    char line3[128] = {0};
    struct pollfd bp3 = {.fd = b3_out, .events = POLLIN};
    if (poll(&bp3, 1, 2000) > 0)
        read(b3_out, line3, sizeof line3 - 1);
    struct client c3 = {0};
    if (connect_client(&c3, bridge3) == 0) {
        struct wl_buffer *badp = make_buffer(&c3, 16, 16, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 2, 0, 1, NULL);
        if (badp) {
            wl_surface_attach(c3.surface, badp, 0, 0);
            wl_surface_commit(c3.surface);
            wl_display_roundtrip(c3.dpy);
        }
        expect(wl_display_get_error(c3.dpy) != 0, "bad plane errors");
    }
    kill(bridge_c, SIGTERM);
    kill(parent_c, SIGTERM);
    waitpid(bridge_c, NULL, 0);
    waitpid(parent_c, NULL, 0);

    char parent4[64], bridge4[64];
    snprintf(parent4, sizeof parent4, "mockp4-%d", getpid());
    snprintf(bridge4, sizeof bridge4, "br4-%d", getpid());
    char *p4[] = {argv[1], parent4, NULL};
    int p4_in = -1, p4_out = -1;
    pid_t parent_d = spawn(argv[1], p4, &p4_in, &p4_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent4, 1);
    char *b4[] = {argv[2], "--listen", bridge4, "--no-child", NULL};
    int b4_out = -1;
    pid_t bridge_d = spawn(argv[2], b4, NULL, &b4_out);
    char line4[128] = {0};
    struct pollfd bp4 = {.fd = b4_out, .events = POLLIN};
    if (poll(&bp4, 1, 2000) > 0)
        read(b4_out, line4, sizeof line4 - 1);
    struct client c4 = {0};
    if (connect_client(&c4, bridge4) == 0) {
        int fd = memfd_create("bounds", MFD_CLOEXEC);
        ftruncate(fd, 4096);
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(c4.dmabuf);
        zwp_linux_buffer_params_v1_add(params, fd, 0, 0, 4, 0, 0);
        zwp_linux_buffer_params_v1_create_immed(params, 32, 32, DRM_FORMAT_XRGB8888, 0);
        wl_display_roundtrip(c4.dpy);
        expect(wl_display_get_error(c4.dpy) != 0, "invalid bounds errors");
    }
    kill(bridge_d, SIGTERM);
    kill(parent_d, SIGTERM);
    waitpid(bridge_d, NULL, 0);
    waitpid(parent_d, NULL, 0);

    char parent5[64], bridge5[64];
    snprintf(parent5, sizeof parent5, "mockp5-%d", getpid());
    snprintf(bridge5, sizeof bridge5, "br5-%d", getpid());
    char *p5[] = {argv[1], parent5, NULL};
    int p5_in = -1, p5_out = -1;
    pid_t parent_e = spawn(argv[1], p5, &p5_in, &p5_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent5, 1);
    char *b5[] = {argv[2], "--listen", bridge5, "--no-child", NULL};
    int b5_out = -1;
    pid_t bridge_e = spawn(argv[2], b5, NULL, &b5_out);
    char line5[128] = {0};
    struct pollfd bp5 = {.fd = b5_out, .events = POLLIN};
    if (poll(&bp5, 1, 2000) > 0)
        read(b5_out, line5, sizeof line5 - 1);
    struct client c5 = {0};
    if (connect_client(&c5, bridge5) == 0) {
        int fd = memfd_create("notdma", MFD_CLOEXEC);
        ftruncate(fd, 65536);
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(c5.dmabuf);
        zwp_linux_buffer_params_v1_add(params, fd, 0, 0, 128, 0, 0);
        struct wl_buffer *bad = zwp_linux_buffer_params_v1_create_immed(params, 16, 16, DRM_FORMAT_XRGB8888, 0);
        wl_display_roundtrip(c5.dpy);
        if (bad && wl_display_get_error(c5.dpy) == 0) {
            wl_surface_attach(c5.surface, bad, 0, 0);
            wl_surface_commit(c5.surface);
            wl_display_roundtrip(c5.dpy);
        }
        expect(wl_display_get_error(c5.dpy) != 0, "sync failure errors");
    }
    kill(bridge_e, SIGTERM);
    kill(parent_e, SIGTERM);
    waitpid(bridge_e, NULL, 0);
    waitpid(parent_e, NULL, 0);

    setenv("WSLG_SYNC_EINTR", "1", 1);
    char parent6[64], bridge6[64];
    snprintf(parent6, sizeof parent6, "mockp6-%d", getpid());
    snprintf(bridge6, sizeof bridge6, "br6-%d", getpid());
    char *p6[] = {argv[1], parent6, NULL};
    int p6_in = -1, p6_out = -1;
    pid_t parent_f = spawn(argv[1], p6, &p6_in, &p6_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent6, 1);
    char *b6[] = {argv[2], "--listen", bridge6, "--no-child", NULL};
    int b6_out = -1;
    pid_t bridge_f = spawn(argv[2], b6, NULL, &b6_out);
    unsetenv("WSLG_SYNC_EINTR");
    char line6[128] = {0};
    struct pollfd bp6 = {.fd = b6_out, .events = POLLIN};
    if (poll(&bp6, 1, 2000) > 0)
        read(b6_out, line6, sizeof line6 - 1);
    struct client c6 = {0};
    if (connect_client(&c6, bridge6) == 0) {
        struct wl_buffer *okb = make_buffer(&c6, 16, 16, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 5, NULL);
        submit(&c6, okb);
        struct got_frame retry_frame = {0};
        expect(read_frame(p6_out, &retry_frame) == 0, "sync retry still publishes");
        if (retry_frame.data)
            check_frame(&retry_frame, 16, 16, DRM_FORMAT_XRGB8888, 5, "sync retry pixels");
        expect(wl_display_get_error(c6.dpy) == 0, "sync retry is not a protocol error");
    }
    kill(bridge_f, SIGTERM);
    kill(parent_f, SIGTERM);
    waitpid(bridge_f, NULL, 0);
    waitpid(parent_f, NULL, 0);

    char parent7[64], bridge7[64];
    snprintf(parent7, sizeof parent7, "mockp7-%d", getpid());
    snprintf(bridge7, sizeof bridge7, "br7-%d", getpid());
    char *p7[] = {argv[1], parent7, NULL};
    int p7_in = -1, p7_out = -1;
    pid_t parent_g = spawn(argv[1], p7, &p7_in, &p7_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent7, 1);
    char *b7[] = {argv[2], "--listen", bridge7, "--no-child", NULL};
    int b7_out = -1;
    pid_t bridge_g = spawn(argv[2], b7, NULL, &b7_out);
    char line7[128] = {0};
    struct pollfd bp7 = {.fd = b7_out, .events = POLLIN};
    if (poll(&bp7, 1, 2000) > 0)
        read(b7_out, line7, sizeof line7 - 1);
    struct client c7 = {0};
    int close_status = -1;
    if (connect_client(&c7, bridge7) == 0) {
        dprintf(p7_in, "CLOSE\n");
        fsync(p7_in);
        int st = 0;
        if (waitpid(bridge_g, &st, 0) == bridge_g) {
            bridge_g = -1;
            close_status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
    }
    expect(close_status == 0, "window close exits 0");
    if (bridge_g > 0)
        kill(bridge_g, SIGTERM);
    kill(parent_g, SIGTERM);
    if (bridge_g > 0)
        waitpid(bridge_g, NULL, 0);
    waitpid(parent_g, NULL, 0);

    char parent8[64], bridge8[64];
    snprintf(parent8, sizeof parent8, "mockp8-%d", getpid());
    snprintf(bridge8, sizeof bridge8, "br8-%d", getpid());
    char *p8[] = {argv[1], parent8, NULL};
    int p8_in = -1, p8_out = -1;
    pid_t parent_h = spawn(argv[1], p8, &p8_in, &p8_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent8, 1);
    char *b8[] = {argv[2], "--listen", bridge8, "--no-child", NULL};
    int b8_out = -1;
    pid_t bridge_h = spawn(argv[2], b8, NULL, &b8_out);
    char line8[128] = {0};
    struct pollfd bp8 = {.fd = b8_out, .events = POLLIN};
    if (poll(&bp8, 1, 2000) > 0)
        read(b8_out, line8, sizeof line8 - 1);
    struct client c8 = {0};
    int transport_status = -1;
    if (connect_client(&c8, bridge8) == 0) {
        kill(parent_h, SIGKILL);
        parent_h = -1;
        int st = 0;
        if (waitpid(bridge_h, &st, 0) == bridge_h) {
            bridge_h = -1;
            transport_status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
    }
    expect(transport_status == 2, "parent loss exits 2");
    if (bridge_h > 0)
        kill(bridge_h, SIGTERM);
    if (parent_h > 0)
        kill(parent_h, SIGTERM);
    if (bridge_h > 0)
        waitpid(bridge_h, NULL, 0);
    if (parent_h > 0)
        waitpid(parent_h, NULL, 0);

    pid_t bare = fork();
    if (bare == 0) {
        unsetenv("WAYLAND_DISPLAY");
        unsetenv("WAYLAND_SOCKET");
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execl("/home/omarchy/.local/bin/omarchy-wslg", "omarchy-wslg", (char *)NULL);
        _exit(127);
    }
    int bare_st = 0;
    waitpid(bare, &bare_st, 0);
    expect(WIFEXITED(bare_st) && WEXITSTATUS(bare_st) == 2, "unset WAYLAND_DISPLAY refuses");

    char parent9[64], bridge9[64];
    snprintf(parent9, sizeof parent9, "mockp9-%d", getpid());
    snprintf(bridge9, sizeof bridge9, "br9-%d", getpid());
    char *p9[] = {argv[1], parent9, NULL};
    int p9_in = -1, p9_out = -1;
    pid_t parent_i = spawn(argv[1], p9, &p9_in, &p9_out);
    usleep(100000);
    setenv("WAYLAND_DISPLAY", parent9, 1);
    char *b9[] = {argv[2], "--listen", bridge9, "--no-child", "--fullscreen", NULL};
    int b9_out = -1;
    pid_t bridge_i = spawn(argv[2], b9, NULL, &b9_out);
    char line9[128] = {0};
    struct pollfd pp9 = {.fd = p9_out, .events = POLLIN};
    int saw_full = 0;
    if (poll(&pp9, 1, 2000) > 0) {
        read(p9_out, line9, sizeof line9 - 1);
        saw_full = strncmp(line9, "FULLSCREEN\n", 11) == 0;
    }
    expect(saw_full, "fullscreen flag reaches the parent");
    kill(bridge_i, SIGTERM);
    kill(parent_i, SIGTERM);
    waitpid(bridge_i, NULL, 0);
    waitpid(parent_i, NULL, 0);
    (void)b9_out;

    printf("FAILURES %d\n", failures);
    fflush(stdout);
    if (bridge > 0)
        kill(bridge, SIGTERM);
    if (parent > 0)
        kill(parent, SIGTERM);
    if (bridge > 0)
        waitpid(bridge, NULL, 0);
    if (parent > 0)
        waitpid(parent, NULL, 0);
    return failures ? 1 : 0;
}
