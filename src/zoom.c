// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Jingrong Chen <crazyboycjr@gmail.com>

#include "zoom.h"

#include <ev.h>
#include <math.h>
#include <xcb/xproto.h>

#include <picom/types.h>

#include "atom.h"
#include "common.h"
#include "log.h"
#include "picom.h"
#include "renderer/renderer.h"
#include "utils/misc.h"
#include "x.h"

/// The highest zoom level.
static const double ZOOM_MAX = 16;
/// Time constant of the easing from the current zoom level to the target, in seconds.
static const double ZOOM_TIME_CONSTANT = 0.040;
/// The zoom level snaps to the target once it is this close, relatively.
static const double ZOOM_SNAP = 0.001;
/// Interval between two queries of the pointer position while zoomed, in seconds.
static const double ZOOM_POINTER_POLL_INTERVAL = 0.004;

struct zoom {
	struct session *ps;
	/// The zoom level asked for through the root window property.
	double target;
	/// The zoom level on screen. It eases towards `target`.
	double current;
	/// When `current` was last updated, in microseconds. Only meaningful while
	/// animating.
	uint64_t last_step_us;
	/// The latest known pointer position.
	ivec2 pointer;
	/// Polls the pointer position while zoomed. The zoomed view follows the
	/// pointer, but no event tells us the pointer moved: X sends motion events
	/// only to clients that selected them on the window under the pointer.
	ev_timer poll_timer;
	/// The pointer query sent by `poll_timer`.
	struct x_async_request_base poll_request;
	/// Whether `poll_request` is waiting for its reply.
	bool poll_pending;
};

static uint64_t zoom_now_us(void) {
	auto now = get_time_timespec();
	return (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
}

bool zoom_is_active(const struct zoom *zoom) {
	return zoom->target > 1 || zoom->current > 1;
}

bool zoom_is_animating(const struct zoom *zoom) {
	return zoom->current != zoom->target;
}

static void
zoom_handle_pointer_reply(struct x_connection * /*c*/, struct x_async_request_base *req,
                          const xcb_raw_generic_event_t *reply_or_error) {
	auto zoom = container_of(req, struct zoom, poll_request);
	zoom->poll_pending = false;
	if (reply_or_error == NULL || reply_or_error->response_type == 0) {
		// Shutting down, or the query failed.
		return;
	}

	auto reply = (const xcb_query_pointer_reply_t *)reply_or_error;
	ivec2 pointer = {.x = reply->root_x, .y = reply->root_y};
	if (reply->same_screen && !ivec2_eq(pointer, zoom->pointer)) {
		zoom->pointer = pointer;
		queue_redraw(zoom->ps);
	}
}

static void zoom_poll_pointer(EV_P attr_unused, ev_timer *w, int revents attr_unused) {
	auto zoom = container_of(w, struct zoom, poll_timer);
	if (zoom->poll_pending) {
		return;
	}
	auto c = &zoom->ps->c;
	zoom->poll_request.callback = zoom_handle_pointer_reply;
	zoom->poll_request.sequence = xcb_query_pointer(c->c, c->screen_info->root).sequence;
	x_await_request(c, &zoom->poll_request);
	zoom->poll_pending = true;
}

/// Poll the pointer position while the screen is zoomed, and only then.
static void zoom_update_pointer_poll(struct zoom *zoom) {
	bool active = zoom_is_active(zoom);
	if (active == ev_is_active(&zoom->poll_timer)) {
		return;
	}
	if (!active) {
		ev_timer_stop(zoom->ps->loop, &zoom->poll_timer);
		return;
	}

	// The first zoomed frame needs to know where the pointer is.
	auto c = &zoom->ps->c;
	auto r = XCB_AWAIT(xcb_query_pointer, c, c->screen_info->root);
	if (r != NULL) {
		zoom->pointer = (ivec2){.x = r->root_x, .y = r->root_y};
		free(r);
	}
	ev_timer_again(zoom->ps->loop, &zoom->poll_timer);
}

bool zoom_update_target(struct zoom *zoom) {
	auto ps = zoom->ps;
	auto prop = x_get_prop(&ps->c, ps->c.screen_info->root, ps->atoms->a_PICOM_ZOOM,
	                       1L, XCB_ATOM_CARDINAL, 32);
	double target = 1;
	if (prop.nitems > 0) {
		target = prop.c32[0] / 65536.0;
	}
	free_winprop(&prop);
	target = fmin(fmax(target, 1), ZOOM_MAX);

	if (target == zoom->target) {
		return false;
	}
	log_debug("Zoom target changed from %f to %f", zoom->target, target);
	if (!zoom_is_animating(zoom)) {
		zoom->last_step_us = zoom_now_us();
	}
	zoom->target = target;
	zoom_update_pointer_poll(zoom);
	return true;
}

void zoom_step(struct zoom *zoom) {
	if (zoom_is_animating(zoom)) {
		auto now_us = zoom_now_us();
		double dt = (double)(now_us - zoom->last_step_us) / 1e6;
		zoom->last_step_us = now_us;

		// Ease in log space, so zooming in and out by the same factor looks the
		// same at any level.
		double log_target = log(zoom->target);
		double log_current = log(zoom->current);
		log_current +=
		    (log_target - log_current) * (1 - exp(-dt / ZOOM_TIME_CONSTANT));
		zoom->current = exp(log_current);
		if (fabs(zoom->current / zoom->target - 1) < ZOOM_SNAP) {
			zoom->current = zoom->target;
		}
		log_trace("Zoom level %f, target %f", zoom->current, zoom->target);
	}
	zoom_update_pointer_poll(zoom);
}

bool zoom_get_view(const struct zoom *zoom, struct renderer_zoom *view) {
	if (zoom->current <= 1) {
		return false;
	}
	// The pointer position names a pixel, keep the middle of it in place.
	*view = (struct renderer_zoom){
	    .scale = zoom->current,
	    .center = {.x = zoom->pointer.x + 0.5, .y = zoom->pointer.y + 0.5},
	};
	return true;
}

struct zoom *zoom_new(struct session *ps) {
	auto zoom = ccalloc(1, struct zoom);
	zoom->ps = ps;
	zoom->target = 1;
	zoom->current = 1;
	ev_timer_init(&zoom->poll_timer, zoom_poll_pointer, 0, ZOOM_POINTER_POLL_INTERVAL);
	zoom_update_target(zoom);
	// If the screen is already zoomed when we start, e.g. after a reset, show it
	// zoomed right away.
	zoom->current = zoom->target;
	return zoom;
}

void zoom_free(struct zoom *zoom) {
	// The X connection is gone by now, it has called back for any pending request.
	assert(!zoom->poll_pending);
	ev_timer_stop(zoom->ps->loop, &zoom->poll_timer);
	free(zoom);
}
