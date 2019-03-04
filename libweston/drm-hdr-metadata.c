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
#include "compositor-drm.h"
#include "shared/helpers.h"
#include "drm-color-transformation.h"

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

void drm_print_display_hdr_metadata(struct drm_edid_hdr_metadata_static *md)
{
	int count = 0;

	if (md) {
		weston_log("\n");
		weston_log_continue("=============== HDR Static md details:=====================\n");
		weston_log_continue("\t|EOTF=0x%x\n \t|mdtype=0x%x\n \t|max_l=%d nits\n \t|min_l=%d nits\n",
			md->eotf, md->metadata_type, md->desired_max_ll, md->desired_min_ll);

		if (md->eotf) {
			for (count = 1; count <= 32; count <<= 1)
				if (count & md->eotf)
					weston_log_continue("\t|EOTF: %s\n", eotf_names[count]);
		}

		weston_log_continue("\t|SMD Descriptor: %s\n", md_type[1]);
		weston_log_continue("==================== End =====================\n");
	}
}

uint32_t
drm_tone_mapping_mode(struct weston_hdr_metadata *content_md,
		struct drm_hdr_metadata_static *target_md)
{
	uint32_t tm_type;

	/* HDR content and HDR display */
	if (content_md && target_md)
		tm_type = DRM_TONE_MAPPING_HDR_TO_HDR;

	/* HDR content and SDR display */
	if (content_md && !target_md)
		tm_type = DRM_TONE_MAPPING_HDR_TO_SDR;

	/* SDR content and HDR display */
	if (!content_md && target_md)
		tm_type = DRM_TONE_MAPPING_SDR_TO_HDR;

	/* SDR content and SDR display */
	if (!content_md && !target_md)
		tm_type = DRM_TONE_MAP_NONE;

	return tm_type;
}

static struct drm_edid_hdr_metadata_static *
drm_get_hdr_static_metadata(const uint8_t *hdr_db, uint32_t data_len)
{
	struct drm_edid_hdr_metadata_static *s;

	if (data_len < 2) {
		weston_log("Invalid metadata input to static parser\n");
		return NULL;
	}

	s = zalloc(sizeof (struct drm_edid_hdr_metadata_static));
	if (!s) {
		weston_log("OOM while parsing static metadata\n");
		return NULL;
	}

	memset(s, 0, sizeof(struct drm_edid_hdr_metadata_static));

	s->eotf = hdr_db[0] & 0x3F;
	s->metadata_type = hdr_db[1];

	if (data_len >  2 && data_len < 6) {
		s->desired_max_ll = hdr_db[2];
		s->desired_max_fall = hdr_db[3];
		s->desired_min_ll = hdr_db[4];

		if (!s->desired_max_ll)
			s->desired_max_ll = 0xFF;
	}
	return s;
}

uint16_t
drm_get_display_clrspace(const uint8_t *edid, uint32_t edid_len)
{
	uint8_t data_len = 0;
	const uint8_t *clr_db;
	uint16_t clrspaces = 0;

	clr_db = edid_find_extended_data_block(edid, &data_len,
			EDID_CEA_TAG_COLORIMETRY);
	if (clr_db && data_len != 0)
		/* db[4] bit 7 is DCI-P3 support information (added in CTA-861-G) */
		clrspaces = ((clr_db[4] & 0x80) << 8) | (clr_db[3]);

	return clrspaces;
}

struct drm_edid_hdr_metadata_static *
drm_get_display_hdr_metadata(const uint8_t *edid, uint32_t edid_len)
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

		drm_print_display_hdr_metadata(md);
		weston_log("Found static HDR metadata in EDID\n");
	}

	return md;
}

void
drm_release_hdr_metadata(struct drm_edid_hdr_metadata_static *md)
{
	free(md);
}
