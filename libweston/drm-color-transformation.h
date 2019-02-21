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
#include <string.h>

/* CEA-861-G new EDID blocks for HDR */
#define EDID_CEA_TAG_COLORIMETRY			0x5
#define EDID_CEA_EXT_TAG_STATIC_METADATA 		0x6
#define EDID_CEA_EXT_TAG_DYNAMIC_METADATA 		0x7

/* CTA-861-G: Electro optical transfer function (EOTF) bitmap */
#define EOTF_ET0_GAMMA_SDR_LUM		(1 << 0)
#define EOTF_ET1_GAMMA_HDR_LUM		(1 << 1)
#define EOTF_ET2_SMPTE_2084_LUM		(1 << 2)
#define EOTF_ET3_HLG_BT_2100_LUM	(1 << 3)

/* CTA-861-G: Static metadata descriptor support bitmap */
#define STATIC_METADATA_TYPE1 (1 << 0)

/* Colorspace bits */
#define EDID_CS_BT2020RGB (1 << 7)
#define EDID_CS_BT2020YCC (1 << 6)
#define EDID_CS_BT2020CYCC (1 << 5)
#define EDID_CS_DCIP3 (1 << 15)
#define EDID_CS_HDR_GAMUT_MASK (EDID_CS_BT2020RGB | \
			EDID_CS_BT2020YCC | \
			EDID_CS_BT2020CYCC | \
			EDID_CS_DCIP3)
#define EDID_CS_HDR_CS_BASIC (EDID_CS_BT2020RGB | EDID_CS_DCIP3)

/* CTA-861-G: HDR Metadata names and types */
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

enum drm_tone_map_type {
	DRM_TONE_MAP_NONE,
	DRM_TONE_MAP_H2H,
	DRM_TONE_MAP_H2S,
	DRM_TONE_MAP_S2H,
	DRM_TONE_MAP_MAX,
};

void
generate_csc_lut(struct drm_backend *b,
			double csc_matrix[3][3],
		    enum drm_colorspace current,
		    enum drm_colorspace target);

struct drm_color_lut *
generate_OETF_2084_lut(struct drm_backend *b,
			int lut_size,
			uint16_t max_val);

struct drm_color_lut *
generate_EOTF_2084_lut(struct drm_backend *b,
			int lut_size,
			uint16_t max_val);

struct drm_color_lut *
generate_gamma_lut(struct drm_backend *b,
			 int lut_size,
			 uint16_t max_val);

struct drm_color_lut *
generate_degamma_lut(struct drm_backend *b,
			 int lut_size,
			 uint16_t max_val);
