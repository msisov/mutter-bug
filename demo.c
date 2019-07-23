#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include "xdg-shell-unstable-v6-client-protocol.h"

int with_grab = 0;

struct wl_display *display = NULL;
struct wl_compositor *compositor = NULL;
struct wl_shell *shell = NULL;
struct wl_shm *shm = NULL;
struct zxdg_shell_v6* zxdg_shell = NULL;

struct window {
  struct wl_surface *surface;
  struct zxdg_surface_v6 *zxdg_surface;
  struct zxdg_toplevel_v6 *zxdg_toplevel;
  struct zxdg_popup_v6 *zxdg_popup;
  struct wl_buffer *buffer;
  struct wl_buffer *buffer2;
  void *shm_data;
  int width;
  int height;
  struct window* parent;
  uint32_t colour;
};

struct window* main_window;
struct window* popup_window;
struct window* nested_popup_window;

// input devices
struct wl_seat *seat;
struct wl_pointer *pointer;

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t sx, wl_fixed_t sy) {}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface) {}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
                      uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {}

static void create_window(struct window* window, int popup);
static void paint_pixels(struct window* window);
static struct wl_buffer* create_buffer(struct window* window);

static uint32_t _serial = 0;

static void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                      uint32_t serial, uint32_t time, uint32_t button,
                      uint32_t state)
{
    _serial = serial;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED)
      return;

    struct window* window = NULL;
    uint32_t colour = 0x000000;
    if (!popup_window) {
      fprintf(stderr, "---------popup window\n");
      popup_window = malloc(sizeof(struct window));
      window = popup_window;
      window->colour = 0xFFFF00;
      window->width = 50;
      window->height = 50;
      window->parent = main_window;
    } else if (!nested_popup_window) {
      fprintf(stderr, "---------nested popup window\n");
      nested_popup_window = malloc(sizeof(struct window));
      window = nested_popup_window;
      window->colour = 0xFF0000;
      window->width = 60;
      window->height = 60;
      window->parent = popup_window;
    } else {
      if (nested_popup_window->zxdg_popup) {
        fprintf(stderr, "---------enough windows, destroy topmost popup\n");
        zxdg_popup_v6_destroy(nested_popup_window->zxdg_popup);
        nested_popup_window->zxdg_popup = NULL;
        zxdg_surface_v6_destroy(nested_popup_window->zxdg_surface);
        wl_surface_attach(nested_popup_window->surface, NULL, 0, 0);
        wl_surface_commit(nested_popup_window->surface);
      } else if (!popup_window->buffer2) {
        fprintf(stderr, "Let's create a second buffer now and attach it\n");
        popup_window->buffer2 = create_buffer(popup_window);
      
        popup_window->colour = 0x000000;
        paint_pixels(popup_window);
        
        wl_surface_attach(popup_window->surface, popup_window->buffer2, 0, 0);
        wl_surface_damage(popup_window->surface, 0, 0, popup_window->width, popup_window->height);
        wl_surface_commit(popup_window->surface);
      }
      return;
    }

    create_window(window, 1 /* popup */);
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                    uint32_t time, uint32_t axis, wl_fixed_t value) {}

static const struct wl_pointer_listener pointer_listener = {
  pointer_handle_enter,
  pointer_handle_leave,
  pointer_handle_motion,
  pointer_handle_button,
  pointer_handle_axis,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
                         enum wl_seat_capability caps)
{
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
  	pointer = wl_seat_get_pointer(seat);
  	wl_pointer_add_listener(pointer, &pointer_listener, NULL);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
	  wl_pointer_destroy(pointer);
  	pointer = NULL;
  }
}

static void
seat_handle_name(void *data,
		     struct wl_seat *wl_seat,
		     const char *name) {}

static const struct wl_seat_listener seat_listener = {
  seat_handle_capabilities,
  seat_handle_name,
};

static int
set_cloexec_or_close(int fd)
{
  long flags;

  if (fd == -1)
          return -1;

  flags = fcntl(fd, F_GETFD);
  if (flags == -1)
          goto err;

  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
          goto err;

  return fd;

err:
  close(fd);
  return -1;
}

static int
create_tmpfile_cloexec(char *tmpname)
{
  int fd = -1;
#ifdef HAVE_MKOSTEMP
  fd = mkostemp(tmpname, O_CLOEXEC);
  if (fd >= 0)
    unlink(tmpname);
#else
  fd = mkstemp(tmpname);
  if (fd >= 0) {
    fd = set_cloexec_or_close(fd);
    unlink(tmpname);
  }
#endif
  return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 */

static int id = 1;

int
os_create_anonymous_file(off_t size)
{
  char template[] = "/weston-shared-XXXXXX";
  const char *path;
  char *name;
  int fd;

  path = getenv("XDG_RUNTIME_DIR");
  if (!path) {
    errno = ENOENT;
    return -1;
  }

  name = malloc(strlen(path) + sizeof(template));
  
  if (!name)
    return -1;
  strcpy(name, path);
  strcat(name, template);

  fd = create_tmpfile_cloexec(name);

  free(name);

  if (fd < 0)
    return -1;

  if (ftruncate(fd, size) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static void
paint_pixels(struct window* window) {
  int n;
  uint32_t *pixel = window->shm_data;

  fprintf(stderr, "Painting pixels\n");
  for (n =0; n < window->width*window->height; n++) {
  	*pixel++ = window->colour;
  }
}

static struct wl_buffer*
create_buffer(struct window* window) {
  struct wl_shm_pool *pool;
  int stride = window->width * 4; // 4 bytes per pixel
  int size = stride * window->height;
  int fd;
  struct wl_buffer *buff;

  fd = os_create_anonymous_file(size);
  if (fd < 0) {
  	fprintf(stderr, "creating a buffer file for %d B failed: %m\n",
	          size);
   	exit(1);
  }
  
  window->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (window->shm_data == MAP_FAILED) {
   	fprintf(stderr, "mmap failed: %m\n");
   	close(fd);
   	exit(1);
  }

  pool = wl_shm_create_pool(shm, fd, size);
  buff = wl_shm_pool_create_buffer(pool, 0,
				  window->width, window->height,
				  stride, 	
				  WL_SHM_FORMAT_XRGB8888);
  if (!buff) {
    fprintf(stderr, "creating shm pool buffer failed\n");
  }

  wl_shm_pool_destroy(pool);
  return buff;
}

static struct wl_surface*
create_wl_surface() {
  struct wl_surface* surface = wl_compositor_create_surface(compositor);
  if (surface == NULL) {
   	fprintf(stderr, "Can't create surface\n");
   	exit(1);
  } else {
    fprintf(stderr, "Created surface\n");
  }
  return surface;
}

static void zxdg_surface_configure (void* data,
                                    struct zxdg_surface_v6* zxdg_surface_v6,
                                    uint32_t serial) {
  fprintf(stderr, "Configure surface\n");
  zxdg_surface_v6_ack_configure(zxdg_surface_v6,
                                serial);

  struct window* window = (struct window*)data;
  if (!window) {
    fprintf(stderr, "Something went wrong during configure call\n");
    exit(1);
  }

  if (window->buffer || window->buffer2)
    return;

  window->buffer = create_buffer(window);

  wl_surface_attach(window->surface, window->buffer, 0, 0);
  wl_surface_damage(window->surface, 0, 0, window->width, window->height);
  wl_surface_commit(window->surface);

  paint_pixels(window);
}

static struct zxdg_surface_v6_listener zxdg_surface_v6_listener = {
  zxdg_surface_configure,
};

static void zxdg_toplevel_configure (void *data,
			  struct zxdg_toplevel_v6 *zxdg_toplevel_v6,
			  int32_t width,
			  int32_t height,
			  struct wl_array *states) {
  fprintf(stderr, "Configure toplevel\n");
}

static void zxdg_toplevel_close(void *data,
		      struct zxdg_toplevel_v6 *zxdg_toplevel_v6) {
  fprintf(stderr, "Close toplevel\n");
}

static struct zxdg_toplevel_v6_listener zxdg_toplevel_v6_listener = {
  zxdg_toplevel_configure,
  zxdg_toplevel_close
};

static struct zxdg_surface_v6*
create_zxdg_surface(struct wl_surface* surface) {
  struct zxdg_surface_v6* zxdg_surface = zxdg_shell_v6_get_xdg_surface(
      zxdg_shell, surface);
  if (zxdg_surface == NULL) {
   	fprintf(stderr, "Can't create zxdg surface\n");
   	exit(1);
  } else {
  	fprintf(stderr, "Created zxdg surface\n");
  }

  return zxdg_surface;
}

static struct zxdg_toplevel_v6*
create_zxdg_toplevel(struct zxdg_surface_v6* zxdg_surface) {
  struct zxdg_toplevel_v6* zxdg_toplevel = zxdg_surface_v6_get_toplevel(zxdg_surface);
  if (zxdg_toplevel == NULL) {
   	fprintf(stderr, "Can't create zxdg toplevel\n");
   	exit(1);
  } else {
  	fprintf(stderr, "Created zxdg toplevel\n");
  }
  return zxdg_toplevel;
}

static struct zxdg_positioner_v6*
create_zxdg_positioner(int width, int height) {
  struct zxdg_positioner_v6* positioner = zxdg_shell_v6_create_positioner(zxdg_shell);
  zxdg_positioner_v6_set_anchor_rect(positioner, 20, 20, width, height);
  zxdg_positioner_v6_set_size(positioner, width, height);
  return positioner; 
}

static struct zxdg_popup_v6* 
create_zxdg_popup(struct zxdg_surface_v6* zxdg_surface,
                  struct zxdg_surface_v6* parent_zxdg_surface,
                  struct zxdg_positioner_v6* positioner) {
  struct zxdg_popup_v6* zxdg_popup =
    zxdg_surface_v6_get_popup(zxdg_surface,
                              parent_zxdg_surface,
                              positioner);
  if (!zxdg_popup) {
    fprintf(stderr, "Failed to create zxdg popup\n");
    exit(1);
  } else {
    fprintf(stderr, "Created zxdg popup\n");
  }
  return zxdg_popup;
}

static void
create_window(struct window* window, int popup) {
  window->buffer = NULL;
  window->buffer2 = NULL;

  window->surface = create_wl_surface();
  window->zxdg_surface = create_zxdg_surface(window->surface);
  zxdg_surface_v6_add_listener(window->zxdg_surface, &zxdg_surface_v6_listener, window);
  if (!popup) {
    fprintf(stderr, "Create toplevel\n");
    window->zxdg_toplevel = create_zxdg_toplevel(window->zxdg_surface);
  } else {
    fprintf(stderr, "Create popup\n");
    struct zxdg_positioner_v6* positioner =
        create_zxdg_positioner(window->width, window->height);
    window->zxdg_popup = create_zxdg_popup(window->zxdg_surface,
                                           window->parent->zxdg_surface,
                                           positioner);
    zxdg_positioner_v6_destroy(positioner);
    if (with_grab) {
      fprintf(stderr, "Using grab\n");
      zxdg_popup_v6_grab(window->zxdg_popup, seat, _serial);
    } else {
      fprintf(stderr, "Not using grab\n");
    }
  }

  wl_surface_commit(window->surface);
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
  fprintf(stderr, "Format %d\n", format);
}

struct wl_shm_listener shm_listener = {
	shm_format
};


static void zxdg_v6_ping(void *data, struct zxdg_shell_v6 *shell_v6, uint32_t serial) {
  zxdg_shell_v6_pong(shell_v6, serial);
}

struct zxdg_shell_v6_listener shell_v6_listener = {
  zxdg_v6_ping
};

static void
global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
	       const char *interface, uint32_t version)
{
  if (strcmp(interface, "wl_compositor") == 0) {
    compositor = wl_registry_bind(registry, 
		     id, 
		     &wl_compositor_interface, 
		     version);
  } else if (strcmp(interface, "wl_shell") == 0) {
    shell = wl_registry_bind(registry, id,
                               &wl_shell_interface, version);
  } else if (strcmp(interface, "wl_shm") == 0) {
    shm = wl_registry_bind(registry, id,
                           &wl_shm_interface, version);
    wl_shm_add_listener(shm, &shm_listener, NULL); 
  } else if (strcmp(interface, "wl_seat") == 0) {
    seat = wl_registry_bind(registry, id,
		                       	&wl_seat_interface, 4);
    wl_seat_add_listener(seat, &seat_listener, NULL);
  } else if (strcmp(interface, "zxdg_shell_v6") == 0) {
    zxdg_shell = wl_registry_bind(registry, id,
		                       	&zxdg_shell_v6_interface, version);
    zxdg_shell_v6_add_listener(zxdg_shell, &shell_v6_listener, NULL);
  }
}

static void
global_registry_remover(void *data, struct wl_registry *registry, uint32_t id)
{
  printf("Got a registry losing event for %d\n", id);
}

static const struct wl_registry_listener registry_listener = {
  global_registry_handler,
  global_registry_remover
};


int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Please pass --with-grab or --without-grab\n");
    exit(1);
  }

  if (strcmp("--with-grab", argv[1]) == 0) {
    with_grab = 1;
  } else if (strcmp("--without-grab", argv[1]) == 0) {
    with_grab = 0; 
  } else {
    fprintf(stderr, "Unknown parameter, please pass --with-grab or --without-grab\n");
    exit(1);
  }

  display = wl_display_connect(NULL);
  if (display == NULL) {
  	fprintf(stderr, "Can't connect to display\n");
	  exit(1);
  }
  printf("connected to display\n");

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);

  wl_display_dispatch(display);
  wl_display_roundtrip(display);

  if (compositor == NULL) {
  	fprintf(stderr, "Can't find compositor\n");
  	exit(1);
  } else {
  	fprintf(stderr, "Found compositor\n");
  }

  main_window = malloc(sizeof(struct window));
  main_window->width = 480;
  main_window->height = 360;
  main_window->colour = 0xffff;
  create_window(main_window, 0 /* not popup */);

  while (wl_display_dispatch(display) != -1) {
  	;
  }

  wl_display_disconnect(display);
  printf("disconnected from display\n");

  free(main_window);
    
  exit(0);
}

