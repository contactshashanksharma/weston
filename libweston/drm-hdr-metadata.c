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
#include "compositor-drm.h"

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

const char *eotf_names[] = {
	[EOTF_ET0_GAMMA_SDR_LUM] = "ET0 SDR GAMMA Range",
	[EOTF_ET1_GAMMA_HDR_LUM] = "ET1 HDR GAMMA Range",
	[EOTF_ET2_SMPTE_2084_LUM] = "ET2 SMPTE 2048 Range",
	[EOTF_ET3_HLG_BT_2100_LUM] = "ET3 HLG BT2100 Range",
	[16] = "Reserved",
	[32] = "Reserved"
};

const char *md_type[] = {
	[1] = "Type 1",
};

#define HIGH_X(val) (val >> 6)
#define HIGH_Y(val) ((val >> 4) & 0x3)
#define LOW_X(val)  ((val >> 2) & 0x3)
#define LOW_Y(val)  ((val >> 4) & 0x3)

static void
drm_set_color_primaries(const uint8_t *edid,
				struct drm_edid_hdr_metadata_static *smd)
{
	uint8_t rxrygxgy_0_1;
	uint8_t bxbywxwy_0_1;
	uint8_t count = 0x19; /* base of chromaticity block values */

	if (!edid || !smd)
		return;

	rxrygxgy_0_1 = edid[count++];
	bxbywxwy_0_1 = edid[count++];

	smd->display_primary_r_x = (edid[count++] << 2) | HIGH_X(rxrygxgy_0_1);
	smd->display_primary_r_y = (edid[count++] << 2) | HIGH_Y(rxrygxgy_0_1);

	smd->display_primary_g_x = (edid[count++] << 2) | LOW_X(rxrygxgy_0_1);
	smd->display_primary_g_y = (edid[count++] << 2) | LOW_Y(rxrygxgy_0_1);

	smd->display_primary_b_x = (edid[count++] << 2) | HIGH_X(bxbywxwy_0_1);
	smd->display_primary_b_y = (edid[count++] << 2) | HIGH_Y(bxbywxwy_0_1);

	smd->white_point_x = (edid[count++] << 2) | LOW_X(bxbywxwy_0_1);
	smd->white_point_y = (edid[count++] << 2) | LOW_X(bxbywxwy_0_1);
}

static struct drm_edid_hdr_metadata_static *
drm_get_hdr_static_metadata(const uint8_t *hdr_db, uint32_t data_len)
{
	struct drm_edid_hdr_metadata_static *s;

	if (data_len < 2) {
		weston_log("Invalid metadata input to static parser\n");
		return NULL;
	}

	s = malloc(sizeof (struct drm_edid_hdr_metadata_static));
	if (!s) {
		weston_log("OOM while parsing static metadata\n");
		return NULL;
	}

	memset(s, 0, sizeof(struct drm_edid_hdr_metadata_static));
	s->eotf = hdr_db[0] & 0x3F;
	s->smd_type_desc = hdr_db[1];
	s->max_cll = hdr_db[2];
	s->max_cfall = hdr_db[3];
	s->min_cll = hdr_db[4];
	return s;
}

struct drm_edid_hdr_metadata_static *
drm_get_hdr_metadata(const uint8_t *edid, uint32_t edid_len)
{
	uint8_t data_len = 0;
	const uint8_t *hdr_db;
	struct drm_edid_hdr_metadata_static *md = NULL;

	if (!edid) {
		weston_log("Invalid EDID\n");
		return NULL;
	}

	hdr_db = edid_find_extended_data_block(edid, &data_len,
			EDID_CEA_EXT_TAG_STATIC_METADATA);
	if (hdr_db && data_len != 0) {
		md = drm_get_hdr_static_metadata(hdr_db, data_len);
		if (!md) {
			weston_log("Can't find static HDR metadata in EDID\n");
			return NULL;
		}

		drm_set_color_primaries(edid, md);
		weston_log("Found static HDR metadata in EDID\n");
	}

	return md;
}

void
drm_release_hdr_metadata(struct drm_edid_hdr_metadata_static *md)
{
	free(md);
}

void drm_print_hdr_metadata(struct drm_edid_hdr_metadata_static *md)
{
	int count = 0;

	if (md) {
		weston_log("\n");
		weston_log_continue("=============== HDR Static md details:=====================\n");
		weston_log_continue("\t|EOTF=0x%x\n \t|desc=0x%x\n \t|max_l=%d nits\n \t|min_l=%d nits\n",
			md->eotf, md->smd_type_desc, md->max_cll, md->min_cll);

		if (md->eotf) {
			for (count = 1; count <= 32; count <<= 1)
				if (count & md->eotf)
					weston_log_continue("\t|EOTF: %s\n", eotf_names[count]);
		}

		weston_log_continue("\t|SMD Descriptor: %s\n", md_type[1]);
		weston_log_continue("==================== End =====================\n");
	}
}

#if 0
static struct drm_fb * 
drm_tone_map(struct drm_backend *b,
			struct drm_plane_state *ps,
			struct drm_edid_hdr_metadata_static *display_md)
{
	uint32_t tone_map_mode = 0;
	struct weston_hdr_metadata *content_md = ps->ev->surface->hdr_metadata;

	/*
	* Our tone mapping policy is pretty much to match output display's
	* capabilities, so here is how we are going to do this:
	* 
	*+-------------+------------------------------------+
	*|Content on   | Display(Sink)| Tone mapping target |
	*|any surface  |              |                     |
	*+--------------------------------------------------+
	*| HDR         | HDR          | Display(H2H)        |
	*|             |              |                     |
	*+--------------------------------------------------+
	*| HDR         | SDR          | Display(H2S)        |
	*|             |              |                     |
	*+--------------------------------------------------+
	*| SDR         | HDR          | Display(S2H)        |
	*|             |              |                     |
	*+--------------------------------------------------+
	*| SDR         | SDR          | No tone mapping     |
	*|             |              |                     |
	*+-------------+--------------+---------------------+
	*/

	/* HDR content and SDR display */
	if (content_md && !display_md)
		tone_map_mode = VA_TONE_MAPPING_HDR_TO_SDR;

	/* HDR content and HDR display */
	if (content_md && display_md)
		tone_map_mode = VA_TONE_MAPPING_HDR_TO_HDR;

	/* SDR content and HDR display */
	if (!content_md && display_md)
		tone_map_mode = VA_TONE_MAPPING_SDR_TO_HDR;

	return drm_va_tone_map(b, ps, tone_map_mode, display_md);
}
#endif
