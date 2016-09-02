/*
 * Copyright (C) 2012-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <drm/drmP.h>
#include <drm/drm_fb_helper.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_data/simplefb.h>
#include <linux/string.h>
#include "simpledrm.h"

static struct drm_driver sdrm_drm_driver;
static DEFINE_MUTEX(sdrm_lock);

static int sdrm_hw_identify(struct platform_device *pdev,
			    struct simplefb_platform_data *modep,
			    struct simplefb_format *formatp,
			    struct resource **memp,
			    u32 *bppp)
{
	static const struct simplefb_format valid_formats[] = SIMPLEFB_FORMATS;
	struct simplefb_platform_data pm = {}, *mode = pdev->dev.platform_data;
	struct device_node *np = pdev->dev.of_node;
	const struct simplefb_format *format = NULL;
	struct resource *mem;
	unsigned int depth;
	int r, bpp;
	size_t i;

	if (!mode) {
		if (!np)
			return -ENODEV;

		mode = &pm;

		r = of_property_read_u32(np, "width", &mode->width);
		if (r >= 0)
			r = of_property_read_u32(np, "height", &mode->height);
		if (r >= 0)
			r = of_property_read_u32(np, "stride", &mode->stride);
		if (r >= 0)
			r = of_property_read_string(np, "format",
						    &mode->format);
		if (r < 0)
			return r;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(valid_formats); ++i) {
		if (!strcmp(mode->format, valid_formats[i].name)) {
			format = &valid_formats[i];
			break;
		}
	}

	if (!format)
		return -ENODEV;

	switch (format->fourcc) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		/*
		 * You must adjust sdrm_put() whenever you add a new format
		 * here, otherwise, blitting operations will not work.
		 * Furthermore, include/linux/platform_data/simplefb.h needs
		 * to be adjusted so the platform-device actually allows this
		 * format.
		 */
		break;
	default:
		return -ENODEV;
	}

	drm_fb_get_bpp_depth(format->fourcc, &depth, &bpp);
	if (!bpp)
		return -ENODEV;
	if (resource_size(mem) < mode->stride * mode->height)
		return -ENODEV;
	if ((bpp + 7) / 8 * mode->width > mode->stride)
		return -ENODEV;

	*modep = *mode;
	*formatp = *format;
	*memp = mem;
	*bppp = bpp;
	return 0;
}

static struct sdrm_hw *sdrm_hw_new(const struct simplefb_platform_data *mode,
				   const struct simplefb_format *format,
				   const struct resource *mem,
				   u32 bpp)
{
	struct sdrm_hw *hw;

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return ERR_PTR(-ENOMEM);

	mutex_init(&hw->lock);
	hw->width = mode->width;
	hw->height = mode->height;
	hw->stride = mode->stride;
	hw->bpp = bpp;
	hw->format = format->fourcc;
	hw->base = mem->start;
	hw->size = resource_size(mem);

	return hw;
}

static struct sdrm_hw *sdrm_hw_free(struct sdrm_hw *hw)
{
	if (!hw)
		return NULL;

	WARN_ON(hw->map);
	mutex_destroy(&hw->lock);
	kfree(hw);

	return NULL;
}

static int sdrm_hw_bind(struct sdrm_hw *hw)
{
	mutex_lock(&hw->lock);
	if (!hw->map)
		hw->map = ioremap_wc(hw->base, hw->size);
	mutex_unlock(&hw->lock);

	return hw->map ? 0 : -EIO;
}

static void sdrm_hw_unbind(struct sdrm_hw *hw)
{
	if (!hw)
		return;

	mutex_lock(&hw->lock);
	if (hw->map) {
		iounmap(hw->map);
		hw->map = NULL;
	}
	mutex_unlock(&hw->lock);
}

static struct sdrm_device *sdrm_device_free(struct sdrm_device *sdrm)
{
	if (!sdrm)
		return NULL;

	WARN_ON(atomic_read(&sdrm->n_used) != INT_MIN);
	sdrm->hw = sdrm_hw_free(sdrm->hw);
	drm_dev_unref(sdrm->ddev);
	kfree(sdrm);

	return NULL;
}

static struct sdrm_device *sdrm_device_new(struct platform_device *pdev,
					   struct sdrm_hw *hw)
{
	struct sdrm_device *sdrm;
	int r;

	sdrm = kzalloc(sizeof(*sdrm), GFP_KERNEL);
	if (!sdrm)
		return ERR_PTR(-ENOMEM);

	atomic_set(&sdrm->n_used, INT_MIN);

	sdrm->ddev = drm_dev_alloc(&sdrm_drm_driver, &pdev->dev);
	if (!sdrm->ddev) {
		r = -ENOMEM;
		goto error;
	}

	sdrm->ddev->dev_private = sdrm;
	sdrm->hw = hw;

	return sdrm;

error:
	sdrm_device_free(sdrm);
	return ERR_PTR(r);
}

static void sdrm_device_unbind(struct sdrm_device *sdrm)
{
	if (sdrm) {
		sdrm_fbdev_unbind(sdrm);
		sdrm_kms_unbind(sdrm);
		sdrm_hw_unbind(sdrm->hw);
		sdrm_of_unbind(sdrm);
	}
}

static int sdrm_device_bind(struct sdrm_device *sdrm)
{
	int r;

	r = sdrm_of_bind(sdrm);
	if (r < 0)
		goto error;

	r = sdrm_hw_bind(sdrm->hw);
	if (r < 0)
		goto error;

	r = sdrm_kms_bind(sdrm);
	if (r < 0)
		goto error;

	sdrm_fbdev_bind(sdrm);

	return 0;

error:
	sdrm_device_unbind(sdrm);
	return r;
}

static int sdrm_device_acquire(struct sdrm_device *sdrm)
{
	return (sdrm && atomic_inc_unless_negative(&sdrm->n_used))
		? 0 : -ENODEV;
}

static void sdrm_device_release(struct sdrm_device *sdrm)
{
	if (sdrm && atomic_dec_return(&sdrm->n_used) == INT_MIN) {
		sdrm_device_unbind(sdrm);
		sdrm_device_free(sdrm);
	}
}

static void sdrm_device_lastclose(struct drm_device *ddev)
{
	struct sdrm_device *sdrm = ddev->dev_private;

	if (sdrm->fbdev)
		drm_fb_helper_restore_fbdev_mode_unlocked(sdrm->fbdev);
}

static int sdrm_fop_open(struct inode *inode, struct file *file)
{
	struct drm_device *ddev;
	int r;

	mutex_lock(&sdrm_lock);
	r = drm_open(inode, file);
	if (r >= 0) {
		ddev = file->private_data;
		r = sdrm_device_acquire(ddev->dev_private);
		if (r < 0)
			drm_release(inode, file);
	}
	mutex_unlock(&sdrm_lock);

	return r;
}

static int sdrm_fop_release(struct inode *inode, struct file *file)
{
	struct drm_file *dfile = file->private_data;
	struct drm_device *ddev = dfile->minor->dev;
	struct sdrm_device *sdrm = ddev->dev_private;
	int res;

	res = drm_release(inode, file);
	sdrm_device_release(sdrm);
	return res;
}

static int sdrm_fop_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct drm_file *dfile = file->private_data;
	struct drm_device *dev = dfile->minor->dev;
	struct drm_gem_object *obj = NULL;
	struct drm_vma_offset_node *node;
	int r;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  vma->vm_pgoff,
						  vma_pages(vma));
	if (likely(node)) {
		obj = container_of(node, struct drm_gem_object, vma_node);
		if (!kref_get_unless_zero(&obj->refcount))
			obj = NULL;
	}
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!obj)
		return -EINVAL;

	if (!drm_vma_node_is_allowed(node, dfile)) {
		drm_gem_object_unreference_unlocked(obj);
		return -EACCES;
	}

	if (vma->vm_file)
		fput(vma->vm_file);
	vma->vm_file = get_file(obj->filp);
	vma->vm_pgoff = 0;

	r = obj->filp->f_op->mmap(obj->filp, vma);
	drm_gem_object_unreference_unlocked(obj);
	return r;
}

static int sdrm_simplefb_probe(struct platform_device *pdev)
{
	struct simplefb_platform_data hw_mode;
	struct simplefb_format hw_format;
	struct sdrm_device *sdrm = NULL;
	struct sdrm_hw *hw = NULL;
	struct resource *hw_mem;
	u32 hw_bpp;
	int r;

	r = sdrm_hw_identify(pdev, &hw_mode, &hw_format, &hw_mem, &hw_bpp);
	if (r < 0)
		goto error;

	hw = sdrm_hw_new(&hw_mode, &hw_format, hw_mem, hw_bpp);
	if (IS_ERR(hw)) {
		r = PTR_ERR(hw);
		hw = NULL;
		goto error;
	}

	sdrm = sdrm_device_new(pdev, hw);
	if (IS_ERR(sdrm)) {
		r = PTR_ERR(sdrm);
		sdrm = NULL;
		goto error;
	}
	hw = NULL;
	platform_set_drvdata(pdev, sdrm);

	r = sdrm_device_bind(sdrm);
	if (r < 0)
		goto error;

	/* mark device as enabled and acquire bus ref */
	WARN_ON(atomic_read(&sdrm->n_used) != INT_MIN);
	atomic_set(&sdrm->n_used, 1);

	r = drm_dev_register(sdrm->ddev, 0);
	if (r < 0) {
		/* mark device as disabled and drop bus ref */
		WARN_ON(atomic_add_return(INT_MIN, &sdrm->n_used) != INT_MIN);
		sdrm_device_release(sdrm);
		return r;
	}

	dev_info(sdrm->ddev->dev, "initialized %s on minor %d\n",
		 sdrm->ddev->driver->name, sdrm->ddev->primary->index);

	return 0;

error:
	sdrm_device_unbind(sdrm);
	sdrm_device_free(sdrm);
	sdrm_hw_free(hw);
	return r;
}

static int sdrm_simplefb_remove(struct platform_device *pdev)
{
	struct sdrm_device *sdrm = platform_get_drvdata(pdev);

	/* mark device as disabled */
	atomic_add(INT_MIN, &sdrm->n_used);
	sdrm_hw_unbind(sdrm->hw);

	mutex_lock(&sdrm_lock);
	drm_dev_unregister(sdrm->ddev);
	sdrm_device_release(sdrm);
	mutex_unlock(&sdrm_lock);

	return 0;
}

static const struct file_operations sdrm_drm_fops = {
	.owner = THIS_MODULE,
	.open = sdrm_fop_open,
	.release = sdrm_fop_release,
	.mmap = sdrm_fop_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct drm_driver sdrm_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &sdrm_drm_fops,
	.lastclose = sdrm_device_lastclose,

	.gem_free_object = sdrm_bo_free,

	.dumb_create = sdrm_dumb_create,
	.dumb_map_offset = sdrm_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,

	.name = "simpledrm",
	.desc = "Simple firmware framebuffer DRM driver",
	.date = "20160901",
};

static const struct of_device_id sdrm_simplefb_of_match[] = {
	{ .compatible = "simple-framebuffer", },
	{},
};
MODULE_DEVICE_TABLE(of, sdrm_simplefb_of_match);

static struct platform_driver sdrm_simplefb_driver = {
	.probe = sdrm_simplefb_probe,
	.remove = sdrm_simplefb_remove,
	.driver = {
		.name = "simple-framebuffer",
		.mod_name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
		.of_match_table = sdrm_simplefb_of_match,
	},
};

static int __init sdrm_init(void)
{
	int r;

	r = platform_driver_register(&sdrm_simplefb_driver);
	if (r < 0)
		return r;

	sdrm_of_bootstrap();
	return 0;
}

static void __exit sdrm_exit(void)
{
	platform_driver_unregister(&sdrm_simplefb_driver);
}

module_init(sdrm_init);
module_exit(sdrm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple firmware framebuffer DRM driver");
MODULE_ALIAS("platform:simple-framebuffer");
