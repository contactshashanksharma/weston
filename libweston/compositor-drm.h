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

#include <va/va.h>
#include <va/va_vpp.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm/drm_fourcc.h>

#include "compositor.h"
#include "libinput-seat.h"
#include "plugin-registry.h"

#ifdef  __cplusplus
extern "C" {
#endif

#define WESTON_DRM_BACKEND_CONFIG_VERSION 3

/* From drm_connector.c */
#define DRM_MODE_COLORIMETRY_DEFAULT 			0
#define DRM_MODE_COLORIMETRY_BT2020_RGB		9
#define DRM_MODE_COLORIMETRY_BT2020_YCC		10
#define DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65		11
#define DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER	12

/* Colorspace bits */
#define EDID_CS_BT2020RGB (1 << 7)
#define EDID_CS_BT2020YCC (1 << 6)
#define EDID_CS_BT2020CYCC (1 << 5)
#define EDID_CS_DCIP3 (1 << 15)
#define EDID_CS_HDR_GAMUT_MASK (EDID_CS_BT2020RGB | \
			EDID_CS_BT2020YCC | \
			EDID_CS_BT2020CYCC | \
			EDID_CS_DCIP3)
#define EDID_CS_HDR_CS_BASIC (EDID_CS_BT2020RGB | \
		EDID_CS_DCIP3 | \
		EDID_CS_BT2020YCC)

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010		 fourcc_code('P', '0', '1', '0')
#endif

/* CTA-861-G: HDR Metadata names and types */
enum drm_hdr_eotf_type {
	DRM_EOTF_SDR_TRADITIONAL = 0,
	DRM_EOTF_HDR_TRADITIONAL,
	DRM_EOTF_HDR_ST2084,
	DRM_EOTF_HLG_BT2100,
	DRM_EOTF_MAX
};

/* Match libva values 1:1 */
enum drm_tone_map_mode {
	DRM_TONE_MAPPING_NONE = 0,
	DRM_TONE_MAPPING_HDR_TO_HDR = 1,
	DRM_TONE_MAPPING_HDR_TO_SDR = 2,
	DRM_TONE_MAPPING_SDR_TO_HDR = 8,
};

enum drm_colorspace {
	DRM_COLORSPACE_INVALID,
	DRM_COLORSPACE_REC709,
	DRM_COLORSPACE_DCIP3,
	DRM_COLORSPACE_REC2020,
	DRM_COLORSPACE_MAX,
};

struct drm_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct udev *udev;
	struct wl_event_source *drm_source;

	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_drm_source;

	struct {
		int id;
		int fd;
		char *filename;
	} drm;
	struct gbm_device *gbm;
	struct wl_listener session_listener;
	uint32_t gbm_format;

	/* we need these parameters in order to not fail drmModeAddFB2()
	 * due to out of bounds dimensions, and then mistakenly set
	 * sprites_are_broken:
	 */
	int min_width, max_width;
	int min_height, max_height;

	struct wl_list plane_list;
	int sprites_are_broken;
	int sprites_hidden;

	void *repaint_data;

	bool state_invalid;

	/* CRTC IDs not used by any enabled output. */
	struct wl_array unused_crtcs;

	int cursors_are_broken;

	bool universal_planes;
	bool atomic_modeset;

	bool use_pixman;
	bool use_pixman_shadow;

	struct udev_input input;

	int32_t cursor_width;
	int32_t cursor_height;

	uint32_t pageflip_timeout;

	bool shutting_down;

	bool aspect_ratio_supported;

	bool fb_modifiers;

	struct weston_debug_scope *debug;
};

enum drm_fb_type {
	BUFFER_INVALID = 0, /**< never used */
	BUFFER_CLIENT, /**< directly sourced from client */
	BUFFER_DMABUF, /**< imported from linux_dmabuf client */
	BUFFER_PIXMAN_DUMB, /**< internal Pixman rendering */
	BUFFER_GBM_SURFACE, /**< internal EGL rendering */
	BUFFER_CURSOR, /**< internal cursor buffer */
};

struct drm_fb {
	enum drm_fb_type type;

	int refcnt;

	uint32_t fb_id, size;
	uint32_t handles[4];
	uint32_t strides[4];
	uint32_t offsets[4];
	int num_planes;
	const struct pixel_format_info *format;
	uint64_t modifier;
	int width, height;
	int fd;
	struct weston_buffer_reference buffer_ref;
	struct weston_buffer_release_reference buffer_release_ref;

	/* Used by gbm fbs */
	struct gbm_bo *bo;
	struct gbm_surface *gbm_surface;

	/* Used by dumb fbs */
	void *map;
};

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

struct drm_tone_map {
	enum drm_tone_map_mode tm_mode;
	struct drm_hdr_metadata_static target_md;
	struct drm_fb *old_fb;
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

struct drm_va_display {
	int render_fd;
	int drm_fd;
	int32_t major_ver;
	int32_t minor_ver;
	int32_t width;
	int32_t height;

	VAConfigID cfg_id;
	VAConfigID ctx_id;
	VADisplay va_display;
	VAConfigAttrib attrib;
	VABufferID pparam_buf_id;
	VABufferID fparam_buf_id;
	VASurfaceID output_surf_id;
	VAHdrMetaData output_metadata;
	VAHdrMetaDataHDR10 out_md_params;
	VAHdrMetaDataHDR10 in_hdr10_md;
	VAProcPipelineParameterBuffer pparam;
	VAProcFilterParameterBufferHDRToneMapping hdr_tm_param;

	struct drm_backend *b;
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

int
drm_fb_addfb(struct drm_backend *b, struct drm_fb *fb);

struct drm_fb *
drm_fb_get_from_vasurf(struct drm_va_display *d,
			VADRMPRIMESurfaceDescriptor *va_desc);

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

uint16_t
color_primary(short val);

/* drm-va-hdr.c */
struct drm_va_display *
drm_va_create_display(struct drm_backend *b);

void
drm_va_destroy_display(struct drm_va_display *d);

struct drm_fb *
drm_va_tone_map(struct drm_va_display *d,
		struct drm_fb *fb,
		struct weston_hdr_metadata *content_md,
		struct drm_tone_map *tm);

#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_DRM_H */
