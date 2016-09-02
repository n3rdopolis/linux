/*
 * Copyright (C) 2012-2016 Red Hat, Inc.
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "simpledrm.h"

static int sdrm_fbdev_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	return -ENODEV;
}

static struct fb_ops sdrm_fbdev_ops = {
	.owner		= THIS_MODULE,
	.fb_fillrect	= drm_fb_helper_sys_fillrect,
	.fb_copyarea	= drm_fb_helper_sys_copyarea,
	.fb_imageblit	= drm_fb_helper_sys_imageblit,
	.fb_check_var	= drm_fb_helper_check_var,
	.fb_set_par	= drm_fb_helper_set_par,
	.fb_setcmap	= drm_fb_helper_setcmap,
	.fb_mmap	= sdrm_fbdev_mmap,
};

static int sdrm_fbdev_probe(struct drm_fb_helper *fbdev,
			    struct drm_fb_helper_surface_size *sizes)
{
	struct sdrm_device *sdrm = fbdev->dev->dev_private;
	struct drm_mode_fb_cmd2 cmd = {
		.width = sdrm->hw->width,
		.height = sdrm->hw->height,
		.pitches[0] = sdrm->hw->stride,
		.pixel_format = sdrm->hw->format,
	};
	struct fb_info *fbi;
	struct sdrm_bo *bo;
	struct sdrm_fb *fb;
	int r;

	fbi = drm_fb_helper_alloc_fbi(fbdev);
	if (IS_ERR(fbi))
		return PTR_ERR(fbi);

	bo = sdrm_bo_new(sdrm->ddev,
			 PAGE_ALIGN(sdrm->hw->height * sdrm->hw->stride));
	if (!bo) {
		r = -ENOMEM;
		goto error;
	}

	fb = sdrm_fb_new(bo, &cmd);
	drm_gem_object_unreference_unlocked(&bo->base);
	if (IS_ERR(fb)) {
		r = PTR_ERR(fb);
		goto error;
	}

	fbdev->fb = &fb->base;
	fbi->par = fbdev;
	fbi->flags = FBINFO_DEFAULT | FBINFO_MISC_FIRMWARE;
	fbi->fbops = &sdrm_fbdev_ops;

	drm_fb_helper_fill_fix(fbi, fb->base.pitches[0], fb->base.depth);
	drm_fb_helper_fill_var(fbi, fbdev, fb->base.width, fb->base.height);

	strncpy(fbi->fix.id, "simpledrmfb", sizeof(fbi->fix.id) - 1);
	fbi->screen_base = bo->vmapping;
	fbi->fix.smem_len = bo->base.size;

	return 0;

error:
	drm_fb_helper_release_fbi(fbdev);
	return r;
}

static const struct drm_fb_helper_funcs sdrm_fbdev_funcs = {
	.fb_probe = sdrm_fbdev_probe,
};

void sdrm_fbdev_bind(struct sdrm_device *sdrm)
{
	struct drm_fb_helper *fbdev;
	int r;

	fbdev = kzalloc(sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev)
		return;

	drm_fb_helper_prepare(sdrm->ddev, fbdev, &sdrm_fbdev_funcs);

	r = drm_fb_helper_init(sdrm->ddev, fbdev, 1, 1);
	if (r < 0)
		goto error;

	r = drm_fb_helper_single_add_all_connectors(fbdev);
	if (r < 0)
		goto error;

	r = drm_fb_helper_initial_config(fbdev,
				sdrm->ddev->mode_config.preferred_depth);
	if (r < 0)
		goto error;

	if (!fbdev->fbdev)
		goto error;

	sdrm->fbdev = fbdev;
	return;

error:
	drm_fb_helper_fini(fbdev);
	kfree(fbdev);
}

void sdrm_fbdev_unbind(struct sdrm_device *sdrm)
{
	struct drm_fb_helper *fbdev = sdrm->fbdev;

	if (!fbdev)
		return;

	sdrm->fbdev = NULL;
	drm_fb_helper_unregister_fbi(fbdev);
	cancel_work_sync(&fbdev->dirty_work);
	drm_fb_helper_release_fbi(fbdev);
	drm_framebuffer_unreference(fbdev->fb);
	fbdev->fb = NULL;
	drm_fb_helper_fini(fbdev);
	kfree(fbdev);
}
