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
			struct drm_edid_hdr_metadata_static *display_md)
{
	return drm_setup_property_blob(b, &state->hdr_metadata_blob_id,
				sizeof(*display_md), (const uint8_t *)display_md);
}

static int
drm_output_setup_gamma(struct drm_backend *b,
			struct drm_output_state *state,
			struct drm_edid_hdr_metadata_static *display_md)
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
	struct drm_edid_hdr_metadata_static *display_md = head->hdr_md;
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

#if 0
		tone_mapped_fb = drm_tone_map(b, ps, display_md);
		if (!tone_mapped_fb) {
			weston_log("Shashank: Failed to tone map\n");
			drm_debug(b, "\t\t[state] Tone mapping failed\n");
			return -1;
		}

#warning: Mem Leak here ?
		/* Replace plane's fb with tone mapped fb, is there a memory leak here ? */
		ps->fb = tone_mapped_fb;
#endif
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

