/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */

#define pr_fmt(fmt) "sysfb: " fmt
#include <linux/console.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/fb.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfb.h>
#include <linux/vt.h>

static bool sysfb_evict_match_resource(struct sysfb_evict_ctx *ctx,
				       struct resource *mem)
{
	struct aperture *g;
	unsigned int i;

	for (i = 0; i < ctx->ap->count; ++i) {
		g = &ctx->ap->ranges[i];

		if (mem->start == g->base)
			return true;
		if (mem->start >= g->base && mem->end < g->base + g->size)
			return true;
		if ((ctx->flags & SYSFB_EVICT_VBE) && mem->start == 0xA0000)
			return true;
	}

	return false;
}

static int sysfb_evict_platform_device(struct device *dev, void *userdata)
{
	struct sysfb_evict_ctx *ctx = userdata;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *mem;

	if (!pdev->name)
		return 0;

	if (!strcmp(pdev->name, "simple-framebuffer")) {
		mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!mem)
			return 0;
		if (!sysfb_evict_match_resource(ctx, mem))
			return 0;

#ifdef CONFIG_OF_ADDRESS
		if (dev->of_node)
			of_platform_device_destroy(dev);
		else
#endif
		if (dev_get_platdata(&pdev->dev))
			platform_device_del(pdev);
	}

	return 0;
}

static int sysfb_evict_platform(struct sysfb_evict_ctx *ctx)
{
	/*
	 * Early-boot architecture setup and boot-loarders sometimes create
	 * preliminary platform devices with a generic framebuffer setup. This
	 * allows graphics access during boot-up, without a real graphics
	 * driver loaded. However, once a real graphics driver takes over, we
	 * have to destroy those platform devices. In the legacy fbdev case, we
	 * just used to unload the fbdev driver. However, to make sure any kind
	 * of driver is unloaded, the platform-eviction code here simply
	 * removes any conflicting platform device directly. This causes any
	 * bound driver to be detached, and then removes the device entirely so
	 * it cannot be bound to later on.
	 *
	 * Please note that any such platform device must be registered by
	 * early architecture setup code. If they are registered after regular
	 * GFX drivers, this will fail horribly.
	 */

	static DEFINE_MUTEX(lock);
	int ret;

	/*
	 * In case of static platform-devices, we must iterate the bus and
	 * remove them manually. We know that we're the only code that might
	 * remove them, so a simple static lock serializes all calls here.
	 */
	mutex_lock(&lock);
	ret = bus_for_each_dev(&platform_bus_type, NULL, ctx,
			       sysfb_evict_platform_device);
	mutex_unlock(&lock);
	return ret;
}

static int sysfb_evict_fbdev(struct sysfb_evict_ctx *ctx)
{
	/*
	 * Usually, evicting platform devices should be enough to also trigger
	 * fbdev unloading. However, some legacy devices (e.g., uvesafb) have
	 * no platform devices that can be evicted, so we still fall back to
	 * the legacy fbdev removal code. Note that this only removes fbdev
	 * devices marked as FBINFO_MISC_FIRMWARE. Anything else is left
	 * untouched.
	 *
	 * As usual, this only works if the fbdev device is probed early,
	 * before any real GFX driver wants to take over.
	 */

	int ret = 0;

#ifdef CONFIG_FB
	ret = remove_conflicting_framebuffers(ctx->ap, "sysfb",
					      ctx->flags & SYSFB_EVICT_VBE);
#endif

	return ret;
}

static int sysfb_evict_vgacon(struct sysfb_evict_ctx *ctx)
{
	/*
	 * The VGACON console driver pokes at VGA registers randomly. If a GFX
	 * driver cannot keep the VGA support alive, it better makes sure to
	 * unload VGACON before probing.
	 *
	 * Unloading VGACON requires us to first force dummycon to take over
	 * from vgacon (but only if vgacon is really in use), followed by a
	 * deregistration of vgacon. Note that this prevents vgacon from being
	 * used again after the GFX driver is unloaded. But that is usually
	 * fine, since VGA state is rarely restored on driver-unload, anyway.
	 *
	 * Note that we rely on VGACON to be probed in early boot (actually
	 * done by ARCH setup code). If it is probed after GFX drivers, this
	 * will fail horribly. You better make sure VGACON is probed early and
	 * GFX drivers are probed as normal modules.
	 */

	int ret = 0;

#ifdef CONFIG_VGA_CONSOLE
	console_lock();
	if (con_is_bound(&vga_con))
		ret = do_take_over_console(&dummy_con, 0,
					   MAX_NR_CONSOLES - 1, 1);
	if (ret == 0) {
		ret = do_unregister_con_driver(&vga_con);
		if (ret == -ENODEV) /* ignore "already unregistered" */
			ret = 0;
	}
	console_unlock();
#endif

	return ret;
}

/**
 * sysfb_evict_conflicts - remove any conflicting system-framebuffers
 * @ctx:		eviction context
 *
 * This function evicts any conflicting system-framebuffers and their bound
 * drivers, according to the data given in @ctx.
 *
 * Depending on @ctx->flags, the following operations are performed:
 *
 *   SYSFB_EVICT_PLATFORM: Firmware framebuffer platform devices (eg.,
 *                         'simple-framebuffer') that overlap @ctx are removed
 *                         from the system, causing drivers to be unbound.
 *                         If SYSFB_EVICT_VBE is given, this also affects
 *                         devices that own the VGA region.
 *
 *   SYSFB_EVICT_FBDEV: Any firmware fbdev drivers that overlap @ctx are
 *                      unloaded.
 *                      Furthermore, if SYSFB_EVICT_VBE is given as well, any
 *                      fbdev driver that maps the VGA region is unloaded.
 *
 *   SYSFB_EVICT_VGACON: The vgacon console driver is unbound and unregistered.
 *
 * This might call into fbdev driver unregistration, or even device_del() on
 * some buses. Hence, make sure you call this from your top-level
 * probe-callbacks, rather than with any gfx-subsystem locks held.
 *
 * RETURNS:
 * 0 on success, negative error code on failure.
 */
int sysfb_evict_conflicts(struct sysfb_evict_ctx *ctx)
{
	int ret;

	if (WARN_ON(!ctx || !ctx->ap))
		return -EINVAL;

	pr_info("removing conflicts (sysfb%s%s%s%s)\n",
		(ctx->flags & SYSFB_EVICT_PLATFORM) ? ", platform" : "",
		(ctx->flags & SYSFB_EVICT_FBDEV) ? ", fbdev" : "",
		(ctx->flags & SYSFB_EVICT_VGACON) ? ", vgacon" : "",
		(ctx->flags & SYSFB_EVICT_VBE) ? ", vbe" : "");

	if (ctx->flags & SYSFB_EVICT_PLATFORM) {
		ret = sysfb_evict_platform(ctx);
		if (ret < 0)
			return ret;
	}

	if (ctx->flags & SYSFB_EVICT_FBDEV) {
		ret = sysfb_evict_fbdev(ctx);
		if (ret < 0)
			return ret;
	}

	if (ctx->flags & SYSFB_EVICT_VGACON) {
		ret = sysfb_evict_vgacon(ctx);
		if (ret < 0)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(sysfb_evict_conflicts);

/**
 * sysfb_evict_conflicts_firmware() - remove all firmware framebuffers
 *
 * This is similar to sysfb_evict_conflicts() but uses a fake aperture spanning
 * the entire address-space. This is suitable for any GFX driver that just
 * wants to get rid of all available firmware framebuffers.
 *
 * RETURNS:
 * 0 on success, negative error code on failure.
 */
int sysfb_evict_conflicts_firmware(void)
{
	struct sysfb_evict_ctx ctx = {};
	int ret;

	ctx.ap = alloc_apertures(1);
	if (!ctx.ap)
		return -ENOMEM;

	ctx.ap->ranges[0].base = 0;
	ctx.ap->ranges[0].size = ~0;

	ctx.flags |= SYSFB_EVICT_FBDEV | SYSFB_EVICT_PLATFORM;
	ret = sysfb_evict_conflicts(&ctx);

	kfree(ctx.ap);
	return ret;
}
EXPORT_SYMBOL(sysfb_evict_conflicts_firmware);

#ifdef CONFIG_PCI
/**
 * sysfb_evict_conflicts_pci() - remove all system framebuffers conflicting
 *                               with the given pci device
 * @pdev:		pci device
 *
 * This is similar to sysfb_evict_conflicts() but generates the eviction
 * context based on the given pci device @pdev.
 *
 * RETURNS:
 * 0 on success, negative error code on failure.
 */
int sysfb_evict_conflicts_pci(struct pci_dev *pdev)
{
	struct sysfb_evict_ctx ctx = {};
	size_t i, n, offset;
	int ret;

	/*
	 * If this device is used as primary VGA device, it is shadowed at the
	 * VBE base address, so make sure to include it in the apertures.
	 */
	if (pdev->resource[PCI_ROM_RESOURCE].flags & IORESOURCE_ROM_SHADOW)
		ctx.flags |= SYSFB_EVICT_VBE;

	/*
	 * If a device is a VGA device, make sure to kick out vgacon. We cannot
	 * rely on the IORESOURCE_ROM_SHADOW, since vgacon can switch between
	 * vga devices at runtime. So kick out vgacon anyway.
	 */
	if ((pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA)
		ctx.flags |= SYSFB_EVICT_VGACON;

	/*
	 * Allocate apertures for all standard PCI resources. Skip them in case
	 * they are empty.
	 */
	ctx.ap = alloc_apertures(PCI_STD_RESOURCE_END - PCI_STD_RESOURCES + 1);
	if (!ctx.ap)
		return -ENOMEM;

	offset = PCI_STD_RESOURCES;
	for (n = 0, i = 0; i < ctx.ap->count; ++i) {
		if (pci_resource_len(pdev, offset + i) < 1)
			continue;

		ctx.ap->ranges[n].base = pci_resource_start(pdev, offset + i);
		ctx.ap->ranges[n].size = pci_resource_len(pdev, offset + i);
		++n;
	}
	ctx.ap->count = n;

	/*
	 * Evict all matching fbdev devices, VBE devices if they shadow this
	 * device, vgacon if this is a vga device, and platform devices if they
	 * match.
	 */
	ctx.flags |= SYSFB_EVICT_FBDEV | SYSFB_EVICT_PLATFORM;
	ret = sysfb_evict_conflicts(&ctx);

	kfree(ctx.ap);
	return ret;
}
EXPORT_SYMBOL(sysfb_evict_conflicts_pci);
#endif /* CONFIG_PCI */
