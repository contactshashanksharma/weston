/*
 * Copyright © 2018 Intel Corporation
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
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/vt.h>
#include <assert.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <time.h>
#include <linux/input.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm/drm_mode.h>

#include "weston-debug.h"
#include "shared/helpers.h"
#include "compositor-drm.h"

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

const char *colorspace_names[] = {
	[DRM_COLORSPACE_REC_709] = "Rec 709 colorspace",
	[DRM_COLORSPACE_DCI_P3] = "DCI_P3 colorspace",
	[DRM_COLORSPACE_REC_2020] = "Rec 2020 colorspace",
};

#if 0
struct drm_color_ctm {
	/* Conversion matrix in S31.32 format. */
	__s64 matrix[9];
};

struct drm_color_lut {
	/*
	 * Data is U0.16 fixed point format.
	 */
	__u16 red;
	__u16 green;
	__u16 blue;
	__u16 reserved;
};
#endif

struct chromaticity {
	double x;		// CIE1931 x
	double y;		// CIE1931 y
	double luminance;	// CIE1931 Y
};

struct colorspace {
	struct chromaticity white;
	struct chromaticity red;
	struct chromaticity green;
	struct chromaticity blue;
};

#define HIGH_X(val) (val >> 6)
#define HIGH_Y(val) ((val >> 4) & 0x3)
#define LOW_X(val)  ((val >> 2) & 0x3)
#define LOW_Y(val)  ((val >> 4) & 0x3)

static void
drm_set_color_primaries(const uint8_t *edid,
				struct drm_edid_hdr_md_static *smd)
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

struct drm_edid_hdr_md_static *
drm_get_hdr_static_metadata(const uint8_t *hdr_db, uint32_t data_len)
{
	struct drm_edid_hdr_md_static *static_md;

	if (data_len < 2) {
		weston_log("Invalid metadata input to static parser\n");
		return NULL;
	}

	static_md = malloc(sizeof (struct drm_edid_hdr_md_static));
	if (!static_md) {
		weston_log("OOM while parsing static metadata\n");
		return NULL;
	}

	memset(static_md, 0, sizeof(struct drm_edid_hdr_md_static));
	static_md->eotf = hdr_db[0] & 0x3F;
	static_md->smd_type_desc = hdr_db[1];
	static_md->max_cll = hdr_db[2];
	static_md->max_cfall = hdr_db[3];
	static_md->min_cll = hdr_db[4];
	return static_md;
}


struct drm_edid_hdr_dynamic_md_block *
drm_get_hdr_dynamic_metadata_block(const uint8_t *hdr_db,
			uint8_t *total_block_len)
{
	struct drm_edid_hdr_dynamic_md_block *dynamic;

	if (*total_block_len < 2) {
		*total_block_len = 0;
		return NULL;
	}

	dynamic = malloc(sizeof(struct drm_edid_hdr_dynamic_md_block));
	if (!dynamic) {
		weston_log("OOM while creating dynamic metadata\n");
		return NULL;
	}
	memset(dynamic, 0, sizeof(struct drm_edid_hdr_dynamic_md_block));

	/* data length is the total length of the dynamic metadata block, which can
	* have many sub-blocks representing support for various types of dynamic
	* HDR metadata, so use the block length here, which is db[0] */
	dynamic->blk_data_size = hdr_db[0];
	dynamic->metadata_type = (hdr_db[2] << 8) | hdr_db[1];
	dynamic->blk_md = malloc(dynamic->blk_data_size);
	if (!dynamic->blk_md) {
		weston_log("OOM while loading dynamic metadata\n");
		free(dynamic);
		return NULL;
	}

	memcpy(dynamic->blk_md, hdr_db + 3, dynamic->blk_data_size);
	for (int i = 0; i < dynamic->blk_data_size; i++)
		weston_log_continue("md[%d]=0x%x\n", i, (unsigned int)dynamic->blk_md[i]);

	/* Reeduced the data bytes read */
	*total_block_len -= dynamic->blk_data_size;
	return dynamic;
}

struct drm_edid_hdr_metadata *
drm_get_hdr_metadata(const uint8_t *edid, uint32_t edid_len)
{
	uint32_t flag;
	uint8_t data_len = 0;
	const uint8_t *hdr_db;
	struct drm_edid_hdr_metadata *md = NULL;

	/* We are looking for CEA extension block so expecting length > 1 block */
	if (!edid || edid_len <= EDID_BLOCK_LENGTH)
		return NULL;

	md = malloc(sizeof(struct drm_edid_hdr_metadata));
	if (!md) {
		weston_log("OOM while getting HDR metadata\n");
		return NULL;
	}
	memset(md, 0, sizeof(struct drm_edid_hdr_metadata));

	/* Ideally, an EDID should either contain a static metadata block, or
	* a dynamic metadata block, not both, but CEA-861-G spec is not
	* very clear about this. Lets try parsing both the blocks, and give priority
	* to static block. */
	flag = EDID_CEA_EXT_TAG_STATIC_METADATA;
	hdr_db = edid_find_extended_data_block(edid, &data_len, flag);
	if (hdr_db && data_len != 0) {
		md->metadata.s = drm_get_hdr_static_metadata(hdr_db, data_len);
		if (md->metadata.s) {
			weston_log("Found static HDR metadata in EDID\n");
			md->type = DRM_HDR_MD_STATIC;
			drm_set_color_primaries(edid, md->metadata.s);
			return md;
		}
	}

	flag = EDID_CEA_EXT_TAG_DYNAMIC_METADATA;
	/* Todo: As per the spec, there can be multiple HDR dynamic metadata blocks
	 * but we are parsing only the first dynamic metadat block */
	hdr_db = edid_find_extended_data_block(edid, &data_len, flag);
	if (hdr_db && data_len != 0) {

		md->metadata.d = malloc(sizeof(struct drm_edid_hdr_md_dynamic));
		if (!md->metadata.d) {
			weston_log("OOM while getting dynamic HDR metadata\n");
			return NULL;
		}

		weston_log("Found dynamic HDR metadata in EDID, size %d\n", data_len);
		md->metadata.d->size = data_len;
		while (data_len) {
			md->metadata.d->md_blks[md->metadata.d->num_blks++] =
				drm_get_hdr_dynamic_metadata_block(hdr_db, &data_len);
		}

		if (md->metadata.d->num_blks)
			weston_log("Found %d dynamic HDR metadata blocks in EDID\n",
				md->metadata.d->num_blks);
		md->type = DRM_HDR_MD_DYNAMIC;
	}

	if (!md->metadata.s || !md->metadata.d) {
		free(md);
		md = NULL;
	}

	return md;
}

void
drm_release_hdr_metadata(struct drm_edid_hdr_metadata *md)
{
	if (md) {
		if (md->metadata.s)
			free(md->metadata.s);

		if (md->metadata.d) {
			int count;

			for (count = 0; count < md->metadata.d->num_blks; count++) {
				struct drm_edid_hdr_dynamic_md_block *block;

				block = md->metadata.d->md_blks[count];
				if (block->blk_md)
					free(block->blk_md);
				free(block);
			}

			free(md->metadata.d);
		}
	}
}

void drm_print_hdr_metadata(struct drm_edid_hdr_metadata *md)
{
	int count = 0;

	if (md->metadata.s) {
		weston_log("\n");
		weston_log_continue("=============== HDR Static md details:=====================\n");
		weston_log_continue("\t|EOTF=0x%x\n \t|desc=0x%x\n \t|max_l=%d nits\n \t|min_l=%d nits\n",
		md->metadata.s->eotf, md->metadata.s->smd_type_desc,
		md->metadata.s->max_cll,
		md->metadata.s->min_cll);

		if (md->metadata.s->eotf) {
			for (count = 1; count <= 32; count <<= 1)
				if (count & md->metadata.s->eotf)
					weston_log_continue("\t|EOTF: %s\n", eotf_names[count]);
		}

		weston_log_continue("\t|SMD Descriptor: %s\n", md_type[1]);
		weston_log_continue("==================== End =====================\n");
	}

	if (md->metadata.d) {
		weston_log_continue("=============== HDR Dynamic md details:=================\n");
		weston_log_continue("Dynamic metadata details:\n \t| total sz=%d blocks=%d\n",
				md->metadata.d->size, md->metadata.d->num_blks);
		for (count = 0; count < md->metadata.d->num_blks; count++) {
			int i;
			struct drm_edid_hdr_dynamic_md_block *block = md->metadata.d->md_blks[count];

			if (!block)
				continue;

			weston_log_continue("\tblock[%d], size=%d\n", count, 
					block->blk_data_size);
			for (i = 0; i < block->blk_data_size; i++)
				weston_log_continue("\t\t|data[%d]=0x%x\n",
					i, (unsigned int)block->blk_md[i]);
		}
		weston_log_continue("========================================================\n");
	}
}

static struct drm_fb * 
drm_tone_map(struct drm_backend *b,
			struct drm_plane_state *ps,
			struct drm_edid_hdr_metadata *display_md)
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

static double
matrix_determinant_3x3(double matrix[3][3])
{
    double result;

    result = matrix[0][0] * (matrix[1][1] * matrix[2][2] -
				matrix[1][2] * matrix[2][1]);
    result -= matrix[0][1] * (matrix[1][0] * matrix[2][2] -
				matrix[1][2] * matrix[2][0]);
    result += matrix[0][2] * (matrix[1][0] * matrix[2][1] -
				matrix[1][1] * matrix[2][0]);

    return result;
}

static int32_t
matrix_inverse_3x3(double matrix[3][3], double result[3][3])
{
    int ret_val = -1;
    double tmp[3][3];
    double determinant = matrix_determinant_3x3(matrix);

    if (0 != determinant) {

        tmp[0][0] = (matrix[1][1] * matrix[2][2] - 
			matrix[1][2] * matrix[2][1])/ determinant;
        tmp[0][1] = (matrix[0][2] * matrix[2][1] -
			matrix[2][2] * matrix[0][1]) / determinant;
        tmp[0][2] = (matrix[0][1] * matrix[1][2] -
			matrix[0][2] * matrix[1][1]) / determinant;
        tmp[1][0] = (matrix[1][2] * matrix[2][0] -
			matrix[1][0] * matrix[2][2]) / determinant;
        tmp[1][1] = (matrix[0][0] * matrix[2][2] -
			matrix[0][2] * matrix[2][0]) / determinant;
        tmp[1][2] = (matrix[0][2] * matrix[1][0] -
			matrix[0][0] * matrix[1][2]) / determinant;
        tmp[2][0] = (matrix[1][0] * matrix[2][1] -
			matrix[1][1] * matrix[2][0]) / determinant;
        tmp[2][1] = (matrix[0][1] * matrix[2][0] -
			matrix[0][0] * matrix[2][1]) / determinant;
        tmp[2][2] = (matrix[0][0] * matrix[1][1] -
			matrix[0][1] * matrix[1][0]) / determinant;

		memcpy(result, tmp, 9 * sizeof(double));
        ret_val = 0;
    }

    return ret_val;
}

static void
matrix_mult_3x3(double matrix1[3][3],
			double matrix2[3][3],
			double result[3][3])
{
	int x, y;
    	double tmp [3][3];

	for(y = 0; y < 3 ; y++) {
		for(x = 0 ; x < 3 ; x++) {
			tmp[y][x] = matrix1[y][0] * matrix2[0][x] + 
				matrix1[y][1] * matrix2[1][x] +
				matrix1[y][2] * matrix2[2][x];
		}
	}

	for (y = 0; y < 3; y++) {
		for (x = 0; x < 3; x++)
			result[y][x] = tmp[y][x];
	}
}

static void 
matrix_mult_3x3_with_3x1(double matrix1[3][3],
			double matrix2[3],
			double result[3])
{
	double tmp[3];

	tmp[0] = matrix1[0][0] * matrix2[0] +
		 matrix1[0][1] * matrix2[1] +
		 matrix1[0][2] * matrix2[2];
	tmp[1] = matrix1[1][0] * matrix2[0] +
		 matrix1[1][1] * matrix2[1] +
		 matrix1[1][2] * matrix2[2];
	tmp[2] = matrix1[2][0] * matrix2[0] +
		 matrix1[2][1] * matrix2[1] +
		 matrix1[2][2] * matrix2[2];

	result[0] = tmp[0];
	result[1] = tmp[1];
	result[2] = tmp[2];
}


/* http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html */
static void
create_rgb_to_xyz_matrix(struct colorspace *cspace,
			double rgb2xyz[3][3])
{
	double z[4];
	double xyzw[3];
	double xyzrgb[3][3];
	double xyzsum[3];
	double mat1[3][3];
	double mat2[3][3];
	struct chromaticity *p_chroma = &cspace->white;

	for (int i = 0; i < 4; i++)
		z[i] = 1 - p_chroma[i].x - p_chroma[i].y;

	xyzw[0] = cspace->white.x / cspace->white.y;
	xyzw[1] = 1;
	xyzw[2] = z[0] / cspace->white.y;

	xyzrgb[0][0] = cspace->red.x;
	xyzrgb[0][1] = cspace->green.x;
	xyzrgb[0][2] = cspace->blue.x;

	xyzrgb[1][0] = cspace->red.y;
	xyzrgb[1][1] = cspace->green.y;
	xyzrgb[1][2] = cspace->blue.y;

	xyzrgb[2][0] = z[1];
	xyzrgb[2][1] = z[2];
	xyzrgb[2][2] = z[3];

	matrix_inverse_3x3(xyzrgb, mat1);
	matrix_mult_3x3_with_3x1(mat1, xyzw, xyzsum);

	memset(mat2, 0, 9 * sizeof(double));
	mat2[0][0] = xyzsum[0];
	mat2[1][1] = xyzsum[1];
	mat2[2][2] = xyzsum[2];
	matrix_mult_3x3(xyzrgb, mat2, rgb2xyz);
}

static void
create_gamut_scaling_matrix(struct colorspace *pSrc,
			struct colorspace *pDst,
			double result[3][3])
{
	double mat1[3][3], mat2[3][3], tmp[3][3];

	create_rgb_to_xyz_matrix(pSrc, mat1);
	create_rgb_to_xyz_matrix(pDst, mat2);

	matrix_inverse_3x3(mat2, tmp);
	matrix_mult_3x3(tmp, mat1, result);
}

/*
* https://en.wikipedia.org/wiki/Rec._2020#System_colorimetry
* https://en.wikipedia.org/wiki/Rec._709#Primary_chromaticities */
static void
create_2020_to_709_matrix(double result[3][3])
{
	struct colorspace bt2020, bt709;

	bt2020.white.x = 0.3127;
	bt2020.white.y = 0.3290;
	bt2020.white.luminance = 100.0;
	
	bt2020.red.x = 0.708;
	bt2020.red.y = 0.292;
	bt2020.green.x = 0.170;
	bt2020.green.y = 0.797;
	bt2020.blue.x = 0.131;
	bt2020.blue.y = 0.046;

	bt709.white.x = 0.3127;
	bt709.white.y = 0.3290;
	bt709.white.luminance = 100.0;

	bt709.red.x = 0.64;
	bt709.red.y = 0.33;
	bt709.green.x = 0.30;
	bt709.green.y = 0.60;
	bt709.blue.x = 0.15;
	bt709.blue.y = 0.06;

	create_gamut_scaling_matrix(&bt2020, &bt709, result);
}

/*
* https://en.wikipedia.org/wiki/Rec._2020#System_colorimetry
* https://en.wikipedia.org/wiki/Rec._709#Primary_chromaticities */
static void
create_709_to_2020_matrix(double result[3][3])
{
	struct colorspace bt2020, bt709;

	bt2020.white.x = 0.3127;
	bt2020.white.y = 0.3290;
	bt2020.white.luminance = 100.0;

	bt2020.red.x = 0.708;
	bt2020.red.y = 0.292;
	bt2020.green.x = 0.170;
	bt2020.green.y = 0.797;
	bt2020.blue.x = 0.131;
	bt2020.blue.y = 0.046;

	bt709.white.x = 0.3127;
	bt709.white.y = 0.3290;
	bt709.white.luminance = 100.0;
	
	bt709.red.x = 0.64;
	bt709.red.y = 0.33;
	bt709.green.x = 0.30;
	bt709.green.y = 0.60;
	bt709.blue.x = 0.15;
	bt709.blue.y = 0.06;

	create_gamut_scaling_matrix(&bt709, &bt2020, result);
}

/*
* https://en.wikipedia.org/wiki/Rec._2020#System_colorimetry
* https://en.wikipedia.org/wiki/DCI-P3#System_colorimetry
*/
static void
create_2020_to_DCIP3_matrix(double result[3][3])
{
	struct colorspace bt2020, dcip3;

	bt2020.white.x = 0.3127;
	bt2020.white.y = 0.3290;
	bt2020.white.luminance = 100.0;
	
	bt2020.red.x = 0.708;
	bt2020.red.y = 0.292;
	bt2020.green.x = 0.170;
	bt2020.green.y = 0.797;
	bt2020.blue.x = 0.131;
	bt2020.blue.y = 0.046;

	dcip3.white.x = 0.314;
	dcip3.white.y = 0.351;
	dcip3.white.luminance = 100.0;
	
	dcip3.red.x = 0.680;
	dcip3.red.y = 0.320;
	dcip3.green.x = 0.265;
	dcip3.green.y = 0.690;
	dcip3.blue.x = 0.150;
	dcip3.blue.y = 0.060;

	create_gamut_scaling_matrix(&bt2020, &dcip3, result);
}

/*
* https://en.wikipedia.org/wiki/DCI-P3#System_colorimetry
* https://en.wikipedia.org/wiki/Rec._709#Primary_chromaticities
*/
static void
create_709_to_DCIP3_matrix(double result[3][3])
{
	struct colorspace bt709, dcip3;

	bt709.white.x = 0.3127;
	bt709.white.y = 0.3290;
	bt709.white.luminance = 100.0;
	
	bt709.red.x = 0.64;
	bt709.red.y = 0.33;
	bt709.green.x = 0.30;
	bt709.green.y = 0.60;
	bt709.blue.x = 0.15;
	bt709.blue.y = 0.06;

	dcip3.white.x = 0.314;
	dcip3.white.y = 0.351;
	dcip3.white.luminance = 100.0;

	dcip3.red.x = 0.680;
	dcip3.red.y = 0.320;
	dcip3.green.x = 0.265;
	dcip3.green.y = 0.690;
	dcip3.blue.x = 0.150;
	dcip3.blue.y = 0.060;

	create_gamut_scaling_matrix(&bt709, &dcip3, result);
}

static void
create_unity_matrix(double result[3][3])
{
	memset(result, 0, 9 * sizeof(double));
	result[0][0] = 1.0;
	result[1][1] = 1.0;
	result[2][2] = 1.0;
}

/* This function is just for the sake of completion of array */
static void
noop_invalid_matrix(double result[3][3])
{
	return;
}

/* Array of function ptrs which generate CSC matrix */
void (*generate_csc_fptrs[][DRM_COLORSPACE_MAX])(double[3][3]) = {
	[DRM_COLORSPACE_REC_709][DRM_COLORSPACE_REC_709] =
		create_unity_matrix,
	[DRM_COLORSPACE_REC_709][DRM_COLORSPACE_DCI_P3] =
		create_709_to_DCIP3_matrix,
	[DRM_COLORSPACE_REC_709][DRM_COLORSPACE_REC_2020] =
		create_709_to_2020_matrix,
	[DRM_COLORSPACE_DCI_P3][DRM_COLORSPACE_REC_709] =
		noop_invalid_matrix,
	[DRM_COLORSPACE_DCI_P3][DRM_COLORSPACE_DCI_P3] =
		noop_invalid_matrix,
	[DRM_COLORSPACE_DCI_P3][DRM_COLORSPACE_REC_2020] =
		noop_invalid_matrix,
	[DRM_COLORSPACE_REC_2020][DRM_COLORSPACE_REC_709] =
		create_2020_to_709_matrix,
	[DRM_COLORSPACE_REC_2020][DRM_COLORSPACE_DCI_P3] =
		create_2020_to_DCIP3_matrix,
	[DRM_COLORSPACE_REC_2020][DRM_COLORSPACE_REC_2020] =
		create_unity_matrix,
};

/* Generate LUT for gamut mapping */
static double *
drm_generate_csc_lut(struct drm_backend *b,
		     enum drm_colorspace current,
		     enum drm_colorspace target)
{
	void (*generate_csc_matrix)(double[3][3]);
	double *csc_matrix;

	if (current < DRM_COLORSPACE_REC_709 ||
		current > DRM_COLORSPACE_REC_2020 ||
		target < DRM_COLORSPACE_REC_709 ||
		target > DRM_COLORSPACE_REC_2020) {
			drm_debug(b, "\t\t[state] invalid input/output colorspace\n");
			return NULL;
	}

	csc_matrix = malloc(9 * sizeof(double));
	if (!csc_matrix) {
		drm_debug(b, "\t\t[state] OOM while applying CSC\n");
		return NULL;
	}

	generate_csc_matrix = generate_csc_fptrs[current][target];

	/* DCI-P3 is not practically an input colorspace, its just an output colorspace */
	if (generate_csc_matrix == noop_invalid_matrix) {
		drm_debug(b, "\t\t[state] invalid input colorspace DCI-P3\n");
		return NULL;
	}

	generate_csc_matrix(csc_matrix);
	return csc_matrix;
}

static int
drm_setup_plane_csc(struct drm_backend *b,
		    struct drm_plane_state *ps,
		    enum drm_colorspace target_cs)
{
	double *csc_lut;
	enum drm_colorspace content_cs = ps->ev->surface->colorspace;

	csc_lut = drm_generate_csc_lut(b, content_cs, target_cs);
	if (!csc_lut) {
		drm_debug(b, "\t\t[state] Failed to get CSC lut for plane\n");
		return -1;
	}

	return drm_setup_property_blob(b, &ps->csc_blob_id,
				9 * sizeof(double), (uint8_t *)csc_lut);
}

static double 
OETF_2084(double input, double src_max_luminance)
{
	/* ST 2084 EOTF constants as per the spec */
	double m1 = 0.1593017578125;
	double m2 = 78.84375;
	double c1 = 0.8359375;
	double c2 = 18.8515625;
	double c3 = 18.6875;
	double cf = 1.0;
	double output = 0.0f;

	if (input != 0.0f) {
		cf = src_max_luminance / 10000.0;

		input *= cf;
		output = pow(((c1 + (c2 * pow(input, m1))) /
			(1 + (c3 * pow(input, m1)))), m2);
	}

	return output;
}

static struct drm_color_lut *
generate_OETF_2084_lut(struct drm_backend *b,
			int lut_size, uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		drm_debug(b, "\t\t[state] invalid input/output colorspace\n");
		return NULL;
	}

	for (i = 0; i < lut_size; i++) {
		double normalized_input = (double)i / (double)(lut_size - 1);

		lut[i].red = (double)max_val * OETF_2084(normalized_input, 10000) + 0.5;
		if (lut[i].red > max_val)
			lut[i].red = max_val;

		lut[i].green = lut[i].blue = lut[i].red;
	}

	return lut;
}

static double
EOTF_2084(double input)
{
	/* ST 2084 EOTF constants as per the spec */
	double m1 = 0.1593017578125;
	double m2 = 78.84375;
	double c1 = 0.8359375;
	double c2 = 18.8515625;
	double c3 = 18.6875;
	double output = 0.0f;

	if (input != 0.0f)
		output = pow(((MAX((pow(input, (1.0 / m2)) - c1), 0)) /
			(c2 - (c3 * pow(input, (1.0 / m2))))), (1.0 / m1));
	return output;
}

static struct drm_color_lut *
generate_EOTF_2084_lut(struct drm_backend *b,
		int lut_size, uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		drm_debug(b, "\t\t[state] invalid input/output colorspace\n");
		return NULL;
	}

	for (i = 0; i < lut_size; i++) {
		double normalized_input = (double)i / (double)(lut_size - 1);
		lut[i].red = (double)max_val * EOTF_2084(normalized_input) + 0.5;

		if (lut[i].red > max_val)
			lut[i].red = max_val;
		lut[i].green = lut[i].blue = lut[i].red;
	}

	return lut;
}

/*
* https://en.wikipedia.org/wiki/SRGB#The_forward_transformation_.28CIE_xyY_or_CIE_XYZ_to_sRGB.29
*/
static inline double
get_SRGB_encoding_value(double input)
{
	return (input <= 0.0031308) ?  input * 12.92 :
		(1.055 * pow(input, 1.0 / 2.4)) - 0.055;
}

/* https://en.wikipedia.org/wiki/SRGB#The_forward_transformation_.28CIE_xyY_or_CIE_XYZ_to_sRGB.29 */
static inline double
get_srgb_decoding_value(double input)
{
	return input <= 0.04045 ? input / 12.92 :
		pow(((input + 0.055) / 1.055), 2.4);
}

static struct drm_color_lut *
generate_gamma_lut(struct drm_backend *b,
			 int lut_size,
			 uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		drm_debug(b, "\t\t[state] invalid input/output colorspace\n");
		return NULL;
	}

	for (i = 0; i < lut_size; i++) {
		double normalized_input = (double)i / (double)(lut_size - 1);

		lut[i].red = (double)max_val * get_SRGB_encoding_value(normalized_input)
					+ 0.5;

		if (lut[i].red > max_val)
			lut[i].red = max_val;

		lut[i].green = lut[i].blue = lut[i].red;
	}

	return lut;
}

static struct drm_color_lut *
generate_degamma_lut(struct drm_backend *b,
			 int lut_size,
			 uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		drm_debug(b, "\t\t[state] invalid input/output colorspace\n");
		return NULL;
	}

	for (i = 0; i < lut_size; i++) {
		double normalized_input = (double)i / (double)(lut_size - 1);

		lut[i].red = (double)max_val * get_srgb_decoding_value(normalized_input)
				+ 0.5;
		if (lut[i].red > max_val)
			lut[i].red = max_val;
		lut[i].green = lut[i].blue = lut[i].red;
	}

	return lut;
}

static int
drm_setup_plane_degamma(struct drm_backend *b,
			struct drm_plane_state *ps)
{
	struct drm_color_lut *deg_lut;
	struct drm_plane *plane = ps->plane;
	struct drm_property_info *info = &plane->props[WDRM_PLANE_DEGAMMA_LUT_SZ];
	uint64_t deg_lut_size = info->enum_values[0].value;
	bool is_content_hdr = !!ps->ev->surface->hdr_metadata;

	if (is_content_hdr)
		deg_lut = generate_EOTF_2084_lut(b, deg_lut_size, 0xffff);
	else
		deg_lut = generate_degamma_lut(b, deg_lut_size, 0xffff);

	return drm_setup_property_blob(b, &ps->degamma_blob_id,
						deg_lut_size * sizeof(struct drm_color_lut),
						(const uint8_t *)deg_lut);
}

static int
drm_output_setup_hdr_metadata(struct drm_backend *b,
			struct drm_output_state *state,
			struct drm_edid_hdr_metadata *display_md)
{
	return drm_setup_property_blob(b, &state->hdr_metadata_blob_id,
				sizeof(*display_md), (const uint8_t *)display_md);
}

static int
drm_output_setup_gamma(struct drm_backend *b,
			struct drm_output_state *state,
			struct drm_edid_hdr_metadata *display_md)
{
	struct drm_color_lut *gamma_lut;

	if (display_md)
		gamma_lut = generate_OETF_2084_lut(b, state->gamma_size, 0xFFFF);
	else
		gamma_lut = generate_gamma_lut(b, state->gamma_size, 0xFFFF);

	if (!gamma_lut) {
		weston_log("Shashank: failed to generate CRTC gamma\n");
		drm_debug(b, "\t\t[state] Failed to create gamma lut\n");
		return -1;
	}

	return drm_setup_property_blob(b, &state->gamma_blob_id,
				state->gamma_size * sizeof(*gamma_lut),
				(const uint8_t *)gamma_lut);
}

/*
* We are going to blend multiple planes, but there is a possibility that
* one or more of the surfaces are in BT2020 colorspace (Like a HDR
* buffer), whereas the others are in REC709(SDR buffer). For accurate
* blending we have to make sure that, before blending:
* 	- All the planes are in the same colorspace (Apply CSC if reqd).
*	- If we need to do gamut mapping, we have to make sure that
*	   the planes have linear data (Apply degamma before CSC).
*	- In case of presence of a HDR buffer, they all should be tone
*	   mapped (all SDR or all HDR).
*/
static int
drm_prepare_plane_for_blending(struct drm_backend *b,
			struct drm_plane_state *ps,
			enum drm_colorspace target)
{
	int ret;

	ret = drm_setup_plane_degamma(b, ps);
	if (ret) {
		weston_log("Shashank: failed to set plane degamma\n");
		drm_debug(b, "\t\t[state] Failed to apply plane degamma\n");
		return -1;
	}

	ret = drm_setup_plane_csc(b, ps, target);
	if (ret) {
		weston_log("Shashank: failed to set plane csc\n");
		drm_debug(b, "\t\t[state] Failed to apply plane degamma\n");
		return -1;
	}

	return 0;
}

int
drm_output_prepare_colorspace(struct drm_output_state *state)
{
	int ret;
	struct drm_plane *p;
	struct drm_plane_state *ps;
	struct weston_surface *psurf;
	struct drm_backend *b = state->output->backend;
	struct drm_head *head = to_drm_head(
				weston_output_get_first_head(&state->output->base));
	struct drm_edid_hdr_metadata *display_md = head->hdr_md;
	enum drm_colorspace display_gamut = head->widest_gamut;

	/* Its safe to assume REC_709 colorspace as default */
	if (display_gamut <= DRM_COLORSPACE_UNKNOWN ||
		display_gamut >= DRM_COLORSPACE_MAX)
		display_gamut = DRM_COLORSPACE_REC_709;

	weston_log("Shashank: Searching planes to blend, target csp=%s, tone=%s\n",
				colorspace_names[display_gamut],
				display_md ? "HDR" : "SDR");
	/* Setup plane color properties */
	wl_list_for_each(p, &state->plane_list, link) {
		struct weston_hdr_metadata *content_md;
		struct drm_fb *tone_mapped_fb;

		if (p->type == WDRM_PLANE_TYPE_CURSOR)
			continue;

		ps = drm_output_state_get_existing_plane(state, p);
		if (!ps || !ps->ev)
			continue;

		weston_log("Shashank: Found a %s plane to blend\n",
			plane_type_enums[p->type].name);

		psurf = ps->ev->surface;
		if (!psurf)
			continue;

		content_md = ps->ev->surface->hdr_metadata;

		/* Its possible that we have few surfaces without colorspace information,
		* but few with proper colorspace information. Its safe to assume that the
		* unknown colorspace is REC709 (most common), and map it to a wider
		* gamut than 709 if required. This case is applicable for HDR playback
		* cases where there might be one HDR buffer (REC2020 space) and other
		* SDR buffers created in REC709 colorspace */
		if (psurf->colorspace == DRM_COLORSPACE_UNKNOWN)
			psurf->colorspace = DRM_COLORSPACE_REC_709;

		if (psurf->colorspace != display_gamut) {
			ret = drm_prepare_plane_for_blending(b, ps, display_gamut);
			if (ret) {
				weston_log("Shashank: Failed to prepare plane for blend\n");
				drm_debug(b, "\t\t[state] Failed to prepare plane for CSC\n");
				return ret;
			}

		}

		/* SDR content on SDR display, no tone map reqd */
		if (!content_md && !display_md)
			continue;

		tone_mapped_fb = drm_tone_map(b, ps, display_md);
		if (!tone_mapped_fb) {
			weston_log("Shashank: Failed to tone map\n");
			drm_debug(b, "\t\t[state] Tone mapping failed\n");
			return -1;
		}

#warning: Mem Leak here ?
		/* Replace plane's fb with tone mapped fb, is there a memory leak here ? */
		ps->fb = tone_mapped_fb;
	}

	drm_debug(b, "\t\t[state] Target colorspace %s tone-mapping %s\n",
				colorspace_names[display_gamut],
				display_md ? "HDR" : "SDR");

	/* Setup connector color property blobs */
	ret = drm_output_setup_hdr_metadata(b, state, display_md);
	if (ret) {
		weston_log("Shashank: failed to setup HDR metadata\n");
		drm_debug(b, "\t\t[state] Failed to setup HDR MD blob\n");
		return ret;
	}

	/* Setup CRTC color property blobs */
	ret = drm_output_setup_gamma(b, state, display_md);
	if (ret) {
		weston_log("Shashank: Failed to setup gamma\n");
		drm_debug(b, "\t\t[state] Failed to setup gamma blob\n");
		return ret;
	}

	drm_debug(b, "\t\t[state] Plane colorspace prepared\n");
	return 0;
}

#if 0
static const int32_t *
drm_get_degamma_curve(struct drm_backend *b,
			struct drm_plane_state *ps,
			uint32_t *len)
{
	const int32_t *deg_func;
	struct weston_surface *surface = ps->ev->surface;

	if (!surface)
		return NULL;

	/* Rec 2020 space has its own OETF degamma function */
	if (surface->colorspace == DRM_COLORSPACE_REC_2020) {
		struct weston_hdr_metadata *md = surface->hdr_metadata;
		enum drm_hdr_eotf_type eotf;

		/* 
		* Three possibilities here:
		* This could be a HDR surface with static metadata, or
		* This could be a HDR surface with dynamic metadata, or
		* This could be a non-HDR REC2020 surface, without metadata
		*/
		if (!md)
			/* Pick standard ST2084 curve for REC2020*/
			eotf = DRM_EOTF_HDR_ST2084;
		else
			eotf = md->metadata.s.eotf;

		drm_debug(b, "\t\t[state] picking EOTF type %s\n", eotf_names[eotf]);
		deg_func = drm_oetf_functions[eotf];
	} else {
		drm_debug(b, "\t\t[state] picking standard degammaEOTF type %s\n", 
				colorspace_names[surface->colorspace]);
		deg_func = drm_degamma_functions[surface->colorspace];
	}

	return deg_func;
}


/* We only need narrow -> wider gamut transformation right now, the other
* entries for just the sake of completion */
static const int32_t csc_transformation_functions[][DRM_COLORSPACE_MAX][9] = {
	[DRM_COLORSPACE_REC_601][DRM_COLORSPACE_REC_601] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_601][DRM_COLORSPACE_REC_709] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_601][DRM_COLORSPACE_DCI_P3] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_601][DRM_COLORSPACE_REC_2020] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_709][DRM_COLORSPACE_REC_601] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_709][DRM_COLORSPACE_REC_709] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_709][DRM_COLORSPACE_DCI_P3] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_709][DRM_COLORSPACE_REC_2020] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_DCI_P3][DRM_COLORSPACE_REC_601] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_DCI_P3][DRM_COLORSPACE_REC_709] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_DCI_P3][DRM_COLORSPACE_DCI_P3] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_DCI_P3][DRM_COLORSPACE_REC_2020] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_2020][DRM_COLORSPACE_REC_601] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_2020][DRM_COLORSPACE_REC_709] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_2020][DRM_COLORSPACE_DCI_P3] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_2020][DRM_COLORSPACE_REC_2020] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
};

static const int32_t drm_degamma_functions[][9] = {
	[DRM_COLORSPACE_REC_601] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_709] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
};

static const int32_t drm_gamma_functions[][9] = {
	[DRM_COLORSPACE_REC_601] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_COLORSPACE_REC_709] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
};

static const int32_t drm_eotf_functions[][9] = {
	[DRM_EOTF_SDR_TRADITIONAL] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_EOTF_HDR_TRADITIONAL] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_EOTF_HDR_ST2084] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_EOTF_HLG_BT2100] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
};

static const int32_t drm_oetf_functions[][9] = {
	[DRM_OETF_SDR_TRADITIONAL] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_OETF_HDR_TRADITIONAL] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_OETF_HDR_ST2084] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
	[DRM_OETF_HLG_BT2100] =
		{1, 0, 0, 0, 1, 0, 0, 0, 1},
};


static int
drm_prepare_plane_for_blending(struct drm_backend *b,
			struct drm_plane_state *ps,
			enum drm_colorspace target)
{
	int ret;
	uint32_t len;
	const int32_t *csc_func;
	const int32_t *deg_func;
	enum drm_colorspace current = ps->ev->surface->colorspace;

	deg_func = drm_get_degamma_curve(b, ps, &len);
	if (!deg_func) {
		drm_debug(b, "\t\t[state] Failed to find degamma curve\n");
		return -1;
	}

	csc_func = drm_get_csc_matrix(b, current, target);
	if (!csc_func) {
		drm_debug(b, "\t\t[state] Failed to find CSC curve\n");
		return -1;
	}

	if (ps->degamma_blob_id) {
		ret = drm_cleanup_plane_property_blob(b, ps, ps->degamma_blob_id); 
		if (ret) {
			drm_debug(b,"\t\t[state] Failed to clean degamma on plane %d\n",
					ps->plane->plane_id);
			return -1;
		}
		ps->degamma_blob_id = -1;
	}

	ret = drm_apply_plane_property_blob(b, ps, &ps->degamma_blob_id,
				deg_blob_len, deg_func); 
	if (ret) {
		drm_debug(b, "\t\t[state] Failed to apply plane degamma\n");
		return -1;
	}

	if (ps->csc_blob_id) {
		ret = drm_cleanup_plane_property_blob(b, ps, ps->csc_blob_id); 
		if (ret) {
			drm_debug(b,"Not applying CSC on plane %d\n", ps->plane->plane_id);
			return -1;
		}
		ps->csc_blob_id = -1;
	}

	ret = drm_apply_plane_property_blob(b, ps, &ps->csc_blob_id,
				9 * sizeof(int32_t),
				csc_func); 
	if (ret) {
		drm_debug(b, "\t\t[state] failed to apply plane CSC\n");
		ret = drm_cleanup_plane_property_blob(b, ps, ps->degamma_blob_id); 
		if (!ret)
			drm_debug(b,"\t\t[state] Failed to clean degamma post CSC\n");
		return -1;
	}

	drm_debug(b, "\t\t[state] Planes prepared for blending in %s\n", 
				colorspace_names[target]);
	return 0;
}

static const int32_t *
drm_get_gamma_curve(struct drm_backend *b,
			struct drm_plane_state *ps,
			uint32_t *len)
{
	const int32_t *deg_func;
	struct weston_surface *surface = ps->ev->surface;

	if (!surface)
		return NULL;

	if (surface->hdr_metadata)
		deg_func = drm_eotf_functions[surface->hdr_metadata->metadata.s.eotf];
	else
		deg_func = drm_gamma_functions[ps->ev->surface->colorspace];

	return deg_func;
}

static const int32_t *
drm_get_csc_matrix(struct drm_backend *b,
			enum drm_colorspace current,
			enum drm_colorspace target)
{
	if (current < DRM_COLORSPACE_REC_601 || current > DRM_COLORSPACE_REC_2020 ||
			target < DRM_COLORSPACE_REC_601 || target > DRM_COLORSPACE_REC_2020) {
		drm_debug(b, "\t\t[state] invalid input/output colorspace\n");
		return NULL;
	}

	return csc_transformation_functions[current][target];
}

/*
* The blending policy is:
*  - To blend everything in REC_2020 colorspace if any of the
*      surface is REC_2020.
*  - To tone map everything to HDR if any of the surface is HDR
* Find if color space conversion and/or tone mapping is required
* in the given output state. If yes, fill and return respective target
* colorspace and tone mapping plane states */

static void
drm_output_compute_changes_required(struct drm_output_state *s,
			struct drm_plane_state **ps_cs,
			struct drm_plane_state **ps_tonemap)
{
	struct drm_plane *p;
	struct drm_plane_state *ps;
	struct drm_plane_state *bt2020_plane = NULL;
	struct drm_plane_state *hdr_plane = NULL;
	bool has_narrow_gamut = false;

	wl_list_for_each(p, &s->plane_list, link) {
		struct weston_surface *surface;

		ps = drm_output_state_get_plane(s, p);
		surface = ps->ev->surface;

		/* Todo: We are supporting only one HDR/BT2020 surface for now.
		* Extend this to handle multiple such surfaces */
		if (surface->colorspace == DRM_COLORSPACE_REC_2020) {
			bt2020_plane = ps;

			/* A HDR surface is always BT2020 colorspace (or wider), but its
			* not the other way around always. There can be a non-HDR
			* BT2020 surface in plane list too, so check it.*/
			if (surface->hdr_metadata)
				hdr_plane = ps;
		} else {
			has_narrow_gamut = true;
		}
	}

	/* We need color space conversion when we have both narrow and wide
	* gamut surfaces to blend */
	if (has_narrow_gamut && bt2020_plane)
		*ps_cs = bt2020_plane;

	/* We need tone mapping when we have both HDR and SDR surfaces
	* to blend */
	if (has_narrow_gamut && hdr_plane)
		*ps_tonemap = hdr_plane;
}

static void
drm_output_get_surface_info(struct drm_output_state *s,
			drm_plane_state **ps_cs,
			drm_plane_state **ps_tonemap,
			struct drm_edid_hdr_metadata *display_md,
			enum drm_colorspace display_cs)
{
	struct drm_plane *p;
	struct drm_plane_state *ps;
	struct drm_plane_state *bt2020_plane = NULL;
	struct drm_plane_state *hdr_plane = NULL;
	bool has_narrow_gamut = false;

	wl_list_for_each(p, &s->plane_list, link) {
		struct weston_surface *surface;
		
		ps = drm_output_state_get_plane(s, p);
		surface = ps->ev->surface;

		/* If monitor is HDR: all the buffers will need H2H tone mapping */
		if (display_md) {
			/* Find a BT2020 buffer with HDR metadata */
			if (surface->colorspace >= DRM_COLORSPACE_DCI_P3 &&
				surface->hdr_metadata)
				tone_map_reqd = true;
		} else {
			/* If monitor is SDR: only the HDR buffer will need H2S mapping */	
		}
	}
}


void
drm_output_get_display_info(struct drm_output_state *state,
				uint8_t *widest_gamut,
				struct drm_edid_hdr_metadata **display_md)
{	
	uint8_t *edid;
	const uint8_t *colorimetry_blk;
	uint8_t data_len = 0;
	uint32_t blob_id;
	struct drm_output *output = state->output;
	struct drm_backend *b = output->backend;
	struct drm_head *head = to_drm_head(
				weston_output_get_first_head(&output->base));
	drmModePropertyBlobPtr edid_blob = NULL;
	drmModeObjectProperties *props;

	if (!widest_gamut || !display_md || !*display_md) {
		drm_debug(b, "NULL input to get info\n");
		return;
	}

	if (!head) {
		drm_debug(b, "No drm head found\n");
		return;
	}

	*widest_gamut = DRM_COLORSPACE_REC_709;
	*display_md = head->hdr_md;

	props = drmModeObjectGetProperties(b->drm.fd,
					   head->connector_id,
					   DRM_MODE_OBJECT_CONNECTOR);

	blob_id =
		drm_property_get_value(&head->props_conn[WDRM_CONNECTOR_EDID],
					props, 0);
	if (!blob_id) {
		drm_debug(b, "No edid blob id found\n");
		return;
	}

	edid_blob = drmModeGetPropertyBlob(head->backend->drm.fd, blob_id);
	if (!edid_blob) {
		drm_debug(b, "No edid blob found\n");
		return;
	}

	edid = edid_blob->data;
	if (!edid) {
		drm_debug(b, "No edid found\n");
		return;
	}

	colorimetry_blk = edid_find_extended_data_block(edid,
					&data_len,
					EDID_CEA_TAG_COLORIMETRY);
	if (!colorimetry_blk || !data_len) {
		drm_debug(b, "No colorimetry block found\n");
		return;
	}

	if (data_len > 1) {
		if (colorimetry_blk[1] & EDID_COLORIMETRY_DCIP3)
			*widest_gamut = DRM_COLORSPACE_DCI_P3;
	}

	if (colorimetry_blk[0] & EDID_COLORIMETRY_BT2020)
		*widest_gamut = DRM_COLORSPACE_REC_2020;

	drm_debug(b, "Widest gamut supported by monitor = %s\n",
			colorspace_names[*widest_gamut]);
}

int
drm_output_set_colorspace(struct drm_output_state *state)
{
	struct drm_plane *p;
	struct drm_plane_state *ps;
	struct weston_surface *psurf;
	enum drm_colorspace target_cspace;
	struct drm_plane_state *target_cs_plane = NULL;
	struct drm_plane_state *target_tonemap_ps = NULL;
	struct drm_backend *b = state->output->backend;
	struct drm_edid_hdr_metadata *display_md = NULL;

	bool ret = true;
	uint8_t tonemap_reqd = 0;
	uint8_t display_gamut = DRM_COLORSPACE_REC_709;

	/* Our blending policy and tone mapping policies are pretty
	* much to match output display's capabilities, so first get the
	* colorspace and hdr support information from display */
	drm_output_get_display_info(state, &display_gamut, &display_md);	

	/* Calculate the changes required to blend smoothly */
	drm_output_compute_changes_required(state,
			&target_cs_plane,
			&target_tonemap_ps);

	if (!target_cs_plane && !target_tonemap_ps)
		return 0;

	if (target_cspace < DRM_COLORSPACE_REC_2020) {
		/* If there is no colorspace information available in any of surfaces
		* Or the widest colorspace we have got is REC709, then
		* we don't need to do color space conversion in display, as
		* we have been blending REC601/609/709 together since
		* forever. We need to bother about this only when one of the
		* surface is in BT2020 colorspace, which is significantly wide from
		* others, and blending without bothering about it, would produce
		* very inaccurate outputs. This would be one of the use cases
		* for handling HDR buffers. */
		drm_debug(b, "\t\t[state] No colorspace transformation required\n");
		return 0;
	}

	/* We are here, means there is atleast one surface with REC_2020
	* colorspace, lets transform all other planes into 2020, so that we can
	* blend planes accurately */
	wl_list_for_each(p, &state->plane_list, link) {
		if (p->type != WDRM_PLANE_TYPE_OVERLAY)
			continue;

		ps = drm_output_state_get_plane(state, p);
		psurf = ps->ev->surface;

		/* Its possible that we have few surfaces without colorspace information,
		* but few with proper colorspace information. Its safe to assume that the
		* unknown colorspace is REC709 (most common), and map it to a wider
		* gamut than 709 if required. This case might be applicable for HDR playback
		* cases where there might be one HDR buffer (REC2020 space) and other
		* SDR buffers created in REC709 colorspace */
		if (psurf->colorspace == DRM_COLORSPACE_UNKNOWN)
			psurf->colorspace = DRM_COLORSPACE_REC_709;

		if (psurf->colorspace < target_cspace) {
			ret = drm_prepare_plane_for_blending(b, ps,
						target_cspace);
			if (!ret) {
				drm_debug(b, "\t\t[state] Failed to prepare plane for CSC\n");
				return -1;
			}

			drm_debug(b, "\t\t[state] CSC transformation done for plane %d\n",
					ps->plane->plane_id);
		}

		ret = drm_tone_map(b, ps, target_tonemap_ps);
		if (!ret) {
			drm_debug(b, "\t\t[state] Tone mapping failed\n");
			return -1;
		}
	}

	/*
	* Plane blending was done in linear space, this means to restore
	* the output accuracy, we have to restore the non-linearity of the
	* output, so apply CRTC output gamma here. */
	ret = drm_output_set_post_blending_gamma(state);
	if (!ret) {
		drm_debug(b, "\t\t[state] Failed to set post blending gamma\n");
		return -1;
	}

	drm_debug(b, "\t\t[state] Colorspace setup for blending done\n");
	return 0;
}

/* Get the wider gamut of the output state, for blending */
static struct drm_plane_state *
drm_output_find_widest_gamut(struct drm_output_state *state,
			struct tone_mapping *tone_map)
{
	struct drm_plane *p;
	struct drm_plane_state *ps;
	struct drm_plane_state *target_cs;
	struct drm_plane_state *target_tonemap;
	enum drm_colorspace widest = DRM_COLORSPACE_UNKNOWN;

	wl_list_for_each(p, &state->plane_list, link) {
		struct weston_surface *surface;

		ps = drm_output_state_get_plane(state, p);
		surface = ps->ev->surface;

		if (surface->colorspace > widest) {
			widest = ps->ev->surface->colorspace;
			target_cs = ps;
			if ()
		}
	}

	return widest;
}

static bool
drm_cleanup_plane_degamma(struct drm_backend *b,
			struct drm_plane_state *ps)
{
	int ret;

	ret = drm_mode_destroy_blob(b, ps->degamma_blob_id);
	if (ret != 0) {
		drm_debug(b, "\t\t[state] cannot destroy degamma blob %d\n", 
				ps->degamma_blob_id);
		return false;
	}

	ps->degamma_blob_id = 0;
	return true;
}

static bool
drm_apply_plane_degamma(struct drm_backend *b,
			struct drm_plane_state *ps,
			const int32_t *deg_func)
{
	int ret;

	if (ps->degamma_blob_id) {
		if (!drm_cleanup_plane_degamma(b, ps)) {
			drm_debug(b,"Not applying degamma on plane %d\n", ps->plane->plane_id);
			return false;
		}
	}

	/* Fixme: Sign of blob data */
	ret = drm_mode_create_blob(b, (uint8_t *)deg_func,
				ps->degamma_size,
				&ps->degamma_blob_id);
	if (ret != 0) {
		drm_debug(b, "\t\t[state] couldn't create degamma blob %d\n",
					ps->csc_blob_id);
		return false;
	}

	drm_debug(b, "\t\t[state] added degamma blob %d\n", ps->csc_blob_id);
	return true;
}

static bool
drm_cleanup_plane_gamma(struct drm_backend *b,
			struct drm_plane_state *ps)
{
	int ret;

	ret = drm_mode_destroy_blob(b, ps->gamma_blob_id);
	if (ret != 0) {
		drm_debug(b, "\t\t[state] cannot destroy gamma blob %d\n", 
				ps->gamma_blob_id);
		return false;
	}

	ps->gamma_blob_id = 0;
	return true;
}

static bool
drm_apply_plane_gamma(struct drm_backend *b,
			struct drm_plane_state *ps,
			const int32_t *gam_func)
{
	int ret;

	if (ps->gamma_blob_id) {
		if (!drm_cleanup_plane_gamma(b, ps)) {
			drm_debug(b,"Not applying gamma on plane %d\n", ps->plane->plane_id);
			return false;
		}
	}

	/* Fixme: Sign of blob data */
	ret = drm_mode_create_blob(b, (uint8_t *)gam_func,
				ps->gamma_size,
				&ps->gamma_blob_id);
	if (ret != 0) {
		drm_debug(b, "\t\t[state] couldn't create gamma blob %d\n",
					ps->gamma_blob_id);
		return false;
	}

	drm_debug(b, "\t\t[state] added gamma blob %d\n", ps->gamma_blob_id);
	return true;
}

static bool
drm_cleanup_plane_csc(struct drm_backend *b,
			struct drm_plane_state *ps)
{
	int ret;

	ret = drm_mode_destroy_blob(b, ps->csc_blob_id);
	if (ret != 0) {
		drm_debug(b, "\t\t[state] cannot destroy CSC blob %d\n",
				"Not applying CSC on plane %d\n",
				ps->csc_blob_id,
				ps->plane->plane_id);
		return false;
	}

	ps->csc_blob_id = 0;
	return true;
}

static bool
drm_apply_plane_csc(struct drm_backend *b,
			struct drm_plane_state *ps,
			const int32_t *csc_matrix)
{
	int ret;

	if (ps->csc_blob_id) {
		if (!drm_cleanup_plane_csc(b, ps)) {
			drm_debug(b,"Not applying CSC on plane %d\n", ps->plane->plane_id);
			return false;
		}
	}

	ret = drm_mode_create_blob(b, (uint8_t *)csc_matrix,
				9 * sizeof(uint32_t),
				&ps->csc_blob_id);
	if (ret != 0) {
		drm_debug(b, "\t\t[state] couldn't create CSC blob %d\n",
					ps->csc_blob_id);
		return false;
	}

	drm_debug(b, "\t\t[state] added CSC blob %d\n", ps->csc_blob_id);
	return true;
}
#endif

