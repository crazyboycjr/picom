// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include "renderer.h"

#include <inttypes.h>
#include <math.h>
#include <picom/backend.h>
#include <test.h>
#include <xcb/xcb_aux.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "command_builder.h"
#include "damage.h"
#include "layout.h"
#include "picom.h"
#include "utils/dynarr.h"
#include "utils/misc.h"

struct renderer {
	/// Intermediate image to hold what will be presented to the back buffer.
	image_handle back_image;
	/// 1x1 white image
	image_handle white_image;
	/// 1x1 black image
	image_handle black_image;
	/// 1x1 image with the monitor repaint color
	image_handle monitor_repaint_pixel;
	/// Copy of back images before they were tainted by monitor repaint
	image_handle *monitor_repaint_copy;
	/// Regions painted over by monitor repaint
	region_t *monitor_repaint_region;
	/// Copy of the entire back buffer
	image_handle *back_buffer_copy;
	/// Current frame index in ring buffer
	int frame_index;
	int max_buffer_age;
	ivec2 canvas_size;
	/// Format to use for back_image and intermediate images
	enum backend_image_format format;
	int shadow_radius;
	void *shadow_blur_context;
	struct conv *shadow_kernel;

	/// A dynarr of region_t for storing culled masks
	region_t *culled_masks;

	/// The part of `back_image` visible in a zoomed frame, copied out so the zoomed
	/// blit works with small coordinates. Only allocated while zooming.
	image_handle zoom_image;
	/// For how many more frames the whole screen must be redrawn, because back
	/// buffers may still hold zoomed pixels.
	int zoom_cooldown;
};

void renderer_free(struct backend_base *backend, struct renderer *r) {
	if (r->white_image) {
		backend->ops.release_image(backend, r->white_image);
	}
	if (r->black_image) {
		backend->ops.release_image(backend, r->black_image);
	}
	if (r->back_image) {
		backend->ops.release_image(backend, r->back_image);
	}
	if (r->zoom_image) {
		backend->ops.release_image(backend, r->zoom_image);
	}
	if (r->monitor_repaint_pixel) {
		backend->ops.release_image(backend, r->monitor_repaint_pixel);
	}
	if (r->shadow_blur_context) {
		backend->ops.destroy_blur_context(backend, r->shadow_blur_context);
	}
	if (r->shadow_kernel) {
		free_conv(r->shadow_kernel);
	}
	if (r->monitor_repaint_region) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			pixman_region32_fini(&r->monitor_repaint_region[i]);
		}
		free(r->monitor_repaint_region);
	}
	if (r->monitor_repaint_copy) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			backend->ops.release_image(backend, r->monitor_repaint_copy[i]);
		}
		free(r->monitor_repaint_copy);
	}
	dynarr_free(r->culled_masks, pixman_region32_fini);
	free(r);
}

static bool renderer_init(struct renderer *renderer, struct backend_base *backend,
                          double shadow_radius, bool dithered_present) {
	auto has_high_precision =
	    backend->ops.is_format_supported(backend, BACKEND_IMAGE_FORMAT_PIXMAP_HIGH);
	renderer->format = has_high_precision && dithered_present
	                       ? BACKEND_IMAGE_FORMAT_PIXMAP_HIGH
	                       : BACKEND_IMAGE_FORMAT_PIXMAP;
	renderer->back_image = NULL;
	renderer->white_image =
	    backend->ops.new_image(backend, renderer->format, (ivec2){1, 1});
	if (!renderer->white_image || !backend->ops.clear(backend, renderer->white_image,
	                                                  (struct color){1, 1, 1, 1})) {
		return false;
	}
	renderer->black_image =
	    backend->ops.new_image(backend, renderer->format, (ivec2){1, 1});
	if (!renderer->black_image || !backend->ops.clear(backend, renderer->black_image,
	                                                  (struct color){0, 0, 0, 1})) {
		return false;
	}
	renderer->canvas_size = (ivec2){0, 0};
	if (shadow_radius > 0) {
		struct gaussian_blur_args args = {
		    .size = (int)shadow_radius,
		    .deviation = gaussian_kernel_std_for_size(shadow_radius, 0.5 / 256.0),
		};
		renderer->shadow_blur_context = backend->ops.create_blur_context(
		    backend, BLUR_METHOD_GAUSSIAN, BACKEND_IMAGE_FORMAT_MASK,
		    (struct blur_args *)&args);
		if (!renderer->shadow_blur_context) {
			log_error("Failed to create shadow blur context");
			return false;
		}
		renderer->shadow_radius = (int)shadow_radius;
		renderer->shadow_kernel = gaussian_kernel_autodetect_deviation(shadow_radius);
		if (!renderer->shadow_kernel) {
			log_error("Failed to create common shadow context");
			return false;
		}
		sum_kernel_preprocess(renderer->shadow_kernel);
	}
	renderer->max_buffer_age = backend->ops.max_buffer_age(backend) + 1;
	renderer->culled_masks = dynarr_new(region_t, 0);
	return true;
}

struct renderer *
renderer_new(struct backend_base *backend, double shadow_radius, bool dithered_present) {
	auto renderer = ccalloc(1, struct renderer);
	if (!renderer_init(renderer, backend, shadow_radius, dithered_present)) {
		renderer_free(backend, renderer);
		return NULL;
	}

	return renderer;
}

static inline bool
renderer_set_root_size(struct renderer *r, struct backend_base *backend, ivec2 root_size) {
	if (r->canvas_size.width == root_size.width &&
	    r->canvas_size.height == root_size.height) {
		return true;
	}
	if (r->back_image) {
		backend->ops.release_image(backend, r->back_image);
	}
	if (r->zoom_image) {
		backend->ops.release_image(backend, r->zoom_image);
		r->zoom_image = NULL;
	}
	if (r->back_buffer_copy) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			backend->ops.release_image(backend, r->back_buffer_copy[i]);
		}
		free(r->back_buffer_copy);
		r->back_buffer_copy = NULL;
	}
	if (r->monitor_repaint_copy) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			backend->ops.release_image(backend, r->monitor_repaint_copy[i]);
		}
		free(r->monitor_repaint_copy);
		r->monitor_repaint_copy = NULL;
	}
	r->back_image = backend->ops.new_image(backend, r->format, root_size);
	if (r->back_image != NULL) {
		r->canvas_size = root_size;
		return true;
	}
	r->canvas_size = (ivec2){0, 0};
	return false;
}

static bool
renderer_bind_mask(struct renderer *r, struct backend_base *backend, struct win *w) {
	ivec2 size = {.width = w->widthb, .height = w->heightb};
	bool succeeded = false;
	auto image = backend->ops.new_image(backend, BACKEND_IMAGE_FORMAT_MASK, size);
	if (!image || !backend->ops.clear(backend, image, (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto err;
	}

	auto bound_region_local = win_get_bounding_shape_global_by_val(w);
	pixman_region32_translate(&bound_region_local, -w->g.x, -w->g.y);
	succeeded = backend->ops.copy_area(backend, (ivec2){0, 0}, (image_handle)image,
	                                   r->white_image, &bound_region_local);
	pixman_region32_fini(&bound_region_local);
	if (!succeeded) {
		log_error("Failed to fill the mask");
		goto err;
	}
	w->mask_image = image;
	image = NULL;

err:
	if (image != NULL) {
		backend->ops.release_image(backend, image);
	}
	return succeeded;
}

image_handle
renderer_shadow_mask_from_shape_mask(struct renderer *r, struct backend_base *backend,
                                     image_handle mask, unsigned int corner_radius,
                                     ivec2 mask_size) {
	image_handle normalized_mask_image = NULL;
	bool succeeded = false;
	int radius = r->shadow_radius;

	log_trace("Generating shadow from mask, mask %p", mask);

	// Apply the properties on the mask image and blit the result into a larger
	// image, each side larger by `2 * radius` so there is space for blurring.
	normalized_mask_image = backend->ops.new_image(
	    backend, BACKEND_IMAGE_FORMAT_MASK,
	    (ivec2){mask_size.width + 2 * radius, mask_size.height + 2 * radius});
	if (!normalized_mask_image || !backend->ops.clear(backend, normalized_mask_image,
	                                                  (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto err;
	}
	{
		region_t target_mask;
		struct backend_mask_image mask_args = {
		    .image = mask,
		    .origin = {0, 0},
		    .corner_radius = corner_radius,
		    .inverted = false,
		};
		struct backend_blit_args args = {
		    .source_image = r->white_image,
		    .source_mask = &mask_args,
		    .target_mask = &target_mask,
		    .shader = NULL,
		    .color_inverted = false,
		    .effective_size = mask_size,
		    .tint = {1, 1, 1, 1},
		    .scale = SCALE_IDENTITY,
		    .corner_radius = 0,
		    .border_width = 0,
		    .max_brightness = 1,
		};
		pixman_region32_init_rect(&target_mask, radius, radius,
		                          (unsigned)mask_size.width,
		                          (unsigned)mask_size.height);
		succeeded = backend->ops.blit(backend, (ivec2){radius, radius},
		                              normalized_mask_image, &args);
		pixman_region32_fini(&target_mask);
		if (!succeeded) {
			log_error("Failed to blit for shadow generation");
			goto err;
		}
	}
	// Then we blur the normalized mask image
	if (r->shadow_blur_context != NULL) {
		region_t target_mask;
		struct backend_blur_args args = {
		    .source_image = normalized_mask_image,
		    .target_mask = &target_mask,
		    .opacity = 1,
		    .blur_context = r->shadow_blur_context,
		};
		pixman_region32_init_rect(&target_mask, 0, 0,
		                          (unsigned)(mask_size.width + 2 * radius),
		                          (unsigned)(mask_size.height + 2 * radius));
		succeeded =
		    backend->ops.blur(backend, (ivec2){0, 0}, normalized_mask_image, &args);
		pixman_region32_fini(&target_mask);
		if (!succeeded) {
			log_error("Failed to blur for shadow generation");
			goto err;
		}
	}

	return normalized_mask_image;

err:
	log_error("Failed to create shadow mask");
	if (normalized_mask_image) {
		backend->ops.release_image(backend, normalized_mask_image);
	}
	return NULL;
}

static bool
renderer_bind_shadow(struct renderer *r, struct backend_base *backend, struct win *w) {
	if (backend->ops.quirks(backend) & BACKEND_QUIRK_SLOW_BLUR) {
		ivec2 shadow_size;
		int shadow_stride;
		uint8_t *shadow_pixels =
		    make_shadow(backend->c, r->shadow_kernel,
		                (ivec2){.width = w->widthb, .height = w->heightb},
		                &shadow_size, &shadow_stride);
		if (!shadow_pixels) {
			log_error("Couldn't generate shadow");
			return false;
		}

		w->shadow_mask = backend->ops.new_image_from_pixels(
		    backend, BACKEND_IMAGE_FORMAT_MASK, shadow_size, shadow_stride,
		    shadow_pixels);
		free(shadow_pixels);
	} else {
		if (!w->mask_image && !renderer_bind_mask(r, backend, w)) {
			return false;
		}
		w->shadow_mask = renderer_shadow_mask_from_shape_mask(
		    r, backend, w->mask_image, win_options(w).corner_radius,
		    (ivec2){.width = w->widthb, .height = w->heightb});
	}
	if (!w->shadow_mask) {
		log_error("Failed to create shadow");
		return false;
	}

	return true;
}

/// Go through the list of commands and replace symbolic image references with real
/// images. Allocate images for windows when necessary.
static bool renderer_prepare_commands(struct renderer *r, struct backend_base *backend,
                                      void *blur_context, struct layout *layout) {
	auto end = &layout->commands[layout->number_of_commands];
	auto cmds = layout->commands;
	// These assertions are the limitation of this renderer. If we expand its
	// capabilities, we might remove these.
	assert(cmds[0].source == BACKEND_COMMAND_SOURCE_CLEAR);
	assert(cmds[0].op == BACKEND_COMMAND_COPY_AREA);
	cmds[0].copy_area.source_image = r->black_image;

	auto layer = layout->layers - 1;
	auto layer_end = &cmds[layout->first_layer_start];
	for (auto cmd = layer_end; cmd != end; cmd++) {
		if (cmd == layer_end) {
			layer += 1;
			assert(layer->number_of_commands > 0);
			layer_end = cmd + layer->number_of_commands;
			log_trace("Prepare commands for layer %#010x @ %#010x (%s)",
			          win_id(layer->win), win_client_id(layer->win, false),
			          layer->win->name);
		}

		auto w = layer->win;
		switch (cmd->op) {
		case BACKEND_COMMAND_BLIT:
			if (cmd->source == BACKEND_COMMAND_SOURCE_SHADOW) {
				if (w->shadow_mask == NULL &&
				    !renderer_bind_shadow(r, backend, w)) {
					log_error("failed to bind shadow for window %s",
					          w->name);
					return false;
				}
				cmd->blit.source_image = w->shadow_mask;
			} else if (cmd->source == BACKEND_COMMAND_SOURCE_WINDOW) {
				assert(w->win_image);
				cmd->blit.source_image = w->win_image;
			} else if (cmd->source == BACKEND_COMMAND_SOURCE_WINDOW_SAVED) {
				assert(w->saved_win_image);
				cmd->blit.source_image = w->saved_win_image;
			}
			if (cmd->blit.source_mask != NULL) {
				if (w->mask_image == NULL &&
				    !renderer_bind_mask(r, backend, w)) {
					return false;
				}
				cmd->source_mask.image = w->mask_image;
			}
			break;
		case BACKEND_COMMAND_BLUR:
			cmd->blur.blur_context = blur_context;
			cmd->blur.source_image = r->back_image;
			if (cmd->blur.source_mask != NULL) {
				if (w->mask_image == NULL &&
				    !renderer_bind_mask(r, backend, w)) {
					return false;
				}
				cmd->source_mask.image = w->mask_image;
			}
			break;
		default:
		case BACKEND_COMMAND_COPY_AREA:
		case BACKEND_COMMAND_INVALID: assert(false);
		}
	}
	return true;
}

void renderer_ensure_images_ready(struct renderer *r, struct backend_base *backend,
                                  bool monitor_repaint) {
	if (monitor_repaint) {
		if (!r->monitor_repaint_pixel) {
			r->monitor_repaint_pixel = backend->ops.new_image(
			    backend, BACKEND_IMAGE_FORMAT_PIXMAP, (ivec2){1, 1});
			BUG_ON(!r->monitor_repaint_pixel);
			backend->ops.clear(backend, r->monitor_repaint_pixel,
			                   (struct color){.alpha = 0.5, .red = 0.5});
		}
		if (!r->monitor_repaint_copy) {
			r->monitor_repaint_copy = ccalloc(r->max_buffer_age, image_handle);
			for (int i = 0; i < r->max_buffer_age; i++) {
				r->monitor_repaint_copy[i] = backend->ops.new_image(
				    backend, BACKEND_IMAGE_FORMAT_PIXMAP,
				    (ivec2){.width = r->canvas_size.width,
				            .height = r->canvas_size.height});
				BUG_ON(!r->monitor_repaint_copy[i]);
			}
		}
		if (!r->monitor_repaint_region) {
			r->monitor_repaint_region = ccalloc(r->max_buffer_age, region_t);
			for (int i = 0; i < r->max_buffer_age; i++) {
				pixman_region32_init(&r->monitor_repaint_region[i]);
			}
		}
	}
	if (global_debug_options.consistent_buffer_age && !r->back_buffer_copy) {
		r->back_buffer_copy = ccalloc(r->max_buffer_age, image_handle);
		for (int i = 0; i < r->max_buffer_age; i++) {
			r->back_buffer_copy[i] =
			    backend->ops.new_image(backend, BACKEND_IMAGE_FORMAT_PIXMAP,
			                           (ivec2){.width = r->canvas_size.width,
			                                   .height = r->canvas_size.height});
			BUG_ON(!r->back_buffer_copy[i]);
		}
	}
}

/// Work out how a zoomed frame samples `back_image` along one axis of the screen.
///
/// Only the part of the screen visible when zoomed, `[*crop_start, *crop_end)` with a
/// pixel of margin for the linear filter, gets magnified. Magnifying all of it would
/// put its origin tens of thousands of pixels off screen, beyond the 16 bit
/// coordinates of the X protocol that the xrender backend is bound by.
///
/// The point `center` should stay in place, so a point `q` of the unzoomed screen
/// lands on `(q - center) * scale + center`. The origin of the crop on screen is
/// rounded to whole pixels, then clamped so every screen pixel samples at least half
/// a pixel inside the crop. Otherwise the linear filter would blend in whatever lies
/// beyond the crop, at the screen edges where the crop has no margin.
///
/// @return where the magnified crop goes on the screen
static int
renderer_zoom_axis(int size, double scale, double center, int *crop_start, int *crop_end) {
	center = fmin(fmax(center, 0.5), size - 0.5);
	double view_start = center * (1 - 1 / scale);
	*crop_start = max2(0, (int)floor(view_start) - 1);
	*crop_end = min2(size, (int)ceil(view_start + size / scale) + 1);

	double origin = round(*crop_start * scale - center * (scale - 1));
	double lowest = ceil(size - 0.5 - (*crop_end - *crop_start - 0.5) * scale);
	double highest = floor(-0.5 * (scale - 1));
	// If no integer origin keeps both edges half a pixel in, which happens when
	// scale is barely above 1, lowest is 0 and comes closest.
	return (int)fmax(fmin(origin, highest), lowest);
}

TEST_CASE(renderer_zoom_axis) {
	const int sizes[] = {2, 7, 640, 1080, 1920, 2160, 3840};
	const double scales[] = {1.0001, 1.001, 1.01, 1.5, 2, 2.9, 4, 7.3, 16};
	for (size_t k = 0; k < ARR_SIZE(sizes) * ARR_SIZE(scales); k++) {
		int size = sizes[k / ARR_SIZE(scales)];
		double scale = scales[k % ARR_SIZE(scales)];
		// Where no integer origin keeps both edges in, allow what the fallback
		// is off by.
		double slack = (size - 0.5) * (scale - 1) < 1 ? 0.5 * (scale - 1) : 1e-9;
		for (int p = 0; p < size; p++) {
			double center = p + 0.5;
			int start, end;
			int origin = renderer_zoom_axis(size, scale, center, &start, &end);
			TEST_TRUE(0 <= start && start < end && end <= size);

			// The first and last pixels of the screen sample inside the crop.
			TEST_TRUE((0.5 - origin) / scale >= 0.5 - slack);
			TEST_TRUE((size - 0.5 - origin) / scale <= end - start - 0.5 + slack);

			// The centre stays within a pixel of where it was, and within
			// rounding when the crop has margins and nothing gets clamped.
			double drift = fabs(origin + (center - start) * scale - center);
			TEST_TRUE(drift <= 1 + 1e-9);
			if (start > 0 && end < size) {
				TEST_TRUE(drift <= 0.5 + 1e-9);
			}

			// Coordinates stay small.
			TEST_TRUE(abs(origin) <= 3 * scale);
			TEST_TRUE((end - start) * scale <= size + 4 * scale);
		}
	}
}

/// Magnify `r->back_image` onto the back buffer.
static bool
renderer_present_zoomed(struct renderer *r, struct backend_base *backend,
                        const struct renderer_zoom *zoom, const region_t *screen_region) {
	if (r->zoom_image == NULL) {
		r->zoom_image = backend->ops.new_image(backend, r->format, r->canvas_size);
		if (r->zoom_image == NULL) {
			log_error("Failed to allocate the image for zooming");
			return false;
		}
	}

	ivec2 crop_start, crop_end, origin;
	origin.x = renderer_zoom_axis(r->canvas_size.width, zoom->scale, zoom->center.x,
	                              &crop_start.x, &crop_end.x);
	origin.y = renderer_zoom_axis(r->canvas_size.height, zoom->scale, zoom->center.y,
	                              &crop_start.y, &crop_end.y);
	auto crop_size = ivec2_sub(crop_end, crop_start);

	region_t crop_region;
	pixman_region32_init_rect(&crop_region, 0, 0, (unsigned)crop_size.width,
	                          (unsigned)crop_size.height);
	struct backend_blit_args blit = {
	    .source_image = r->back_image,
	    .target_mask = &crop_region,
	    .effective_size = r->canvas_size,
	    .tint = {1, 1, 1, 1},
	    .max_brightness = 1,
	    .scale = SCALE_IDENTITY,
	};
	bool succeeded =
	    backend->ops.blit(backend, ivec2_neg(crop_start), r->zoom_image, &blit);
	pixman_region32_fini(&crop_region);
	if (!succeeded) {
		return false;
	}

	blit.source_image = r->zoom_image;
	blit.target_mask = screen_region;
	blit.effective_size = crop_size;
	blit.scale = (vec2){zoom->scale, zoom->scale};
	return backend->ops.blit(backend, origin, backend->ops.back_buffer(backend), &blit);
}

/// @return true if a frame is rendered, false if this frame is skipped.
bool renderer_render(struct renderer *r, struct backend_base *backend,
                     image_handle root_image, const rect_t *root_image_extent,
                     struct layout_manager *lm, struct command_builder *cb,
                     void *blur_context, uint64_t render_start_us,
                     xcb_sync_fence_t xsync_fence, bool use_damage, bool monitor_repaint,
                     bool force_blend, bool blur_frame, bool inactive_dim_fixed,
                     double max_brightness, const struct x_monitors *monitors,
                     const struct shader_info *root_pixmap_shader,
                     const struct shader_info *shaders, const struct renderer_zoom *zoom,
                     uint64_t *after_damage_us) {
	if (xsync_fence != XCB_NONE) {
		// Trigger the fence but don't immediately wait on it. Let it run
		// concurrent with our CPU tasks to save time.
		x_set_error_action_abort(
		    backend->c, xcb_sync_trigger_fence(backend->c->c, xsync_fence));
	}
	// TODO(yshui) In some cases we can render directly into the back buffer, and
	// don't need the intermediate back_image. Several conditions need to be met: no
	// dithered present; no blur, with blur we will render areas that's just for blur
	// and can't be presented;
	auto layout = layout_manager_layout(lm, 0);
	if (!renderer_set_root_size(r, backend,
	                            (ivec2){layout->size.width, layout->size.height})) {
		log_error("Failed to allocate back image");
		return false;
	}

	renderer_ensure_images_ready(r, backend, monitor_repaint);

	command_builder_build(cb, layout, force_blend, blur_frame, inactive_dim_fixed,
	                      max_brightness, monitors, root_image, root_image_extent,
	                      root_pixmap_shader, shaders);
	if (log_get_level_tls() <= LOG_LEVEL_TRACE) {
		auto layer = layout->layers - 1;
		auto layer_end = &layout->commands[layout->first_layer_start];
		auto end = &layout->commands[layout->number_of_commands];
		log_trace("Desktop background");
		for (auto i = layout->commands; i != end; i++) {
			if (i == layer_end) {
				layer += 1;
				layer_end += layer->number_of_commands;
				log_trace("Layer for window %#010x @ %#010x (%s)",
				          win_id(layer->win),
				          win_client_id(layer->win, false), layer->win->name);
			}
			log_backend_command(TRACE, *i);
		}
	}
	region_t screen_region, damage_region;
	pixman_region32_init_rect(&screen_region, 0, 0, (unsigned)r->canvas_size.width,
	                          (unsigned)r->canvas_size.height);
	pixman_region32_init(&damage_region);
	pixman_region32_copy(&damage_region, &screen_region);
	ivec2 blur_size = {};
	if (backend->ops.get_blur_size && blur_context) {
		backend->ops.get_blur_size(blur_context, &blur_size.width, &blur_size.height);
	}
	auto buffer_age =
	    (use_damage || monitor_repaint) ? backend->ops.buffer_age(backend) : 0;
	if (zoom != NULL || r->zoom_cooldown > 0) {
		// A zoomed frame shows all of `back_image`, which is only up to date
		// inside the damage: blur makes us render the layers below a blurred
		// window around the damage without the window itself, and monitor
		// repaint leaves its tint behind. After zooming, the back buffers hold
		// zoomed pixels. So render and present everything.
		buffer_age = 0;
	}
	if (buffer_age > 0 && global_debug_options.consistent_buffer_age &&
	    buffer_age < r->max_buffer_age) {
		int past_frame =
		    (r->frame_index + r->max_buffer_age - buffer_age) % r->max_buffer_age;
		region_t region;
		pixman_region32_init_rect(&region, 0, 0, (unsigned)r->canvas_size.width,
		                          (unsigned)r->canvas_size.height);
		backend->ops.copy_area(backend, (ivec2){}, backend->ops.back_buffer(backend),
		                       r->back_buffer_copy[past_frame], &region);
		pixman_region32_fini(&region);
	}
	if (buffer_age > 0 && (unsigned)buffer_age <= layout_manager_max_buffer_age(lm)) {
		layout_manager_damage(lm, (unsigned)buffer_age, blur_size, &damage_region);
	}

	dynarr_resize(r->culled_masks, layout->number_of_commands, pixman_region32_init,
	              pixman_region32_fini);
	commands_cull_with_damage(layout, &damage_region, blur_size, r->culled_masks);

	auto now = get_time_timespec();
	*after_damage_us = (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
	log_trace("Getting damage took %" PRIu64 " us", *after_damage_us - render_start_us);

	if (!renderer_prepare_commands(r, backend, blur_context, layout)) {
		log_error("Failed to prepare render commands");
		return false;
	}

	if (xsync_fence != XCB_NONE) {
		x_set_error_action_abort(
		    backend->c, xcb_sync_await_fence(backend->c->c, 1, &xsync_fence));
		// Making sure the wait is completed by receiving a response from the X
		// server
		xcb_aux_sync(backend->c->c);
		x_set_error_action_abort(
		    backend->c, xcb_sync_reset_fence(backend->c->c, xsync_fence));
	}

	if (backend->ops.prepare) {
		backend->ops.prepare(backend, &layout->commands[0].target_mask);
	}

	if (monitor_repaint && buffer_age <= r->max_buffer_age) {
		// Restore the area of back buffer that was tainted by monitor repaint
		int past_frame =
		    (r->frame_index + r->max_buffer_age - buffer_age) % r->max_buffer_age;
		backend->ops.copy_area(backend, (ivec2){}, backend->ops.back_buffer(backend),
		                       r->monitor_repaint_copy[past_frame],
		                       &r->monitor_repaint_region[past_frame]);
	}

	if (!backend_execute(backend, r->back_image, layout->number_of_commands,
	                     layout->commands)) {
		log_error("Failed to complete execution of the render commands");
		return false;
	}

	if (monitor_repaint) {
		// Keep a copy of un-tainted back image
		backend->ops.copy_area(backend, (ivec2){},
		                       r->monitor_repaint_copy[r->frame_index],
		                       r->back_image, &damage_region);
		pixman_region32_copy(&r->monitor_repaint_region[r->frame_index], &damage_region);

		struct backend_blit_args blit = {
		    .source_image = r->monitor_repaint_pixel,
		    .max_brightness = 1,
		    .tint = {1, 1, 1, 1},
		    .effective_size = r->canvas_size,
		    .source_mask = NULL,
		    .target_mask = &damage_region,
		    .scale = SCALE_IDENTITY,
		};
		log_trace("Blit for monitor repaint");
		backend->ops.blit(backend, (ivec2){}, r->back_image, &blit);
	}

	if (zoom != NULL && renderer_present_zoomed(r, backend, zoom, &screen_region)) {
		r->zoom_cooldown = r->max_buffer_age;
	} else {
		backend->ops.copy_area_quantize(backend, (ivec2){},
		                                backend->ops.back_buffer(backend),
		                                r->back_image, &damage_region);
		if (r->zoom_cooldown > 0) {
			r->zoom_cooldown--;
		}
		if (zoom == NULL && r->zoom_image != NULL) {
			backend->ops.release_image(backend, r->zoom_image);
			r->zoom_image = NULL;
		}
	}

	if (global_debug_options.consistent_buffer_age) {
		region_t region;
		pixman_region32_init_rect(&region, 0, 0, (unsigned)r->canvas_size.width,
		                          (unsigned)r->canvas_size.height);
		backend->ops.copy_area(backend, (ivec2){}, r->back_buffer_copy[r->frame_index],
		                       backend->ops.back_buffer(backend), &region);
		pixman_region32_fini(&region);
	}

	if (backend->ops.present && !backend->ops.present(backend)) {
		log_warn("Failed to present the frame");
	}

	// "Un-cull" the render commands, so later damage calculation using those commands
	// will not use culled regions.
	commands_uncull(layout);

	pixman_region32_fini(&screen_region);
	pixman_region32_fini(&damage_region);

	r->frame_index = (r->frame_index + 1) % r->max_buffer_age;
	return true;
}
