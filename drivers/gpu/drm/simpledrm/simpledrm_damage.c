/*
 * Copyright (C) 2012-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <asm/unaligned.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <linux/kernel.h>
#include "simpledrm.h"

static inline void sdrm_put(u8 *dst, u32 four_cc, u16 r, u16 g, u16 b)
{
	switch (four_cc) {
	case DRM_FORMAT_RGB565:
		r >>= 11;
		g >>= 10;
		b >>= 11;
		put_unaligned((u16)((r << 11) | (g << 5) | b), (u16 *)dst);
		break;
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
		r >>= 11;
		g >>= 11;
		b >>= 11;
		put_unaligned((u16)((r << 10) | (g << 5) | b), (u16 *)dst);
		break;
	case DRM_FORMAT_RGB888:
		r >>= 8;
		g >>= 8;
		b >>= 8;
#ifdef __LITTLE_ENDIAN
		dst[2] = r;
		dst[1] = g;
		dst[0] = b;
#elif defined(__BIG_ENDIAN)
		dst[0] = r;
		dst[1] = g;
		dst[2] = b;
#endif
		break;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		r >>= 8;
		g >>= 8;
		b >>= 8;
		put_unaligned((u32)((r << 16) | (g << 8) | b), (u32 *)dst);
		break;
	case DRM_FORMAT_ABGR8888:
		r >>= 8;
		g >>= 8;
		b >>= 8;
		put_unaligned((u32)((b << 16) | (g << 8) | r), (u32 *)dst);
		break;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		r >>= 4;
		g >>= 4;
		b >>= 4;
		put_unaligned((u32)((r << 20) | (g << 10) | b), (u32 *)dst);
		break;
	}
}

static void sdrm_blit_from_xrgb8888(const u8 *src,
				    u32 src_stride,
				    u32 src_bpp,
				    u8 *dst,
				    u32 dst_stride,
				    u32 dst_bpp,
				    u32 dst_four_cc,
				    u32 width,
				    u32 height)
{
	u32 val, i;

	while (height--) {
		for (i = 0; i < width; ++i) {
			val = get_unaligned((const u32 *)&src[i * src_bpp]);
			sdrm_put(&dst[i * dst_bpp], dst_four_cc,
				 (val & 0x00ff0000U) >> 8,
				 (val & 0x0000ff00U),
				 (val & 0x000000ffU) << 8);
		}

		src += src_stride;
		dst += dst_stride;
	}
}

static void sdrm_blit_from_rgb565(const u8 *src,
				  u32 src_stride,
				  u32 src_bpp,
				  u8 *dst,
				  u32 dst_stride,
				  u32 dst_bpp,
				  u32 dst_four_cc,
				  u32 width,
				  u32 height)
{
	u32 val, i;

	while (height--) {
		for (i = 0; i < width; ++i) {
			val = get_unaligned((const u16 *)&src[i * src_bpp]);
			sdrm_put(&dst[i * dst_bpp], dst_four_cc,
				 (val & 0xf800),
				 (val & 0x07e0) << 5,
				 (val & 0x001f) << 11);
		}

		src += src_stride;
		dst += dst_stride;
	}
}

static void sdrm_blit_lines(const u8 *src,
			    u32 src_stride,
			    u8 *dst,
			    u32 dst_stride,
			    u32 bpp,
			    u32 width,
			    u32 height)
{
	u32 len;

	len = width * bpp;

	while (height--) {
		memcpy(dst, src, len);
		src += src_stride;
		dst += dst_stride;
	}
}

static void sdrm_blit(struct sdrm_hw *hw,
		      struct sdrm_fb *fb,
		      u32 x,
		      u32 y,
		      u32 width,
		      u32 height)
{
	u32 src_bpp, dst_bpp;
	u8 *src, *dst;

	src = fb->bo->vmapping;
	src_bpp = DIV_ROUND_UP(fb->base.bits_per_pixel, 8);
	src += fb->base.offsets[0] + y * fb->base.pitches[0] + x * src_bpp;

	dst = hw->map;
	dst_bpp = DIV_ROUND_UP(hw->bpp, 8);
	dst += y * hw->stride + x * dst_bpp;

	if (fb->base.pixel_format == hw->format) {
		/* if formats are identical, do a line-by-line copy.. */
		sdrm_blit_lines(src, fb->base.pitches[0],
				dst, hw->stride, src_bpp, width, height);
	} else {
		/* ..otherwise call slow blit-function */
		switch (fb->base.pixel_format) {
		case DRM_FORMAT_ARGB8888:
		case DRM_FORMAT_XRGB8888:
			sdrm_blit_from_xrgb8888(src, fb->base.pitches[0],
						src_bpp, dst, hw->stride,
						dst_bpp, hw->format,
						width, height);
			break;
		case DRM_FORMAT_RGB565:
			sdrm_blit_from_rgb565(src, fb->base.pitches[0],
					      src_bpp, dst, hw->stride,
					      dst_bpp, hw->format,
					      width, height);
			break;
		}
	}
}

void sdrm_dirty(struct sdrm_fb *fb, u32 x, u32 y, u32 width, u32 height)
{
	struct sdrm_device *sdrm = fb->base.dev->dev_private;

	if (WARN_ON(!fb->bo->vmapping))
		return;

	mutex_lock(&sdrm->hw->lock);
	if (sdrm->hw->map)
		sdrm_blit(sdrm->hw, fb, x, y, width, height);
	mutex_unlock(&sdrm->hw->lock);
}
