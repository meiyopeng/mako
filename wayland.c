#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "mako.h"
#include "notification.h"
#include "render.h"
#include "wayland.h"

static void noop() {
	// This space intentionally left blank
}


static void xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output,
		const char *name) {
	struct mako_output *output = data;
	output->name = strdup(name);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = noop,
	.logical_size = noop,
	.done = noop,
	.name = xdg_output_handle_name,
	.description = noop,
};

static void get_xdg_output(struct mako_output *output) {
	if (output->state->xdg_output_manager == NULL ||
			output->xdg_output != NULL) {
		return;
	}

	output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
		output->state->xdg_output_manager, output->wl_output);
	zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener,
		output);
}

static void create_output(struct mako_state *state,
		struct wl_output *wl_output, uint32_t global_name) {
	struct mako_output *output = calloc(1, sizeof(struct mako_output));
	if (output == NULL) {
		fprintf(stderr, "allocation failed\n");
		return;
	}
	output->state = state;
	output->global_name = global_name;
	output->wl_output = wl_output;
	wl_list_insert(&state->outputs, &output->link);

	get_xdg_output(output);
}

static void destroy_output(struct mako_output *output) {
	wl_list_remove(&output->link);
	if (output->xdg_output != NULL) {
		zxdg_output_v1_destroy(output->xdg_output);
	}
	wl_output_destroy(output->wl_output);
	free(output->name);
	free(output);
}


static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct mako_pointer *pointer = data;
	pointer->x = wl_fixed_to_int(surface_x);
	pointer->y = wl_fixed_to_int(surface_y);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button,
		uint32_t button_state) {
	struct mako_pointer *pointer = data;
	struct mako_state *state = pointer->state;

	struct mako_notification *notif;
	wl_list_for_each(notif, &state->notifications, link) {
		if (hotspot_at(&notif->hotspot, pointer->x, pointer->y)) {
			notification_handle_button(notif, button, button_state);
			return;
		}
	}

	send_frame(state);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = noop,
	.leave = noop,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = noop,
};

static void create_pointer(struct mako_state *state,
		struct wl_pointer *wl_pointer) {
	struct mako_pointer *pointer = calloc(1, sizeof(struct mako_pointer));
	if (pointer == NULL) {
		fprintf(stderr, "allocation failed\n");
		return;
	}
	pointer->state = state;
	pointer->wl_pointer = wl_pointer;
	wl_pointer_add_listener(wl_pointer, &pointer_listener, pointer);
	wl_list_insert(&state->pointers, &pointer->link);
}

static void destroy_pointer(struct mako_pointer *pointer) {
	wl_list_remove(&pointer->link);
	wl_pointer_destroy(pointer->wl_pointer);
	free(pointer);
}


static void seat_handle_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities) {
	struct mako_state *state = data;

	if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
		struct wl_pointer *wl_pointer = wl_seat_get_pointer(seat);
		create_pointer(state, wl_pointer);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
};


static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct mako_state *state = data;

	state->configured = true;
	state->width = width;
	state->height = height;

	zwlr_layer_surface_v1_ack_configure(surface, serial);
	send_frame(state);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct mako_state *state = data;
	state->event_loop.running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};


static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct mako_state *state = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 3);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, state);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output *output =
			wl_registry_bind(registry, name, &wl_output_interface, 1);
		create_output(state, output, name);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0 &&
			version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
		state->xdg_output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface,
			ZXDG_OUTPUT_V1_NAME_SINCE_VERSION);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct mako_state *state = data;

	struct mako_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &state->outputs, link) {
		if (output->global_name == name) {
			destroy_output(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

bool init_wayland(struct mako_state *state) {
	wl_list_init(&state->pointers);
	wl_list_init(&state->outputs);

	state->display = wl_display_connect(NULL);

	state->registry = wl_display_get_registry(state->display);
	wl_registry_add_listener(state->registry, &registry_listener, state);
	wl_display_roundtrip(state->display);

	if (state->compositor == NULL) {
		fprintf(stderr, "compositor doesn't support wl_compositor\n");
		return false;
	}
	if (state->shm == NULL) {
		fprintf(stderr, "compositor doesn't support wl_shm\n");
		return false;
	}
	if (state->layer_shell == NULL) {
		fprintf(stderr, "compositor doesn't support zwlr_layer_shell_v1\n");
		return false;
	}

	if (state->xdg_output_manager != NULL) {
		struct mako_output *output;
		wl_list_for_each(output, &state->outputs, link) {
			get_xdg_output(output);
		}
		wl_display_roundtrip(state->display);
	}
	if (state->xdg_output_manager == NULL &&
			strcmp(state->config.output, "") != 0) {
		fprintf(stderr, "warning: configured an output but compositor doesn't "
			"support xdg-output-unstable-v1 version 2\n");
	}

	return true;
}

void finish_wayland(struct mako_state *state) {
	if (state->layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(state->layer_surface);
	}
	if (state->surface != NULL) {
		wl_surface_destroy(state->surface);
	}
	finish_buffer(&state->buffers[0]);
	finish_buffer(&state->buffers[1]);

	struct mako_pointer *pointer, *pointer_tmp;
	wl_list_for_each_safe(pointer, pointer_tmp, &state->pointers, link) {
		destroy_pointer(pointer);
	}

	struct mako_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &state->outputs, link) {
		destroy_output(output);
	}

	if (state->xdg_output_manager != NULL) {
		zxdg_output_manager_v1_destroy(state->xdg_output_manager);
	}
	zwlr_layer_shell_v1_destroy(state->layer_shell);
	wl_compositor_destroy(state->compositor);
	wl_shm_destroy(state->shm);
	wl_registry_destroy(state->registry);
	wl_display_disconnect(state->display);
}

static struct wl_region *get_input_region(struct mako_state *state) {
	struct wl_region *region =
		wl_compositor_create_region(state->compositor);

	struct mako_notification *notif;
	wl_list_for_each(notif, &state->notifications, link) {
		struct mako_hotspot *hotspot = &notif->hotspot;
		wl_region_add(region, hotspot->x, hotspot->y,
			hotspot->width, hotspot->height);
	}

	return region;
}

static struct mako_output *get_configured_output(struct mako_state *state) {
	const char *output_name = state->config.output;
	if (strcmp(output_name, "") == 0) {
		return NULL;
	}

	struct mako_output *output;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->name != NULL && strcmp(output->name, output_name) == 0) {
			return output;
		}
	}

	return NULL;
}

void send_frame(struct mako_state *state) {
	state->current_buffer = get_next_buffer(state->shm, state->buffers,
		state->width, state->height);

	struct mako_output *output = get_configured_output(state);
	int height = render(state, state->current_buffer);

	if (height == 0 || state->layer_surface_output != output) {
		if (state->layer_surface != NULL) {
			zwlr_layer_surface_v1_destroy(state->layer_surface);
			state->layer_surface = NULL;
		}
		if (state->surface != NULL) {
			wl_surface_destroy(state->surface);
			state->surface = NULL;
		}
		state->width = state->height = 0;
		state->configured = false;
	}

	if (height == 0) {
		return; // nothing to render
	}

	if (state->layer_surface == NULL) {
		struct wl_output *wl_output = NULL;
		if (output != NULL) {
			wl_output = output->wl_output;
		}
		state->layer_surface_output = output;

		state->surface = wl_compositor_create_surface(state->compositor);
		state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			state->layer_shell, state->surface, wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_TOP, "notifications");
		zwlr_layer_surface_v1_add_listener(state->layer_surface,
			&layer_surface_listener, state);

		struct mako_config *config = &state->config;
		zwlr_layer_surface_v1_set_size(state->layer_surface,
			config->width, height);
		zwlr_layer_surface_v1_set_anchor(state->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_margin(state->layer_surface,
			config->margin.top, config->margin.right, config->margin.bottom,
			config->margin.left);
		wl_surface_commit(state->surface);
		return;
	}

	if (!state->configured) {
		return;
	}

	// TODO: if the compositor doesn't send a configure with the size we
	// requested, we'll enter an infinite loop
	if (state->height != height) {
		zwlr_layer_surface_v1_set_size(state->layer_surface,
			state->config.width, height);
		wl_surface_commit(state->surface);
		return;
	}

	struct wl_region *input_region = get_input_region(state);
	wl_surface_set_input_region(state->surface, input_region);
	wl_region_destroy(input_region);

	wl_surface_attach(state->surface, state->current_buffer->buffer, 0, 0);
	wl_surface_damage(state->surface, 0, 0, state->width, state->height);
	wl_surface_commit(state->surface);
}