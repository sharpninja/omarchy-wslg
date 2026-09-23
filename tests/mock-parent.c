#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-shell-server.h"

struct seat {
    struct wl_resource *pointer;
    struct wl_resource *keyboard;
    struct wl_resource *surface;
    uint32_t serial;
    int entered;
    int kentered;
    int hold;
    int32_t width;
    int32_t height;
    int configured;
};

static struct wl_display *display;
static struct seat seat;
static struct wl_resource *xdg_surface_res;
static struct wl_resource *toplevel_res;

static void keymap_send(struct wl_resource *keyboard) {
    static const char map[] =
        "xkb_keymap {\n"
        " xkb_keycodes { minimum = 8; maximum = 255; };\n"
        " xkb_types { };\n"
        " xkb_compat { };\n"
        " xkb_symbols { };\n"
        "};\n";
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    if (fd < 0)
        return;
    if (write(fd, map, sizeof map - 1) != (ssize_t)(sizeof map - 1)) {
        close(fd);
        return;
    }
    wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, sizeof map - 1);
    close(fd);
    wl_keyboard_send_repeat_info(keyboard, 25, 600);
}

static void send_configure(struct wl_resource *xdg_surface, struct wl_resource *toplevel) {
    struct wl_array states;
    wl_array_init(&states);
    if (seat.width <= 0)
        seat.width = 1280;
    if (seat.height <= 0)
        seat.height = 720;
    xdg_toplevel_send_configure(toplevel, seat.width, seat.height, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(xdg_surface, ++seat.serial);
    seat.configured = 1;
}

static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    if (seat.surface == resource)
        seat.surface = NULL;
    wl_resource_destroy(resource);
}
static void surface_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)c;
    (void)x;
    (void)y;
    wl_resource_set_user_data(r, buffer);
}
static void surface_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static struct wl_resource *pending_frame;

static void surface_frame2(struct wl_client *client, struct wl_resource *resource, uint32_t callback) {
    struct wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, callback);
    if (!cb) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(cb, NULL, resource, NULL);
    pending_frame = cb;
}
static void surface_commit(struct wl_client *c, struct wl_resource *resource) {
    (void)c;
    struct wl_resource *buffer = wl_resource_get_user_data(resource);
    int had_buffer = buffer != NULL;
    struct wl_shm_buffer *shm = buffer ? wl_shm_buffer_get(buffer) : NULL;
    if (shm) {
        int32_t w = wl_shm_buffer_get_width(shm);
        int32_t h = wl_shm_buffer_get_height(shm);
        int32_t stride = wl_shm_buffer_get_stride(shm);
        uint32_t format = wl_shm_buffer_get_format(shm);
        wl_shm_buffer_begin_access(shm);
        void *data = wl_shm_buffer_get_data(shm);
        fprintf(stdout, "FRAME %d %d %d %u\n", w, h, stride, format);
        fwrite(data, 1, (size_t)stride * (size_t)h, stdout);
        fflush(stdout);
        wl_shm_buffer_end_access(shm);
        if (!seat.hold)
            wl_buffer_send_release(buffer);
        if (pending_frame) {
            wl_callback_send_done(pending_frame, 0);
            wl_resource_destroy(pending_frame);
            pending_frame = NULL;
        }
    }
    wl_resource_set_user_data(resource, NULL);
    if (!had_buffer && xdg_surface_res && toplevel_res && !seat.configured)
        send_configure(xdg_surface_res, toplevel_res);
}
static void surface_set_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *region) {
    (void)c;
    (void)r;
    (void)region;
}
static void surface_transform(struct wl_client *c, struct wl_resource *r, int32_t transform) {
    (void)c;
    (void)r;
    (void)transform;
}
static void surface_scale(struct wl_client *c, struct wl_resource *r, int32_t scale) {
    (void)c;
    (void)r;
    (void)scale;
}
static void surface_damage_buffer(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    surface_damage(c, r, x, y, w, h);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame2,
    .set_opaque_region = surface_set_region,
    .set_input_region = surface_set_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_transform,
    .set_buffer_scale = surface_scale,
    .damage_buffer = surface_damage_buffer,
};

static void region_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static void region_sub(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    region_add(c, r, x, y, w, h);
}
static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_sub,
};

static void comp_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *surface = wl_resource_create(client, &wl_surface_interface, 4, id);
    if (!surface) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surface, &surface_impl, NULL, NULL);
    seat.surface = surface;
}
static void comp_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface compositor_impl = {
    .create_surface = comp_create_surface,
    .create_region = comp_create_region,
};

static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surface, int32_t hx, int32_t hy) {
    (void)c;
    (void)r;
    (void)serial;
    (void)surface;
    (void)hx;
    (void)hy;
}
static void pointer_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    if (seat.pointer == r)
        seat.pointer = NULL;
    wl_resource_destroy(r);
}
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = pointer_release,
};
static void keyboard_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    if (seat.keyboard == r)
        seat.keyboard = NULL;
    wl_resource_destroy(r);
}
static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *pointer = wl_resource_create(client, &wl_pointer_interface, 7, id);
    if (!pointer) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(pointer, &pointer_impl, NULL, NULL);
    seat.pointer = pointer;
}
static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *keyboard = wl_resource_create(client, &wl_keyboard_interface, 7, id);
    if (!keyboard) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(keyboard, &keyboard_impl, NULL, NULL);
    seat.keyboard = keyboard;
    keymap_send(keyboard);
}
static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct wl_resource *touch = wl_resource_create(client, &wl_touch_interface, 7, id);
    if (!touch) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(touch, NULL, NULL, NULL);
}
static void seat_release(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    if (version > 4) {
        wl_client_post_implementation_error(client, "compositor version %u", version);
        return;
    }
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(resource, &compositor_impl, NULL, NULL);
    fprintf(stderr, "BIND wl_compositor %u\n", version);
}
static void bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    if (version > 7) {
        wl_client_post_implementation_error(client, "seat version %u", version);
        return;
    }
    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(resource, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    fprintf(stderr, "BIND wl_seat %u\n", version);
}

static struct wl_resource *xdg_base_res;

static void xdg_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static void xdg_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c;
    (void)r;
    (void)serial;
}
static void top_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    if (toplevel_res == r)
        toplevel_res = NULL;
    wl_resource_destroy(r);
}
static void top_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *parent) {
    (void)c;
    (void)r;
    (void)parent;
}
static void top_set_title(struct wl_client *c, struct wl_resource *r, const char *title) {
    (void)c;
    (void)r;
    fprintf(stderr, "TITLE %s\n", title);
}
static void top_set_app_id(struct wl_client *c, struct wl_resource *r, const char *app_id) {
    (void)c;
    (void)r;
    fprintf(stderr, "APPID %s\n", app_id);
}
static void top_show_window_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat_res, uint32_t serial, int32_t x, int32_t y) {
    (void)c;
    (void)r;
    (void)seat_res;
    (void)serial;
    (void)x;
    (void)y;
}
static void top_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat_res, uint32_t serial) {
    (void)c;
    (void)r;
    (void)seat_res;
    (void)serial;
}
static void top_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat_res, uint32_t serial, uint32_t edges) {
    (void)c;
    (void)r;
    (void)seat_res;
    (void)serial;
    (void)edges;
}
static void top_set_max(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)w;
    (void)h;
}
static void top_set_min(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    top_set_max(c, r, w, h);
}
static void top_set_maximized(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    (void)r;
}
static void top_unset_maximized(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    (void)r;
}
static void top_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *output) {
    (void)c;
    (void)r;
    (void)output;
    fprintf(stdout, "FULLSCREEN\n");
    fflush(stdout);
}
static void top_unset_fullscreen(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    (void)r;
}
static void top_set_minimized(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    (void)r;
}
static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = top_destroy,
    .set_parent = top_set_parent,
    .set_title = top_set_title,
    .set_app_id = top_set_app_id,
    .show_window_menu = top_show_window_menu,
    .move = top_move,
    .resize = top_resize,
    .set_max_size = top_set_max,
    .set_min_size = top_set_min,
    .set_maximized = top_set_maximized,
    .unset_maximized = top_unset_maximized,
    .set_fullscreen = top_set_fullscreen,
    .unset_fullscreen = top_unset_fullscreen,
    .set_minimized = top_set_minimized,
};

static void xdg_surface_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    if (xdg_surface_res == r)
        xdg_surface_res = NULL;
    wl_resource_destroy(r);
}
static void xdg_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *top = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
    if (!top) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(top, &toplevel_impl, resource, NULL);
    toplevel_res = top;
}
static void xdg_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *positioner) {
    (void)r;
    (void)parent;
    (void)positioner;
    struct wl_resource *popup = wl_resource_create(c, &xdg_popup_interface, 1, id);
    if (popup)
        wl_resource_set_implementation(popup, NULL, NULL, NULL);
}
static void xdg_set_window_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static void xdg_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c;
    (void)r;
    (void)serial;
}
static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_get_toplevel,
    .get_popup = xdg_get_popup,
    .set_window_geometry = xdg_set_window_geometry,
    .ack_configure = xdg_ack,
};
static void xdg_get_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
    (void)resource;
    uint32_t version = wl_resource_get_version(resource);
    fprintf(stderr, "BIND xdg_wm_base %u\n", version);
    if (version > 1) {
        wl_client_post_implementation_error(client, "xdg version %u", version);
        return;
    }
    struct wl_resource *xdg_surface = wl_resource_create(client, &xdg_surface_interface, version, id);
    if (!xdg_surface) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(xdg_surface, &xdg_surface_impl, surface, NULL);
    xdg_surface_res = xdg_surface;
    seat.surface = surface;
}
static void positioner_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}
static void positioner_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)w;
    (void)h;
}
static void positioner_set_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
static void positioner_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t anchor) {
    (void)c;
    (void)r;
    (void)anchor;
}
static void positioner_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t gravity) {
    (void)c;
    (void)r;
    (void)gravity;
}
static void positioner_set_constraint(struct wl_client *c, struct wl_resource *r, uint32_t adj) {
    (void)c;
    (void)r;
    (void)adj;
}
static void positioner_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c;
    (void)r;
    (void)x;
    (void)y;
}
static void positioner_set_reactive(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    (void)r;
}
static void positioner_set_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    (void)r;
    (void)w;
    (void)h;
}
static void positioner_set_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c;
    (void)r;
    (void)serial;
}
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = positioner_destroy,
    .set_size = positioner_set_size,
    .set_anchor_rect = positioner_set_anchor_rect,
    .set_anchor = positioner_set_anchor,
    .set_gravity = positioner_set_gravity,
    .set_constraint_adjustment = positioner_set_constraint,
    .set_offset = positioner_set_offset,
    .set_reactive = positioner_set_reactive,
    .set_parent_size = positioner_set_parent_size,
    .set_parent_configure = positioner_set_parent_configure,
};
static void xdg_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *pos = wl_resource_create(client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    if (pos)
        wl_resource_set_implementation(pos, &positioner_impl, NULL, NULL);
}
static const struct xdg_wm_base_interface xdg_impl = {
    .destroy = xdg_destroy,
    .create_positioner = xdg_create_positioner,
    .get_xdg_surface = xdg_get_surface,
    .pong = xdg_pong,
};
static void bind_xdg(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    if (version > 1) {
        wl_client_post_implementation_error(client, "xdg_wm_base version %u", version);
        return;
    }
    xdg_base_res = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    wl_resource_set_implementation(xdg_base_res, &xdg_impl, NULL, NULL);
    fprintf(stderr, "BIND xdg_wm_base %u\n", version);
}

static int on_stdin(int fd, uint32_t mask, void *data) {
    (void)mask;
    (void)data;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (strncmp(line, "HOLD ", 5) == 0) {
        seat.hold = atoi(line + 5);
    } else if (strncmp(line, "RESIZE ", 7) == 0) {
        sscanf(line + 7, "%d %d", &seat.width, &seat.height);
        seat.configured = 0;
        if (xdg_surface_res && toplevel_res)
            send_configure(xdg_surface_res, toplevel_res);
    } else if (strncmp(line, "CLOSE", 5) == 0) {
        if (toplevel_res)
            xdg_toplevel_send_close(toplevel_res);
    } else if (strncmp(line, "PENTER", 6) == 0) {
        if (seat.pointer && seat.surface) {
            wl_pointer_send_enter(seat.pointer, ++seat.serial, seat.surface, wl_fixed_from_int(1), wl_fixed_from_int(1));
            wl_pointer_send_frame(seat.pointer);
            seat.entered = 1;
        }
    } else if (strncmp(line, "MOTION ", 7) == 0) {
        int x = 0, y = 0;
        sscanf(line + 7, "%d %d", &x, &y);
        if (seat.pointer) {
            wl_pointer_send_motion(seat.pointer, 1, wl_fixed_from_int(x), wl_fixed_from_int(y));
            wl_pointer_send_frame(seat.pointer);
        }
    } else if (strncmp(line, "BUTTON ", 7) == 0) {
        unsigned button = 0, pressed = 0;
        sscanf(line + 7, "%u %u", &button, &pressed);
        if (seat.pointer) {
            wl_pointer_send_button(seat.pointer, ++seat.serial, 1, button,
                                   pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            wl_pointer_send_frame(seat.pointer);
        }
    } else if (strncmp(line, "KENTER", 6) == 0) {
        if (seat.keyboard && seat.surface) {
            struct wl_array keys;
            wl_array_init(&keys);
            wl_keyboard_send_enter(seat.keyboard, ++seat.serial, seat.surface, &keys);
            wl_array_release(&keys);
            wl_keyboard_send_modifiers(seat.keyboard, seat.serial, 0, 0, 0, 0);
            seat.kentered = 1;
        }
    } else if (strncmp(line, "KEY ", 4) == 0) {
        unsigned key = 0, state = 0;
        sscanf(line + 4, "%u %u", &key, &state);
        if (seat.keyboard)
            wl_keyboard_send_key(seat.keyboard, ++seat.serial, 1, key,
                                 state ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    } else if (strncmp(line, "MODS ", 5) == 0) {
        unsigned mods = 0;
        sscanf(line + 5, "%u", &mods);
        if (seat.keyboard)
            wl_keyboard_send_modifiers(seat.keyboard, seat.serial, mods, 0, 0, 0);
    } else if (strncmp(line, "AXIS ", 5) == 0) {
        int axis = 0, discrete = 0;
        sscanf(line + 5, "%d %d", &axis, &discrete);
        if (seat.pointer) {
            wl_pointer_send_axis_source(seat.pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            wl_pointer_send_axis_discrete(seat.pointer, axis, discrete);
            wl_pointer_send_axis(seat.pointer, 1, axis, wl_fixed_from_int(discrete));
            wl_pointer_send_frame(seat.pointer);
        }
    }
    }
    wl_display_flush_clients(display);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mock-parent NAME\n");
        return 2;
    }
    display = wl_display_create();
    if (!display)
        return 2;
    wl_display_init_shm(display);
    wl_global_create(display, &wl_compositor_interface, 4, NULL, bind_compositor);
    wl_global_create(display, &wl_seat_interface, 7, NULL, bind_seat);
    wl_global_create(display, &xdg_wm_base_interface, 1, NULL, bind_xdg);
    if (wl_display_add_socket(display, argv[1]) != 0) {
        fprintf(stderr, "add_socket failed\n");
        return 2;
    }
    fprintf(stderr, "PARENT READY %s\n", argv[1]);
    fflush(stderr);
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    wl_event_loop_add_fd(loop, STDIN_FILENO, WL_EVENT_READABLE, on_stdin, NULL);
    seat.width = 1280;
    seat.height = 720;
    wl_display_run(display);
    wl_display_destroy(display);
    return 0;
}
