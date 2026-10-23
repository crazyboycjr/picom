// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
#include <xcb/sync.h>

#include <picom/types.h>

struct renderer;
struct layout_manager;
struct backend_base;
struct command_builder;
struct shader_info;
typedef struct image_handle *image_handle;
struct x_monitors;
struct wm;
struct win_option;
typedef struct pixman_region32 region_t;
typedef struct pixman_box32 rect_t;

/// Magnification applied to a frame when it is presented.
struct renderer_zoom {
	/// The zoom level, greater than 1.
	double scale;
	/// The point of the screen that stays in place, in screen coordinates.
	vec2 center;
	/// How the screen is magnified.
	enum scale_filter filter;
};

void renderer_free(struct backend_base *backend, struct renderer *r);
struct renderer *
renderer_new(struct backend_base *backend, double shadow_radius, bool dithered_present);
/// Render a frame and present it.
///
/// @param zoom magnification of the presented frame, NULL for none.
bool renderer_render(struct renderer *r, struct backend_base *backend,
                     image_handle root_image, const rect_t *root_image_extent,
                     struct layout_manager *lm, struct command_builder *cb,
                     void *blur_context, uint64_t render_start_us,
                     xcb_sync_fence_t xsync_fence, bool use_damage, bool monitor_repaint,
                     bool force_blend, bool blur_frame, bool inactive_dim_fixed,
                     double max_brightness, const struct x_monitors *monitors,
                     const struct shader_info *root_pixmap_shader,
                     const struct shader_info *shaders, const struct renderer_zoom *zoom,
                     uint64_t *after_damage_us);
