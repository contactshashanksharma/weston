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

static void drm_va_destroy_context(VADisplay va_display, VAContextID context)
{
	VAStatus va_status;

	va_status = vaDestroyContext(va_display, context);
	va_check_status(va_status, "vaDestroyContext");
}

static void drm_va_destroy_config(VADisplay va_display, VAConfigID cfg_id)
{
	VAStatus va_status;

	va_status = vaDestroyConfig(va_display, cfg_id);
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
				VASurfaceID *surface_id,
				VAContextID *context_id,
				int out_w, int out_h)
{
	VAStatus va_status;

	/* Create a Video processing Context */
	va_status = vaCreateContext(d->va_display,
								d->cfg_id,
								out_w, out_h, VA_PROGRESSIVE,
								surface_id, 1, context_id);
	if (!va_check_status(va_status,"vaCreateContext"))
		return -1;

	return 0;
}

static VAConfigID
drm_va_create_config(struct drm_va_display *d)
{
	VAStatus va_status;
	VAConfigID cfg_id;

	/* Create a video processing config */
	va_status = vaCreateConfig(d->va_display,
					VAProfileNone, VAEntrypointVideoProc,
					&d->attrib, 1, &cfg_id);
	if (!va_check_status(va_status,"vaCreateConfig"))
		return VA_INVALID_ID;

	return cfg_id;
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

static VADisplay
drm_va_init_display(struct drm_va_display *d)
{
	int render_fd;
	VAStatus va_status;
	VADisplay va_display;

	if (!d)
		return NULL;

#if 1
	render_fd = open("/dev/dri/renderD128", O_RDWR);
	if (render_fd < 0) {
		weston_log_continue("failed to open render device\n");
		return NULL;
	}
	
	va_display = vaGetDisplayDRM(render_fd);
#else
	va_display = vaGetDisplayWl(d->wl_display);
#endif
	if (!va_display) {
		weston_log("Can't get DRM display\n");
		close(render_fd);
		return NULL;
	}

	va_status = vaInitialize(va_display, &d->major_ver,
				&d->minor_ver);
	if (!va_check_status(va_status,"vaInitialize")) {
		close(render_fd);
		return NULL;
	}

	d->render_fd = render_fd;
	return va_display;
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

	d->va_display = drm_va_init_display(d);
	if (!d->va_display) {
		weston_log_continue("VA: Init failed\n");
		free(d);
		return NULL;
	}

	if (drm_va_check_entrypoints(d)) {
		weston_log_continue("VA: Entry point check failed\n");
		free(d);
		return NULL;
	}

	if (drm_va_check_attributes(d)) {
		weston_log_continue("VA: Attribute check failed\n");
		free(d);
		return NULL;
	}

	d->cfg_id = drm_va_create_config(d);
	if (d->cfg_id == VA_INVALID_ID) {
		weston_log_continue("VA: Cant create config\n");
		free(d);
		return NULL;
	}

	weston_log_continue("VA: Successfully created display config\n");
	return d;
}

void drm_va_destroy_display(struct drm_va_display *d)
{
	VAStatus status;

	status = vaDestroyConfig(d->va_display, d->cfg_id);
	if (!va_check_status(status,"vaDestroyConfig"))
		weston_log_continue("VA: Can't destroy config\n");
	close(d->render_fd);
	return;
}

bool va_write_surface_to_frame(VADisplay *d, VASurfaceID surface_id)
{
	VAStatus va_status;
	VAImage  va_image;
	FILE *fp;

	int i = 0;
	int frame_size = 0, y_size = 0, u_size = 0;
	unsigned char *y_src = NULL, *u_src = NULL, *v_src = NULL;
	unsigned char *y_dst = NULL, *u_dst = NULL, *v_dst = NULL;
	int bytes_per_pixel = 2;
	void *in_buf = NULL;
	unsigned char *dst_buffer = NULL;

	fp = fopen("/home/shashanks/code/clean/frame.a2rgb10", "w");
	if (fp == NULL) {
		weston_log("Open failed\n");
		return -1;
	}

	// This function blocks until all pending operations on the surface have been completed.
	va_status = vaSyncSurface(d, surface_id);
	if (!va_check_status(va_status, "Sync"))
		return -1;

	va_status = vaDeriveImage(d, surface_id, &va_image);
	if (!va_check_status(va_status, "Derive"))
		return -1;

	va_status = vaMapBuffer(d, va_image.buf, &in_buf);
	if (!va_check_status(va_status, "vaMapBuffer"))
		return -1;

	weston_log("write_surface_to_frame: va_image.width %d, va_image.height %d, va_image.pitches[0]: %d, va_image.pitches[1] %d, va_image.pitches[2] %d\n", 
		va_image.width, va_image.height, va_image.pitches[0], va_image.pitches[1], va_image.pitches[1]);    


	switch (va_image.format.fourcc) {
	case VA_FOURCC_P010:
		frame_size = va_image.width * va_image.height * bytes_per_pixel * 3 / 2;
		dst_buffer = (unsigned char*)malloc(frame_size);
		y_size = va_image.width * va_image.height * bytes_per_pixel;
		u_size = (va_image.width / 2 * bytes_per_pixel) * (va_image.height >> 1);
		y_dst = dst_buffer;
		u_dst = dst_buffer + y_size; // UV offset for P010
		y_src = (unsigned char*)in_buf + va_image.offsets[0];
		u_src = (unsigned char*)in_buf + va_image.offsets[1]; // U offset for P010    

		for (i = 0; i < va_image.height; i++)  {
			memcpy(y_dst, y_src, va_image.width * 2);
			y_dst += va_image.width * 2;
			y_src += va_image.pitches[0];
		}

		for (i = 0; i < va_image.height >> 1; i++)  {
			memcpy(u_dst, u_src, va_image.width * 2);
			u_dst += va_image.width * 2;
			u_src += va_image.pitches[1];
		}
		weston_log("write_frame: P010 \n");
		break;

	case VA_FOURCC_RGBA:
	case VA_FOURCC_ABGR:
		frame_size = va_image.width * va_image.height * 4;
		dst_buffer = (unsigned char*)malloc(frame_size);        
		y_dst = dst_buffer;
		y_src = (unsigned char*)in_buf + va_image.offsets[0];         

		for (i = 0; i < va_image.height; i++) {
			memcpy(y_dst, y_src, va_image.width * 4);
			y_dst += va_image.pitches[0];
			y_src += va_image.width * 4;
		} 
		weston_log("write_frame: RGBA and A2R10G10B10 \n");       
		break;

	default: // should not come here
		weston_log("VA_STATUS_ERROR_INVALID_IMAGE_FORMAT %x\n", va_image.format.fourcc);
		va_status = VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
	break;
	}

	fwrite(dst_buffer, 1, frame_size, fp);

	if (dst_buffer)  {
		free(dst_buffer);
		dst_buffer = NULL;
	}

	vaUnmapBuffer(d, va_image.buf);
	vaDestroyImage(d, va_image.image_id);
	return va_status;
}


static struct drm_fb *
drm_va_create_fb_from_surface(struct drm_va_display *d,
                        VASurfaceID surface_id,
                        VADRMPRIMESurfaceDescriptor *va_desc)
{
	VAStatus status;
#if 0
	uint32_t export_flags = VA_EXPORT_SURFACE_SEPARATE_LAYERS |
#else
	uint32_t export_flags =  VA_EXPORT_SURFACE_COMPOSED_LAYERS |
#endif
			VA_EXPORT_SURFACE_READ_ONLY |
			VA_EXPORT_SURFACE_WRITE_ONLY;

	/* Sync surface before expoting buffer, blocking call */
	status = vaSyncSurface(d->va_display, surface_id);
	if (!va_check_status(status, "vaSyncSurface")) {
		weston_log_continue("VA: Failed to sync surface to buffer\n");
		return NULL;
	}

	status = va_write_surface_to_frame(d->va_display, surface_id);
	if (status == VA_STATUS_SUCCESS) {
		weston_log_continue("VA: saved snapshot\n");
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
#if 1
	uint32_t surf_fourcc  = VA_FOURCC('P', '0', '1', '0');
	uint32_t surf_format  = VA_RT_FORMAT_YUV420;
#else
	uint32_t surf_fourcc  = VA_FOURCC_P010;
	uint32_t surf_format  = VA_RT_FORMAT_YUV420_10;
#endif
#if 0
	struct drm_gem_flink arg;
	memset(&arg, 0, sizeof(arg));
	arg.handle = fb->handles[0];
	ret = drmIoctl(fb->fd, DRM_IOCTL_GEM_FLINK, &arg);
	if (ret) {
		weston_log("VA: drmloctl DRM_IOCTL_GEM_FLINK Failed\n");
		return VA_INVALID_SURFACE;
	}
	handle = (unsigned long)arg.name;
#else
	ret = drmPrimeHandleToFD(fb->fd, fb->handles[0], DRM_CLOEXEC, &prime_fd);
	if (ret) {
		weston_log_continue("VA: drmloctl get prime handle Failed\n");
		return VA_INVALID_SURFACE;
	}
#endif
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
#if 0
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM;
#else
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
#endif
	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
	attribs[1].value.type = VAGenericValueTypePointer;
	attribs[1].value.value.p = &external;

	va_status = vaCreateSurfaces(d->va_display, surf_format,
					fb->width, fb->height,
					&surface, 1, attribs,
					ARRAY_SIZE(attribs));
	if (!va_check_status(va_status, "vaCreateSurfaces")) {
		weston_log_continue("VA: failed to create surface\n");
		return VA_INVALID_SURFACE;
	}

	weston_log_continue("VA: Created input surface 0x%x, drm format 0x%x fourcc %s\n",
		surface, VA_FOURCC_P010, drm_print_format_name(surf_fourcc));
	return surface;
}

static VASurfaceID
drm_va_create_surface(struct drm_va_display *d,
				int width, int height)
{
    VAStatus va_status;
	VASurfaceID surface_id;
    VASurfaceAttrib surface_attrib;
#if 0
	uint32_t surf_fourcc = VA_FOURCC_XRGB; //Fails vaExportSurfaceHandle
	uint32_t surf_format = VA_RT_FORMAT_RGB32_10;

	uint32_t surf_fourcc = VA_FOURCC_RGBX; //Fails vaExportSurfaceHandle
	uint32_t surf_format = VA_RT_FORMAT_RGB32_10;

	uint32_t surf_fourcc = VA_FOURCC_RGBA;//Fails vaExportSurfaceHandle
	uint32_t surf_format = VA_RT_FORMAT_RGB32;

	uint32_t surf_fourcc  = VA_FOURCC_P010;//Fails vaExportSurfaceHandle
	uint32_t surf_format  = VA_RT_FORMAT_YUV420_10;

	uint32_t surf_fourcc = VA_FOURCC_ARGB; //Fails vaEndPicture
	uint32_t surf_format = VA_RT_FORMAT_RGB32_10;

	uint32_t surf_fourcc = VA_FOURCC_RGBA; //Success
	uint32_t surf_format = VA_RT_FORMAT_RGB32_10;
#endif
	uint32_t surf_fourcc = VA_FOURCC_RGBA;
	uint32_t surf_format = VA_RT_FORMAT_RGB32_10;

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

	weston_log_continue("VA: Created output surface 0x%x, format %x fourcc %s\n",
		surface_id, surf_format, drm_print_format_name(surf_fourcc));
	return surface_id;
}

static int
drm_va_create_surfaces(struct drm_va_display *d,
					VASurfaceID *surface_in,
					VASurfaceID *surface_out,
					struct drm_fb *in_fb)
{
	*surface_in = drm_va_create_surface_from_fb(d, in_fb);
	if (VA_INVALID_SURFACE == *surface_in) {
		weston_log("VA: Failed to create in surface\n");
		return -1;
	}

	*surface_out = drm_va_create_surface(d, in_fb->width, in_fb->height);
	if (VA_INVALID_SURFACE == *surface_out) {
		weston_log("VA: Failed to create out surface\n");
		vaDestroySurfaces(d->va_display, surface_in, 1);
		*surface_in = VA_INVALID_SURFACE;
		return -1;
	}

	weston_log_continue("Shashank: in_surf_id:0x%x out_surf_id:0x%x\n",
			*surface_in, *surface_out);
	return 0;
}

static int
drm_va_process(struct drm_va_display *d,
				VABufferID *pparam_buf_id,
				VAContextID context_id,
				VASurfaceID out_surface_id)
{
	VAStatus va_status;

	va_status = vaBeginPicture(d->va_display,
                               context_id,
                               out_surface_id);
    if (!va_check_status(va_status, "vaBeginPicture"))
		return -1;

	weston_log_continue("VA: Begin\n");
    va_status = vaRenderPicture(d->va_display,
                                context_id,
                                pparam_buf_id,
                                1);
    if (!va_check_status(va_status, "vaRenderPicture"))
		return -1;

	weston_log_continue("VA: Render\n");
    va_status = vaEndPicture(d->va_display, context_id);
    if (!va_check_status(va_status, "vaEndPicture"))
		return -1;

	weston_log_continue("VA: surface processing done\n");
	return 0;
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
				const struct drm_hdr_metadata_static *target_md,
				VAHdrMetaDataHDR10 *o_hdr10_md,
				VAHdrMetaData *out_metadata)
{
	const struct drm_hdr_metadata_static *t_smd;
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
		weston_log_continue("VA: No output metadata found\n");
		return 0;
	}

	t_smd = target_md;
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
					VAContextID context_id,
					uint8_t tm_type,
					VABufferID *fparam_buf_id,
					VAHdrMetaDataHDR10 *in_hdr10_md,
					VAProcFilterParameterBufferHDRToneMapping *hdr_tm_param)
{
    VAStatus va_status = VA_STATUS_ERROR_INVALID_VALUE;
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


static VAStatus
drm_va_create_hdr_filter(struct drm_va_display *d,
			struct weston_hdr_metadata *c_md,
			VAContextID context_id,
			uint8_t tm_type,
			VABufferID *fparam_buf_id,
			VAHdrMetaDataHDR10 *in_hdr10_md,
			VAProcFilterParameterBufferHDRToneMapping *hdr_tm_param)
{
	uint32_t i;
	VAStatus va_status;
	uint32_t num_hdr_tm_caps = VAProcHighDynamicRangeMetadataTypeCount;
	VAProcFilterCapHighDynamicRange hdr_tm_caps[num_hdr_tm_caps];

#if 1
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
#endif
	return drm_va_create_input_tm_filter(d, c_md, context_id, tm_type,
			fparam_buf_id, in_hdr10_md, hdr_tm_param);
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
	struct drm_fb *out_fb = NULL;
	VABufferID pparam_buf_id = VA_INVALID_ID;
	VABufferID fparam_buf_id = VA_INVALID_ID;
	VASurfaceID in_surface_id = VA_INVALID_SURFACE;
	VASurfaceID out_surface_id = VA_INVALID_SURFACE;
	VAContextID context_id = VA_INVALID_ID;
	VARectangle surface_region = {0, };
	VARectangle output_region = {0, };
	VAHdrMetaData output_metadata = {0, };
	VAHdrMetaDataHDR10 in_hdr10_md = {0, };
	VAHdrMetaDataHDR10 out_md_params = {0, };
	VAProcPipelineParameterBuffer pparam = {0, };
	VAProcFilterParameterBufferHDRToneMapping hdr_tm_param = {0, };
	VADRMPRIMESurfaceDescriptor va_desc;
	VAStatus va_status = VA_STATUS_SUCCESS;

	if (!d || !fb || !tm) {
		weston_log_continue("VA: NULL input, VA not initialized ?\n");
		return NULL;
	}

	if (fb->format->format != DRM_FORMAT_P010) {
		weston_log("VA: Current implementation supports P010 format only\n");
		return NULL;
	}

	ret = drm_va_create_surfaces(d, &in_surface_id, &out_surface_id, fb);
	if (ret) {
		weston_log_continue("VA: Can't create surface, tone map failed\n");
		return NULL;
	}

	ret = drm_va_create_context(d, &in_surface_id, &context_id,
					fb->width, fb->height);
	if (ret) {
		weston_log_continue("VA: Cant create context\n");
		ret = -1;
		goto clear_surf;
	}

	/* Setup input tonemapping buffer filter */
	va_status  = drm_va_create_hdr_filter(d, content_md, context_id,
		tm->tone_map_mode, &fparam_buf_id, &in_hdr10_md, &hdr_tm_param);
	if (va_status != VA_STATUS_SUCCESS) {
		weston_log_continue("VA: Can't create HDR filter, tone map failed\n");
		ret = -1;
		goto clear_ctx;
	}

	/* Setup output tonemapping properties */
	ret = drm_va_set_output_tm_metadata(content_md, &tm->target_md, &out_md_params,
				&output_metadata);
	if (ret) {
		weston_log_continue("VA: Can't setup HDR metadata\n");
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

	/* Create pipeline buffer */
	va_status = vaCreateBuffer(d->va_display, context_id,
				   VAProcPipelineParameterBufferType,
				   sizeof(VAProcPipelineParameterBuffer),
				   1, &pparam, &pparam_buf_id);
	if (!va_check_status(va_status, "vaCreateBuffer")) {
		weston_log_continue("VA: Failed to create pparam buffer\n");
		ret = -1;
		goto clear_filter;
	}

	ret = drm_va_process(d, &pparam_buf_id, context_id, out_surface_id);
	if (ret < 0) {
		weston_log_continue("VA: Failed to tone map buffer\n");
		ret = -1;
		goto clear_filter;
	}

#if 0
	struct wl_buffer *out_buffer = NULL;
	va_status = vaGetSurfaceBufferWl(d->va_display, out_surface_id, 0, &out_buffer);
	if (!va_check_status(va_status, "vaGetSurfaceBufferWl")) {
		weston_log_continue("Failed to get wl buffer\n");
		ret = -1;
		goto clear_filter;
	}
#else
	out_fb = drm_va_create_fb_from_surface(d, out_surface_id, &va_desc);
	if (!out_fb)
		weston_log_continue("VA: Failed to tone map buffer\n");
#endif

	drm_va_destroy_buffer(d->va_display, pparam_buf_id);
clear_filter:
	drm_va_destroy_buffer(d->va_display, fparam_buf_id);
clear_ctx:
	drm_va_destroy_context(d->va_display, context_id);
	drm_va_destroy_config(d->va_display, d->cfg_id);
clear_surf:
	drm_va_destroy_surface(d->va_display, out_surface_id);
	drm_va_destroy_surface(d->va_display, in_surface_id);
	return out_fb;
}
