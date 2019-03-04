/*
 * Copyright Â© 2018 Intel Corporation
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

#ifndef WESTON_HDR_METADATA_DEFS_H
#define WESTON_HDR_METADATA_DEFS_H

#include <stdint.h>

#ifdef  __cplusplus
extern "C" {
#endif

enum hdr_metadata_type {
	HDR_METADATA_TYPE1,
	HDR_METADATA_TYPE2,
};

enum hdr_metadata_eotf {
	EOTF_TRADITIONAL_GAMMA_SDR,
	EOTF_TRADITIONAL_GAMMA_HDR,
	EOTF_ST2084,
	EOTF_HLG,
};

enum hdr_tone_map_mode {
	WESTON_TONE_MAP_NONE,
	WESTON_TONE_MAP_HDR_TO_HDR,
	WESTON_TONE_MAP_HDR_TO_SDR,
	WESTON_TONE_MAP_SDR_TO_HDR
};

struct weston_hdr_metadata_dynamic {
	uint8_t size;
	uint8_t *metadata;
};

struct weston_hdr_metadata_static {
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
	uint8_t eotf;
};

struct weston_hdr_metadata {
	enum hdr_metadata_type metadata_type;
	union {
		struct weston_hdr_metadata_static static_metadata;
		struct weston_hdr_metadata_dynamic dynamic_metadata;
	} metadata;
};

#ifdef  __cplusplus
}
#endif

#endif
