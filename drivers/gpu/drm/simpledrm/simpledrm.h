#ifndef __SDRM_SIMPLEDRM_H
#define __SDRM_SIMPLEDRM_H

/*
 * Copyright (C) 2012-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_gem.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

struct clk;
struct regulator;
struct simplefb_format;

struct sdrm_hw {
	struct mutex lock;
	u32 width;
	u32 height;
	u32 stride;
	u32 bpp;
	u32 format;
	unsigned long base;
	unsigned long size;
	void *map;
};

struct sdrm_bo {
	struct drm_gem_object base;
	struct page **pages;
	void *vmapping;
};

struct sdrm_fb {
	struct drm_framebuffer base;
	struct sdrm_bo *bo;
};

struct sdrm_device {
	atomic_t n_used;
	struct drm_device *ddev;
	struct sdrm_hw *hw;

	size_t n_clks;
	size_t n_regulators;
	struct clk **clks;
	struct regulator **regulators;

	struct drm_simple_display_pipe pipe;
	struct drm_connector conn;
};

void sdrm_of_bootstrap(void);
int sdrm_of_bind(struct sdrm_device *sdrm);
void sdrm_of_unbind(struct sdrm_device *sdrm);

int sdrm_kms_bind(struct sdrm_device *sdrm);
void sdrm_kms_unbind(struct sdrm_device *sdrm);

void sdrm_dirty(struct sdrm_fb *fb, u32 x, u32 y, u32 width, u32 height);

struct sdrm_bo *sdrm_bo_new(struct drm_device *ddev, size_t size);
void sdrm_bo_free(struct drm_gem_object *obj);
int sdrm_bo_vmap(struct sdrm_bo *bo);

int sdrm_dumb_create(struct drm_file *dfile,
		     struct drm_device *ddev,
		     struct drm_mode_create_dumb *arg);
int sdrm_dumb_map_offset(struct drm_file *dfile,
			 struct drm_device *ddev,
			 uint32_t handle,
			 uint64_t *offset);

#endif /* __SDRM_SIMPLEDRM_H */
