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
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "simpledrm.h"

static const uint32_t sdrm_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
};

static int sdrm_conn_get_modes(struct drm_connector *conn)
{
	struct sdrm_device *sdrm = conn->dev->dev_private;
	struct drm_display_mode *mode;

	mode = drm_cvt_mode(sdrm->ddev, sdrm->hw->width, sdrm->hw->height,
			    60, false, false, false);
	if (!mode)
		return 0;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(conn, mode);

	return 1;
}

static const struct drm_connector_helper_funcs sdrm_conn_hfuncs = {
	.get_modes	= sdrm_conn_get_modes,
	.best_encoder	= drm_atomic_helper_best_encoder,
};

static enum drm_connector_status sdrm_conn_detect(struct drm_connector *conn,
						  bool force)
{
	/*
	 * We simulate an always connected monitor. simple-fb doesn't
	 * provide any way to detect whether the connector is active. Hence,
	 * signal DRM core that it is always connected.
	 */
	return connector_status_connected;
}

static const struct drm_connector_funcs sdrm_conn_ops = {
	.dpms			= drm_atomic_helper_connector_dpms,
	.reset			= drm_atomic_helper_connector_reset,
	.detect			= sdrm_conn_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static void sdrm_crtc_send_vblank_event(struct drm_crtc *crtc)
{
	if (crtc->state && crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}
}

void sdrm_display_pipe_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *plane_state)
{
	struct drm_framebuffer *dfb = pipe->plane.state->fb;
	struct sdrm_fb *fb;

	sdrm_crtc_send_vblank_event(&pipe->crtc);

	if (dfb) {
		fb = container_of(dfb, struct sdrm_fb, base);
		pipe->plane.fb = dfb;
		sdrm_dirty(fb, 0, 0, dfb->width, dfb->height);
	}
}

static void sdrm_display_pipe_enable(struct drm_simple_display_pipe *pipe,
				     struct drm_crtc_state *crtc_state)
{
	sdrm_crtc_send_vblank_event(&pipe->crtc);
}

static void sdrm_display_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	sdrm_crtc_send_vblank_event(&pipe->crtc);
}

static const struct drm_simple_display_pipe_funcs sdrm_pipe_funcs = {
	.update		= sdrm_display_pipe_update,
	.enable		= sdrm_display_pipe_enable,
	.disable	= sdrm_display_pipe_disable,
};

static int sdrm_fb_create_handle(struct drm_framebuffer *dfb,
				 struct drm_file *dfile,
				 unsigned int *handle)
{
	struct sdrm_fb *fb = container_of(dfb, struct sdrm_fb, base);

	return drm_gem_handle_create(dfile, &fb->bo->base, handle);
}

static int sdrm_fb_dirty(struct drm_framebuffer *dfb,
			 struct drm_file *dfile,
			 unsigned int flags,
			 unsigned int color,
			 struct drm_clip_rect *clips,
			 unsigned int n_clips)
{
	struct sdrm_fb *fb = container_of(dfb, struct sdrm_fb, base);
	struct sdrm_device *sdrm = dfb->dev->dev_private;
	unsigned int i;

	drm_modeset_lock_all(sdrm->ddev);
	if (dfb == sdrm->pipe.plane.fb) {
		if (!clips || !n_clips) {
			sdrm_dirty(fb, 0, 0, dfb->width, dfb->height);
		} else {
			for (i = 0; i < n_clips; i++) {
				if (clips[i].x1 > clips[i].x2 ||
				    clips[i].x2 > dfb->width ||
				    clips[i].y1 > clips[i].y2 ||
				    clips[i].y2 > dfb->height)
					continue;

				sdrm_dirty(fb, clips[i].x1, clips[i].y1,
					   clips[i].x2 - clips[i].x1,
					   clips[i].y2 - clips[i].y1);
			}
		}
	}
	drm_modeset_unlock_all(sdrm->ddev);

	return 0;
}

static void sdrm_fb_destroy(struct drm_framebuffer *dfb)
{
	struct sdrm_fb *fb = container_of(dfb, struct sdrm_fb, base);

	drm_framebuffer_cleanup(dfb);
	drm_gem_object_unreference_unlocked(&fb->bo->base);
	kfree(fb);
}

static const struct drm_framebuffer_funcs sdrm_fb_ops = {
	.create_handle		= sdrm_fb_create_handle,
	.dirty			= sdrm_fb_dirty,
	.destroy		= sdrm_fb_destroy,
};

static struct drm_framebuffer *
sdrm_fb_create(struct drm_device *ddev,
	       struct drm_file *dfile,
	       const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_gem_object *dobj;
	struct sdrm_fb *fb = NULL;
	struct sdrm_bo *bo;
	int r;

	if (cmd->flags)
		return ERR_PTR(-EINVAL);

	dobj = drm_gem_object_lookup(dfile, cmd->handles[0]);
	if (!dobj)
		return ERR_PTR(-EINVAL);

	bo = container_of(dobj, struct sdrm_bo, base);

	r = sdrm_bo_vmap(bo);
	if (r < 0)
		goto error;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb) {
		r = -ENOMEM;
		goto error;
	}

	fb->bo = bo;
	drm_helper_mode_fill_fb_struct(&fb->base, cmd);

	r = drm_framebuffer_init(ddev, &fb->base, &sdrm_fb_ops);
	if (r < 0)
		goto error;

	DRM_DEBUG_KMS("[FB:%d] pixel_format: %s\n", fb->base.base.id,
		      drm_get_format_name(fb->base.pixel_format));

	return &fb->base;

error:
	kfree(fb);
	drm_gem_object_unreference_unlocked(dobj);
	return ERR_PTR(r);
}

static const struct drm_mode_config_funcs sdrm_mode_config_ops = {
	.fb_create		= sdrm_fb_create,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

int sdrm_kms_bind(struct sdrm_device *sdrm)
{
	struct drm_connector *conn = &sdrm->conn;
	struct drm_device *ddev = sdrm->ddev;
	int r;

	drm_mode_config_init(ddev);
	ddev->mode_config.min_width = sdrm->hw->width;
	ddev->mode_config.max_width = sdrm->hw->width;
	ddev->mode_config.min_height = sdrm->hw->height;
	ddev->mode_config.max_height = sdrm->hw->height;
	ddev->mode_config.preferred_depth = sdrm->hw->bpp;
	ddev->mode_config.funcs = &sdrm_mode_config_ops;
	drm_connector_helper_add(conn, &sdrm_conn_hfuncs);

	r = drm_connector_init(ddev, conn, &sdrm_conn_ops,
			       DRM_MODE_CONNECTOR_VIRTUAL);
	if (r < 0)
		goto error;

	r = drm_simple_display_pipe_init(ddev, &sdrm->pipe, &sdrm_pipe_funcs,
					 sdrm_formats,
					 ARRAY_SIZE(sdrm_formats), conn);
	if (r < 0)
		goto error;

	drm_mode_config_reset(ddev);
	return 0;

error:
	drm_mode_config_cleanup(ddev);
	return r;
}

void sdrm_kms_unbind(struct sdrm_device *sdrm)
{
	if (sdrm->ddev->mode_config.funcs)
		drm_mode_config_cleanup(sdrm->ddev);
}
