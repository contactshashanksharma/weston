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

#include <xf86drmMode.h>

#include "compositor.h"
#include "libinput-seat.h"
#include "plugin-registry.h"
#include "drm-color-management.h"

#ifdef  __cplusplus
extern "C" {
#endif

#define WESTON_DRM_BACKEND_CONFIG_VERSION 3

/* EDID parsing */
#define EDID_DESCRIPTOR_ALPHANUMERIC_DATA_STRING	0xfe
#define EDID_DESCRIPTOR_DISPLAY_PRODUCT_NAME		0xfc
#define EDID_DESCRIPTOR_DISPLAY_PRODUCT_SERIAL_NUMBER	0xff
#define EDID_OFFSET_DATA_BLOCKS				0x36
#define EDID_OFFSET_LAST_BLOCK				0x6c
#define EDID_OFFSET_PNPID				0x08
#define EDID_OFFSET_SERIAL				0x0c

#define EDID_BLOCK_LENGTH 				128
#define EDID_CEA_EXT_ID 				0x02
#define EDID_CEA_TAG_EXTENDED 				0x7
#define EDID_CEA_TAG_COLORIMETRY			0x5
#define EDID_CEA_EXT_TAG_STATIC_METADATA 		0x6
#define EDID_CEA_EXT_TAG_DYNAMIC_METADATA 		0x7

#define EDID_COLORIMETRY_BT2020_RGB		(1 << 0x7) /* Byte 2 */
#define EDID_COLORIMETRY_BT2020_YCBCR	(1 << 0x6)
#define EDID_COLORIMETRY_BT2020_CYCBCR	(1 << 0x5)
#define EDID_COLORIMETRY_BT2020	(EDID_COLORIMETRY_BT2020_RGB || \
						EDID_COLORIMETRY_BT2020_YCBCR || \
						EDID_COLORIMETRY_BT2020_CYCBCR)
#define EDID_COLORIMETRY_DCIP3		(1 << 0x7) /* Byte 3 */

/* Utility */
#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof(arr[0]))

/**
 * A small wrapper to print information into the 'drm-backend' debug scope.
 *
 * The following conventions are used to print variables:
 *
 *  - fixed uint32_t values, including Weston object IDs such as weston_output
 *    IDs, DRM object IDs such as CRTCs or properties, and GBM/DRM formats:
 *      "%lu (0x%lx)" (unsigned long) value, (unsigned long) value
 *
 *  - fixed uint64_t values, such as DRM property values (including object IDs
 *    when used as a value):
 *      "%llu (0x%llx)" (unsigned long long) value, (unsigned long long) value
 *
 *  - non-fixed-width signed int:
 *      "%d" value
 *
 *  - non-fixed-width unsigned int:
 *      "%u (0x%x)" value, value
 *
 *  - non-fixed-width unsigned long:
 *      "%lu (0x%lx)" value, value
 *
 * Either the integer or hexadecimal forms may be omitted if it is known that
 * one representation is not useful (e.g. width/height in hex are rarely what
 * you want).
 *
 * This is to avoid implicit widening or narrowing when we use fixed-size
 * types: uint32_t can be resolved by either unsigned int or unsigned long
 * on a 32-bit system but only unsigned int on a 64-bit system, with uint64_t
 * being unsigned long long on a 32-bit system and unsigned long on a 64-bit
 * system. To avoid confusing side effects, we explicitly cast to the widest
 * possible type and use a matching format specifier.
 */
#define drm_debug(b, ...) \
	weston_debug_scope_printf((b)->debug, __VA_ARGS__)

#define MAX_CLONED_CONNECTORS 4

/**
 * aspect ratio info taken from the drmModeModeInfo flag bits 19-22,
 * which should be used to fill the aspect ratio field in weston_mode.
 */
#define DRM_MODE_FLAG_PIC_AR_BITS_POS	19
#ifndef DRM_MODE_FLAG_PIC_AR_MASK
#define DRM_MODE_FLAG_PIC_AR_MASK (0xF << DRM_MODE_FLAG_PIC_AR_BITS_POS)
#endif

/**
 * Represents the values of an enum-type KMS property
 */
struct drm_property_enum_info {
	const char *name; /**< name as string (static, not freed) */
	bool valid; /**< true if value is supported; ignore if false */
	uint64_t value; /**< raw value */
};

/**
 * Holds information on a DRM property, including its ID and the enum
 * values it holds.
 *
 * DRM properties are allocated dynamically, and maintained as DRM objects
 * within the normal object ID space; they thus do not have a stable ID
 * to refer to. This includes enum values, which must be referred to by
 * integer values, but these are not stable.
 *
 * drm_property_info allows a cache to be maintained where Weston can use
 * enum values internally to refer to properties, with the mapping to DRM
 * ID values being maintained internally.
 */
struct drm_property_info {
	const char *name; /**< name as string (static, not freed) */
	uint32_t prop_id; /**< KMS property object ID */
	unsigned int num_enum_values; /**< number of enum values */
	struct drm_property_enum_info *enum_values; /**< array of enum values */
};

/**
 * List of properties attached to DRM planes
 */
enum wdrm_plane_property {
	WDRM_PLANE_TYPE = 0,
	WDRM_PLANE_SRC_X,
	WDRM_PLANE_SRC_Y,
	WDRM_PLANE_SRC_W,
	WDRM_PLANE_SRC_H,
	WDRM_PLANE_CRTC_X,
	WDRM_PLANE_CRTC_Y,
	WDRM_PLANE_CRTC_W,
	WDRM_PLANE_CRTC_H,
	WDRM_PLANE_FB_ID,
	WDRM_PLANE_CRTC_ID,
	WDRM_PLANE_IN_FORMATS,
#if 1
	WDRM_PLANE_DEGAMMA,
	WDRM_PLANE_DEGAMMA_LUT_SZ,
	WDRM_PLANE_CTM,
	WDRM_PLANE_GAMMA,
	WDRM_PLANE_GAMMA_LUT_SZ,
	WDRM_PLANE_COLOR_ENCODING,
	WDRM_PLANE_COLOR_RANGE,
	WDRM_HDR_SUPPORT,
#endif
	WDRM_PLANE__COUNT
};

/**
 * Possible values for the WDRM_PLANE_TYPE property.
 */
enum wdrm_plane_type {
	WDRM_PLANE_TYPE_PRIMARY = 0,
	WDRM_PLANE_TYPE_CURSOR,
	WDRM_PLANE_TYPE_OVERLAY,
	WDRM_PLANE_TYPE__COUNT
};

static struct drm_property_enum_info plane_type_enums[] = {
	[WDRM_PLANE_TYPE_PRIMARY] = {
		.name = "Primary",
	},
	[WDRM_PLANE_TYPE_OVERLAY] = {
		.name = "Overlay",
	},
	[WDRM_PLANE_TYPE_CURSOR] = {
		.name = "Cursor",
	},
};

static const struct drm_property_info plane_props[] = {
	[WDRM_PLANE_TYPE] = {
		.name = "type",
		.enum_values = plane_type_enums,
		.num_enum_values = WDRM_PLANE_TYPE__COUNT,
	},
	[WDRM_PLANE_SRC_X] = { .name = "SRC_X", },
	[WDRM_PLANE_SRC_Y] = { .name = "SRC_Y", },
	[WDRM_PLANE_SRC_W] = { .name = "SRC_W", },
	[WDRM_PLANE_SRC_H] = { .name = "SRC_H", },
	[WDRM_PLANE_CRTC_X] = { .name = "CRTC_X", },
	[WDRM_PLANE_CRTC_Y] = { .name = "CRTC_Y", },
	[WDRM_PLANE_CRTC_W] = { .name = "CRTC_W", },
	[WDRM_PLANE_CRTC_H] = { .name = "CRTC_H", },
	[WDRM_PLANE_FB_ID] = { .name = "FB_ID", },
	[WDRM_PLANE_CRTC_ID] = { .name = "CRTC_ID", },
	[WDRM_PLANE_IN_FORMATS] = { .name = "IN_FORMATS" },
#if 1
	[WDRM_PLANE_DEGAMMA] = { .name = "PLANE_DEGAMMA_LUT"},
	[WDRM_PLANE_DEGAMMA_LUT_SZ] = { .name = "PLANE_DEGAMMA_LUT_SIZE"},
	[WDRM_PLANE_CTM] = { .name = "PLANE_CTM"},
	[WDRM_PLANE_GAMMA] = { .name = "PLANE_GAMMA_LUT"},
	[WDRM_PLANE_GAMMA_LUT_SZ] = { .name = "PLANE_GAMMA_LUT_SIZE"},
	[WDRM_PLANE_COLOR_ENCODING] = { .name = "COLOR_ENCODING"},
	[WDRM_PLANE_COLOR_RANGE] = { .name = "COLOR_RANGE"},
	[WDRM_HDR_SUPPORT] = {.name = "HDR_SUPPORTED"}
#endif
};

/**
 * List of properties attached to a DRM connector
 */
enum wdrm_connector_property {
	WDRM_CONNECTOR_EDID = 0,
	WDRM_CONNECTOR_DPMS,
	WDRM_CONNECTOR_CRTC_ID,
	WDRM_CONNECTOR_NON_DESKTOP,
#ifdef HDR_CONNECTOR
	WDRM_CONNECTOR_HDR_METADATA,
#endif
	WDRM_CONNECTOR__COUNT
};

enum wdrm_dpms_state {
	WDRM_DPMS_STATE_OFF = 0,
	WDRM_DPMS_STATE_ON,
	WDRM_DPMS_STATE_STANDBY, /* unused */
	WDRM_DPMS_STATE_SUSPEND, /* unused */
	WDRM_DPMS_STATE__COUNT
};

static struct drm_property_enum_info dpms_state_enums[] = {
	[WDRM_DPMS_STATE_OFF] = {
		.name = "Off",
	},
	[WDRM_DPMS_STATE_ON] = {
		.name = "On",
	},
	[WDRM_DPMS_STATE_STANDBY] = {
		.name = "Standby",
	},
	[WDRM_DPMS_STATE_SUSPEND] = {
		.name = "Suspend",
	},
};

static const struct drm_property_info connector_props[] = {
	[WDRM_CONNECTOR_EDID] = { .name = "EDID" },
	[WDRM_CONNECTOR_DPMS] = {
		.name = "DPMS",
		.enum_values = dpms_state_enums,
		.num_enum_values = WDRM_DPMS_STATE__COUNT,
	},
	[WDRM_CONNECTOR_CRTC_ID] = { .name = "CRTC_ID", },
	[WDRM_CONNECTOR_NON_DESKTOP] = { .name = "non-desktop", },
#ifdef HDR_CONNECTOR
	[WDRM_CONNECTOR_HDR_METADATA]= { .name = "HDR_METADATA"},
#endif
};

/**
 * List of properties attached to DRM CRTCs
 */
enum wdrm_crtc_property {
	WDRM_CRTC_MODE_ID = 0,
	WDRM_CRTC_ACTIVE,
	WDRM_CRTC_GAMMA_LUT_SIZE,
	WDRM_CRTC_GAMMA_LUT,
	WDRM_CRTC__COUNT
};

static const struct drm_property_info crtc_props[] = {
	[WDRM_CRTC_MODE_ID] = { .name = "MODE_ID", },
	[WDRM_CRTC_ACTIVE] = { .name = "ACTIVE", },
	[WDRM_CRTC_GAMMA_LUT_SIZE] = { .name = "GAMMA_LUT_SIZE"},
	[WDRM_CRTC_GAMMA_LUT] = { .name = "GAMMA_LUT"},
};

/**
 * Mode for drm_output_state_duplicate.
 */
enum drm_output_state_duplicate_mode {
	DRM_OUTPUT_STATE_CLEAR_PLANES, /**< reset all planes to off */
	DRM_OUTPUT_STATE_PRESERVE_PLANES, /**< preserve plane state */
};

/**
 * Mode for drm_pending_state_apply and co.
 */
enum drm_state_apply_mode {
	DRM_STATE_APPLY_SYNC, /**< state fully processed */
	DRM_STATE_APPLY_ASYNC, /**< state pending event delivery */
	DRM_STATE_TEST_ONLY, /**< test if the state can be applied */
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

	struct drm_va_display vd;

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

	int use_pixman;
	bool use_pixman_shadow;

	struct udev_input input;

	int32_t cursor_width;
	int32_t cursor_height;

	uint32_t pageflip_timeout;

	bool shutting_down;

	bool aspect_ratio_supported;

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

struct drm_edid {
	char eisa_id[13];
	char monitor_name[13];
	char pnp_id[5];
	char serial_number[13];
};

/**
 * Pending state holds one or more drm_output_state structures, collected from
 * performing repaint. This pending state is transient, and only lives between
 * beginning a repaint group and flushing the results: after flush, each
 * output state will complete and be retired separately.
 */
struct drm_pending_state {
	struct drm_backend *backend;
	struct wl_list output_list;
};

/*
 * Output state holds the dynamic state for one Weston output, i.e. a KMS CRTC,
 * plus >= 1 each of encoder/connector/plane. Since everything but the planes
 * is currently statically assigned per-output, we mainly use this to track
 * plane state.
 *
 * pending_state is set when the output state is owned by a pending_state,
 * i.e. when it is being constructed and has not yet been applied. When the
 * output state has been applied, the owning pending_state is freed.
 */
struct drm_output_state {
	struct drm_pending_state *pending_state;
	struct drm_output *output;
	struct wl_list link;
	enum dpms_enum dpms;
	uint32_t gamma_size;
	uint32_t gamma_blob_id;
	uint32_t hdr_metadata_blob_id;
	struct wl_list plane_list;
};

/**
 * Plane state holds the dynamic state for a plane: where it is positioned,
 * and which buffer it is currently displaying.
 *
 * The plane state is owned by an output state, except when setting an initial
 * state. See drm_output_state for notes on state object lifetime.
 */
struct drm_plane_state {
	struct drm_plane *plane;
	struct drm_output *output;
	struct drm_output_state *output_state;

	struct drm_fb *fb;

	struct weston_view *ev; /**< maintained for drm_assign_planes only */

	int32_t src_x, src_y;
	uint32_t src_w, src_h;
	int32_t dest_x, dest_y;
	uint32_t dest_w, dest_h;

	uint32_t csc_blob_id;
	uint32_t gamma_blob_id;
	uint32_t degamma_blob_id;

	uint32_t gamma_size;
	uint32_t degamma_size;

	bool complete;

	struct wl_list link; /* drm_output_state::plane_list */
};

enum wdrm_hdr_support {
	WDRM_HDR_NOT_SUPPORTED,
	WDRM_HDR_SUPPORT_STATIC,
	WDRM_HDR_SUPPORT_DYNAMIC,
	WDRM_HDR__COUNT
};

/**
 * A plane represents one buffer, positioned within a CRTC, and stacked
 * relative to other planes on the same CRTC.
 *
 * Each CRTC has a 'primary plane', which use used to display the classic
 * framebuffer contents, as accessed through the legacy drmModeSetCrtc
 * call (which combines setting the CRTC's actual physical mode, and the
 * properties of the primary plane).
 *
 * The cursor plane also has its own alternate legacy API.
 *
 * Other planes are used opportunistically to display content we do not
 * wish to blit into the primary plane. These non-primary/cursor planes
 * are referred to as 'sprites'.
 */
struct drm_plane {
	struct weston_plane base;

	struct drm_backend *backend;

	enum wdrm_plane_type type;

	enum wdrm_hdr_support hdr_support;

	uint32_t possible_crtcs;
	uint32_t plane_id;
	uint32_t count_formats;


	struct drm_property_info props[WDRM_PLANE__COUNT];

	/* The last state submitted to the kernel for this plane. */
	struct drm_plane_state *state_cur;

	struct wl_list link;

	struct {
		uint32_t format;
		uint32_t count_modifiers;
		uint64_t *modifiers;
	} formats[];
};

struct drm_output {
	struct weston_output base;
	struct drm_backend *backend;

	uint32_t crtc_id; /* object ID to pass to DRM functions */
	int pipe; /* index of CRTC in resource array / bitmasks */

	/* Holds the properties for the CRTC */
	struct drm_property_info props_crtc[WDRM_CRTC__COUNT];

	int vblank_pending;
	int page_flip_pending;
	int atomic_complete_pending;
	int destroy_pending;
	int disable_pending;
	int dpms_off_pending;

	struct drm_fb *gbm_cursor_fb[2];
	struct drm_plane *cursor_plane;
	struct weston_view *cursor_view;
	int current_cursor;

	struct gbm_surface *gbm_surface;
	uint32_t gbm_format;

	/* Plane being displayed directly on the CRTC */
	struct drm_plane *scanout_plane;

	/* The last state submitted to the kernel for this CRTC. */
	struct drm_output_state *state_cur;
	/* The previously-submitted state, where the hardware has not
	 * yet acknowledged completion of state_cur. */
	struct drm_output_state *state_last;

	struct drm_fb *dumb[2];
	pixman_image_t *image[2];
	int current_image;
	pixman_region32_t previous_damage;

	struct vaapi_recorder *recorder;
	struct wl_listener recorder_frame_listener;

	struct wl_event_source *pageflip_timer;
};

struct drm_head {
	struct weston_head base;
	struct drm_backend *backend;

	drmModeConnector *connector;
	uint32_t connector_id;
	struct drm_edid edid;

	/* Holds the properties for the connector */
	struct drm_property_info props_conn[WDRM_CONNECTOR__COUNT];

	struct backlight *backlight;
	struct drm_edid_hdr_metadata *hdr_md;
	enum drm_colorspace widest_gamut;

	drmModeModeInfo inherited_mode;	/**< Original mode on the connector */
	uint32_t inherited_crtc_id;	/**< Original CRTC assignment */
};

static const char *const aspect_ratio_as_string[] = {
	[WESTON_MODE_PIC_AR_NONE] = "",
	[WESTON_MODE_PIC_AR_4_3] = " 4:3",
	[WESTON_MODE_PIC_AR_16_9] = " 16:9",
	[WESTON_MODE_PIC_AR_64_27] = " 64:27",
	[WESTON_MODE_PIC_AR_256_135] = " 256:135",
};

#ifndef DRM_CLIENT_CAP_ASPECT_RATIO
#define DRM_CLIENT_CAP_ASPECT_RATIO	4
#endif

#ifndef GBM_BO_USE_CURSOR
#define GBM_BO_USE_CURSOR GBM_BO_USE_CURSOR_64X64
#endif

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

struct drm_head *
to_drm_head(struct weston_head *base);

int
drm_setup_property_blob(struct drm_backend *b,
			uint32_t *blob_id,
			uint32_t blob_size,
			const uint8_t *blob_data);

const uint8_t *
edid_find_cea_extension_block(const uint8_t *edid);

const uint8_t *
edid_find_extended_data_block(const uint8_t *edid,
			uint8_t *data_len,
			uint32_t block_tag);

struct drm_plane_state *
drm_output_state_get_existing_plane(struct drm_output_state *state_output,
				    struct drm_plane *plane);
#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_DRM_H */
