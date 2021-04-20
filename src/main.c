/**
 * Copyright 2021 Garmin Ltd. or its subsidaries
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE 1

#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

struct format {
  struct wl_list link;

  uint32_t format;
  uint64_t modifier;
};

struct buffer {
  struct wl_list link;
  struct gbm_bo *bo;
  struct wl_buffer *buffer;
};

static int render_fd = -1;
static struct gbm_device *gbm;
static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct zwp_linux_dmabuf_v1 *dmabuf;

static struct wl_surface *surface;
static struct xdg_surface *shell_surface;
static struct xdg_toplevel *toplevel;

static struct wl_list dmabuf_formats;
static struct wl_list buffers;

static bool running = true;
static bool wait_for_configure = true;
static int arg_fill_val = 0xFF;
static uint32_t arg_format = DRM_FORMAT_NV12;

static void redraw(void);

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell,
                             uint32_t serial) {
  xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    xdg_wm_base_ping,
};

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {}

static struct wl_shm_listener const shm_listener = {
    shm_format,
};

static void dmabuf_format(void *data,
                          struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                          uint32_t format) {
  struct format *f = calloc(1, sizeof(*f));
  f->format = format;
  f->modifier = DRM_FORMAT_MOD_INVALID;

  wl_list_insert(&dmabuf_formats, &f->link);
}

static void dmabuf_modifier(void *data,
                            struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                            uint32_t format, uint32_t modifier_hi,
                            uint32_t modifier_lo) {
  struct format *f = calloc(1, sizeof(*f));
  f->format = format;
  f->modifier = (uint64_t)modifier_hi << 32 | modifier_lo;

  wl_list_insert(&dmabuf_formats, &f->link);
}

static struct zwp_linux_dmabuf_v1_listener const dmabuf_listener = {
    dmabuf_format,
    dmabuf_modifier,
};

static void handle_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height,
                                      struct wl_array *states) {}

static void handle_toplevel_close(void *data,
                                  struct xdg_toplevel *xdg_toplevel) {
  running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    handle_toplevel_configure,
    handle_toplevel_close,
};

static void handle_surface_configure(void *data, struct xdg_surface *surface,
                                     uint32_t serial) {
  xdg_surface_ack_configure(surface, serial);

  if (wait_for_configure) {
    redraw();
    wait_for_configure = false;
  }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    handle_surface_configure,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  if (strcmp(interface, "wl_compositor") == 0) {
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                  MIN(version, 4));

  } else if (strcmp(interface, "wl_subcompositor") == 0) {
    subcompositor = wl_registry_bind(
        registry, name, &wl_subcompositor_interface, MIN(version, 1));

  } else if (strcmp(interface, "xdg_wm_base") == 0) {
    wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

  } else if (strcmp(interface, "wl_shm") == 0) {
    shm = wl_registry_bind(registry, name, &wl_shm_interface, MIN(version, 1));
    wl_shm_add_listener(shm, &shm_listener, NULL);

  } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
    dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                              MIN(version, 3));
    zwp_linux_dmabuf_v1_add_listener(dmabuf, &dmabuf_listener, NULL);
  }
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

static void frame_done(void *data, struct wl_callback *callback,
                       uint32_t time) {
  wl_callback_destroy(callback);
  redraw();
}

static struct wl_callback_listener const frame_listener = {
    frame_done,
};

static void buffer_released(void *data, struct wl_buffer *buffer) {
  struct buffer *b = data;
  wl_list_insert(&buffers, &b->link);
}

static struct wl_buffer_listener const buffer_listener = {
    buffer_released,
};

static void fill_fd(int fd, uint32_t offset, size_t length, int val) {
  void *ptr =
      mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
  assert(ptr != MAP_FAILED);
  memset(ptr, val, length);
  munmap(ptr, length);
}

static void fill_buffer(struct gbm_bo *bo) {
  uint32_t format = gbm_bo_get_format(bo);
  uint32_t height = gbm_bo_get_height(bo);

  // TODO: It should not be necessary to dup the descriptor, but gbm_bo_get_fd
  // is returning an internal descriptor that we cannot close
  int fd = dup(gbm_bo_get_fd(bo));

  uint32_t offset = gbm_bo_get_offset(bo, 0);
  uint32_t stride = gbm_bo_get_stride_for_plane(bo, 0);
  size_t length = stride * height;
  fill_fd(fd, offset, length, arg_fill_val);

  if (format == DRM_FORMAT_NV12) {
    offset = gbm_bo_get_offset(bo, 1);
    stride = gbm_bo_get_stride_for_plane(bo, 1);
    length = stride * height / 2;
    fill_fd(fd, offset, length, arg_fill_val);
  }
  close(fd);
}

static struct buffer *get_buffer(void) {
  if (!wl_list_empty(&buffers)) {
    struct buffer *buffer = wl_container_of(buffers.next, buffer, link);
    wl_list_remove(&buffer->link);
    return buffer;
  }

  struct buffer *buffer = calloc(1, sizeof(*buffer));

  buffer->bo = gbm_bo_create(gbm, 200, 200, arg_format, GBM_BO_USE_SCANOUT);
  assert(buffer->bo);

  // TODO: It should not be necessary to dup the descriptor, but gbm_bo_get_fd
  // is returning an internal descriptor that we cannot close
  int fd = dup(gbm_bo_get_fd(buffer->bo));
  int plane_count = gbm_bo_get_plane_count(buffer->bo);

  // TODO: Lie to weston about the modifier; it only supports
  // DRM_FORMAT_MOD_INVALID
  uint64_t modifier = DRM_FORMAT_MOD_INVALID;

  struct zwp_linux_buffer_params_v1 *params =
      zwp_linux_dmabuf_v1_create_params(dmabuf);
  for (int i = 0; i < plane_count; i++) {
    zwp_linux_buffer_params_v1_add(params, fd, i,
                                   gbm_bo_get_offset(buffer->bo, i),
                                   gbm_bo_get_stride_for_plane(buffer->bo, i),
                                   modifier >> 32, modifier & 0xFFFFFFFF);
  }
  close(fd);

  buffer->buffer = zwp_linux_buffer_params_v1_create_immed(
      params, gbm_bo_get_width(buffer->bo), gbm_bo_get_height(buffer->bo),
      gbm_bo_get_format(buffer->bo), 0);
  wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
  zwp_linux_buffer_params_v1_destroy(params);

  return buffer;
}

static void redraw(void) {
  struct buffer *buffer = get_buffer();

  fill_buffer(buffer->bo);

  struct wl_callback *frame = wl_surface_frame(surface);
  wl_callback_add_listener(frame, &frame_listener, NULL);

  wl_surface_attach(surface, buffer->buffer, 0, 0);
  wl_surface_damage(surface, 0, 0, gbm_bo_get_width(buffer->bo),
                    gbm_bo_get_height(buffer->bo));
  wl_surface_commit(surface);
}

int main(int argc, char **argv) {
  static struct option const options[] = {
      {"fill", required_argument, 0, 'f'},
      {"nv12", no_argument, 0, 'n'},
      {"rgba", no_argument, 0, 'r'},
      {"help", no_argument, 0, 'h'},
      {0},
  };

  while (1) {
    int option_index;
    int c = getopt_long(argc, argv, "f:nrh", options, &option_index);

    printf("%d\n", c);
    if (c == -1) {
      break;
    }

    switch (c) {
    case 'f':
      arg_fill_val = atoi(optarg);
      break;

    case 'n':
      arg_format = DRM_FORMAT_NV12;
      break;

    case 'r':
      arg_format = DRM_FORMAT_ARGB8888;
      break;

    default:
    case 'h':
      printf("Usage: %s [-f VAL] [-h] [-n] [-r]\n", argv[0]);
      printf("  --fill|-f VAL   Fill buffer with value VAL\n");
      printf("  --help          Show help\n");
      printf("  --nv12|-n       Use NV12 buffer\n");
      printf("  --rgba|-r       Use ARGB8888 buffer\n");
      return 0;
    }
  }

  wl_list_init(&dmabuf_formats);
  wl_list_init(&buffers);

  render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  assert(render_fd >= 0);

  gbm = gbm_create_device(render_fd);
  assert(gbm);

  display = wl_display_connect(NULL);
  assert(display);

  registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);

  wl_display_roundtrip(display);
  wl_display_roundtrip(display);

  assert(compositor);
  assert(subcompositor);
  assert(wm_base);
  assert(shm);
  assert(dmabuf);

  surface = wl_compositor_create_surface(compositor);

  shell_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
  xdg_surface_add_listener(shell_surface, &xdg_surface_listener, NULL);

  toplevel = xdg_surface_get_toplevel(shell_surface);
  xdg_toplevel_add_listener(toplevel, &xdg_toplevel_listener, NULL);

  xdg_toplevel_set_title(toplevel, "nv12-client");
  wl_surface_commit(surface);

  while (running && wl_display_dispatch(display) != -1) {
  }

  return 0;
}
