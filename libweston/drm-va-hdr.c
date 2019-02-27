#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <va/va.h>
#include <va/va_vpp.h>
#include <va/va_drm.h>
#include <va/va_wayland.h>
#include <va/va_drmcommon.h>
#include <drm/drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "compositor-drm.h"
#include "pixel-formats.h"

static int
va_check_status(VAStatus va_status, const char *msg)
{
	if (va_status != VA_STATUS_SUCCESS) {
		weston_log_continue("VA error: %s: %s\n", msg, vaErrorStr(va_status));
		return 0;
	}
	return 1;
}

static void drm_va_destroy_surface(VADisplay va_display, VASurfaceID va_surface)
{
	VAStatus va_status;

	va_status = vaDestroySurfaces(va_display, &va_surface, 1);
	va_check_status(va_status, "vaDestroySurfaces");
}

static inline float
va_primary(short val)
{
	short temp = val & 0x3FF;
	short count = 1;
	float result = 0;

	/* Primary values in EDID are ecoded in 10 bit format, where every bit
	* represents 2 pow negative bit position, ex 0.500 = 1/2 = 2 ^ -1 = (1 << 9) */
	while (temp) {
		result += (!!(temp & (1 << 9)) * pow(2, -count));
		count++;
		temp <<= 1;
	}

	/* libVA wants the values to be scaled up between 1 - 50000 */
	return result * 50000;
}

static void drm_va_destroy_context(VADisplay va_display, VAContextID ctx_id)
{
	VAStatus va_status;

	if (ctx_id != VA_INVALID_ID) {
		va_status = vaDestroyContext(va_display, ctx_id);
		va_check_status(va_status, "vaDestroyContext");
	}
}

static void drm_va_destroy_config(VADisplay va_display, VAConfigID cfg_id)
{
	VAStatus va_status;

	if (cfg_id != VA_INVALID_ID) {
		va_status = vaDestroyConfig(va_display, cfg_id);
		va_check_status(va_status, "vaDestroyConfig");
	}
}

static void drm_va_destroy_buffer(VADisplay dpy,VABufferID buffer_id)
{
	VAStatus va_status;

	va_status = vaDestroyBuffer(dpy, buffer_id);
	va_check_status(va_status, "vaDestroyBuffer");
}

void drm_va_destroy_display(struct drm_va_display *d)
{
	if (d->output_surf_id != VA_INVALID_ID)
		drm_va_destroy_surface(d->va_display, d->output_surf_id);
	if (d->output_subsurf_id != VA_INVALID_ID)
		drm_va_destroy_surface(d->va_display, d->output_subsurf_id);
	if (d->pparam_buf_id != VA_INVALID_ID)
		drm_va_destroy_buffer(d->va_display, d->pparam_buf_id);
	if (d->cfg_id != VA_INVALID_ID)
		drm_va_destroy_config(d->va_display, d->cfg_id);
	if (d->ctx_id != VA_INVALID_ID)
		drm_va_destroy_context(d->va_display, d->ctx_id);
	vaTerminate(d->va_display);
	close(d->render_fd);
	free(d);
	return;
}

static struct drm_fb *
drm_va_create_fb_from_surface(struct drm_va_display *d,
                        VASurfaceID surface_id,
                        VADRMPRIMESurfaceDescriptor *va_desc)
{
	VAStatus status;
	uint32_t export_flags =  VA_EXPORT_SURFACE_COMPOSED_LAYERS |
			VA_EXPORT_SURFACE_READ_ONLY |
			VA_EXPORT_SURFACE_WRITE_ONLY;

	/* Sync surface before expoting buffer, blocking call */
	status = vaSyncSurface(d->va_display, surface_id);
	if (!va_check_status(status, "vaSyncSurface")) {
		weston_log_continue("VA: Failed to sync surface to buffer\n");
		return NULL;
	}

	/* Get a prime handle of the fb assosiated with surface */
	status = vaExportSurfaceHandle(d->va_display,
			surface_id,
			VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
			export_flags,
			va_desc);
	if (!va_check_status(status, "vaExportSurfaceHandle")) {
		weston_log_continue("VA: Failed to export surface to buffer\n");
		return NULL;
	}

	return drm_fb_get_from_vasurf(d, va_desc);
}

static VASurfaceID
drm_va_create_surface_from_fb(struct drm_va_display *d,
				struct drm_fb *fb)
{
	int ret;
	int prime_fd;
	VAStatus va_status;
	VASurfaceID surface;
	VASurfaceAttribExternalBuffers external;
	VASurfaceAttrib attribs[2];
	uint32_t surf_fourcc;
	uint32_t surf_format;

	/* We support only P010 (Video) or RGB32(subs) currently */
	if (fb->format->format == DRM_FORMAT_P010) {
		surf_fourcc  = VA_FOURCC('P', '0', '1', '0');
		surf_format  = VA_RT_FORMAT_YUV420_10;
	} else {
		surf_fourcc  = VA_FOURCC_ARGB;
		surf_format  = VA_RT_FORMAT_RGB32;
	}

	ret = drmPrimeHandleToFD(fb->fd, fb->handles[0], DRM_CLOEXEC, &prime_fd);
	if (ret) {
		weston_log("VA: drmloctl get prime handle Failed\n");
		return VA_INVALID_SURFACE;
	}

	memset(&external, 0, sizeof(external));
	external.pixel_format = surf_fourcc;
	external.width = fb->width;
	external.height = fb->height;
	external.data_size = fb->width * fb->height * 32 / 8;
	external.num_planes = 1;
	external.pitches[0] = fb->strides[0];
	external.buffers = (uintptr_t *)&prime_fd;
	external.num_buffers = 1;

	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].type = VASurfaceAttribMemoryType;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
	attribs[1].value.type = VAGenericValueTypePointer;
	attribs[1].value.value.p = &external;

	va_status = vaCreateSurfaces(d->va_display, surf_format,
					fb->width, fb->height,
					&surface, 1, attribs,
					ARRAY_SIZE(attribs));
	if (!va_check_status(va_status, "vaCreateSurfaces")) {
		weston_log("VA: failed to create surface\n");
		return VA_INVALID_SURFACE;
	}

	close(prime_fd);
	weston_log("VA: Created input surface, format 0x%x\n",VA_FOURCC_P010);
	return surface;
}

static VASurfaceID
drm_va_create_surface(struct drm_va_display *d,
				int width, int height, uint32_t surf_format)
{
    VAStatus va_status;
	VASurfaceID surface_id;
    VASurfaceAttrib surface_attrib;
	uint32_t surf_fourcc = VA_FOURCC_RGBA;

    surface_attrib.type =  VASurfaceAttribPixelFormat;
    surface_attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    surface_attrib.value.type = VAGenericValueTypeInteger;
    surface_attrib.value.value.i = surf_fourcc;

    va_status = vaCreateSurfaces(d->va_display,
                                 surf_format,
                                 width,
                                 height,
                                 &surface_id,
                                 1,
                                 &surface_attrib,
                                 1);

    if (!va_check_status(va_status, "vaCreateSurfaces")) {
		weston_log_continue("VA: failed to create surface\n");
		return VA_INVALID_SURFACE;
	}

	weston_log_continue("VA: Created output surface, format %x\n", surf_format);
	return surface_id;
}

static void
drm_va_setup_surfaces(VARectangle *surface_region,
	VARectangle *output_region, struct drm_fb *fb)
{
	surface_region->x = 0;
	surface_region->y = 0;
	surface_region->width = fb->width;
	surface_region->height = fb->height;

	output_region->x = 0;
	output_region->y = 0;
	output_region->width = fb->width;
	output_region->height = fb->height;
}

static int
drm_va_set_output_tm_metadata(struct weston_hdr_metadata *content_md, 
				struct drm_tone_map *tm,
				VAHdrMetaDataHDR10 *o_hdr10_md,
				VAHdrMetaData *out_metadata)
{
	const struct drm_hdr_metadata_static *t_smd = &tm->target_md;

	/* SDR target display */
	if (tm->tone_map_mode == VA_TONE_MAPPING_HDR_TO_SDR) {

		/* Hard coding values to standard SDR libva values */
		o_hdr10_md->display_primaries_x[0] = 15000;
        o_hdr10_md->display_primaries_y[0] = 30000;
        o_hdr10_md->display_primaries_x[1] = 32000;
        o_hdr10_md->display_primaries_y[1] = 16500;
        o_hdr10_md->display_primaries_x[2] = 7500;
        o_hdr10_md->display_primaries_y[2] = 3000;
        o_hdr10_md->white_point_x = 15635;
        o_hdr10_md->white_point_y = 16450;
		o_hdr10_md->max_display_mastering_luminance = 500;
		o_hdr10_md->min_display_mastering_luminance = 1;
		o_hdr10_md->max_content_light_level = 4000;
		weston_log_continue("VA: No output metadata found\n");
		return 0;
	}

	o_hdr10_md->max_display_mastering_luminance = t_smd->max_mastering_luminance;
	o_hdr10_md->min_display_mastering_luminance = t_smd->min_mastering_luminance;
	o_hdr10_md->max_pic_average_light_level = t_smd->max_fall;
	o_hdr10_md->max_content_light_level = t_smd->max_cll;

	o_hdr10_md->white_point_x = va_primary(t_smd->white_point_x);
	o_hdr10_md->white_point_y = va_primary(t_smd->white_point_y);
	o_hdr10_md->display_primaries_x[0] = va_primary(t_smd->primary_g_x);
	o_hdr10_md->display_primaries_x[1] = va_primary(t_smd->primary_b_x);
	o_hdr10_md->display_primaries_x[2] = va_primary(t_smd->primary_r_x);
	o_hdr10_md->display_primaries_y[0] = va_primary(t_smd->primary_g_y);
	o_hdr10_md->display_primaries_y[1] = va_primary(t_smd->primary_b_y);
	o_hdr10_md->display_primaries_y[2] = va_primary(t_smd->primary_r_y);

	out_metadata->metadata_type = VAProcHighDynamicRangeMetadataHDR10;
	out_metadata->metadata = o_hdr10_md;
	out_metadata->metadata_size = sizeof(VAHdrMetaDataHDR10);
	return 0;
}

static VAStatus
drm_va_create_input_tm_filter(struct drm_va_display *d,
					struct weston_hdr_metadata *c_md,
					uint8_t tm_type)
{
    VAStatus va_status = VA_STATUS_ERROR_INVALID_VALUE;
	VAContextID context_id = d->ctx_id;
	VABufferID *fparam_buf_id = &d->fparam_buf_id;
	VAHdrMetaDataHDR10 *in_hdr10_md =  &d->in_hdr10_md;
	VAProcFilterParameterBufferHDRToneMapping *hdr_tm_param = &d->hdr_tm_param;
	struct weston_hdr_metadata_static *s;

	switch (tm_type) {
	case VA_TONE_MAPPING_HDR_TO_HDR:
	case VA_TONE_MAPPING_HDR_TO_SDR:
		/* input is HDR frame */
		if (!c_md) {
			weston_log_continue("VA: No input HDR metadata for tone mapping\n");
			return va_status;
		}

		s = &c_md->metadata.static_metadata;
		in_hdr10_md->max_display_mastering_luminance = s->max_luminance;
		in_hdr10_md->min_display_mastering_luminance = s->min_luminance;
		in_hdr10_md->max_content_light_level = s->max_cll;
		in_hdr10_md->max_pic_average_light_level = s->max_fall;
		in_hdr10_md->display_primaries_x[0] = va_primary(s->display_primary_b_x);
		in_hdr10_md->display_primaries_y[0] = va_primary(s->display_primary_b_y);
		in_hdr10_md->display_primaries_x[1] = va_primary(s->display_primary_g_x);
		in_hdr10_md->display_primaries_y[1] = va_primary(s->display_primary_g_y);
		in_hdr10_md->display_primaries_x[2] = va_primary(s->display_primary_r_x);
		in_hdr10_md->display_primaries_y[2] = va_primary(s->display_primary_r_y);
		in_hdr10_md->white_point_x = va_primary(s->white_point_x);
		in_hdr10_md->white_point_y = va_primary(s->white_point_y);
		break;

	case VA_TONE_MAPPING_SDR_TO_HDR:
		/* Hard coding values to standard SDR libva values */
		in_hdr10_md->display_primaries_x[0] = 15000;
        in_hdr10_md->display_primaries_y[0] = 30000;
        in_hdr10_md->display_primaries_x[1] = 32000;
        in_hdr10_md->display_primaries_y[1] = 16500;
        in_hdr10_md->display_primaries_x[2] = 7500;
        in_hdr10_md->display_primaries_y[2] = 3000;
        in_hdr10_md->white_point_x = 15635;
        in_hdr10_md->white_point_y = 16450;
		in_hdr10_md->max_display_mastering_luminance = 500;
		in_hdr10_md->min_display_mastering_luminance = 1;
		in_hdr10_md->max_content_light_level = 4000;
		break;
	}

	hdr_tm_param->type = VAProcFilterHighDynamicRangeToneMapping;
	hdr_tm_param->data.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
	hdr_tm_param->data.metadata = in_hdr10_md;
	hdr_tm_param->data.metadata_size = sizeof(VAHdrMetaDataHDR10);

	va_status = vaCreateBuffer(d->va_display, context_id,
				VAProcFilterParameterBufferType, 
				sizeof(VAProcFilterParameterBufferHDRToneMapping),
				1, (void *)hdr_tm_param,
				fparam_buf_id);
	if (!va_check_status(va_status,"vaCreateBuffer tonemapping"))
		return va_status;

	return VA_STATUS_SUCCESS;
}

static void
drm_va_init_hdr_buffers(struct drm_va_display *d)
{
	memset(&d->out_md_params, 0, sizeof(d->out_md_params));
	memset(&d->in_hdr10_md, 0, sizeof(d->in_hdr10_md));
	memset(&d->hdr_tm_param, 0, sizeof(d->hdr_tm_param));
	memset(&d->output_metadata, 0, sizeof(d->output_metadata));
}

static int
drm_va_create_pipeline_buffer(struct drm_va_display *d)
{
	VAStatus va_status;
	VAProcPipelineParameterBuffer *pparam = &d->pparam;

	memset(&d->pparam, 0, sizeof(d->pparam));
	pparam->input_color_properties.colour_primaries = 9;
	pparam->input_color_properties.transfer_characteristics = 16;
	pparam->output_color_properties.colour_primaries = 9;
	pparam->output_color_properties.transfer_characteristics = 16;
	pparam->output_color_standard = VAProcColorStandardExplicit;
	pparam->surface_color_standard = VAProcColorStandardExplicit;
	pparam->output_hdr_metadata = &d->output_metadata;

	/* Create pipeline buffer */
	va_status = vaCreateBuffer(d->va_display, d->ctx_id,
				   VAProcPipelineParameterBufferType,
				   sizeof(VAProcPipelineParameterBuffer),
				   1, &pparam, &d->pparam_buf_id);
	if (!va_check_status(va_status, "vaCreateBuffer")) {
		weston_log_continue("VA: Failed to create pipeline buffer\n");
		return -1;
	}

	return 0;
}

static VAStatus
drm_va_create_hdr_filter(struct drm_va_display *d,
			struct weston_hdr_metadata *c_md,
			uint8_t tm_type)
{
	uint32_t i;
	VAStatus va_status;
	VAContextID context_id = d->ctx_id;
	uint32_t num_hdr_tm_caps = VAProcHighDynamicRangeMetadataTypeCount;
	VAProcFilterCapHighDynamicRange hdr_tm_caps[num_hdr_tm_caps];

	memset(hdr_tm_caps, 0, num_hdr_tm_caps *
			sizeof(VAProcFilterCapHighDynamicRange));
	va_status = vaQueryVideoProcFilterCaps(d->va_display, context_id,
					VAProcFilterHighDynamicRangeToneMapping,
					(void *)hdr_tm_caps, &num_hdr_tm_caps);
	if (!va_check_status(va_status, "Check HDR capability\n") ||
			!num_hdr_tm_caps) {
		weston_log_continue("VA: No HDR capability found\n");
		return -1;
	}

	for (i = 0; i < num_hdr_tm_caps; ++i) {
		weston_log_continue("VA: tm caps[%d]: metadata type %d, flag %d\n", i,
			hdr_tm_caps[i].metadata_type, hdr_tm_caps[i].caps_flag);
	}

	return drm_va_create_input_tm_filter(d, c_md, tm_type);
}

uint32_t
drm_tone_mapping_mode(struct weston_hdr_metadata *content_md,
		struct drm_edid_hdr_metadata_static *target_md)
{
	uint32_t tm_type;

	/* HDR content and HDR display */
	if (content_md && target_md)
		tm_type = VA_TONE_MAPPING_HDR_TO_HDR;

	/* HDR content and SDR display */
	if (content_md && !target_md)
		tm_type = VA_TONE_MAPPING_HDR_TO_SDR;

	/* SDR content and HDR display */
	if (!content_md && target_md)
		tm_type = VA_TONE_MAPPING_SDR_TO_HDR;

	/* SDR content and SDR display */
	if (!content_md && !target_md)
		return 0;

	return tm_type;
}

static VAStatus
drm_va_process_buffer(struct drm_va_display *d,
	const VARectangle *surface_region,
	const VARectangle *output_region,
	VASurfaceID in_surf_id,
	VASurfaceID out_surf_id)
{
	VAProcPipelineParameterBuffer *pipeline_param;
	VAStatus status;

	status = vaMapBuffer(d->va_display, d->pparam_buf_id,
				(void **) &pipeline_param);
	if (!va_check_status(status, "vaMapBuffer")) {
		weston_log("VA: failed to remap pipeline buffer\n");
		return status;
	}

	memset(pipeline_param, 0, sizeof *pipeline_param);
	pipeline_param->filter_flags = 0;
	pipeline_param->surface = in_surf_id;
	pipeline_param->num_filters = 1;
	pipeline_param->filters = &d->fparam_buf_id;
	pipeline_param->surface_region = surface_region;
	pipeline_param->output_region = output_region;

	pipeline_param->input_color_properties.colour_primaries = 9;
	pipeline_param->input_color_properties.transfer_characteristics = 16;
	pipeline_param->output_color_properties.colour_primaries = 9;
	pipeline_param->output_color_properties.transfer_characteristics = 16;
	pipeline_param->output_color_standard = VAProcColorStandardExplicit;
	pipeline_param->surface_color_standard = VAProcColorStandardExplicit;
	pipeline_param->output_hdr_metadata = &d->output_metadata;

	status = vaUnmapBuffer(d->va_display, d->pparam_buf_id);
	if (!va_check_status(status, "vaUnMapBuffer")) {
		weston_log("VA: failed to re-unmap pipeline buffer\n");
		return status;
	}

	status = vaBeginPicture(d->va_display, d->ctx_id, out_surf_id);
	if (!va_check_status(status, "vaBeginPicture")) {
		weston_log("VA: failed vaBegin\n");
		return status;
	}

	status = vaRenderPicture(d->va_display, d->ctx_id, &d->pparam_buf_id, 1);
	if (!va_check_status(status, "vaRenderPicture")) {
		weston_log("VA: failed vaRender\n");
		return status;
	}

	status = vaEndPicture(d->va_display, d->ctx_id);
	if (!va_check_status(status, "vaEndPicture")) {
		weston_log("VA: failed vaEnd\n");
		return status;
	}

	weston_log("VA: Success: processing\n");
	return status;
}

/*
* This is a limited tone mapping API in DRM backend, which uses
* libVA's tone mapping infrastructure, and maps a SDR input buffer
* to HDR10 luminance range. Currently only HDR10 is supported
* among all HDR options.
*
* inputs:
* d: drm_va_display structure, which contains initialized display reference.
* ps: drm plane state, which already contains a weston surface framebuffer.
* tm_type: tone mapping type (like S2H, H2H etc)
* target_md: hdr metadata descriptor for target of tone mapping.
*
* returns: tone mapped framebuffer on success, else NULL
*/
struct drm_fb *
drm_va_tone_map(struct drm_va_display *d,
		struct drm_fb *fb,
		struct weston_hdr_metadata *content_md,
		struct drm_tone_map *tm)
{
	int ret = 0;
	VAStatus va_status = VA_STATUS_SUCCESS;
	VASurfaceID in_surface_id = VA_INVALID_SURFACE;
	VASurfaceID out_surface_id = VA_INVALID_SURFACE;
	VARectangle surface_region = {0,};
	VARectangle output_region = {0,};
	VADRMPRIMESurfaceDescriptor va_desc = {0,};
	struct drm_fb *out_fb = NULL;

	if (!d || !fb || !tm) {
		weston_log_continue("VA: NULL input, d=%p fb=%p tm=%p VA not initialized ?\n", d, fb, tm);
		return NULL;
	}

	in_surface_id = drm_va_create_surface_from_fb(d, fb);
	if (VA_INVALID_SURFACE == in_surface_id) {
		weston_log("VA: Failed to create input surface\n");
		goto clear_surf;
	}

	/* Setup input tonemapping buffer filter */
	va_status  = drm_va_create_hdr_filter(d, content_md, tm->tone_map_mode);
	if (va_status != VA_STATUS_SUCCESS) {
		weston_log("VA: Can't create HDR filter, tone map failed\n");
		ret = -1;
		goto clear_surf;
	}

	/* Setup output tonemapping properties */
	ret = drm_va_set_output_tm_metadata(content_md, tm, &d->out_md_params,
				&d->output_metadata);
	if (ret) {
		weston_log("VA: Can't setup HDR metadata\n");
		ret = -1;
		goto clear_buf;
	}

	/* Steup target rectangles */
	drm_va_setup_surfaces(&surface_region, &output_region, fb);

	/* Try to accommodate subtitles or smaller frames in small surface */
	if (fb->width < 1000 && fb->height < 300)
		out_surface_id = d->output_subsurf_id;
	else
		out_surface_id = d->output_surf_id;

	/* Do the actual magic */
	va_status = drm_va_process_buffer(d, &surface_region, &output_region,
		in_surface_id, out_surface_id);
	if (va_status != VA_STATUS_SUCCESS) {
		weston_log("VA: failed to process tone mapping buffer\n");
		goto clear_buf;
	}

	/* Get a drm buffer from tone mapped buffer */
	out_fb = drm_va_create_fb_from_surface(d, out_surface_id, &va_desc);
	if (!out_fb)
		weston_log("VA: Failed to tone map buffer\n");

clear_buf:
	drm_va_destroy_buffer(d->va_display, d->fparam_buf_id);
clear_surf:
	drm_va_destroy_surface(d->va_display, in_surface_id);
	return out_fb;
}

static VAContextID
drm_va_create_context_nosurf(struct drm_va_display *d,
				VASurfaceID *surface_id,
				VAContextID *context_id,
				int out_w, int out_h)
{
	VAStatus va_status;

	/* Create a Video processing Context */
	va_status = vaCreateContext(d->va_display,
								d->cfg_id,
								out_w, out_h, 0,
								NULL, 0, context_id);

	if (!va_check_status(va_status,"vaCreateContext"))
		return -1;
	return 0;
}

static int
drm_va_create_config(struct drm_va_display *d)
{
	VAStatus va_status;
	VAConfigID cfg_id;

	/* Create a video processing config */
	va_status = vaCreateConfig(d->va_display,
					VAProfileNone, VAEntrypointVideoProc,
					&d->attrib, 1, &cfg_id);
	if (!va_check_status(va_status,"vaCreateConfig"))
		return -1;

	d->cfg_id = cfg_id;
	return 0;
}

static int drm_va_check_entrypoints(struct drm_va_display *d)
{
	int i;
	int32_t num_entrypoints;
	VAStatus va_status;
	VAEntrypoint entrypoints[5];
	VADisplay va_display = d->va_display;

	/* Query supported Entrypoint for video processing, Check whether VPP is supported by driver */
	num_entrypoints = vaMaxNumEntrypoints(va_display);
	va_status = vaQueryConfigEntrypoints(va_display, VAProfileNone,
					entrypoints, &num_entrypoints);
	if (!va_check_status(va_status,"vaQueryConfigEntrypoints"))
		return -1;

	for (i = 0; i < num_entrypoints; i++)
		if (entrypoints[i] == VAEntrypointVideoProc)
			break;

	if (i == num_entrypoints) {
		weston_log("No entry point found\n");
		return -1;
	}

	return 0;
}

static int drm_va_check_attributes(struct drm_va_display *d)
{
	VAStatus va_status;

	/* Query supported RenderTarget format */
	d->attrib.type = VAConfigAttribRTFormat;
	va_status = vaGetConfigAttributes(d->va_display, VAProfileNone,
				VAEntrypointVideoProc, &d->attrib, 1);
	if (!va_check_status(va_status,"vaGetConfigAttributes")) {
		weston_log_continue("VA: failed to get attributes\n");
		return -1;
	}

	return 0;
}

static int
drm_va_init_display(struct drm_va_display *d)
{
	int render_fd;
	VAStatus va_status;
	VADisplay va_display;

	if (!d)
		return -1;

	render_fd = open("/dev/dri/renderD128", O_RDWR);
	if (render_fd < 0) {
		weston_log_continue("failed to open render device\n");
		return -1;
	}
	
	va_display = vaGetDisplayDRM(render_fd);
	if (!va_display) {
		weston_log("Can't get DRM display\n");
		close(render_fd);
		return -1;
	}

	va_status = vaInitialize(va_display, &d->major_ver,
				&d->minor_ver);
	if (!va_check_status(va_status,"vaInitialize")) {
		close(render_fd);
		return -1;
	}

	d->render_fd = render_fd;
	d->va_display = va_display;
	return 0;
}

struct drm_va_display *
drm_va_create_display(struct drm_backend *backend)
{
	struct drm_va_display *d;

	d = malloc(sizeof(struct drm_va_display));
	if (!d) {
		weston_log_continue("OOM va init\n");
		return NULL;
	}

	d->b = backend;
	d->ctx_id = VA_INVALID_ID;
	d->cfg_id = VA_INVALID_ID;
	d->pparam_buf_id = VA_INVALID_ID;
	d->fparam_buf_id = VA_INVALID_ID;
	d->output_subsurf_id = VA_INVALID_ID;
	d->output_surf_id = VA_INVALID_ID;

	if (drm_va_init_display(d)) {
		weston_log_continue("VA: Init failed\n");
		free(d);
		return NULL;
	}

	if (drm_va_check_entrypoints(d)) {
		weston_log_continue("VA: Entry point check failed\n");
		goto error;
	}

	if (drm_va_check_attributes(d)) {
		weston_log_continue("VA: Attribute check failed\n");
		goto error;
	}

	if (drm_va_create_config(d)) {
		weston_log_continue("VA: Cant create config\n");
		goto error;
	}

	if (drm_va_create_context_nosurf(d, NULL, &d->ctx_id, 3840, 2160)) {
		weston_log_continue("VA: Can't create config\n");
		goto error;
	}

	if (drm_va_create_pipeline_buffer(d)) {
		weston_log_continue("VA: Can't create config\n");
		goto error;
	}

	d->output_surf_id = drm_va_create_surface(d,
			3840,
			2160,
			VA_RT_FORMAT_RGB32_10);
	if (d->output_surf_id == VA_INVALID_ID) {
		weston_log_continue("VA: Can't create output surface\n");
		goto error;
	}

	d->output_subsurf_id = drm_va_create_surface(d,
			1000,
			200,
			VA_RT_FORMAT_RGB32);
	if (d->output_surf_id == VA_INVALID_ID) {
		weston_log_continue("VA: Can't create output surface\n");
		goto error;
	}

	drm_va_init_hdr_buffers(d);
	weston_log_continue("VA: Successfully created initial display config\n");
	return d;

error:
	drm_va_destroy_display(d);
	return NULL;
}

