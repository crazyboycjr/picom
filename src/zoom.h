// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Jingrong Chen <crazyboycjr@gmail.com>

#pragma once

#include <stdbool.h>

/// Full-desktop zoom: magnify the whole screen around the pointer.
///
/// The zoom level is controlled through the `_PICOM_ZOOM` property on the root
/// window: CARDINAL, format 32, a single value giving the target zoom level in 16.16
/// fixed point (65536 means 1x). A missing property, or a value of at most 65536,
/// means no zoom; values above 16x are clamped to 16x. The zoom level eases
/// towards the target, and the point under the pointer stays where it is.

struct session;
struct renderer_zoom;
struct zoom;

struct zoom *zoom_new(struct session *ps);
void zoom_free(struct zoom *zoom);

/// Read the target zoom level from the root window property.
///
/// @return whether the target changed
bool zoom_update_target(struct zoom *zoom);

/// Advance the zoom animation to the current time. Call once before each frame is
/// drawn.
void zoom_step(struct zoom *zoom);

/// Whether the screen is zoomed, or about to be. The screen must stay redirected
/// while this is true.
bool zoom_is_active(const struct zoom *zoom);

/// Whether the zoom level is still moving towards its target, so more frames are
/// needed.
bool zoom_is_animating(const struct zoom *zoom);

/// Get the parameters for rendering a zoomed frame.
///
/// @return false if the screen is not zoomed, in which case `view` is untouched.
bool zoom_get_view(const struct zoom *zoom, struct renderer_zoom *view);
