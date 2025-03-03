/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <neatvnc.h>

#include "output.h"
#include "strlcpy.h"

#include "xdg-output-unstable-v1.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void output_transform_coord(const struct output* self,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t* dst_x, uint32_t* dst_y)
{
	switch (self->transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		*dst_x = src_x;
		*dst_y = src_y;
		break;
	case WL_OUTPUT_TRANSFORM_90:
		*dst_x = src_y;
		*dst_y = self->height - src_x;
		break;
	case WL_OUTPUT_TRANSFORM_180:
		*dst_x = self->width - src_x;
		*dst_y = self->height - src_y;
		break;
	case WL_OUTPUT_TRANSFORM_270:
		*dst_x = self->width - src_y;
		*dst_y = src_x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		*dst_x = self->width - src_x;
		*dst_y = src_y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		*dst_x = src_y;
		*dst_y = src_x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*dst_x = src_x;
		*dst_y = self->height - src_y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*dst_x = self->width - src_y;
		*dst_y = self->height - src_x;
		break;
	}
}

void output_transform_box_coord(const struct output* self,
                                uint32_t src_x0, uint32_t src_y0,
                                uint32_t src_x1, uint32_t src_y1,
                                uint32_t* dst_x0, uint32_t* dst_y0,
				uint32_t* dst_x1, uint32_t* dst_y1)
{
	uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

	output_transform_coord(self, src_x0, src_y0, &x0, &y0);
	output_transform_coord(self, src_x1, src_y1, &x1, &y1);

	*dst_x0 = MIN(x0, x1);
	*dst_x1 = MAX(x0, x1);
	*dst_y0 = MIN(y0, y1);
	*dst_y1 = MAX(y0, y1);
}

static bool is_transform_90_degrees(enum wl_output_transform transform)
{
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return true;
	default:
		break;
	}

	return false;
}

uint32_t output_get_transformed_width(const struct output* self)
{
	return is_transform_90_degrees(self->transform)
	      ? self->height : self->width;
}

uint32_t output_get_transformed_height(const struct output* self)
{
	return is_transform_90_degrees(self->transform)
	      ? self->width : self->height;
}

static void output_handle_geometry(void* data, struct wl_output* wl_output,
				   int32_t x, int32_t y, int32_t phys_width,
				   int32_t phys_height, int32_t subpixel,
				   const char* make, const char* model,
				   int32_t transform)
{
	struct output* output = data;

	if (transform != (int32_t)output->transform)
		output->is_transform_changed = true;

	output->x = x;
	output->y = y;
	output->transform = transform;

	strlcpy(output->make, make, sizeof(output->make));
	strlcpy(output->model, model, sizeof(output->model));
}

static void output_handle_mode(void* data, struct wl_output* wl_output,
			       uint32_t flags, int32_t width, int32_t height,
			       int32_t refresh)
{
	struct output* output = data;

	if (!(flags & WL_OUTPUT_MODE_CURRENT))
		return;

	if (width != (int32_t)output->width || height != (int32_t)output->height)
		output->is_dimension_changed = true;

	output->width = width;
	output->height = height;
}

static void output_handle_done(void* data, struct wl_output* wl_output)
{
	struct output* output = data;

	if (output->is_dimension_changed && output->on_dimension_change)
		output->on_dimension_change(output);

	if (output->is_transform_changed && output->on_transform_change)
		output->on_transform_change(output);

	output->is_dimension_changed = false;
	output->is_transform_changed = false;
}

static void output_handle_scale(void* data, struct wl_output* wl_output,
				int32_t factor)
{
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
	.scale = output_handle_scale,
};

void output_destroy(struct output* output)
{
	zxdg_output_v1_destroy(output->xdg_output);
	wl_output_destroy(output->wl_output);
	free(output);
}

void output_list_destroy(struct wl_list* list)
{
	struct output* output;
	struct output* tmp;

	wl_list_for_each_safe(output, tmp, list, link) {
		wl_list_remove(&output->link);
		output_destroy(output);
	}
}

struct output* output_new(struct wl_output* wl_output, uint32_t id)
{
	struct output* output = calloc(1, sizeof(*output));
	if (!output) {
		nvnc_log(NVNC_LOG_ERROR, "OOM");
		return NULL;
	}

	output->wl_output = wl_output;
	output->id = id;

	wl_output_add_listener(output->wl_output, &output_listener,
			output);

	return output;
}

void output_logical_position(void* data, struct zxdg_output_v1* xdg_output,
                             int32_t x, int32_t y)
{
}

void output_logical_size(void* data, struct zxdg_output_v1* xdg_output,
                         int32_t width, int32_t height)
{
}

void output_name(void* data, struct zxdg_output_v1* xdg_output,
                 const char* name)
{
	struct output* self = data;

	strlcpy(self->name, name, sizeof(self->name));
}

void output_description(void* data, struct zxdg_output_v1* xdg_output,
                        const char* description)
{
	struct output* self = data;

	strlcpy(self->description, description, sizeof(self->description));
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = output_logical_position,
	.logical_size = output_logical_size,
	.done = NULL, /* Deprecated */
	.name = output_name,
	.description = output_description,
};

void output_set_xdg_output(struct output* self,
                           struct zxdg_output_v1* xdg_output)
{
	self->xdg_output = xdg_output;

	zxdg_output_v1_add_listener(self->xdg_output, &xdg_output_listener,
	                            self);
}

struct output* output_find_by_id(struct wl_list* list, uint32_t id)
{
	struct output* output;

	wl_list_for_each(output, list, link)
		if (output->id == id)
			return output;

	return NULL;
}

struct output* output_find_by_name(struct wl_list* list, const char* name)
{
	struct output* output;

	wl_list_for_each(output, list, link)
		if (strcmp(output->name, name) == 0)
			return output;

	return NULL;
}

struct output* output_first(struct wl_list* list)
{
	struct output* output;

	wl_list_for_each(output, list, link)
		return output;

	return output;
}

struct output* output_cycle(const struct wl_list* list,
		const struct output* current,
		enum output_cycle_direction direction)
{
	const struct wl_list* iter = current ? &current->link : list;
	iter = (direction == OUTPUT_CYCLE_FORWARD) ?
		iter->next : iter->prev;
	if (iter == list) {
		if (wl_list_empty(list))
			return NULL;
		iter = (direction == OUTPUT_CYCLE_FORWARD) ?
			iter->next : iter->prev;
	}
	struct output* output;
	return wl_container_of(iter, output, link);
}
