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

#define HIGH_X(val) (val >> 6)
#define HIGH_Y(val) ((val >> 4) & 0x3)
#define LOW_X(val)  ((val >> 2) & 0x3)
#define LOW_Y(val)  ((val >> 4) & 0x3)

#define NON_NULL(a, b) (a ? a->b : -1)
#define MIN_NT_ZERO(c, d) (c ? MIN(c, d) : d)

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

static void drm_print_metadata(struct weston_hdr_metadata_static *s,
		struct drm_edid_hdr_metadata_static *d,
		struct drm_hdr_metadata_static *o)
{
	weston_log("========= All metadata ===========\n");
	weston_log("Property Surface Display Output \n");
	weston_log("Max Lum \t %d \t %d \t %d\n",
		NON_NULL(s, max_luminance),
		NON_NULL(d, desired_max_ll),
		NON_NULL(o, max_mastering_luminance));
	weston_log("Min Lum \t %d \t %d \t %d\n",
		NON_NULL(s, min_luminance),
		NON_NULL(d, desired_min_ll),
		NON_NULL(o, min_mastering_luminance));
	weston_log("Max CLL \t %d \t %d \t %d\n",
		NON_NULL(s, max_cll),
		NON_NULL(d, desired_max_ll),
		NON_NULL(o, max_cll));
	weston_log("Max FALL  %d \t %d \t %d\n",
		NON_NULL(s, max_fall),
		NON_NULL(d, desired_max_fall),
		NON_NULL(o, max_fall));
	weston_log("EOTF \t %d \t %d \t %d\n",
		NON_NULL(s, eotf),
		NON_NULL(d, eotf),
		NON_NULL(o, eotf));
	weston_log("R x,y \t %d,%d \t %d,%d \t %d,%d\n", 
		NON_NULL(s, display_primary_r_x),
		NON_NULL(s, display_primary_r_y),
		NON_NULL(d, display_primary_r_x),
		NON_NULL(d, display_primary_r_y),
		NON_NULL(o, primary_r_x),
		NON_NULL(o, primary_r_y));
	weston_log("G x,y \t %d,%d \t %d,%d \t %d,%d\n", 
		NON_NULL(s, display_primary_g_x),
		NON_NULL(s, display_primary_g_y),
		NON_NULL(d, display_primary_g_x),
		NON_NULL(d, display_primary_g_y),
		NON_NULL(o, primary_g_x),
		NON_NULL(o, primary_g_y));
	weston_log("B x,y \t %d,%d \t %d,%d \t %d,%d\n", 
		NON_NULL(s, display_primary_b_x),
		NON_NULL(s, display_primary_b_y),
		NON_NULL(d, display_primary_b_x),
		NON_NULL(d, display_primary_b_y),
		NON_NULL(o, primary_b_x),
		NON_NULL(o, primary_b_y));
	weston_log("WP x,y \t %d,%d \t %d,%d \t %d,%d\n", 
		NON_NULL(s, white_point_x),
		NON_NULL(s, white_point_y),
		NON_NULL(d, white_point_x),
		NON_NULL(d, white_point_y),
		NON_NULL(o, white_point_x),
		NON_NULL(o, white_point_y));
	weston_log("========= END ===========\n");
}

static void
drm_print_display_metadata(struct drm_edid_hdr_metadata_static *md)
{
	int count = 0;

	if (md) {
		weston_log("\n");
		weston_log_continue("=============== HDR Static md details:=====================\n");
		weston_log_continue("\t|EOTF=0x%x\n \t|mdtype=0x%x\n \t|max_l=%d nits\n \t|min_l=%d nits\n",
			(unsigned int)md->eotf,
			(unsigned int)md->metadata_type,
			(unsigned int)md->desired_max_ll,
			(unsigned int)md->desired_min_ll);

		if (md->eotf) {
			for (count = 1; count <= 32; count <<= 1)
				if (count & md->eotf)
					weston_log_continue("\t|EOTF: %s\n", eotf_names[count]);
		}

		weston_log_continue("\t|SMD Descriptor: %s\n", md_type[1]);
		weston_log_continue("==================== End =====================\n");
	}
}

/* Prepare target HDR metadata for tone mapping. If the surface has
* its own metadata, take surface's metadata, else map it to monitor's
* capabilities */
void
drm_prepare_output_hdr_metadata(struct drm_backend *b,
		struct weston_hdr_metadata *ref_hdr_md,
		struct drm_edid_hdr_metadata_static *dmd,
		struct drm_hdr_metadata_static *out_md)
{
	struct weston_hdr_metadata_static *cmd = NULL;

	cmd = &ref_hdr_md->metadata.static_metadata;
	out_md->max_mastering_luminance = cmd->max_luminance;
	out_md->min_mastering_luminance = cmd->min_luminance;

	out_md->primary_r_x = MIN_NT_ZERO(cmd->display_primary_r_x,
			dmd->display_primary_r_x);
	out_md->primary_r_y = MIN_NT_ZERO(cmd->display_primary_r_y,
			dmd->display_primary_r_y);
	out_md->primary_g_x = MIN_NT_ZERO(cmd->display_primary_g_x,
			dmd->display_primary_g_x);
	out_md->primary_g_y = MIN_NT_ZERO(cmd->display_primary_g_y,
			dmd->display_primary_g_y);
	out_md->primary_b_x = MIN_NT_ZERO(cmd->display_primary_b_x,
			dmd->display_primary_b_x);
	out_md->primary_b_y = MIN_NT_ZERO(cmd->display_primary_b_y,
			dmd->display_primary_b_y);	
	out_md->white_point_x = MIN_NT_ZERO(cmd->white_point_x,
			dmd->white_point_x);
	out_md->white_point_x = MIN_NT_ZERO(cmd->white_point_y,
			dmd->white_point_y);

	out_md->max_cll = MIN_NT_ZERO(cmd->max_cll, dmd->desired_max_ll);
	out_md->max_fall = MIN_NT_ZERO(cmd->max_fall, dmd->desired_max_fall);
	out_md->eotf = EOTF_ET1_GAMMA_HDR_LUM;//EOTF_ET2_SMPTE_2084_LUM;
	out_md->metadata_type = 1;

	drm_print_metadata(cmd, dmd, out_md);
}

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
	s->metadata_type = hdr_db[1];

	if (data_len >  2 && data_len < 6) {
		s->desired_max_ll = hdr_db[2];
		s->desired_max_fall = hdr_db[3];
		s->desired_min_ll = hdr_db[4];
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
		clrspaces = ((clr_db[3] & 0x80) << 8) | (clr_db[2]);

	return clrspaces;
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
		drm_print_display_metadata(md);
		weston_log("Found static HDR metadata in EDID\n");
	}

	return md;
}

void
drm_release_hdr_metadata(struct drm_edid_hdr_metadata_static *md)
{
	free(md);
}
