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
#include <va/va_drmcommon.h>
#include <drm/drm_fourcc.h>


#include "compositor.h"
#include "plugin-registry.h"

#ifdef  __cplusplus
extern "C" {
#endif

#define WESTON_DRM_BACKEND_CONFIG_VERSION 3

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

	/* Used by gbm fbs */
	struct gbm_bo *bo;
	struct gbm_surface *gbm_surface;

	/* Used by dumb fbs */
	void *map;
};

/* va-hdr.c */
struct drm_va_display {
	int render_fd;
	int32_t major_ver;
	int32_t minor_ver;
	int32_t width;
	int32_t height;

	VAConfigID cfg_id;
	VAConfigID ctx_id;
	VADisplay va_display;
	VAConfigAttrib attrib;
#if 1
	VABufferID pparam_buf_id;
	VABufferID fparam_buf_id;
	VASurfaceID output_surf_id;
	VAHdrMetaData output_metadata;
	VAHdrMetaDataHDR10 out_md_params;
	VAHdrMetaDataHDR10 in_hdr10_md;
	VAProcPipelineParameterBuffer pparam;
	VAProcFilterParameterBufferHDRToneMapping hdr_tm_param;
#endif
	struct drm_backend *b;
};

/* Made same as kernel structure */
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

/* Monitor's HDR metadata */
struct drm_edid_hdr_metadata_static {
	uint8_t eotf;
	uint8_t metadata_type;
	uint8_t desired_max_ll;
	uint8_t desired_max_fall;
	uint8_t desired_min_ll;
	uint16_t display_primary_r_x;
	uint16_t display_primary_r_y;
	uint16_t display_primary_g_x;
	uint16_t display_primary_g_y;
	uint16_t display_primary_b_x;
	uint16_t display_primary_b_y;
	uint16_t white_point_x;
	uint16_t white_point_y;
};

struct drm_tone_map {
	uint8_t tone_map_mode;
	struct drm_hdr_metadata_static target_md;
};

#define weston_log_hdr(fmt, ...) \
{	\
	if (ev->surface->hdr_metadata)	\
		weston_log_continue(fmt); \
}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

struct drm_format_name {
	uint32_t format;
	char name[128];
};

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010         fourcc_code('P', '0', '1', '0')
#endif

static struct drm_format_name format_names[] = {
	{.format=DRM_FORMAT_C8,.name="FORMAT_C8"},
	{.format=DRM_FORMAT_R8,.name="FORMAT_R8"},
	{.format=DRM_FORMAT_R16,.name="FORMAT_R16"},
	{.format=DRM_FORMAT_RG88,.name="FORMAT_RG88"},
	{.format=DRM_FORMAT_GR88,.name="FORMAT_GR88"},
	{.format=DRM_FORMAT_RG1616,.name="FORMAT_RG1616"},
	{.format=DRM_FORMAT_GR1616,.name="FORMAT_GR1616"},
	{.format=DRM_FORMAT_RGB332,.name="FORMAT_RGB332"},
	{.format=DRM_FORMAT_BGR233,.name="FORMAT_BGR233"},
	{.format=DRM_FORMAT_XRGB4444,.name="FORMAT_XRGB4444"},
	{.format=DRM_FORMAT_XBGR4444,.name="FORMAT_XBGR4444"},
	{.format=DRM_FORMAT_RGBX4444,.name="FORMAT_RGBX4444"},
	{.format=DRM_FORMAT_BGRX4444,.name="FORMAT_BGRX4444"},
	{.format=DRM_FORMAT_ARGB4444,.name="FORMAT_ARGB4444"},
	{.format=DRM_FORMAT_ABGR4444,.name="FORMAT_ABGR4444"},
	{.format=DRM_FORMAT_RGBA4444,.name="FORMAT_RGBA4444"},
	{.format=DRM_FORMAT_BGRA4444,.name="FORMAT_BGRA4444"},
	{.format=DRM_FORMAT_XRGB1555,.name="FORMAT_XRGB1555"},
	{.format=DRM_FORMAT_XBGR1555,.name="FORMAT_XBGR1555"},
	{.format=DRM_FORMAT_RGBX5551,.name="FORMAT_RGBX5551"},
	{.format=DRM_FORMAT_BGRX5551,.name="FORMAT_BGRX5551"},
	{.format=DRM_FORMAT_ARGB1555,.name="FORMAT_ARGB1555"},
	{.format=DRM_FORMAT_ABGR1555,.name="FORMAT_ABGR1555"},
	{.format=DRM_FORMAT_RGBA5551,.name="FORMAT_RGBA5551"},
	{.format=DRM_FORMAT_BGRA5551,.name="FORMAT_BGRA5551"},
	{.format=DRM_FORMAT_RGB565,.name="FORMAT_RGB565"},
	{.format=DRM_FORMAT_BGR565,.name="FORMAT_BGR565"},
	{.format=DRM_FORMAT_RGB888,.name="FORMAT_RGB888"},
	{.format=DRM_FORMAT_BGR888,.name="FORMAT_BGR888"},
	{.format=DRM_FORMAT_XRGB8888,.name="FORMAT_XRGB8888"},
	{.format=DRM_FORMAT_XBGR8888,.name="FORMAT_XBGR8888"},
	{.format=DRM_FORMAT_RGBX8888,.name="FORMAT_RGBX8888"},
	{.format=DRM_FORMAT_BGRX8888,.name="FORMAT_BGRX8888"},
	{.format=DRM_FORMAT_ARGB8888,.name="FORMAT_ARGB8888"},
	{.format=DRM_FORMAT_ABGR8888,.name="FORMAT_ABGR8888"},
	{.format=DRM_FORMAT_RGBA8888,.name="FORMAT_RGBA8888"},
	{.format=DRM_FORMAT_BGRA8888,.name="FORMAT_BGRA8888"},
	{.format=DRM_FORMAT_XRGB2101010,.name="FORMAT_XRGB2101010"},
	{.format=DRM_FORMAT_XBGR2101010,.name="FORMAT_XBGR2101010"},
	{.format=DRM_FORMAT_RGBX1010102,.name="FORMAT_RGBX1010102"},
	{.format=DRM_FORMAT_BGRX1010102,.name="FORMAT_BGRX1010102"},
	{.format=DRM_FORMAT_ARGB2101010,.name="FORMAT_ARGB2101010"},
	{.format=DRM_FORMAT_ABGR2101010,.name="FORMAT_ABGR2101010"},
	{.format=DRM_FORMAT_RGBA1010102,.name="FORMAT_RGBA1010102"},
	{.format=DRM_FORMAT_BGRA1010102,.name="FORMAT_BGRA1010102"},
	{.format=DRM_FORMAT_YUYV,.name="FORMAT_YUYV"},
	{.format=DRM_FORMAT_YVYU,.name="FORMAT_YVYU"},
	{.format=DRM_FORMAT_UYVY,.name="FORMAT_UYVY"},
	{.format=DRM_FORMAT_VYUY,.name="FORMAT_VYUY"},
	{.format=DRM_FORMAT_AYUV,.name="FORMAT_AYUV"},
	{.format=DRM_FORMAT_XRGB8888_A8,.name="FORMAT_XRGB8888_A8"},
	{.format=DRM_FORMAT_XBGR8888_A8,.name="FORMAT_XBGR8888_A8"},
	{.format=DRM_FORMAT_RGBX8888_A8,.name="FORMAT_RGBX8888_A8"},
	{.format=DRM_FORMAT_BGRX8888_A8,.name="FORMAT_BGRX8888_A8"},
	{.format=DRM_FORMAT_RGB888_A8,.name="FORMAT_RGB888_A8"},
	{.format=DRM_FORMAT_BGR888_A8,.name="FORMAT_BGR888_A8"},
	{.format=DRM_FORMAT_RGB565_A8,.name="FORMAT_RGB565_A8"},
	{.format=DRM_FORMAT_BGR565_A8,.name="FORMAT_BGR565_A8"},
	{.format=DRM_FORMAT_NV12,.name="FORMAT_NV12"},
	{.format=DRM_FORMAT_NV21,.name="FORMAT_NV21"},
	{.format=DRM_FORMAT_NV16,.name="FORMAT_NV16"},
	{.format=DRM_FORMAT_NV61,.name="FORMAT_NV61"},
	{.format=DRM_FORMAT_NV24,.name="FORMAT_NV24"},
	{.format=DRM_FORMAT_NV42,.name="FORMAT_NV42"},
	{.format=DRM_FORMAT_YUV410,.name="FORMAT_YUV410"},
	{.format=DRM_FORMAT_YVU410,.name="FORMAT_YVU410"},
	{.format=DRM_FORMAT_YUV411,.name="FORMAT_YUV411"},
	{.format=DRM_FORMAT_YVU411,.name="FORMAT_YVU411"},
	{.format=DRM_FORMAT_YUV420,.name="FORMAT_YUV420"},
	{.format=DRM_FORMAT_YVU420,.name="FORMAT_YVU420"},
	{.format=DRM_FORMAT_YUV422,.name="FORMAT_YUV422"},
	{.format=DRM_FORMAT_YVU422,.name="FORMAT_YVU422"},
	{.format=DRM_FORMAT_YUV444,.name="FORMAT_YUV444"},
	{.format=DRM_FORMAT_YVU444,.name="FORMAT_YVU444"},
	{.format=DRM_FORMAT_P010,.name="FORMAT_P010"},
};

static const char *drm_print_format_name(uint32_t format)
{
	uint32_t i;

	for (i = 0; i < ARRAY_SIZE(format_names); i++)
		if (format == format_names[i].format)
			return format_names[i].name;
	return NULL;
}

/* drm-compositor.c */
const uint8_t *
edid_find_extended_data_block(const uint8_t *edid,
					uint8_t *data_len,
					uint32_t block_tag);
struct drm_fb *
drm_fb_get_from_vasurf(struct drm_va_display *d,
			VADRMPRIMESurfaceDescriptor *va_desc);

/* drm-hdr-metadata.c */
uint32_t
drm_tone_mapping_mode(struct weston_hdr_metadata *content_md,
		struct drm_edid_hdr_metadata_static *target_md);

int
drm_prepare_output_hdr_metadata(struct drm_backend *b,
		struct weston_hdr_metadata *surface_md,
		struct drm_edid_hdr_metadata_static *display_md,
		struct drm_hdr_metadata_static *out_md);

struct drm_edid_hdr_metadata_static *
drm_get_hdr_metadata(const uint8_t *edid, uint32_t edid_len);

void
drm_release_hdr_metadata(struct drm_edid_hdr_metadata_static *md);

struct drm_va_display *drm_va_create_display(struct drm_backend *b);
void drm_va_destroy_display(struct drm_va_display *d);
struct drm_fb *
drm_va_tone_map(struct drm_va_display *d,
		struct drm_fb *fb,
		struct weston_hdr_metadata *content_md,
		struct drm_tone_map *tm);

#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_DRM_H */
