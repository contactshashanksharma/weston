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
#include <va/va_drmcommon.h>
#include <drm/drm.h>
#include <xf86drm.h>

#include "compositor-drm.h"
#include "pixel-formats.h"

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

static int
va_check_status(VAStatus va_status, const char *msg)
{
	if (va_status != VA_STATUS_SUCCESS) {
		weston_log("VA error: %s: %s\n", msg, vaErrorStr(va_status));
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

static void drm_va_destroy_context(VADisplay va_display, VAContextID context)
{
	VAStatus va_status;

	va_status = vaDestroyContext(va_display, context);
	va_check_status(va_status, "vaDestroyContext");
}

static void drm_va_destroy_config(VADisplay va_display, VAConfigID config_id)
{
	VAStatus va_status;

	va_status = vaDestroyConfig(va_display, config_id);
	va_check_status(va_status, "vaDestroyConfig");
}

static void drm_va_destroy_buffer(VADisplay dpy,VABufferID buffer_id)
{
	VAStatus va_status;

	va_status = vaDestroyBuffer(dpy, buffer_id);
	va_check_status(va_status, "vaDestroyBuffer");
}

static VAContextID
drm_va_create_context(struct drm_va_display *d,
				VASurfaceID surface_id,
				VAContextID *context_id,
				int out_w, int out_h)
{
	VAStatus va_status;

	/* Create a Video processing Context */
	va_status = vaCreateContext(d->va_display,
								d->config_id,
								out_w, out_h, VA_PROGRESSIVE,
								&surface_id, 1, context_id);
	if (!va_check_status(va_status,"vaCreateContext"))
		return -1;

	return 0;
}

static VAConfigID
drm_va_create_config(struct drm_va_display *d)
{
	VAStatus va_status;
	VAConfigID config_id;

	/* Create a video processing config */
	va_status = vaCreateConfig(d->va_display,
					VAProfileNone, VAEntrypointVideoProc,
					&d->attrib, 1, &config_id);
	if (!va_check_status(va_status,"vaCreateConfig"))
		return VA_INVALID_ID;

	return config_id;
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
		printf("No entry point found\n");
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
		weston_log("VA: failed to get attributes\n");
		return -1;
	}

	return 0;
}

static VADisplay drm_va_init_display(struct drm_va_display *d)
{
	VAStatus va_status;
	VADisplay va_display;

	if (!d)
		return NULL;

	d->drm_fd = open("/dev/dri/card0", O_RDWR);
	if (d->drm_fd < 0) {
		printf("Cant get DRM device\n");
		return NULL;
	}

	va_display = vaGetDisplayDRM(d->drm_fd);
	if (!va_display) {
		printf("Cant get DRM display\n");
		close(d->drm_fd);
		return NULL;
	}

	va_status = vaInitialize(va_display, &d->major_ver,
				&d->minor_ver);
	if (!va_check_status(va_status,"vaInitialize"))
		return NULL;

	return va_display;
}

int drm_va_create_display(struct drm_va_display *d)
{
	d->va_display = drm_va_init_display(d);
	if (!d->va_display) {
		weston_log("VA: Init failed\n");
		return -1;
	}

	if (drm_va_check_entrypoints(d)) {
		weston_log("VA: Entry point check failed\n");
		return -1;
	}

	if (drm_va_check_attributes(d)) {
		weston_log("VA: Attribute check failed\n");
		return -1;
	}

	d->config_id = drm_va_create_config(d);
	if (d->config_id == VA_INVALID_ID) {
		weston_log("VA: Cant create config\n");
		return -1;
	}

	return 0;
}

void drm_va_destroy_display(struct drm_va_display *d)
{
	if (d->va_display) {
		drm_va_destroy_config(d->va_display, d->config_id);
		vaTerminate(d->va_display);
	}
	close(d->drm_fd);
}

static VABufferID
drm_va_create_input_tm_filter(struct drm_va_display *d,
					struct weston_hdr_metadata *c_md,
					VAContextID context_id,
					uint8_t tm_type)
{
    VAStatus va_status = VA_STATUS_SUCCESS;
	VAHdrMetaDataHDR10 in_hdr10_md = {0, };
	VABufferID filter_param_buf_id = VA_INVALID_ID;
	VAProcFilterParameterBufferHDRToneMapping hdr_tm_param;
	struct weston_hdr_metadata_static *s;

	switch (tm_type) {
	case VA_TONE_MAPPING_HDR_TO_HDR:
	case VA_TONE_MAPPING_HDR_TO_SDR:
		/* input is HDR frame */
		if (!c_md) {
			weston_log("VA: No input HDR metadata for tone mapping\n");
			return -1;
		}

		s = &c_md->metadata.s;
		in_hdr10_md.max_display_mastering_luminance = s->max_luminance;
		in_hdr10_md.min_display_mastering_luminance = s->min_luminance;
		in_hdr10_md.max_content_light_level = s->max_cll;
		in_hdr10_md.max_pic_average_light_level = s->max_fall;
		in_hdr10_md.display_primaries_x[0] = va_primary(s->display_primary_b_x);
		in_hdr10_md.display_primaries_y[0] = va_primary(s->display_primary_b_y);
		in_hdr10_md.display_primaries_x[1] = va_primary(s->display_primary_g_x);
		in_hdr10_md.display_primaries_y[1] = va_primary(s->display_primary_g_y);
		in_hdr10_md.display_primaries_x[2] = va_primary(s->display_primary_r_x);
		in_hdr10_md.display_primaries_y[2] = va_primary(s->display_primary_r_y);
		in_hdr10_md.white_point_x = va_primary(s->white_point_x);
		in_hdr10_md.white_point_y = va_primary(s->white_point_y);
		break;

	case VA_TONE_MAPPING_SDR_TO_HDR:
		/* Hard coding values to standard SDR libva values */
		in_hdr10_md.display_primaries_x[0] = 15000;
        in_hdr10_md.display_primaries_y[0] = 30000;
        in_hdr10_md.display_primaries_x[1] = 32000;
        in_hdr10_md.display_primaries_y[1] = 16500;
        in_hdr10_md.display_primaries_x[2] = 7500;
        in_hdr10_md.display_primaries_y[2] = 3000;
        in_hdr10_md.white_point_x = 15635;
        in_hdr10_md.white_point_y = 16450;
		in_hdr10_md.max_display_mastering_luminance = 500;
		in_hdr10_md.min_display_mastering_luminance = 1;
		in_hdr10_md.max_content_light_level = 4000;
		break;
	}

    hdr_tm_param.type = VAProcFilterHighDynamicRangeToneMapping;
    hdr_tm_param.data.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
    hdr_tm_param.data.metadata= &in_hdr10_md;
    hdr_tm_param.data.metadata_size = sizeof(VAHdrMetaDataHDR10);

    va_status = vaCreateBuffer(d->va_display, context_id,
					VAProcFilterParameterBufferType, 
    				sizeof(hdr_tm_param), 1, 
    				(void *)&hdr_tm_param,
    				&filter_param_buf_id);
	if (!va_check_status(va_status,"vaCreateBuffer tonemapping"))
		return VA_INVALID_ID;

    return filter_param_buf_id;
}

static int
drm_va_set_output_tm_metadata(struct weston_hdr_metadata *content_md, 
				const struct drm_edid_hdr_metadata *target_md,
				VAHdrMetaDataHDR10 *o_hdr10_md,
				VAHdrMetaData *out_metadata,
				uint8_t tm_type)
{
	struct drm_edid_hdr_md_static *t_smd;
	/* TODO: Add support for dynamic metadat too */

	/* SDR target display */
	if (!target_md) {

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
		weston_log("VA: No output metadata found\n");
		return 0;
	}

	t_smd = target_md->metadata.s;
	o_hdr10_md->max_display_mastering_luminance = t_smd->max_cll;
	o_hdr10_md->min_display_mastering_luminance = t_smd->min_cll;
	o_hdr10_md->max_pic_average_light_level = t_smd->max_cfall;
	o_hdr10_md->max_content_light_level = t_smd->max_cll;

	o_hdr10_md->white_point_x = va_primary(t_smd->white_point_x);
	o_hdr10_md->white_point_y = va_primary(t_smd->white_point_y);
	o_hdr10_md->display_primaries_x[0] = va_primary(t_smd->display_primary_g_x);
	o_hdr10_md->display_primaries_x[1] = va_primary(t_smd->display_primary_b_x);
	o_hdr10_md->display_primaries_x[2] = va_primary(t_smd->display_primary_r_x);
	o_hdr10_md->display_primaries_y[0] = va_primary(t_smd->display_primary_g_y);
	o_hdr10_md->display_primaries_y[1] = va_primary(t_smd->display_primary_b_y);
	o_hdr10_md->display_primaries_y[2] = va_primary(t_smd->display_primary_r_y);

	out_metadata->metadata_type = VAProcHighDynamicRangeMetadataHDR10;
    out_metadata->metadata = o_hdr10_md;
    out_metadata->metadata_size = sizeof(VAHdrMetaDataHDR10);
	return 0;

#if 0
	switch (tm_type) {
	case VA_TONE_MAPPING_HDR_TO_HDR:
		weston_log("VA: Preparing metadata for H2H tone mapping\n");
		o_hdr10_md->display_primaries_x[0] = 8500;
		o_hdr10_md->display_primaries_y[0] = 39850;
		o_hdr10_md->display_primaries_x[1] = 35400;
		o_hdr10_md->display_primaries_y[1] = 14600;
		o_hdr10_md->display_primaries_x[2] = 6550;
		o_hdr10_md->display_primaries_y[2] = 2300;
		break;

	case VA_TONE_MAPPING_HDR_TO_SDR:
		weston_log("VA: Preparing metadata for H2H tone mapping\n");
		o_hdr10_md->display_primaries_x[0] = 15000;
		o_hdr10_md->display_primaries_y[0] = 30000;
		o_hdr10_md->display_primaries_x[1] = 32000;
		o_hdr10_md->display_primaries_y[1] = 16500;
		o_hdr10_md->display_primaries_x[2] = 7500;
		o_hdr10_md->display_primaries_y[2] = 3000;
		break;

	case VA_TONE_MAPPING_SDR_TO_HDR:
		weston_log("VA: Preparing metadata for S2H tone mapping\n");
		break;

	default:
		weston_log("VA: Unknown tone mapping mode\n");
		return -1;
		break;
	}
#endif
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
drm_va_process(struct drm_va_display *d,
				VABufferID pparam_buf_id,
				VAContextID context_id,
				VASurfaceID out_surface_id)
{
	VAStatus va_status;

	va_status = vaBeginPicture(d->va_display,
                               context_id,
                               out_surface_id);
    if (!va_check_status(va_status, "vaBeginPicture"))
		return -1;

    va_status = vaRenderPicture(d->va_display,
                                context_id,
                                &pparam_buf_id,
                                1);
    if (!va_check_status(va_status, "vaRenderPicture"))
		return -1;

    va_status = vaEndPicture(d->va_display, context_id);
    if (!va_check_status(va_status, "vaRenderPicture"))
		return -1;

	return 0;
}

static VABufferID
va_create_pipeline_buffer(struct drm_va_display *d,
					VAProcPipelineParameterBuffer *pparam,
					VAContextID context_id)
{
	VAStatus va_status;
	VABufferID pipeline_param_buf_id = VA_INVALID_ID;

	/* Create pipeline buffer */
	va_status = vaCreateBuffer(d->va_display, context_id,
					VAProcPipelineParameterBufferType,
					sizeof(pparam), 1, pparam,
					&pipeline_param_buf_id);
	if (!va_check_status(va_status, "vaCreateBuffer"))
		return VA_INVALID_ID;
	return pipeline_param_buf_id;
}

static VABufferID
drm_va_create_hdr_filter(struct drm_va_display *d,
			struct weston_hdr_metadata *c_md,
			VAContextID context_id,
			uint8_t tm_type)
{
	uint32_t i;
	VAStatus va_status;
	uint32_t num_hdr_tm_caps = VAProcHighDynamicRangeMetadataTypeCount;
	VAProcFilterCapHighDynamicRange hdr_tm_caps[num_hdr_tm_caps];

	memset(hdr_tm_caps, 0, num_hdr_tm_caps *
			sizeof(VAProcFilterCapHighDynamicRange));
	va_status = vaQueryVideoProcFilterCaps(d->va_display, context_id,
					VAProcFilterHighDynamicRangeToneMapping,
					(void *)hdr_tm_caps, &num_hdr_tm_caps);
	if (!va_check_status(va_status, "Check HDR capability\n") ||
			!num_hdr_tm_caps) {
		weston_log("VA: No HDR capability found\n");
		return -1;
	}

	for (i = 0; i < num_hdr_tm_caps; ++i) {
		weston_log("VA: tm caps[%d]: metadata type %d, flag %d\n", i,
			hdr_tm_caps[i].metadata_type, hdr_tm_caps[i].caps_flag);
	}

	return drm_va_create_input_tm_filter(d, c_md, context_id, tm_type);
}

static struct drm_fb *
drm_va_create_out_fb(int drm_fd, struct drm_fb *in_fb)
{
	int ret;
	struct drm_fb *out_fb = zalloc(sizeof(struct drm_fb));

	if (!out_fb) {
		weston_log("VA: OOM while creating fb for out surface\n");
		return NULL;
	}

#if 0
	struct drm_i915_gem_create create;
	uint32_t offsets[4], pitches[4], handles[4];

	create.handle = 0;
	create.size = ALIGN(in_fb->size, 4096);
	drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_CREATE, &create);
	if (create.handle == 0) {
		weston_log("VA: failed to create fb for out surface\n");
		free(out_fb);
		return NULL;
	}
	out_fb->handles[0] = create.handle;
#endif

	out_fb->offsets[0] = 0;
	out_fb->width = in_fb->width;
	out_fb->height = in_fb->height;
	out_fb->fd = in_fb->fd;
	out_fb->format = in_fb->format;
	out_fb->modifier = in_fb->modifier;
	out_fb->size = in_fb->size;
	out_fb->type = in_fb->type;

	ret = drmModeAddFB2(drm_fd,
			     out_fb->width, out_fb->height, out_fb->format->format,
			     out_fb->handles, out_fb->strides, out_fb->offsets,
			     &out_fb->fb_id, 0);
	if (ret) {
		weston_log("VA: failed to create fb for out surface\n");
		free(out_fb);
		return NULL;
	}

	return out_fb;
}

static VASurfaceID
drm_va_create_surface_from_fb(struct drm_va_display *d,
				struct drm_fb *fb)
{
	int ret;
	unsigned long handle;
	VAStatus va_status;
	VASurfaceID surface;
	VASurfaceAttribExternalBuffers external;
	VASurfaceAttrib attribs[2];
	uint32_t surf_fourcc  = VA_FOURCC('N', 'V', '1', '2');
	uint32_t surf_format  = VA_FOURCC_P010;
	struct drm_gem_flink arg;

	memset(&arg, 0, sizeof(arg));
	arg.handle = fb->handles[0];
	ret = drmIoctl(fb->fd, DRM_IOCTL_GEM_FLINK, &arg);
	if (ret) {
		printf("VA: drmloctl DRM_IOCTL_GEM_FLINK Failed\n");
		return VA_INVALID_SURFACE;
	}
	handle = (unsigned long)arg.name;

	memset(&external, 0, sizeof(external));
	external.pixel_format = surf_fourcc;
	external.width = fb->width;
	external.height = fb->height;
	external.data_size = fb->width * fb->height * 32 / 8;
	external.num_planes = 1;
	external.pitches[0] = fb->strides[0];
	external.buffers = &handle;
	external.num_buffers = 1;

	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].type = VASurfaceAttribMemoryType;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM;

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

	return surface;
}

static int
drm_va_create_surfaces(struct drm_va_display *d,
					VASurfaceID *surface_in,
					VASurfaceID *surface_out,
					struct drm_fb *in_fb,
					struct drm_fb *out_fb)
{
	*surface_in = drm_va_create_surface_from_fb(d, in_fb);
	if (VA_INVALID_SURFACE == *surface_in) {
		printf("VA: Failed to create in surface\n");
		return -1;
	}

	*surface_out = drm_va_create_surface_from_fb(d, out_fb);
	if (VA_INVALID_SURFACE == *surface_out) {
		printf("VA: Failed to create out surface\n");
		vaDestroySurfaces(d->va_display, surface_in, 1);
		*surface_in = VA_INVALID_SURFACE;
		return -1;
	}

	return 0;
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
drm_va_tone_map(struct drm_backend *b,
				struct drm_plane_state *ps,
				uint32_t tm_type,
				const struct drm_edid_hdr_metadata *target_md)
{
	int ret = 0;
	struct drm_fb *out_fb;
	struct drm_fb *fb = ps->fb;
	struct drm_va_display *d;
	struct weston_hdr_metadata *content_md;

	VABufferID pparam_buf_id = VA_INVALID_ID;
	VABufferID fparam_buf_id = VA_INVALID_ID;
	VASurfaceID in_surface_id = VA_INVALID_SURFACE;
	VASurfaceID out_surface_id = VA_INVALID_SURFACE;
	VAContextID context_id = VA_INVALID_ID;
	VARectangle surface_region = {};
	VARectangle output_region = {};
	VAHdrMetaData output_metadata = {};
	VAHdrMetaDataHDR10 out_md_params = {};
	VAProcPipelineParameterBuffer pparam = {0, };

	if (!fb || !target_md ) {
		weston_log("VA: NULL input\n");
		return 0;
	}

	content_md = ps->ev->surface->hdr_metadata;
	d = &b->vd;

	if (!d->va_display) {
		weston_log("VA: libVA not initialized\n");
		return 0;
	}	

	/* VA needs a separate surface to write output into */
	out_fb = drm_va_create_out_fb(d->drm_fd, fb);
	if (!out_fb) {
		printf("VA: Failed to create new fb\n");
		return NULL;
	}

	ret = drm_va_create_surfaces(d, &in_surface_id, &out_surface_id, fb, out_fb);
	if (ret) {
		weston_log("VA: Can't create surface, tone map failed\n");
		return NULL;
	}

	ret = drm_va_create_context(d, in_surface_id,
					&context_id,
					fb->width,
					fb->height);
	if (ret) {
		weston_log("VA: Cant create context\n");
		ret = -1;
		goto clear_surf;
	}

	/* Setup input tonemapping buffer filter */
	fparam_buf_id  = drm_va_create_hdr_filter(d, content_md, context_id, tm_type);
	if (fparam_buf_id == VA_INVALID_ID) {
		weston_log("VA: Can't create HDR filter, tone map failed\n");
		ret = -1;
		goto clear_ctx;
	}

	/* Setup output tonemapping properties */
	ret = drm_va_set_output_tm_metadata(content_md, target_md, &out_md_params,
				&output_metadata, tm_type);
	if (ret) {
		weston_log("VA: Can't setup HDR metadata\n");
		ret = -1;
		goto clear_ctx;
	}

	drm_va_setup_surfaces(&surface_region, &output_region, fb);

	/* Prepare tone mapping pipeline */
	pparam.filter_flags = 0;
	pparam.surface = in_surface_id;
	pparam.num_filters = 1;
	pparam.filters = &fparam_buf_id;
	pparam.surface_region = &surface_region;
	pparam.output_region = &output_region;

	pparam.input_color_properties.colour_primaries = 9;
	pparam.input_color_properties.transfer_characteristics = 16;
	pparam.output_color_properties.colour_primaries = 9;
	pparam.output_color_properties.transfer_characteristics = 16;
	pparam.output_color_standard = VAProcColorStandardExplicit;
	pparam.surface_color_standard = VAProcColorStandardExplicit;
	pparam.output_hdr_metadata = &output_metadata;

	pparam_buf_id = va_create_pipeline_buffer(d, &pparam, context_id);
	if (pparam_buf_id == VA_INVALID_ID) {
		weston_log("VA: Can't create filter buffer, tone map failed\n");
		ret = -1;
		goto clear_filter;
	}

	ret = drm_va_process(d, pparam_buf_id, context_id, out_surface_id);
	if (ret < 0) {
		weston_log("VA: Failed to tone map buffer\n");
		drmModeRmFB(d->drm_fd, out_fb->fb_id);
		free(out_fb);
		out_fb = NULL;
	}

	drm_va_destroy_buffer(d->va_display, pparam_buf_id);
clear_filter:
	drm_va_destroy_buffer(d->va_display, fparam_buf_id);
clear_ctx:
	drm_va_destroy_context(d->va_display, context_id);
clear_surf:
	drm_va_destroy_surface(d->va_display, out_surface_id);
	drm_va_destroy_surface(d->va_display, in_surface_id);
	return out_fb;
}


#if 0
/* Standard SDR to HDR conversion values */
static uint32_t in_max_display_lum = 1000;
static uint32_t in_min_display_lum = 1;
static uint32_t in_max_content_lum = 4000;
static uint32_t in_pic_average_lum = 1000;
static uint32_t in_clr_primaries = 9;
static uint32_t in_xfer_characteristics = 16;

static uint32_t	out_max_display_lum = 1000;
static uint32_t out_min_display_lum = 1;
static uint32_t out_max_content_lum = 4000;
static uint32_t out_pic_average_lum = 1000;
static uint32_t out_clr_primaries = 9;
static uint32_t out_xfer_characteristics = 16;

void va_setup_surfaces(struct picture_frame *pic, uint32_t surf_id,
				int iy, int ix, int oy, int ox)
{
	if (!pic)
		return;

	pic->in_pic_h = iy;
	pic->in_pic_w = ix;
	pic->out_pic_h = oy;
	pic->out_pic_h = ox;

	pic->in_surface_id = surf_id;
	pic->surface_region.x = 0;
	pic->surface_region.y = 0;
	pic->surface_region.width = pic->in_pic_w;
	pic->surface_region.height = pic->in_pic_h;

	pic->output_region.x = 0;
	pic->output_region.y = 0;
	pic->output_region.width = pic->out_pic_w;
	pic->output_region.height = pic->out_pic_h;
}

VABufferID va_create_hdr_filter_buffer(VADisplay va_display, int32_t 
context_id)
{
	VAStatus va_status;
	VAProcFilterParameterBufferHdr hdr_param;
	VABufferID filter_param_buf_id = VA_INVALID_ID;

	/* Load Frame HDR metadata */
	va_get_frame_hdr_metadata(&hdr_param);

	/* Create fiter param buffer */
	va_status = vaCreateBuffer(va_display, context_id, 
VAProcFilterParameterBufferType,
					sizeof(hdr_param), 1, &hdr_param, &filter_param_buf_id);
	if (!va_check_status(va_status, "vaCreateBuffer"))
		return -1;
	return filter_param_buf_id;
}

static VADisplay *
drm_vpp_context_create()
{
	int32_t j;
	int32_t i;
	int32_t major_ver, minor_ver;
	int32_t num_entrypoints;
	VAStatus va_status = VA_STATUS_SUCCESS;
	VADisplay va_dpy = NULL;
	VAEntrypoint entrypoints[5];
	VAConfigAttrib attrib;
	VAContextID context_id = 0;
	VAConfigID  config_id = 0;
	VASurfaceID g_in_surface_id = VA_INVALID_ID;
	VASurfaceID g_out_surface_id = VA_INVALID_ID;

	/* VA driver initialization */
	va_dpy = va_open_display();
	va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
	if (va_status != VA_STATUS_SUCCESS) {
		weston_log("Failed to initialize VA\n");
		return NULL;
	}

	/* Check whether VPP is supported by driver */
	num_entrypoints = vaMaxNumEntrypoints(va_dpy);
	va_status = vaQueryConfigEntrypoints(va_dpy, VAProfileNone,
				entrypoints,
				&num_entrypoints);
	CHECK_VASTATUS(va_status, "vaQueryConfigEntrypoints");

	for (j = 0; j < num_entrypoints; j++) {
		if (entrypoints[j] == VAEntrypointVideoProc)
			break;
	}

	if (j == num_entrypoints) {
		weston_log("VPP is not supported by driver\n");
		return NULL;
	}

	/* Render target surface format check */
	attrib.type = VAConfigAttribRTFormat;
	va_status = vaGetConfigAttributes(va_dpy, VAProfileNone,
					VAEntrypointVideoProc,
					&attrib, 1);
	CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
	if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
		weston_log("RT format %d is not supported by VPP !\n");
		return NULL;
	}

#if 1
	/* Create surface/config/context for VPP pipeline */
	va_status = create_surface(&g_in_surface_id, g_in_pic_width, g_in_pic_height,
	g_in_fourcc, g_in_format);
	CHECK_VASTATUS(va_status, "vaCreateSurfaces for input");

	va_status = create_surface(&g_out_surface_id, g_out_pic_width, g_out_pic_height,
	g_out_fourcc, g_out_format);
	CHECK_VASTATUS(va_status, "vaCreateSurfaces for output");
#endif

	va_status = vaCreateConfig(va_dpy, VAProfileNone, VAEntrypointVideoProc,
					&attrib, 1,	&config_id);
	CHECK_VASTATUS(va_status, "vaCreateConfig");

	va_status = vaCreateContext(va_dpy, config_id,
					g_out_pic_width, g_out_pic_height,
					VA_PROGRESSIVE, &g_out_surface_id,
					1, &context_id);
	CHECK_VASTATUS(va_status, "vaCreateContext");

	uint32_t supported_filter_num = VAProcFilterCount;
	VAProcFilterType supported_filter_types[VAProcFilterCount];

	va_status = vaQueryVideoProcFilters(va_dpy,
	context_id,
	supported_filter_types,
	&supported_filter_num);

	CHECK_VASTATUS(va_status, "vaQueryVideoProcFilters");

	for (i = 0; i < supported_filter_num; i++){
	if (supported_filter_types[i] == VAProcFilterHighDynamicRangeToneMapping)
	break;
	}

	if (i == supported_filter_num) {
	printf("VPP filter type VAProcFilterHighDynamicRangeToneMapping is not supported by driver !\n");        
	}
	return va_status;
}

void va_process(VADisplay va_display, int32_t context_id,
			int32_t in_surface_id, VABufferID pipeline_param_buf_id)
{
	VAStatus va_status;

	/* for face detection only usage, it is input surface */
	va_status = vaBeginPicture(va_display, context_id, in_surface_id); 
	if (!va_check_status(va_status, "vaBeginPicture"))
		return;

	va_status = vaRenderPicture(va_display, context_id, &pipeline_param_buf_id, 1);
	if (!va_check_status(va_status, "vaRenderPicture"))
		return;

	va_status = vaEndPicture(va_display, context_id);
	if (!va_check_status(va_status, "vaEndPicture"))
		return;
}

VAHdrMetaData *
va_get_display_hdr_metadata(uint32_t map_type)
{
	VAHdrMetaData *output_hdr_metadata;

	output_hdr_metadata = malloc(sizeof(VAHdrMetaData));
	if (!output_hdr_metadata) {
		printf("OOM while allocating metadata\n");
		return NULL;
	}

	output_hdr_metadata->metadata_type =
		VAProcHighDynamicRangeMetadataHDR10;
	output_hdr_metadata->metadata_size = 5;
	output_hdr_metadata->metadata = va_get_metadata_from_drm();
	if (!output_hdr_metadata->metadata) {
		printf("Couldn't get display metadata\n");
		free(output_hdr_metadata);
		return NULL;
	}
	return output_hdr_metadata;	

#if 0
	output_hdr_metadata->display_primaries_x[0] = 13250;
	output_hdr_metadata->display_primaries_x[1] = 7500;
	output_hdr_metadata->display_primaries_x[2] = 34000;
	output_hdr_metadata->display_primaries_y[0] = 34500;
	output_hdr_metadata->display_primaries_y[1] = 3000;
	output_hdr_metadata->display_primaries_y[2] = 16000;
	output_hdr_metadata->white_point_x = 15635;
	output_hdr_metadata->white_point_y = 16450;
	output_hdr_metadata->MaxCLL = 1000;
	output_hdr_metadata->MaxFALL = 400;

	if (map_type == TONE_MAP_H2S)
		output_hdr_metadata->EOTF = VAProcHdrEotfTraditionalGammaSdr;
	else
		output_hdr_metadata->EOTF = VAProcHdrEotfSmpteSt2084;
#endif
}

void va_get_frame_hdr_metadata(VAProcFilterParameterBufferHdr *hdr_param)
{
#warning "Code needed here"
#if 0
	va_get_frame_hdr_metadata_from_ffmpeg();
	fill_hdr_params_with_this_frame_metadata();
#endif
	av_frame_get_side_data();
	hdr_param->type = VAProcFilterHighDynamicRangeToneMapping;
	hdr_param->hdr_meta_data.EOTF = VAProcHdrEotfSmpteSt2084;
	hdr_param->hdr_meta_data.display_primaries_x[0] = 13250;
	hdr_param->hdr_meta_data.display_primaries_x[1] = 7500;
	hdr_param->hdr_meta_data.display_primaries_x[2] = 34000;
	hdr_param->hdr_meta_data.display_primaries_y[0] = 34500;
	hdr_param->hdr_meta_data.display_primaries_y[1] = 3000;
	hdr_param->hdr_meta_data.display_primaries_y[2] = 16000;
	hdr_param->hdr_meta_data.white_point_x = 15635;
	hdr_param->hdr_meta_data.white_point_y = 16450;
	hdr_param->hdr_meta_data.MaxCLL = 1000;
	hdr_param->hdr_meta_data.MaxFALL = 400;
}

VABufferID
va_tone_map_h2x_and_show(VADisplay va_display,
			enum tone_map_type type, struct picture_frame *pic)
{
	VAProcPipelineParameterBuffer pipeline_param;
	VABufferID pipeline_param_buf_id = VA_INVALID_ID;

	if (!pic) {
		printf("No pic dimentions\n");
		return -1;
	}

	/* Setup pipeline */
	memset(&pipeline_param, 0, sizeof(pipeline_param));
	pipeline_param.surface = pic->in_surface_id;
	pipeline_param.surface_region = &pic->surface_region;
	pipeline_param.output_region  = &pic->output_region;
	pipeline_param.filters = &pic->filter_param_buf_id;
	pipeline_param.num_filters  = 1;
	pipeline_param.filter_flags = 0;
	pipeline_param.output_hdr_metadata = va_get_display_hdr_metadata(type);
	if (!pipeline_param.output_hdr_metadata) {
		printf("OOM while HDR metadata creation\n");
		return -1;
	}

	pipeline_param_buf_id = va_create_pipeline_buffer(va_display, pic->context_id,
					&pipeline_param);
	if (pipeline_param_buf_id <= 0) {
		printf("Failed to create pipeline buffer\n");
		free(pipeline_param.output_hdr_metadata);
		return -1;
	}

	/* Render now */
	va_show(va_display, pic->context_id, pic->in_surface_id, pipeline_param_buf_id);
	va_destroy_buffer(va_display, pipeline_param_buf_id);
	return 0;
}

int va_check_hdr_cap(VADisplay va_display, int32_t context_id)
{
	VAStatus va_status;
	//VAProcFilterCapValue hdr_cap;
	VAProcFilterCap hdr_cap;

	/* Check if HDR supported from VA */
	hdr_cap.type = VAProcHdrCapEotfType;
	hdr_cap.size = VAProcHdrEotfNum;
	va_status = vaQueryVideoProcFilterCaps(va_display, context_id, VAProcHdrCapEotfType,
					&hdr_cap, &num_query_caps);
	if (!va_check_status(va_status,"vaQueryVideoProcFilterCaps"))
		return -1;

	printf("eotf SMPTE ST2084 supported =%d\r\n", hdr_cap.value & VA_HDR_EOTF_SMPTE_ST_2084);
	if (!hdr_cap.value & VA_HDR_EOTF_SMPTE_ST_2084) {
		printf("HDR capability not supported in VA\n");
		return -1;
	}

	return 0;
}

int va_select_hdr_filter(VADisplay va_display, int32_t context_id)
{
	int i;
	uint32_t supported_filter_num = VAProcFilterCount;
	VAProcFilterType supported_filter_types[VAProcFilterCount];
	VAStatus va_status;

	va_status = vaQueryVideoProcFilters(va_display, context_id, supported_filter_types,
					&supported_filter_num);
	if (!va_check_status(va_status, "vaQueryVideoProcFilters"))
		return -1;

	/*Choose High Dynamic Range Filter */
	for (i = 0; i < supported_filter_num; i++)
		if (supported_filter_types[i] == VAProcFilterHighDynamicRangeToneMapping )
				break;

	if (i == supported_filter_num) {
		printf("No HDR filter supported\n");
		return -1;
	}

	return 0;
}
#endif

