/*
 * Copyright © 2019 Intel Corporation
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
#include <va/va.h>

/* CTA-861-G: Electro optical transfer function (EOTF) bitmap */
#define EOTF_ET0_GAMMA_SDR_LUM		(1 << 0)
#define EOTF_ET1_GAMMA_HDR_LUM		(1 << 1)
#define EOTF_ET2_SMPTE_2084_LUM		(1 << 2)
#define EOTF_ET3_HLG_BT_2100_LUM	(1 << 3)

/* CTA-861-G: Static metadata descriptor support bitmap */
#define STATIC_METADATA_TYPE1 (1 << 0)
 
/* Colorspace, in increasing order of width */
enum drm_colorspace {
	DRM_COLORSPACE_UNKNOWN = -1,
	DRM_COLORSPACE_REC_709,
	DRM_COLORSPACE_DCI_P3,
	DRM_COLORSPACE_REC_2020,
	DRM_COLORSPACE_MAX
};

enum drm_hdr_metadata_type {
	DRM_HDR_MD_STATIC = 0,
	DRM_HDR_MD_DYNAMIC,
};

enum drm_hdr_eotf_type {
	DRM_EOTF_SDR_TRADITIONAL,
	DRM_EOTF_HDR_TRADITIONAL,
	DRM_EOTF_HDR_ST2084,
	DRM_EOTF_HLG_BT2100,
	DRM_EOTF_MAX
};

enum drm_hdr_oetf_type {
	DRM_OETF_SDR_TRADITIONAL,
	DRM_OETF_HDR_TRADITIONAL,
	DRM_OETF_HDR_ST2084,
	DRM_OETF_HLG_BT2100,
	DRM_OETF_MAX
};

struct drm_hdr_eotf {
enum drm_hdr_eotf_type type;
	uint16_t display_primary_r_x;
	uint16_t display_primary_r_y;
	uint16_t display_primary_g_x;
	uint16_t display_primary_g_y;
	uint16_t display_primary_b_x;
	uint16_t display_primary_b_y;
	uint16_t white_point_x;
	uint16_t white_point_y;
	uint16_t max_luminance;
	uint16_t min_luminance;
	uint16_t max_cll;
	uint16_t max_fall;
};

/* EDID's hdr static metadata block to parse */
struct drm_edid_hdr_md_static {
	uint8_t eotf;
	uint8_t smd_type_desc;
	uint8_t max_cll;
	uint8_t max_cfall;
	uint8_t min_cll;
	uint16_t display_primary_r_x;
	uint16_t display_primary_r_y;
	uint16_t display_primary_g_x;
	uint16_t display_primary_g_y;
	uint16_t display_primary_b_x;
	uint16_t display_primary_b_y;
	uint16_t white_point_x;
	uint16_t white_point_y;
};

/* EDIDs HDR dynamic metadata for one type */
struct drm_edid_hdr_dynamic_md_block {
	uint8_t blk_data_size;
	uint16_t metadata_type;
	uint8_t *blk_md;
};

/* EDID's hdr dynamic metadata (all type) */
struct drm_edid_hdr_md_dynamic {
	uint8_t size;
	uint8_t num_blks;
	struct drm_edid_hdr_dynamic_md_block **md_blks;
};

struct drm_edid_hdr_metadata {
	enum drm_hdr_metadata_type type;
	struct _metadata {
		struct drm_edid_hdr_md_static *s;
		struct drm_edid_hdr_md_dynamic *d;
	} metadata;
};

struct drm_va_display {
	int drm_fd;
	int32_t major_ver;
	int32_t minor_ver;

	VAConfigID config_id;
	VADisplay va_display;
	VAConfigAttrib attrib;
};

struct drm_output_state;
struct drm_edid_hdr_metadata;
struct drm_plane_state;
struct drm_backend;

/* drm-color-magaement.c */
void
drm_print_hdr_metadata(struct drm_edid_hdr_metadata *md);
int
drm_output_prepare_colorspace(struct drm_output_state *state);
struct drm_edid_hdr_md_static
*drm_get_hdr_static_metadata(const uint8_t *hdr_db,
			uint32_t data_len);
struct drm_edid_hdr_dynamic_md_block
*drm_get_hdr_dynamic_metadata_block(const uint8_t *hdr_db,
			uint8_t *data_len);
void
drm_release_hdr_metadata(struct drm_edid_hdr_metadata *md);
struct drm_edid_hdr_metadata *
drm_get_hdr_metadata(const uint8_t *edid, uint32_t data_len);

/* drm-va.c */
int
drm_va_create_display(struct drm_va_display *d);
void
drm_va_destroy_display(struct drm_va_display *d);
struct drm_fb *
drm_va_tone_map(struct drm_backend * b,
			struct drm_plane_state *ps,
			uint32_t tm_type,
			const struct drm_edid_hdr_metadata *target_md);
