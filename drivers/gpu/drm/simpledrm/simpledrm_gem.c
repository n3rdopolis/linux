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
#include <drm/drm_gem.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "simpledrm.h"

struct sdrm_bo *sdrm_bo_new(struct drm_device *ddev, size_t size)
{
	struct sdrm_bo *bo;

	WARN_ON(!size || (size & ~PAGE_MASK) != 0);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return NULL;

	if (drm_gem_object_init(ddev, &bo->base, size)) {
		kfree(bo);
		return NULL;
	}

	return bo;
}

void sdrm_bo_free(struct drm_gem_object *dobj)
{
	struct sdrm_bo *bo = container_of(dobj, struct sdrm_bo, base);

	if (bo->vmapping)
		vunmap(bo->vmapping);
	if (bo->pages)
		drm_gem_put_pages(dobj, bo->pages, false, false);
	drm_gem_object_release(dobj);
	kfree(bo);
}

int sdrm_bo_vmap(struct sdrm_bo *bo)
{
	int r;

	if (!bo->pages) {
		bo->pages = drm_gem_get_pages(&bo->base);
		if (IS_ERR(bo->pages)) {
			r = PTR_ERR(bo->pages);
			bo->pages = NULL;
			return r;
		}
	}

	if (!bo->vmapping) {
		bo->vmapping = vmap(bo->pages, bo->base.size / PAGE_SIZE, 0,
				    PAGE_KERNEL);
		if (!bo->vmapping)
			return -ENOMEM;
	}

	return 0;
}

int sdrm_dumb_create(struct drm_file *dfile,
		     struct drm_device *ddev,
		     struct drm_mode_create_dumb *args)
{
	struct sdrm_bo *bo;
	int r;

	/* overflow checks are done by DRM core */
	args->pitch = (args->bpp + 7) / 8 * args->width;
	args->size = PAGE_ALIGN(args->pitch * args->height);

	bo = sdrm_bo_new(ddev, args->size);
	if (!bo)
		return -ENOMEM;

	r = drm_gem_handle_create(dfile, &bo->base, &args->handle);
	drm_gem_object_unreference_unlocked(&bo->base);
	return r;
}

int sdrm_dumb_map_offset(struct drm_file *dfile,
			 struct drm_device *ddev,
			 uint32_t handle,
			 uint64_t *offset)
{
	struct drm_gem_object *dobj;
	int r;

	dobj = drm_gem_object_lookup(dfile, handle);
	if (!dobj)
		return -ENOENT;

	r = drm_gem_create_mmap_offset(dobj);
	if (r >= 0)
		*offset = drm_vma_node_offset_addr(&dobj->vma_node);
	drm_gem_object_unreference_unlocked(dobj);
	return r;
}
