#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <xf86drm.h>

#include "linux-dmabuf-server.h"
#include "xdg-shell-client.h"
#include "xdg-shell-server.h"

#define STAGING 3

struct buffer_state {
    int fd;
    uint32_t format;
    uint32_t stride;
    uint32_t offset;
    int32_t width;
    int32_t height;
    uint64_t modifier;
    int planes;
    int busy;
    int dead;
    int used;
    int released;
    struct wl_resource *resource;
};

struct surface_state {
    struct wl_resource *resource;
    struct wl_resource *pending_buffer;
    struct wl_resource *frame;
    struct wl_resource *xdg;
    struct wl_resource *toplevel;
    int role;
    int ack;
    int32_t buf_x;
    int32_t buf_y;
};

struct staging {
    struct wl_buffer *buffer;
    void *map;
    int busy;
};

struct keyboard_res {
    struct wl_resource *resource;
    int got_repeat;
    int entered;
};

struct pointer_res {
    struct wl_resource *resource;
};

static struct {
    struct wl_display *server;
    struct wl_event_loop *loop;
    struct wl_display *up;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *xdg;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_surface *surface;
    struct wl_shm_pool *pool;
    int pool_fd;
    struct staging slot[STAGING];
    int width;
    int height;
    uint32_t shm_format;
    int parent_closed;
    int run;
    int no_child;
    pid_t child;
    char listen_name[64];
    char config[512];
    dev_t vkms_rdev;
    struct wl_resource *compositor_client;
    struct wl_list keyboards;
    struct wl_list pointers;
    uint32_t serial;
    uint32_t up_enter_serial;
    int up_entered;
    int keymap_format;
    void *keymap;
    uint32_t keymap_size;
    int repeat_sent;
    struct wl_array pending_keys;
    int have_mods;
    uint32_t mods;
    int key_is_modifier;
    uint32_t pending_key;
    uint32_t pending_key_state;
    int have_pending_key;
    int fullscreen;
    int had_parent_display;
    char parent_display[64];
} G;

struct link {
    struct wl_list link;
    struct wl_resource *resource;
};

static int slots_listening;
static int client_hooked;
static struct buffer_state *queued;
static struct surface_state *queued_surface;
static struct wl_listener gone_listener;
static void client_gone(struct wl_listener *listener, void *data);

static int sync_eintr_left;

static int sync_buf(int fd, uint64_t flags) {
    struct dma_buf_sync sync = {.flags = flags};
    for (;;) {
        if (sync_eintr_left > 0) {
            sync_eintr_left--;
            errno = EINTR;
            continue;
        }
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
            return 0;
        if (errno == EINTR || errno == EAGAIN)
            continue;
        return -1;
    }
}

static int is_modifier_key(uint32_t key) {
    switch (key) {
    case 29:
    case 42:
    case 54:
    case 56:
    case 97:
    case 100:
    case 125:
    case 126:
        return 1;
    default:
        return 0;
    }
}

static int shm_of(uint32_t drm, uint32_t *out) {
    if (drm == DRM_FORMAT_XRGB8888) {
        *out = WL_SHM_FORMAT_XRGB8888;
        return 1;
    }
    if (drm == DRM_FORMAT_ARGB8888) {
        *out = WL_SHM_FORMAT_ARGB8888;
        return 1;
    }
    return 0;
}

static int find_vkms(dev_t *rdev) {
    drmDevicePtr devices[16];
    int count = drmGetDevices2(0, devices, 16);
    if (count < 0)
        return -1;
    for (int i = 0; i < count; i++) {
        if (!(devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;
        const char *primary = devices[i]->nodes[DRM_NODE_PRIMARY];
        const char *base = strrchr(primary, '/');
        if (!base)
            continue;
        char sys[256];
        snprintf(sys, sizeof sys, "/sys/class/drm/%s", base + 1);
        char *resolved = realpath(sys, NULL);
        int match = resolved && strstr(resolved, "/vkms/");
        free(resolved);
        if (!match)
            continue;
        struct stat st;
        if (stat(primary, &st) != 0)
            continue;
        *rdev = st.st_rdev;
        drmFreeDevices(devices, count);
        return 0;
    }
    drmFreeDevices(devices, count);
    return -1;
}

static struct staging *free_slot(void) {
    for (int i = 0; i < STAGING; i++)
        if (!G.slot[i].busy && G.slot[i].buffer)
            return &G.slot[i];
    return NULL;
}

static void destroy_pool(void) {
    for (int i = 0; i < STAGING; i++) {
        if (G.slot[i].buffer)
            wl_buffer_destroy(G.slot[i].buffer);
        G.slot[i].buffer = NULL;
        G.slot[i].map = NULL;
        G.slot[i].busy = 0;
    }
    if (G.pool) {
        wl_shm_pool_destroy(G.pool);
        G.pool = NULL;
    }
    if (G.pool_fd >= 0) {
        close(G.pool_fd);
        G.pool_fd = -1;
    }
}

static int ensure_pool(int32_t width, int32_t height, uint32_t shm_format) {
    if (G.pool && G.width == width && G.height == height && G.shm_format == shm_format)
        return 0;
    for (int i = 0; i < STAGING; i++)
        if (G.slot[i].busy)
            return -1;
    destroy_pool();
    slots_listening = 0;
    int stride = width * 4;
    size_t frame = (size_t)stride * (size_t)height;
    size_t bytes = frame * STAGING;
    int fd = memfd_create("stage", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0)
        return -1;
    void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    G.pool = wl_shm_create_pool(G.shm, fd, (int32_t)bytes);
    G.pool_fd = fd;
    G.width = width;
    G.height = height;
    G.shm_format = shm_format;
    for (int i = 0; i < STAGING; i++) {
        G.slot[i].buffer = wl_shm_pool_create_buffer(G.pool, (int32_t)(i * frame), width, height, stride, (int32_t)shm_format);
        G.slot[i].map = (char *)map + i * frame;
        G.slot[i].busy = 0;
    }
    return 0;
}

static void release_client(struct buffer_state *buf) {
    if (!buf || buf->released)
        return;
    buf->busy = 0;
    buf->released = 1;
    if (queued == buf) {
        queued = NULL;
        queued_surface = NULL;
    }
    if (!buf->dead && buf->resource)
        wl_buffer_send_release(buf->resource);
    if (buf->dead) {
        if (buf->fd >= 0)
            close(buf->fd);
        free(buf);
    }
}

static int stage_buffer(struct surface_state *surface, struct buffer_state *buf);

static void pump_queue(void) {
    if (!queued || !queued_surface)
        return;
    struct buffer_state *buf = queued;
    struct surface_state *surface = queued_surface;
    int rc = stage_buffer(surface, buf);
    if (rc <= 0) {
        queued = NULL;
        queued_surface = NULL;
    }
}

static void shm_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    struct staging *slot = data;
    slot->busy = 0;
    pump_queue();
    wl_display_flush_clients(G.server);
}

static const struct wl_buffer_listener shm_listener = {.release = shm_release};

static void arm_slots(void) {
    if (slots_listening)
        return;
    for (int i = 0; i < STAGING; i++) {
        if (G.slot[i].buffer)
            wl_buffer_add_listener(G.slot[i].buffer, &shm_listener, &G.slot[i]);
    }
    slots_listening = 1;
}

static void send_down_configure(void) {
    struct link *link;
    wl_list_for_each(link, &G.keyboards, link) {
        (void)link;
    }
    /* Find toplevel surfaces. */
}

static struct surface_state *main_surface;

static void emit_configure(struct surface_state *surface) {
    if (!surface->toplevel || !surface->xdg)
        return;
    struct wl_array caps;
    wl_array_init(&caps);
    if (wl_resource_get_version(surface->toplevel) >= 5)
        xdg_toplevel_send_wm_capabilities(surface->toplevel, &caps);
    wl_array_release(&caps);
    struct wl_array states;
    wl_array_init(&states);
    int32_t w = G.width > 0 ? G.width : 1280;
    int32_t h = G.height > 0 ? G.height : 720;
    xdg_toplevel_send_configure(surface->toplevel, w, h, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(surface->xdg, ++G.serial);
}

static int copy_buffer(struct buffer_state *buf, struct staging *slot) {
    if (sync_buf(buf->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) != 0)
        return -1;
    size_t map_len = (size_t)buf->offset + (size_t)buf->stride * (size_t)buf->height;
    void *src = mmap(NULL, map_len, PROT_READ, MAP_SHARED, buf->fd, 0);
    if (src == MAP_FAILED) {
        sync_buf(buf->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return -1;
    }
    int stride = buf->width * 4;
    for (int y = 0; y < buf->height; y++) {
        memcpy((char *)slot->map + (size_t)y * (size_t)stride,
               (char *)src + buf->offset + (size_t)y * buf->stride, (size_t)stride);
    }
    if (sync_buf(buf->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) != 0) {
        munmap(src, map_len);
        return -1;
    }
    munmap(src, map_len);
    return 0;
}

static void up_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    struct wl_resource *frame = data;
    if (frame)
        wl_callback_send_done(frame, time);
    wl_callback_destroy(cb);
    wl_display_flush_clients(G.server);
}
static const struct wl_callback_listener frame_listener = {.done = up_frame_done};

static int stage_buffer(struct surface_state *surface, struct buffer_state *buf) {
    uint32_t shm = 0;
    if (!shm_of(buf->format, &shm)) {
        wl_resource_post_error(buf->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT, "format");
        return -1;
    }
    if (ensure_pool(buf->width, buf->height, shm) != 0)
        return 1;
    arm_slots();
    struct staging *slot = free_slot();
    if (!slot)
        return 1;
    if (copy_buffer(buf, slot) != 0) {
        wl_resource_post_error(buf->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER, "sync");
        return -1;
    }
    slot->busy = 1;
    wl_surface_attach(G.surface, slot->buffer, 0, 0);
    wl_surface_damage_buffer(G.surface, 0, 0, buf->width, buf->height);
    struct wl_resource *frame = surface->frame;
    surface->frame = NULL;
    struct wl_callback *cb = wl_surface_frame(G.surface);
    wl_callback_add_listener(cb, &frame_listener, frame);
    wl_surface_commit(G.surface);
    wl_display_flush(G.up);
    release_client(buf);
    return 0;
}

static void enqueue_buffer(struct surface_state *surface, struct buffer_state *buf) {
    buf->busy = 1;
    if (queued && queued != buf)
        release_client(queued);
    queued = buf;
    queued_surface = surface;
}

static void present_fixed(struct surface_state *surface) {
    struct buffer_state *buf = surface->pending_buffer ? wl_resource_get_user_data(surface->pending_buffer) : NULL;
    surface->pending_buffer = NULL;
    if (!buf)
        return;
    int rc = stage_buffer(surface, buf);
    if (rc == 1)
        enqueue_buffer(surface, buf);
}

static void surface_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct surface_state *s = wl_resource_get_user_data(r);
    if (main_surface == s)
        main_surface = NULL;
    free(s);
    wl_resource_destroy(r);
}
static void surface_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)c;
    struct surface_state *s = wl_resource_get_user_data(r);
    if (buffer) {
        struct buffer_state *buf = wl_resource_get_user_data(buffer);
        if (buf && buf->busy) {
            wl_resource_post_error(buffer, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "premature reuse");
            return;
        }
        if (buf)
            buf->released = 0;
    }
    s->pending_buffer = buffer;
    s->buf_x = x;
    s->buf_y = y;
}
static void surface_nop_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct surface_state *s = wl_resource_get_user_data(resource);
    struct wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (!cb) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(cb, NULL, NULL, NULL);
    s->frame = cb;
}
static void surface_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *region) {
    (void)c;
    (void)r;
    (void)region;
}
static void surface_commit(struct wl_client *c, struct wl_resource *resource) {
    (void)c;
    struct surface_state *s = wl_resource_get_user_data(resource);
    if (s->toplevel && !s->ack && !s->pending_buffer) {
        emit_configure(s);
        return;
    }
    if (s->pending_buffer && !s->toplevel) {
        struct buffer_state *buf = wl_resource_get_user_data(s->pending_buffer);
        s->pending_buffer = NULL;
        if (buf && !buf->busy)
            release_client(buf);
        if (s->frame) {
            wl_callback_send_done(s->frame, 0);
            wl_resource_destroy(s->frame);
            s->frame = NULL;
        }
        return;
    }
    if (s->toplevel && s->pending_buffer)
        present_fixed(s);
}
static void surface_transform(struct wl_client *c, struct wl_resource *r, int32_t t) {
    (void)c;
    (void)r;
    (void)t;
}
static void surface_scale(struct wl_client *c, struct wl_resource *r, int32_t scale) {
    (void)c;
    (void)r;
    (void)scale;
}
static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c;
    struct surface_state *s = wl_resource_get_user_data(r);
    s->buf_x = x;
    s->buf_y = y;
}
static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_nop_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_region,
    .set_input_region = surface_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_transform,
    .set_buffer_scale = surface_scale,
    .damage_buffer = surface_nop_damage,
    .offset = surface_offset,
};

static void region_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static void region_math(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_math,
    .subtract = region_math,
};
static void comp_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct surface_state *s = calloc(1, sizeof *s);
    struct wl_resource *surface = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (!surface || !s) {
        free(s);
        wl_client_post_no_memory(client);
        return;
    }
    s->resource = surface;
    wl_resource_set_implementation(surface, &surface_impl, s, NULL);
    if (!G.compositor_client)
        G.compositor_client = wl_resource_get_client(surface) ? surface : surface;
}
static void comp_region(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface comp_impl = {
    .create_surface = comp_surface,
    .create_region = comp_region,
};

static void forward_key(uint32_t key, uint32_t state) {
    struct link *item;
    wl_list_for_each(item, &G.keyboards, link) {
        struct keyboard_res *k = wl_resource_get_user_data(item->resource);
        if (!k->got_repeat) {
            wl_keyboard_send_repeat_info(item->resource, 25, 600);
            k->got_repeat = 1;
        }
        wl_keyboard_send_key(item->resource, ++G.serial, 1, key,
                             state ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
}
static void forward_mods(uint32_t mods) {
    struct link *item;
    wl_list_for_each(item, &G.keyboards, link) {
        wl_keyboard_send_modifiers(item->resource, G.serial, mods, 0, 0, 0);
    }
}

static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surface, int32_t hx, int32_t hy) {
    (void)c;
    (void)r;
    (void)surface;
    (void)hx;
    (void)hy;
    if (!G.pointer || serial != G.up_enter_serial)
        return;
    wl_pointer_set_cursor(G.pointer, G.up_enter_serial, NULL, 0, 0);
}
static void pointer_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct link *item, *tmp;
    wl_list_for_each_safe(item, tmp, &G.pointers, link) {
        if (item->resource == r) {
            wl_list_remove(&item->link);
            free(item);
        }
    }
    wl_resource_destroy(r);
}
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = pointer_release,
};
static void keyboard_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct link *item, *tmp;
    wl_list_for_each_safe(item, tmp, &G.keyboards, link) {
        if (item->resource == r) {
            wl_list_remove(&item->link);
            free(wl_resource_get_user_data(r));
            free(item);
        }
    }
    wl_resource_destroy(r);
}
static const struct wl_keyboard_interface keyboard_impl = {.release = keyboard_release};

static void seat_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *pointer = wl_resource_create(client, &wl_pointer_interface, 9, id);
    if (!pointer) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(pointer, &pointer_impl, NULL, NULL);
    struct link *item = calloc(1, sizeof *item);
    item->resource = pointer;
    wl_list_insert(&G.pointers, &item->link);
}
static void seat_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct keyboard_res *k = calloc(1, sizeof *k);
    struct wl_resource *keyboard = wl_resource_create(client, &wl_keyboard_interface, 9, id);
    if (!keyboard || !k) {
        free(k);
        wl_client_post_no_memory(client);
        return;
    }
    k->resource = keyboard;
    wl_resource_set_implementation(keyboard, &keyboard_impl, k, NULL);
    struct link *item = calloc(1, sizeof *item);
    item->resource = keyboard;
    wl_list_insert(&G.keyboards, &item->link);
    if (G.keymap && G.keymap_size) {
        int fd = memfd_create("keymap", MFD_CLOEXEC);
        if (fd >= 0 && write(fd, G.keymap, G.keymap_size) == (ssize_t)G.keymap_size) {
            wl_keyboard_send_keymap(keyboard, G.keymap_format, fd, G.keymap_size);
            close(fd);
        }
    }
    wl_keyboard_send_repeat_info(keyboard, 25, 600);
    k->got_repeat = 1;
}
static void seat_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *touch = wl_resource_create(client, &wl_touch_interface, 1, id);
    if (touch)
        wl_resource_set_implementation(touch, NULL, NULL, NULL);
}
static void seat_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_pointer,
    .get_keyboard = seat_keyboard,
    .get_touch = seat_touch,
    .release = seat_release,
};

static void nop_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static void buffer_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct buffer_state *buf = wl_resource_get_user_data(r);
    if (buf && buf->busy) {
        buf->dead = 1;
        buf->resource = NULL;
        wl_resource_set_user_data(r, NULL);
        wl_resource_destroy(r);
        return;
    }
    if (buf) {
        if (queued == buf) {
            queued = NULL;
            queued_surface = NULL;
        }
        if (buf->fd >= 0)
            close(buf->fd);
        free(buf);
        wl_resource_set_user_data(r, NULL);
    }
    wl_resource_destroy(r);
}
static const struct wl_buffer_interface buffer_impl = {.destroy = buffer_destroy};

static void params_destroy(struct wl_client *c, struct wl_resource *r) {
    nop_destroy(c, r);
}
static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t plane, uint32_t offset, uint32_t stride, uint32_t mod_hi, uint32_t mod_lo) {
    (void)c;
    struct buffer_state *buf = wl_resource_get_user_data(r);
    if (plane != 0) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane");
        close(fd);
        return;
    }
    if (buf->planes) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane set");
        close(fd);
        return;
    }
    buf->fd = fd;
    buf->offset = offset;
    buf->stride = stride;
    buf->modifier = ((uint64_t)mod_hi << 32) | mod_lo;
    buf->planes = 1;
}
static void params_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
    (void)c;
    (void)w;
    (void)h;
    (void)format;
    (void)flags;
    wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER, "create is rejected");
}
static int params_ok(struct wl_resource *r, struct buffer_state *buf, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
    if (!buf->planes) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "no plane");
        return 0;
    }
    uint32_t shm = 0;
    if (flags != 0 || buf->modifier != DRM_FORMAT_MOD_LINEAR || !shm_of(format, &shm)) {
        fprintf(stderr, "reject format=%u flags=%u modifier=0x%llx\n", format, flags, (unsigned long long)buf->modifier);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT, "format");
        return 0;
    }
    if (w <= 0 || h <= 0) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS, "size");
        return 0;
    }
    uint64_t need = (uint64_t)buf->offset + (uint64_t)buf->stride * (uint64_t)h;
    if (buf->stride < (uint32_t)w * 4 || need < buf->offset) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS, "bounds");
        return 0;
    }
    buf->width = w;
    buf->height = h;
    buf->format = format;
    return 1;
}
static void params_create_immed(struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
    struct buffer_state *buf = wl_resource_get_user_data(resource);
    if (buf->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "used");
        return;
    }
    buf->used = 1;
    if (!params_ok(resource, buf, w, h, format, flags))
        return;
    struct wl_resource *buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer) {
        wl_client_post_no_memory(client);
        return;
    }
    buf->resource = buffer;
    wl_resource_set_user_data(resource, NULL);
    wl_resource_set_implementation(buffer, &buffer_impl, buf, NULL);
}
static void params_set_sampling(struct wl_client *c, struct wl_resource *r, struct wl_array *device) {
    (void)c;
    (void)r;
    (void)device;
}
static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
    .set_sampling_device = params_set_sampling,
};

static void send_feedback(struct wl_resource *feedback) {
    struct {
        uint32_t format;
        uint32_t pad;
        uint64_t modifier;
    } table[2] = {
        {DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_LINEAR},
        {DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_LINEAR},
    };
    int fd = memfd_create("formats", MFD_CLOEXEC);
    if (fd < 0)
        return;
    if (write(fd, table, sizeof table) != (ssize_t)sizeof table) {
        close(fd);
        return;
    }
    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, fd, sizeof table);
    close(fd);
    struct wl_array device;
    wl_array_init(&device);
    dev_t *slot = wl_array_add(&device, sizeof(dev_t));
    *slot = G.vkms_rdev;
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &device);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, 0);
    struct wl_array indices;
    wl_array_init(&indices);
    uint16_t *a = wl_array_add(&indices, sizeof(uint16_t));
    uint16_t *b = wl_array_add(&indices, sizeof(uint16_t));
    *a = 0;
    *b = 1;
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
    zwp_linux_dmabuf_feedback_v1_send_done(feedback);
    wl_array_release(&device);
    wl_array_release(&indices);
}
static void feedback_destroy(struct wl_client *c, struct wl_resource *r) {
    nop_destroy(c, r);
}
static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {.destroy = feedback_destroy};

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *r) {
    nop_destroy(c, r);
}
static void dmabuf_params(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct buffer_state *buf = calloc(1, sizeof *buf);
    buf->fd = -1;
    struct wl_resource *params = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, 4, id);
    if (!params || !buf) {
        free(buf);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(params, &params_impl, buf, NULL);
}
static void dmabuf_feedback(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *feedback = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 4, id);
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(feedback, &feedback_impl, NULL, NULL);
    send_feedback(feedback);
}
static void dmabuf_surface_feedback(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
    (void)surface;
    dmabuf_feedback(client, resource, id);
}
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_params,
    .get_default_feedback = dmabuf_feedback,
    .get_surface_feedback = dmabuf_surface_feedback,
};

static void top_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static void top_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) {
    (void)c;
    (void)r;
    (void)p;
}
static void top_title(struct wl_client *c, struct wl_resource *r, const char *title) {
    (void)c;
    (void)r;
    if (G.toplevel && title)
        xdg_toplevel_set_title(G.toplevel, title);
}
static void top_app(struct wl_client *c, struct wl_resource *r, const char *id) {
    (void)c;
    (void)r;
    if (G.toplevel && id)
        xdg_toplevel_set_app_id(G.toplevel, id);
}
static void top_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
    (void)c;
    (void)r;
    (void)seat;
    (void)serial;
    (void)x;
    (void)y;
}
static void top_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    (void)c;
    (void)r;
    (void)seat;
    (void)serial;
}
static void top_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    (void)c;
    (void)r;
    (void)seat;
    (void)serial;
    (void)edges;
}
static void top_max(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)w;
    (void)h;
}
static void top_flag(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    (void)r;
}
static void top_output(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) {
    (void)c;
    (void)r;
    (void)o;
}
static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = top_destroy,
    .set_parent = top_parent,
    .set_title = top_title,
    .set_app_id = top_app,
    .show_window_menu = top_menu,
    .move = top_move,
    .resize = top_resize,
    .set_max_size = top_max,
    .set_min_size = top_max,
    .set_maximized = top_flag,
    .unset_maximized = top_flag,
    .set_fullscreen = top_output,
    .unset_fullscreen = top_flag,
    .set_minimized = top_flag,
};
static void xs_destroy(struct wl_client *c, struct wl_resource *r) {
    nop_destroy(c, r);
}
static void xs_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct surface_state *s = wl_resource_get_user_data(resource);
    struct wl_resource *top = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (!top) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(top, &toplevel_impl, s, NULL);
    s->toplevel = top;
    main_surface = s;
}
static void xs_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *pos) {
    (void)r;
    (void)parent;
    (void)pos;
    struct wl_resource *popup = wl_resource_create(c, &xdg_popup_interface, 1, id);
    if (popup)
        wl_resource_set_implementation(popup, NULL, NULL, NULL);
}
static void xs_geom(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c;
    (void)serial;
    struct surface_state *s = wl_resource_get_user_data(r);
    if (s)
        s->ack = 1;
}
static const struct xdg_surface_interface xs_impl = {
    .destroy = xs_destroy,
    .get_toplevel = xs_toplevel,
    .get_popup = xs_popup,
    .set_window_geometry = xs_geom,
    .ack_configure = xs_ack,
};
static void xdg_destroy(struct wl_client *c, struct wl_resource *r) {
    nop_destroy(c, r);
}
static void pos_destroy(struct wl_client *c, struct wl_resource *r) {
    nop_destroy(c, r);
}
static void pos_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)w;
    (void)h;
}
static void pos_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static void pos_u(struct wl_client *c, struct wl_resource *r, uint32_t v) {
    (void)c;
    (void)r;
    (void)v;
}
static void pos_off(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
}
static void pos_none(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    (void)r;
}
static const struct xdg_positioner_interface pos_impl = {
    .destroy = pos_destroy,
    .set_size = pos_size,
    .set_anchor_rect = pos_rect,
    .set_anchor = pos_u,
    .set_gravity = pos_u,
    .set_constraint_adjustment = pos_u,
    .set_offset = pos_off,
    .set_reactive = pos_none,
    .set_parent_size = pos_size,
    .set_parent_configure = pos_u,
};
static void xdg_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *pos = wl_resource_create(client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    if (pos)
        wl_resource_set_implementation(pos, &pos_impl, NULL, NULL);
}
static void xdg_get_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
    struct surface_state *s = wl_resource_get_user_data(surface);
    struct wl_resource *xdg = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (!xdg) {
        wl_client_post_no_memory(client);
        return;
    }
    s->xdg = xdg;
    wl_resource_set_implementation(xdg, &xs_impl, s, NULL);
    G.compositor_client = surface;
}
static void xdg_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c;
    (void)r;
    (void)serial;
}
static const struct xdg_wm_base_interface xdg_base_impl = {
    .destroy = xdg_destroy,
    .create_positioner = xdg_positioner,
    .get_xdg_surface = xdg_get_surface,
    .pong = xdg_pong,
};

static void bind_global(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    if (!client_hooked) {
        gone_listener.notify = client_gone;
        wl_client_add_destroy_listener(client, &gone_listener);
        client_hooked = 1;
    }
    const char *name = data;
    if (strcmp(name, "wl_compositor") == 0) {
        struct wl_resource *res = wl_resource_create(client, &wl_compositor_interface, version, id);
        wl_resource_set_implementation(res, &comp_impl, NULL, NULL);
    } else if (strcmp(name, "wl_seat") == 0) {
        struct wl_resource *res = wl_resource_create(client, &wl_seat_interface, version, id);
        wl_resource_set_implementation(res, &seat_impl, NULL, NULL);
        wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    } else if (strcmp(name, "xdg_wm_base") == 0) {
        struct wl_resource *res = wl_resource_create(client, &xdg_wm_base_interface, version, id);
        wl_resource_set_implementation(res, &xdg_base_impl, NULL, NULL);
    } else if (strcmp(name, "zwp_linux_dmabuf_v1") == 0) {
        struct wl_resource *res = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
        wl_resource_set_implementation(res, &dmabuf_impl, NULL, NULL);
    }
}

static int compositor_gone;
static int transport_failed;
static volatile sig_atomic_t got_signal;

static void client_gone(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    compositor_gone = 1;
    G.run = 0;
}

static void up_pointer_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)p;
    (void)surface;
    G.up_enter_serial = serial;
    G.up_entered = 1;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link) {
        if (main_surface)
            wl_pointer_send_enter(item->resource, serial, main_surface->resource, x, y);
    }
}
static void up_pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface) {
    (void)d;
    (void)p;
    (void)surface;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link) {
        if (main_surface)
            wl_pointer_send_leave(item->resource, serial, main_surface->resource);
    }
}
static void up_pointer_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)d;
    (void)p;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link)
        wl_pointer_send_motion(item->resource, time, x, y);
}
static void up_pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)d;
    (void)p;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link)
        wl_pointer_send_button(item->resource, serial, time, button, state);
}
static void up_pointer_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)d;
    (void)p;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link)
        wl_pointer_send_axis(item->resource, time, axis, value);
}
static void up_pointer_frame(void *d, struct wl_pointer *p) {
    (void)d;
    (void)p;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link)
        wl_pointer_send_frame(item->resource);
    wl_display_flush_clients(G.server);
}
static void up_pointer_axis_source(void *d, struct wl_pointer *p, uint32_t source) {
    (void)d;
    (void)p;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link)
        wl_pointer_send_axis_source(item->resource, source);
}
static void up_pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis) {
    (void)d;
    (void)p;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link)
        wl_pointer_send_axis_stop(item->resource, time, axis);
}
static void up_pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
    (void)d;
    (void)p;
    struct link *item;
    wl_list_for_each(item, &G.pointers, link) {
        if (wl_resource_get_version(item->resource) >= 8)
            wl_pointer_send_axis_value120(item->resource, axis, discrete * 120);
    }
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = up_pointer_enter,
    .leave = up_pointer_leave,
    .motion = up_pointer_motion,
    .button = up_pointer_button,
    .axis = up_pointer_axis,
    .frame = up_pointer_frame,
    .axis_source = up_pointer_axis_source,
    .axis_stop = up_pointer_axis_stop,
    .axis_discrete = up_pointer_axis_discrete,
};

static void up_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) {
    (void)d;
    (void)k;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        free(G.keymap);
        G.keymap = malloc(size);
        if (G.keymap) {
            memcpy(G.keymap, map, size);
            G.keymap_size = size;
            G.keymap_format = (int)format;
        }
        munmap(map, size);
    }
    close(fd);
}
static void up_kenter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    (void)d;
    (void)k;
    (void)surface;
    struct link *item;
    wl_list_for_each(item, &G.keyboards, link) {
        struct keyboard_res *kb = wl_resource_get_user_data(item->resource);
        if (!kb->got_repeat) {
            wl_keyboard_send_repeat_info(item->resource, 25, 600);
            kb->got_repeat = 1;
        }
        if (main_surface)
            wl_keyboard_send_enter(item->resource, serial, main_surface->resource, keys);
        wl_keyboard_send_modifiers(item->resource, serial, 0, 0, 0, 0);
        kb->entered = 1;
    }
    wl_display_flush_clients(G.server);
}
static void up_kleave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface) {
    (void)d;
    (void)k;
    (void)surface;
    struct link *item;
    wl_list_for_each(item, &G.keyboards, link) {
        if (main_surface)
            wl_keyboard_send_leave(item->resource, serial, main_surface->resource);
    }
}
static void up_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)d;
    (void)k;
    (void)serial;
    (void)time;
    if (!is_modifier_key(key)) {
        forward_key(key, state);
        return;
    }
    G.have_pending_key = 1;
    G.pending_key = key;
    G.pending_key_state = state;
    G.key_is_modifier = 1;
}
static void up_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    (void)d;
    (void)k;
    (void)serial;
    (void)latched;
    (void)locked;
    (void)group;
    if (G.have_pending_key && G.key_is_modifier) {
        forward_key(G.pending_key, G.pending_key_state);
        G.have_pending_key = 0;
    }
    forward_mods(depressed);
    G.have_mods = 1;
    G.mods = depressed;
    if (G.have_pending_key) {
        forward_key(G.pending_key, G.pending_key_state);
        G.have_pending_key = 0;
    }
    wl_display_flush_clients(G.server);
}
static void up_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d;
    (void)k;
    struct link *item;
    wl_list_for_each(item, &G.keyboards, link)
        wl_keyboard_send_repeat_info(item->resource, rate, delay);
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = up_keymap,
    .enter = up_kenter,
    .leave = up_kleave,
    .key = up_key,
    .modifiers = up_mods,
    .repeat_info = up_repeat,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !G.pointer) {
        G.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(G.pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !G.keyboard) {
        G.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(G.keyboard, &keyboard_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *seat, const char *name) {
    (void)d;
    (void)seat;
    (void)name;
}
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

static void xdg_ping(void *d, struct xdg_wm_base *xdg, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(xdg, serial);
}
static const struct xdg_wm_base_listener xdg_listener = {.ping = xdg_ping};
static void top_configure(void *d, struct xdg_toplevel *top, int32_t w, int32_t h, struct wl_array *states) {
    (void)d;
    (void)top;
    (void)states;
    if (w <= 0)
        w = 1280;
    if (h <= 0)
        h = 720;
    G.width = w;
    G.height = h;
}
static void top_bounds(void *d, struct xdg_toplevel *top, int32_t w, int32_t h) {
    (void)d;
    (void)top;
    (void)w;
    (void)h;
}
static void top_caps(void *d, struct xdg_toplevel *top, struct wl_array *caps) {
    (void)d;
    (void)top;
    (void)caps;
}
static void top_close(void *d, struct xdg_toplevel *top) {
    (void)d;
    (void)top;
    G.parent_closed = 1;
    if (main_surface && main_surface->toplevel)
        xdg_toplevel_send_close(main_surface->toplevel);
    wl_display_flush_clients(G.server);
    G.run = 0;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = top_configure,
    .close = top_close,
    .configure_bounds = top_bounds,
    .wm_capabilities = top_caps,
};
static void xs_configure(void *d, struct xdg_surface *surface, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(surface, serial);
    if (main_surface)
        emit_configure(main_surface);
}
static const struct xdg_surface_listener xs_listener = {.configure = xs_configure};

static void registry_global(void *d, struct wl_registry *registry, uint32_t id, const char *iface, uint32_t version) {
    (void)d;
    if (strcmp(iface, "wl_compositor") == 0) {
        uint32_t v = version < 4 ? version : 4;
        G.compositor = wl_registry_bind(registry, id, &wl_compositor_interface, v);
    } else if (strcmp(iface, "wl_shm") == 0) {
        G.shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (strcmp(iface, "wl_seat") == 0) {
        uint32_t v = version < 7 ? version : 7;
        G.seat = wl_registry_bind(registry, id, &wl_seat_interface, v);
        wl_seat_add_listener(G.seat, &seat_listener, NULL);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        uint32_t v = version < 1 ? version : 1;
        G.xdg = wl_registry_bind(registry, id, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(G.xdg, &xdg_listener, NULL);
    }
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void)d;
    (void)r;
    (void)id;
}
static const struct wl_registry_listener registry_listener = {.global = registry_global, .global_remove = registry_remove};

static volatile sig_atomic_t inject_chord;

static void on_sig(int sig) {
    got_signal = sig;
    G.run = 0;
}

static void on_usr1(int sig) {
    (void)sig;
    inject_chord = 1;
}

static void deliver_super_return(void) {
    /* Same seat order as a parent modifier chord: Super press, Mod4, Return, then release. */
    up_key(NULL, NULL, 0, 0, 125, 1);
    up_mods(NULL, NULL, 0, 64, 0, 0, 0);
    up_key(NULL, NULL, 0, 0, 28, 1);
    up_key(NULL, NULL, 0, 0, 28, 0);
    up_key(NULL, NULL, 0, 0, 125, 0);
    up_mods(NULL, NULL, 0, 0, 0, 0, 0);
}

static void restore_user_environment(void) {
    if (G.no_child || !G.had_parent_display)
        return;
    char cmd[192];
    snprintf(cmd, sizeof cmd, "systemctl --user set-environment WAYLAND_DISPLAY=%s >/dev/null 2>&1", G.parent_display);
    int rc = system(cmd);
    (void)rc;
}

static int finish(int code) {
    restore_user_environment();
    return code;
}

static int spawn_child(void) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        unsetenv("WAYLAND_SOCKET");
        setenv("WAYLAND_DISPLAY", G.listen_name, 1);
        setenv("GBM_ALWAYS_SOFTWARE", "1", 1);
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        setenv("GALLIUM_DRIVER", "llvmpipe", 1);
        if (G.config[0])
            execl("/usr/bin/Hyprland", "Hyprland", "--config", G.config, (char *)NULL);
        else
            execl("/usr/bin/Hyprland", "Hyprland", (char *)NULL);
        _exit(127);
    }
    G.child = pid;
    return 0;
}

int main(int argc, char **argv) {
    G.pool_fd = -1;
    G.child = -1;
    G.run = 1;
    wl_list_init(&G.keyboards);
    wl_list_init(&G.pointers);
    snprintf(G.listen_name, sizeof G.listen_name, "omarchy-wslg");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-child") == 0)
            G.no_child = 1;
        else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc)
            snprintf(G.listen_name, sizeof G.listen_name, "%s", argv[++i]);
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            snprintf(G.config, sizeof G.config, "%s", argv[++i]);
        else if (strcmp(argv[i], "--fullscreen") == 0)
            G.fullscreen = 1;
    }
    if (!G.fullscreen) {
        const char *full = getenv("OMARCHY_WSLG_FULLSCREEN");
        if (full && strcmp(full, "1") == 0)
            G.fullscreen = 1;
    }
    const char *parent_display = getenv("WAYLAND_DISPLAY");
    if (parent_display && parent_display[0] && strlen(parent_display) < sizeof G.parent_display) {
        snprintf(G.parent_display, sizeof G.parent_display, "%s", parent_display);
        G.had_parent_display = 1;
    }
    const char *eintr_once = getenv("WSLG_SYNC_EINTR");
    if (eintr_once)
        sync_eintr_left = atoi(eintr_once);
    if (find_vkms(&G.vkms_rdev) != 0) {
        fprintf(stderr, "no vkms device\n");
        return 2;
    }
    G.up = wl_display_connect(NULL);
    if (!G.up) {
        fprintf(stderr, "parent connect failed\n");
        return 2;
    }
    G.registry = wl_display_get_registry(G.up);
    wl_registry_add_listener(G.registry, &registry_listener, NULL);
    wl_display_roundtrip(G.up);
    wl_display_roundtrip(G.up);
    if (!G.compositor || !G.shm || !G.xdg) {
        fprintf(stderr, "parent globals missing\n");
        return 2;
    }
    G.surface = wl_compositor_create_surface(G.compositor);
    G.xdg_surface = xdg_wm_base_get_xdg_surface(G.xdg, G.surface);
    xdg_surface_add_listener(G.xdg_surface, &xs_listener, NULL);
    G.toplevel = xdg_surface_get_toplevel(G.xdg_surface);
    xdg_toplevel_add_listener(G.toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(G.toplevel, "Omarchy Desktop");
    xdg_toplevel_set_app_id(G.toplevel, "omarchy");
    if (G.fullscreen)
        xdg_toplevel_set_fullscreen(G.toplevel, NULL);
    wl_surface_commit(G.surface);
    wl_display_roundtrip(G.up);

    G.server = wl_display_create();
    wl_display_init_shm(G.server);
    G.loop = wl_display_get_event_loop(G.server);
    wl_global_create(G.server, &wl_compositor_interface, 6, "wl_compositor", bind_global);
    wl_global_create(G.server, &wl_seat_interface, 9, "wl_seat", bind_global);
    wl_global_create(G.server, &xdg_wm_base_interface, 6, "xdg_wm_base", bind_global);
    wl_global_create(G.server, &zwp_linux_dmabuf_v1_interface, 4, "zwp_linux_dmabuf_v1", bind_global);
    char sockpath[512];
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        return 2;
    snprintf(sockpath, sizeof sockpath, "%s/%s", runtime, G.listen_name);
    unlink(sockpath);
    if (wl_display_add_socket(G.server, G.listen_name) != 0) {
        fprintf(stderr, "listen failed\n");
        return 2;
    }
    printf("LISTEN %s\n", G.listen_name);
    fflush(stdout);
    if (!G.no_child && spawn_child() != 0)
        return 2;
    struct sigaction term = {0};
    term.sa_handler = on_sig;
    sigaction(SIGTERM, &term, NULL);
    sigaction(SIGINT, &term, NULL);
    struct sigaction usr1 = {0};
    usr1.sa_handler = on_usr1;
    sigaction(SIGUSR1, &usr1, NULL);
    int up_fd = wl_display_get_fd(G.up);
    int srv_fd = wl_event_loop_get_fd(G.loop);
    while (G.run) {
        if (inject_chord) {
            inject_chord = 0;
            deliver_super_return();
        }
        wl_display_flush_clients(G.server);
        while (wl_display_prepare_read(G.up) != 0)
            wl_display_dispatch_pending(G.up);
        wl_display_flush(G.up);
        struct pollfd pf[2] = {
            {.fd = srv_fd, .events = POLLIN},
            {.fd = up_fd, .events = POLLIN},
        };
        int pr = poll(pf, 2, G.child > 0 ? 200 : -1);
        if (pr < 0 && errno == EINTR) {
            wl_display_cancel_read(G.up);
            continue;
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR))
            wl_display_read_events(G.up);
        else
            wl_display_cancel_read(G.up);
        if (wl_display_dispatch_pending(G.up) < 0) {
            transport_failed = 1;
            break;
        }
        if (pf[0].revents & POLLIN)
            wl_event_loop_dispatch(G.loop, 0);
        wl_display_flush_clients(G.server);
        if (G.child > 0) {
            int status = 0;
            pid_t got = waitpid(G.child, &status, WNOHANG);
            if (got == G.child) {
                G.child = -1;
                G.run = 0;
                if (WIFEXITED(status))
                    return finish(WEXITSTATUS(status));
                if (WIFSIGNALED(status))
                    return finish(128 + WTERMSIG(status));
            }
        }
        if (G.parent_closed)
            break;
    }
    int child_code = -1;
    if (G.child > 0) {
        kill(G.child, SIGTERM);
        for (int i = 0; i < 50; i++) {
            int status = 0;
            if (waitpid(G.child, &status, WNOHANG) == G.child) {
                if (WIFSIGNALED(status))
                    child_code = 128 + WTERMSIG(status);
                else if (WIFEXITED(status))
                    child_code = WEXITSTATUS(status);
                G.child = -1;
                break;
            }
            usleep(10000);
        }
        if (G.child > 0) {
            kill(G.child, SIGKILL);
            int status = 0;
            if (waitpid(G.child, &status, 0) == G.child && WIFSIGNALED(status))
                child_code = 128 + WTERMSIG(status);
        }
    }
    if (got_signal)
        return finish(128 + got_signal);
    if (G.parent_closed)
        return finish(0);
    if (child_code >= 0)
        return finish(child_code);
    if (compositor_gone)
        return finish(0);
    (void)transport_failed;
    return finish(2);
}
