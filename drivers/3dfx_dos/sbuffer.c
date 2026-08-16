/*
 * Copyright (c) 1993-1995 Argonaut Technologies Limited. All rights reserved.
 *
 * Stored buffer methods
 */
#include <stddef.h>
#include <string.h>

#include "texture.h"

#include "drv.h"
#include "shortcut.h"
#include "brassert.h"

/*
 * Report textures the 3Dfx driver refuses to cache.  A rejected texture is
 * marked uncacheable, so match.c falls through to its untextured branch and the
 * object renders as flat Gouraud-shaded geometry.
 */
#define FXA_REJECT(reason, pm) ((void)0)
#define FXA_REPORT_PAD(rw, rh, pw, ph, sstride, dstride) ((void)0)


/*
 * Check if a number of a power of 2
 */
static br_boolean isPowerOf2(br_int_32 x)
{
	return !((x-1) & x);
}

static pixelmapBPP(br_device_pixelmap *pm)
{
	switch(pm->pm_type) {
	default:
		return 1;
	case BR_PMT_RGB_555:
	case BR_PMT_RGB_565:
	case BR_PMT_DEPTH_16:
	case BR_PMT_RGBA_4444:
		return 2;

	case BR_PMT_RGB_888:
		return 3;

	case BR_PMT_RGBX_888:
	case BR_PMT_RGBA_8888:
		return 4;
	}
}

/*
 * Default dispatch table for primitive state (defined at end of file)
 */
static struct br_buffer_stored_dispatch bufferStoredDispatch;

/*
 * Primitive state info. template
 */
#define F(f)	offsetof(struct br_buffer_stored, f)

static struct br_tv_template_entry bufferStoredTemplateEntries[] = {
	{BRT_IDENTIFIER_CSTR,	0,	F(identifier),		BRTV_QUERY | BRTV_ALL,	BRTV_CONV_COPY, },
};
#undef F

static struct br_tv_template bufferStoredTemplate = {
	BR_ASIZE(bufferStoredTemplateEntries),
	bufferStoredTemplateEntries
};



void BufferStored3DfxClearTemplate(void)
{
	CLEAR_TEMPLATE(bufferStored);
}



/*
 */
br_error Setup3DfxBuffer(br_buffer_stored *buffer, br_device_pixelmap *pm)
{
	br_colour *map;
	br_uint_32 large_dimension;
	br_uint_32 padded_w, padded_h;
	br_uint_32 real_w, real_h;

	/* Any failure inside here will make the texture uncacheable and unrenderable */
	buffer->buffer.uncacheable = BR_TRUE;

	/*
	 * Inspect the texture dimensions
	 */
	if(pm->pm_width > 256 || pm->pm_height > 256) {
		FXA_REJECT("oversize", pm);
        return BRE_DEV_UNSUPPORTED;
	}

	/*
	 * Voodoo hardware only accepts power-of-two textures with an aspect ratio
	 * between 1:8 and 8:1, so the original driver simply refused anything else.
	 * Carmageddon's pedestrian and cow sprites are sized naturally (20x25,
	 * 28x44, ...), so every one of them was rejected and the game fell back to
	 * drawing untextured black quads.
	 *
	 * Round the dimensions up to the next power of two instead, and record the
	 * real size so the texture upload can pad the pixels and the map
	 * coordinates can be scaled to address only the used part.
	 */
	real_w = pm->pm_width;
	real_h = pm->pm_height;
	for(padded_w = 1; padded_w < real_w; padded_w <<= 1)
		;
	for(padded_h = 1; padded_h < real_h; padded_h <<= 1)
		;

	/* Keep within the 8:1 aspect limit by growing the shorter side. */
	while(padded_w > padded_h * 8)
		padded_h <<= 1;
	while(padded_h > padded_w * 8)
		padded_w <<= 1;

	if(padded_w > 256 || padded_h > 256) {
		FXA_REJECT("oversize-after-pad", pm);
        return BRE_DEV_UNSUPPORTED;
	}


	/* Work from the power-of-two upload size.  NPOT images are resampled to
	 * fill this complete area below, so the normal Glide coordinate range can
	 * be used without a second real/padded correction. */
	if(padded_w >= padded_h) {
		large_dimension = padded_w;
		switch(padded_w / padded_h) {
		case 8:
			buffer->buffer.info.aspectRatio = GR_ASPECT_8x1;
			buffer->buffer.u_range = BR_SCALAR(256.0);
			buffer->buffer.v_range = BR_SCALAR( 32.0);
			break;
		case 4:
			buffer->buffer.info.aspectRatio = GR_ASPECT_4x1;
			buffer->buffer.u_range = BR_SCALAR(256.0);
			buffer->buffer.v_range = BR_SCALAR( 64.0);
			break;
		case 2:
			buffer->buffer.info.aspectRatio = GR_ASPECT_2x1;
			buffer->buffer.u_range = BR_SCALAR(256.0);
			buffer->buffer.v_range = BR_SCALAR(128.0);
			break;
		case 1:
			buffer->buffer.info.aspectRatio = GR_ASPECT_1x1;
			buffer->buffer.u_range = BR_SCALAR(256.0);
			buffer->buffer.v_range = BR_SCALAR(256.0);
			break;
		default:
			FXA_REJECT("aspect-ratio", pm);
			return BRE_DEV_UNSUPPORTED;
		}
	} else {
		large_dimension = padded_h;
		switch(padded_h / padded_w) {
		case 8:
			buffer->buffer.info.aspectRatio = GR_ASPECT_1x8;
			buffer->buffer.u_range = BR_SCALAR( 32.0);
			buffer->buffer.v_range = BR_SCALAR(256.0);
			break;
		case 4:
			buffer->buffer.info.aspectRatio = GR_ASPECT_1x4;
			buffer->buffer.u_range = BR_SCALAR( 64.0);
			buffer->buffer.v_range = BR_SCALAR(256.0);
			break;
		case 2:
			buffer->buffer.info.aspectRatio = GR_ASPECT_1x2;
			buffer->buffer.u_range = BR_SCALAR(128.0);
			buffer->buffer.v_range = BR_SCALAR(256.0);
			break;
		default:
			FXA_REJECT("aspect-ratio", pm);
			return BRE_DEV_UNSUPPORTED;
		}
	}

	switch(large_dimension) {
	case 256:
        buffer->buffer.info.largeLod = GR_LOD_256;
		break;
	case 128:
        buffer->buffer.info.largeLod = GR_LOD_128;
		break;
	case 64:
        buffer->buffer.info.largeLod = GR_LOD_64;
		break;
	case 32:
        buffer->buffer.info.largeLod = GR_LOD_32;
		break;
	case 16:
        buffer->buffer.info.largeLod = GR_LOD_16;
		break;
	case 8:
        buffer->buffer.info.largeLod = GR_LOD_8;
		break;
	case 4:
        buffer->buffer.info.largeLod = GR_LOD_4;
		break;
	case 2:
        buffer->buffer.info.largeLod = GR_LOD_2;
		break;
	case 1:
        buffer->buffer.info.largeLod = GR_LOD_1;
		break;
	}

    buffer->buffer.width_p = padded_w;
    buffer->buffer.width_b = padded_w * pixelmapBPP(pm);
	FXA_REPORT_PAD(real_w, real_h, padded_w, padded_h,
		(unsigned)pm->pm_row_bytes, (unsigned)buffer->buffer.width_b);
    buffer->buffer.height = padded_h;

	/* Check for mipmapping */
	if (pm->pm_mip_offset)
		buffer->buffer.info.smallLod = GR_LOD_1;
	else
		buffer->buffer.info.smallLod = buffer->buffer.info.largeLod;

	/* Resolve pointer to source */
    buffer->pixels_pointer = (char *) (pm->pm_pixels) +
                             pm->pm_base_y * pm->pm_row_bytes +
                             pm->pm_base_x * pixelmapBPP(pm);

	/*
	 * Non-power-of-two textures (the pedestrian and cow sprites) were rounded
	 * up above.  Build a contiguous power-of-two image here, where both the
	 * source pointer and its row stride are known.  Stretch the complete source
	 * over the upload area with nearest-neighbour sampling.  Leaving a blank
	 * padded border required a matching real/padded UV correction; on the old
	 * MiniGL/Warp3D path that correction was applied inconsistently when maps
	 * changed, producing halves, crops or repeated sprites.
	 *
	 * Two earlier approaches failed: indexing the original pixels from the
	 * shim through a stride passed in a global read past the end of the source
	 * and crashed (privilege violation, A0 = $F741F653); and rewriting width_b
	 * unconditionally broke textures that needed no padding at all -- the car's
	 * power bars vanished because their data still had pm_row_bytes while
	 * width_b claimed padded_w*bpp.  So: copy only when padding is required,
	 * and only then does width_b describe our own buffer.
	 */
	/* Only ever touch pixels we are allowed to read.  Setup3DfxBuffer() also
	 * runs for device pixelmaps that live on the card -- the frame and depth
	 * buffers -- and those carry pm_pixels == NULL with BR_PMF_NO_ACCESS set
	 * (devpixmp.c:270, 439, 480).  Copying from them dereferenced a near-NULL
	 * address, which on 68k is supervisor space: privilege violation, the crash
	 * that kept coming back no matter how the buffer was allocated. */
	/* Skip the 24/32-bit formats: the conversion further down rebuilds them
	 * into a padded RGB565 buffer in one pass, so padding them here first would
	 * make that conversion re-read its own output with the wrong bpp. */
	if ((real_w != padded_w || real_h != padded_h) &&
	    pm->pm_pixels != NULL && !(pm->pm_flags & BR_PMF_NO_ACCESS) &&
	    pm->pm_type != BR_PMT_RGB_888 && pm->pm_type != BR_PMT_RGBX_888 &&
	    pm->pm_type != BR_PMT_RGBA_8888) {
		br_uint_32 bpp = pixelmapBPP(pm);
		br_uint_32 dst_stride = padded_w * bpp;
		br_uint_32 need = dst_stride * padded_h;
		char *dst;
		br_uint_32 x, y;

		/* Treat padded_size as capacity.  Pedestrian animation updates this
		 * same stored buffer with frames of different dimensions.  Reallocating
		 * on every size change churns BRender's resource heap and eventually
		 * trips its magic-pointer assertion.  A larger allocation is safe: the
		 * current dst_stride and texture dimensions below describe the active
		 * prefix, and memset clears that entire prefix before it is uploaded. */
		if (buffer->padded_pixels != NULL && buffer->padded_size < need) {
			BrMemFree(buffer->padded_pixels);
			buffer->padded_pixels = NULL;
			buffer->padded_size = 0;
		}
		if (buffer->padded_pixels == NULL) {
			buffer->padded_pixels = BrMemAllocate(need, BR_MEMORY_PIXELS);
			if (buffer->padded_pixels == NULL) {
				FXA_REJECT("no-memory-for-pad", pm);
				return BRE_DEV_UNSUPPORTED;
			}
			buffer->padded_size = need;
		}

		dst = (char *)buffer->padded_pixels;
		for (y = 0; y < padded_h; y++) {
			const char *src_row = (const char *)buffer->pixels_pointer
				+ (y * real_h / padded_h) * pm->pm_row_bytes;
			char *dst_row = dst + y * dst_stride;
			for (x = 0; x < padded_w; x++)
				memcpy(dst_row + x * bpp,
					src_row + (x * real_w / padded_w) * bpp, bpp);
		}

		buffer->pixels_pointer = buffer->padded_pixels;
		/* Only now does the advertised stride describe the actual data. */
		buffer->buffer.width_b = dst_stride;
	}

    /* Only paletted textures carry a palette */
    buffer->buffer.palette_pointer = NULL;

	switch(pm->pm_type) {
	case BR_PMT_INDEX_8:
        buffer->buffer.info.format = GR_TEXFMT_P_8;

        if (pm->pm_map)
			map = pm->pm_map->pixels;
		else
			map = ObjectDevice(buffer)->clut->entries;

        /* Store palette for use when rendering */
        buffer->buffer.palette_pointer = map;
		buffer->buffer.blended = BR_FALSE;
		break;

	case BR_PMT_RGB_555:
		buffer->buffer.info.format = GR_TEXFMT_ARGB_1555;
		buffer->buffer.blended = BR_FALSE;
		break;

	case BR_PMT_RGB_565:
		buffer->buffer.info.format = GR_TEXFMT_RGB_565;
		buffer->buffer.blended = BR_FALSE;
		break;

	case BR_PMT_RGBA_4444:
		buffer->buffer.info.format = GR_TEXFMT_ARGB_4444;
		buffer->buffer.blended = BR_TRUE;
		break;

		/* The 3Dfx doesn't support 24-bit textures */
	case BR_PMT_RGB_888:
	case BR_PMT_RGBX_888:
	case BR_PMT_RGBA_8888: {
		/* The car's power bars (A P O) and similar art are 24/32-bit, which
		 * Voodoo cannot sample, so the original driver rejected them outright
		 * and the bars simply never appeared.  Convert down to RGB565 into a
		 * buffer of our own instead. */
		br_uint_32 src_bpp = pixelmapBPP(pm);
		br_uint_32 count = padded_w * padded_h;
		br_uint_16 *out;
		br_uint_32 x, y;

		if (pm->pm_pixels == NULL || (pm->pm_flags & BR_PMF_NO_ACCESS)) {
			FXA_REJECT("pixelmap-type-noaccess", pm);
			return BRE_DEV_UNSUPPORTED;
		}
		/* As above, keep a larger allocation as reusable capacity. */
		if (buffer->padded_pixels != NULL && buffer->padded_size < count * 2) {
			BrMemFree(buffer->padded_pixels);
			buffer->padded_pixels = NULL;
			buffer->padded_size = 0;
		}
		if (buffer->padded_pixels == NULL) {
			buffer->padded_pixels = BrMemAllocate(count * 2, BR_MEMORY_PIXELS);
			if (buffer->padded_pixels == NULL) {
				FXA_REJECT("no-memory-for-16bit", pm);
				return BRE_DEV_UNSUPPORTED;
			}
			buffer->padded_size = count * 2;
		}
		out = (br_uint_16 *)buffer->padded_pixels;
		for (y = 0; y < padded_h; y++) {
			br_uint_8 *sp = (br_uint_8 *)buffer->pixels_pointer
				+ (y * real_h / padded_h) * pm->pm_row_bytes;
			br_uint_16 *dp = out + y * padded_w;
			for (x = 0; x < padded_w; x++) {
				/* BRender stores these as B,G,R[,X] in memory order. */
				sp = (br_uint_8 *)buffer->pixels_pointer
					+ (y * real_h / padded_h) * pm->pm_row_bytes
					+ (x * real_w / padded_w) * src_bpp;
				br_uint_32 b = sp[0], g = sp[1], r = sp[2];
				dp[x] = (br_uint_16)(((r & 0xf8) << 8) |
					((g & 0xfc) << 3) | (b >> 3));
			}
		}
		buffer->pixels_pointer = buffer->padded_pixels;
		buffer->buffer.width_b = padded_w * 2;
		buffer->buffer.info.format = GR_TEXFMT_RGB_565;
		buffer->buffer.blended = BR_FALSE;
		break;
	}

	default:
		/*
         * Unknown source
		 */
		FXA_REJECT("pixelmap-type", pm);
        return(BRE_DEV_UNSUPPORTED);
	}

	/* Set start address */
	buffer->buffer.info.data = buffer->pixels_pointer;

    /* Everything on Glide TMU 0 for now. */
	buffer->buffer.tmu_id = GR_TMU0;

	/* It's all OK, so we can mark it as usable now! */
	buffer->buffer.uncacheable = BR_FALSE;

    return BRE_OK;
}


/*
 * Set up a static device object
 */
struct br_buffer_stored *BufferStored3DfxAllocate(struct br_primitive_library *plib,
	br_token use, struct br_device_pixelmap *pm, br_token_value *tv)
{
	struct br_buffer_stored * self;
	char *ident;

	switch(use) {
	case BRT_TEXTURE_O:
	case BRT_COLOUR_MAP_O:
		ident ="Texture";
		break;

	default:
		return NULL;
	}

	self = BrResAllocate(DRIVER_RESOURCE, sizeof(*self), BR_MEMORY_OBJECT);

	if(self == NULL)
		return NULL;

	self->dispatch = &bufferStoredDispatch;
	/* BrResAllocate() does not zero, and Setup3DfxBuffer() frees any previous
	 * padded image -- so this must start out NULL. */
	self->padded_pixels = NULL;
	self->padded_size = 0;
	self->identifier = ident;
	self->plib = plib;

	/*
	 * If caller does not garuantee to keep source data around, or
	 * source data is not memory mapped, then clone an in-memory copy
	 */

/* <dave> Below is an entry for the programming Hall Of Fame.
 * Congratulations due to whoever came up with this one.... */
#if 0
	if() {
	} else {
	}
#endif

	self->flags |= SBUFF_SHARED;

    self->buffer.on_card = BR_FALSE;
	Setup3DfxBuffer(self, pm);

	ObjectContainerAddFront(plib,(br_object *)self);

	return self;
}


static br_error BR_CMETHOD_DECL(br_buffer_stored_3dfx, update)(
	struct br_buffer_stored *self,
	struct br_device_pixelmap *pm,
	br_token_value *tv)
{
	Setup3DfxBuffer(self, pm);

    self->buffer.force_reload = BR_TRUE;

	return BRE_OK;
}


static void BR_CMETHOD_DECL(br_buffer_stored_3dfx, free)(br_buffer_stored *self)
{
    TextureCacheClearEntry(&self->buffer);

	/* Plain BrMemAllocate() block, not a child resource. */
	if (self->padded_pixels != NULL) {
		BrMemFree(self->padded_pixels);
		self->padded_pixels = NULL;
		self->padded_size = 0;
	}

	ObjectContainerRemove(self->plib, (br_object *)self);

	BrResFreeNoCallback(self);
}

static br_token BR_CMETHOD_DECL(br_buffer_stored_3dfx, type)(br_buffer_stored *self)
{
	return BRT_BUFFER_STORED;
}

static br_boolean BR_CMETHOD_DECL(br_buffer_stored_3dfx, isType)(br_buffer_stored *self, br_token t)
{
	return (t == BRT_BUFFER_STORED) || (t == BRT_OBJECT);
}

static br_int_32 BR_CMETHOD_DECL(br_buffer_stored_3dfx, space)(br_buffer_stored *self)
{
	return BrResSizeTotal(self);
}

static struct br_tv_template * BR_CMETHOD_DECL(br_buffer_stored_3dfx,templateQuery)
	(br_buffer_stored *self)
{
	bufferStoredTemplate.res = DRIVER_RESOURCE;
	return &bufferStoredTemplate;
}

/*
 * Default dispatch table for device
 */
static struct br_buffer_stored_dispatch bufferStoredDispatch = {
	NULL,
	NULL,
	NULL,
	NULL,
	BR_CMETHOD_REF(br_buffer_stored_3dfx,	free),
	BR_CMETHOD_REF(br_object_3dfx,			identifier),
	BR_CMETHOD_REF(br_buffer_stored_3dfx, 	type),
	BR_CMETHOD_REF(br_buffer_stored_3dfx, 	isType),
	BR_CMETHOD_REF(br_object_3dfx, 			device),
	BR_CMETHOD_REF(br_buffer_stored_3dfx, 	space),

	BR_CMETHOD_REF(br_buffer_stored_3dfx,	templateQuery),
	BR_CMETHOD_REF(br_object,				query),
	BR_CMETHOD_REF(br_object, 				queryBuffer),
	BR_CMETHOD_REF(br_object, 				queryMany),
	BR_CMETHOD_REF(br_object, 				queryManySize),
	BR_CMETHOD_REF(br_object, 				queryAll),
	BR_CMETHOD_REF(br_object,	 			queryAllSize),

	BR_CMETHOD_REF(br_buffer_stored_3dfx,	update),
};
