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
#include "shared/colorspace.h"
#include "compositor-drm.h"
#include "drm-color-transformation.h"

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

enum weston_colorspace_enums
drm_to_weston_colorspace(uint8_t drm_cs)
{
	if (drm_cs == DRM_COLORSPACE_REC709)
		return WESTON_CS_BT709;
	if (drm_cs == DRM_COLORSPACE_REC2020)
		return WESTON_CS_BT2020;
	if (drm_cs == DRM_COLORSPACE_DCIP3)
		return WESTON_CS_DCI_P3;

	return WESTON_CS_UNDEFINED;
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

/* This function is just for the sake of completion of array */
static void
noop_invalid_matrix(double result[3][3])
{
	return;
}

void
create_unity_matrix(double result[3][3])
{	
	memset(result, 0, 9 * sizeof(double));
	result[0][0] = 1.0;
	result[1][1] = 1.0;
	result[2][2] = 1.0;
	return;
}

/* Array of function ptrs which generate CSC matrix */
void (*generate_csc_fptrs[][DRM_COLORSPACE_MAX])(double[3][3]) = {
	[DRM_COLORSPACE_REC709][DRM_COLORSPACE_REC709] =
		create_unity_matrix,
	[DRM_COLORSPACE_REC709][DRM_COLORSPACE_DCIP3] =
		create_709_to_DCIP3_matrix,
	[DRM_COLORSPACE_REC709][DRM_COLORSPACE_REC2020] =
		create_709_to_2020_matrix,
	[DRM_COLORSPACE_DCIP3][DRM_COLORSPACE_REC709] =
		noop_invalid_matrix,
	[DRM_COLORSPACE_DCIP3][DRM_COLORSPACE_DCIP3] =
		noop_invalid_matrix,
	[DRM_COLORSPACE_DCIP3][DRM_COLORSPACE_REC2020] =
		noop_invalid_matrix,
	[DRM_COLORSPACE_REC2020][DRM_COLORSPACE_REC709] =
		create_2020_to_709_matrix,
	[DRM_COLORSPACE_REC2020][DRM_COLORSPACE_DCIP3] =
		create_2020_to_DCIP3_matrix,
	[DRM_COLORSPACE_REC2020][DRM_COLORSPACE_REC2020] =
		create_unity_matrix,
};

/* Generate LUT for gamut mapping */
void
generate_csc_lut(struct drm_backend *b,
			double csc_matrix[3][3],
		    enum drm_colorspace current,
		    enum drm_colorspace target)
{
	void (*generate_csc_matrix_fn)(double[3][3]);

	generate_csc_matrix_fn = generate_csc_fptrs[current][target];
	if (generate_csc_matrix_fn == noop_invalid_matrix) {
		weston_log("invalid input colorspace\n");
		return;
	}

	generate_csc_matrix_fn(csc_matrix);
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

struct drm_color_lut *
generate_OETF_2084_lut(struct drm_backend *b,
			int lut_size, uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		weston_log("\t\t[state] invalid input/output colorspace\n");
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

struct drm_color_lut *
generate_EOTF_2084_lut(struct drm_backend *b,
		int lut_size, uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		weston_log("\t\t[state] invalid input/output colorspace\n");
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

struct drm_color_lut *
generate_gamma_lut(struct drm_backend *b,
			 int lut_size,
			 uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		weston_log("\t\t[state] OOM creating gamma lut\n");
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

/* https://en.wikipedia.org/wiki/SRGB#The_forward_transformation_.28CIE_xyY_or_CIE_XYZ_to_sRGB.29 */
static inline double
get_srgb_decoding_value(double input)
{
	return input <= 0.04045 ? input / 12.92 :
		pow(((input + 0.055) / 1.055), 2.4);
}

struct drm_color_lut *
generate_degamma_lut(struct drm_backend *b,
			 int lut_size,
			 uint16_t max_val)
{
	int i;
	struct drm_color_lut *lut;

	lut = malloc(lut_size * sizeof(struct drm_color_lut));
	if (!lut) {
		weston_log("\t\t[state] invalid input/output colorspace\n");
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
