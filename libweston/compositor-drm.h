/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2015 Giulio Camuffo
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WESTON_COMPOSITOR_DRM_H
#define WESTON_COMPOSITOR_DRM_H

#include "compositor.h"
#include "plugin-registry.h"

#ifdef  __cplusplus
extern "C" {
#endif

#define WESTON_DRM_BACKEND_CONFIG_VERSION 3

/* Static HDR metadata to be sent to kernel, matches kernel structure */
struct drm_hdr_metadata_static {
	uint8_t eotf;
	uint8_t metadata_type;
	uint16_t primary_r_x;
	uint16_t primary_r_y;
	uint16_t primary_g_x;
	uint16_t primary_g_y;
	uint16_t primary_b_x;
	uint16_t primary_b_y;
	uint16_t white_point_x;
	uint16_t white_point_y;
	uint16_t max_mastering_luminance;
	uint16_t min_mastering_luminance;
	uint16_t max_fall;
	uint16_t max_cll;
};

/* Connector's color correction status */
struct drm_conn_color_state {
	bool changed;
	bool can_handle_hdr;
	bool output_is_hdr;

	uint8_t o_cs;
	uint8_t o_eotf;
	uint32_t hdr_md_blob_id;
	struct drm_hdr_metadata_static o_md;
};

/* Monitor's HDR metadata */
struct drm_edid_hdr_metadata_static {
	uint8_t eotf;
	uint8_t metadata_type;
	uint8_t desired_max_ll;
	uint8_t desired_max_fall;
	uint8_t desired_min_ll;
};

/* Monitor's color primaries */
struct drm_display_color_primaries {
	uint16_t display_primary_r_x;
	uint16_t display_primary_r_y;
	uint16_t display_primary_g_x;
	uint16_t display_primary_g_y;
	uint16_t display_primary_b_x;
	uint16_t display_primary_b_y;
	uint16_t white_point_x;
	uint16_t white_point_y;
};

struct libinput_device;

enum weston_drm_backend_output_mode {
	/** The output is disabled */
	WESTON_DRM_BACKEND_OUTPUT_OFF,
	/** The output will use the current active mode */
	WESTON_DRM_BACKEND_OUTPUT_CURRENT,
	/** The output will use the preferred mode. A modeline can be provided
	 * by setting weston_backend_output_config::modeline in the form of
	 * "WIDTHxHEIGHT" or in the form of an explicit modeline calculated
	 * using e.g. the cvt tool. If a valid modeline is supplied it will be
	 * used, if invalid or NULL the preferred available mode will be used. */
	WESTON_DRM_BACKEND_OUTPUT_PREFERRED,
};

#define WESTON_DRM_OUTPUT_API_NAME "weston_drm_output_api_v1"

struct weston_drm_output_api {
	/** The mode to be used by the output. Refer to the documentation
	 *  of WESTON_DRM_BACKEND_OUTPUT_PREFERRED for details.
	 *
	 * Returns 0 on success, -1 on failure.
	 */
	int (*set_mode)(struct weston_output *output,
			enum weston_drm_backend_output_mode mode,
			const char *modeline);

	/** The pixel format to be used by the output. Valid values are:
	 * - NULL - The format set at backend creation time will be used;
	 * - "xrgb8888";
	 * - "rgb565"
	 * - "xrgb2101010"
	 */
	void (*set_gbm_format)(struct weston_output *output,
			       const char *gbm_format);

	/** The seat to be used by the output. Set to NULL to use the
	 *  default seat.
	 */
	void (*set_seat)(struct weston_output *output,
			 const char *seat);
};

static inline const struct weston_drm_output_api *
weston_drm_output_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor, WESTON_DRM_OUTPUT_API_NAME,
				    sizeof(struct weston_drm_output_api));

	return (const struct weston_drm_output_api *)api;
}

#define WESTON_DRM_VIRTUAL_OUTPUT_API_NAME "weston_drm_virtual_output_api_v1"

struct drm_fb;
typedef int (*submit_frame_cb)(struct weston_output *output, int fd,
			       int stride, struct drm_fb *buffer);

struct weston_drm_virtual_output_api {
	/** Create virtual output.
	 * This is a low-level function, where the caller is expected to wrap
	 * the weston_output function pointers as necessary to make the virtual
	 * output useful. The caller must set up output make, model, serial,
	 * physical size, the mode list and current mode.
	 *
	 * Returns output on success, NULL on failure.
	 */
	struct weston_output* (*create_output)(struct weston_compositor *c,
					       char *name);

	/** Set pixel format same as drm_output set_gbm_format().
	 *
	 * Returns the set format.
	 */
	uint32_t (*set_gbm_format)(struct weston_output *output,
				   const char *gbm_format);

	/** Set a callback to be called when the DRM-backend has drawn a new
	 * frame and submits it for display.
	 * The callback will deliver a buffer to the virtual output's the
	 * owner and assumes the buffer is now reserved for the owner. The
	 * callback is called in virtual output repaint function.
	 * The caller must call buffer_released() and finish_frame().
	 *
	 * The callback parameters are output, FD and stride (bytes) of dmabuf,
	 * and buffer (drm_fb) pointer.
	 * The callback returns 0 on success, -1 on failure.
	 *
	 * The submit_frame_cb callback hook is responsible for closing the fd
	 * if it returns success. One needs to call the buffer release and
	 * finish frame functions if and only if this hook returns success.
	 */
	void (*set_submit_frame_cb)(struct weston_output *output,
				    submit_frame_cb cb);

	/** Get fd for renderer fence.
	 * The returned fence signals when the renderer job has completed and
	 * the buffer is fully drawn.
	 *
	 * Returns fd on success, -1 on failure.
	 */
	int (*get_fence_sync_fd)(struct weston_output *output);

	/** Notify that the caller has finished using buffer */
	void (*buffer_released)(struct drm_fb *fb);

	/** Notify finish frame
	 * This function allows the output repainting mechanism to advance to
	 * the next frame.
	 */
	void (*finish_frame)(struct weston_output *output,
			     struct timespec *stamp,
			     uint32_t presented_flags);
};

static inline const struct weston_drm_virtual_output_api *
weston_drm_virtual_output_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor,
				    WESTON_DRM_VIRTUAL_OUTPUT_API_NAME,
				    sizeof(struct weston_drm_virtual_output_api));
	return (const struct weston_drm_virtual_output_api *)api;
}

/** The backend configuration struct.
 *
 * weston_drm_backend_config contains the configuration used by a DRM
 * backend.
 */
struct weston_drm_backend_config {
	struct weston_backend_config base;

	/** The tty to be used. Set to 0 to use the current tty. */
	int tty;

	/** Whether to use the pixman renderer instead of the OpenGL ES renderer. */
	bool use_pixman;

	/** The seat to be used for input and output.
	 *
	 * If seat_id is NULL, the seat is taken from XDG_SEAT environment
	 * variable. If neither is set, "seat0" is used. The backend will
	 * take ownership of the seat_id pointer and will free it on
	 * backend destruction.
	 */
	char *seat_id;

	/** The pixel format of the framebuffer to be used.
	 *
	 * Valid values are:
	 * - NULL - The default format ("xrgb8888") will be used;
	 * - "xrgb8888";
	 * - "rgb565"
	 * - "xrgb2101010"
	 * The backend will take ownership of the format pointer and will free
	 * it on backend destruction.
	 */
	char *gbm_format;

	/** Callback used to configure input devices.
	 *
	 * This function will be called by the backend when a new input device
	 * needs to be configured.
	 * If NULL the device will use the default configuration.
	 */
	void (*configure_device)(struct weston_compositor *compositor,
				 struct libinput_device *device);

	/** Maximum duration for a pageflip event to arrive, after which the
	 * compositor will consider the DRM driver crashed and will try to exit
	 * cleanly.
	 *
	 * It is exprimed in milliseconds, 0 means disabled. */
	uint32_t pageflip_timeout;

	/** Specific DRM device to open
	 *
	 * A DRM device name, like "card0", to open. If NULL, use heuristics
	 * based on seat names and boot_vga to find the right device.
	 */
	char *specific_device;

	/** Use shadow buffer if using Pixman-renderer. */
	bool use_pixman_shadow;
};

/* drm-hdr-metadata.c */
struct drm_edid_hdr_metadata_static *
drm_get_display_hdr_metadata(const uint8_t *edid, uint32_t edid_len);

uint16_t
drm_get_display_clrspace(const uint8_t *edid, uint32_t edid_len);

void
drm_get_color_primaries(struct drm_display_color_primaries *p,
		const uint8_t *edid);

void
drm_release_hdr_metadata(struct drm_edid_hdr_metadata_static *md);

#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_DRM_H */
