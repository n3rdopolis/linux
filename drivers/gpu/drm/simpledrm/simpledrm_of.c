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
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include "simpledrm.h"

#ifdef CONFIG_COMMON_CLK

static int sdrm_of_bind_clocks(struct sdrm_device *sdrm,
			       struct device_node *np)
{
	struct clk *clock;
	size_t i, n;
	int r;

	n = of_clk_get_parent_count(np);
	if (n < 1)
		return 0;

	sdrm->clks = kcalloc(n, sizeof(*sdrm->clks), GFP_KERNEL);
	if (!sdrm->clks)
		return -ENOMEM;

	for (i = 0; i < n; ++i) {
		clock = of_clk_get(np, i);
		if (!IS_ERR(clock)) {
			sdrm->clks[sdrm->n_clks++] = clock;
		} else if (PTR_ERR(clock) == -EPROBE_DEFER) {
			r = -EPROBE_DEFER;
			goto error;
		} else {
			dev_err(sdrm->ddev->dev, "cannot find clock %zu: %ld\n",
				i, PTR_ERR(clock));
		}
	}

	for (i = 0; i < sdrm->n_clks; ++i) {
		if (!sdrm->clks[i])
			continue;

		r = clk_prepare_enable(sdrm->clks[i]);
		if (r < 0) {
			dev_err(sdrm->ddev->dev,
				"cannot find clock %zu: %d\n", i, r);
			clk_put(sdrm->clks[i]);
			sdrm->clks[i] = NULL;
		}
	}

	return 0;

error:
	for (i = 0; i < sdrm->n_clks; ++i)
		clk_put(sdrm->clks[i]);
	kfree(sdrm->clks);
	sdrm->clks = NULL;
	sdrm->n_clks = 0;
	return r;
}

static void sdrm_of_unbind_clocks(struct sdrm_device *sdrm)
{
	size_t i;

	for (i = 0; i < sdrm->n_clks; ++i) {
		clk_disable_unprepare(sdrm->clks[i]);
		clk_put(sdrm->clks[i]);
	}

	kfree(sdrm->clks);
	sdrm->clks = NULL;
	sdrm->n_clks = 0;
}

#else /* CONFIG_COMMON_CLK */

static int sdrm_of_bind_clocks(struct sdrm_device *sdrm,
			       struct device_node *np)
{
	return 0;
}

static void sdrm_of_unbind_clocks(struct sdrm_device *sdrm)
{
}

#endif /* CONFIG_COMMON_CLK */

#ifdef CONFIG_REGULATOR

static int sdrm_of_bind_regulators(struct sdrm_device *sdrm,
				   struct device_node *np)
{
	struct regulator *regulator;
	struct property *prop;
	char *p, *name;
	size_t i, n;
	int r;

	n = 0;
	for_each_property_of_node(np, prop) {
		p = strstr(prop->name, "-supply");
		if (p && p != prop->name)
			++n;
	}

	if (n < 1)
		return 0;

	sdrm->regulators = kcalloc(n, sizeof(*sdrm->regulators), GFP_KERNEL);
	if (!sdrm->regulators)
		return -ENOMEM;

	for_each_property_of_node(np, prop) {
		p = strstr(prop->name, "-supply");
		if (!p || p == prop->name)
			continue;

		name = kstrndup(prop->name, p - prop->name, GFP_TEMPORARY);
		if (!name)
			continue;

		regulator = regulator_get_optional(sdrm->ddev->dev, name);
		kfree(name);

		if (!IS_ERR(regulator)) {
			sdrm->regulators[sdrm->n_regulators++] = regulator;
		} else if (PTR_ERR(regulator) == -EPROBE_DEFER) {
			r = -EPROBE_DEFER;
			goto error;
		} else {
			dev_warn(sdrm->ddev->dev,
				 "cannot find regulator %s: %ld\n",
				 prop->name, PTR_ERR(regulator));
		}
	}

	for (i = 0; i < sdrm->n_regulators; ++i) {
		if (!sdrm->regulators[i])
			continue;

		r = regulator_enable(sdrm->regulators[i]);
		if (r < 0) {
			dev_warn(sdrm->ddev->dev,
				 "cannot enable regulator %zu: %d\n", i, r);
			regulator_put(sdrm->regulators[i]);
			sdrm->regulators[i] = NULL;
		}
	}

	return 0;

error:
	for (i = 0; i < sdrm->n_regulators; ++i)
		if (sdrm->regulators[i])
			regulator_put(sdrm->regulators[i]);
	kfree(sdrm->regulators);
	sdrm->regulators = NULL;
	sdrm->n_regulators = 0;
	return r;
}

static void sdrm_of_unbind_regulators(struct sdrm_device *sdrm)
{
	size_t i;

	for (i = 0; i < sdrm->n_regulators; ++i) {
		if (sdrm->regulators[i]) {
			regulator_disable(sdrm->regulators[i]);
			regulator_put(sdrm->regulators[i]);
		}
	}

	kfree(sdrm->regulators);
	sdrm->regulators = NULL;
	sdrm->n_regulators = 0;
}

#else /* CONFIG_REGULATORS */

static int sdrm_of_bind_regulators(struct sdrm_device *sdrm,
				   struct device_node *np)
{
	return 0;
}

static void sdrm_of_unbind_regulators(struct sdrm_device *sdrm)
{
}

#endif /* CONFIG_REGULATORS */

#ifdef CONFIG_OF

void sdrm_of_bootstrap(void)
{
#ifdef CONFIG_OF_ADDRESS
	struct device_node *np;

	for_each_compatible_node(np, NULL, "simple-framebuffer")
		of_platform_device_create(np, NULL, NULL);
#endif
}

int sdrm_of_bind(struct sdrm_device *sdrm)
{
	int r;

	if (WARN_ON(sdrm->n_clks > 0 || sdrm->n_regulators > 0))
		return 0;
	if (!sdrm->ddev->dev->of_node)
		return 0;

	r = sdrm_of_bind_clocks(sdrm, sdrm->ddev->dev->of_node);
	if (r < 0)
		goto error;

	r = sdrm_of_bind_regulators(sdrm, sdrm->ddev->dev->of_node);
	if (r < 0)
		goto error;

	return 0;

error:
	sdrm_of_unbind(sdrm);
	return r;
}

void sdrm_of_unbind(struct sdrm_device *sdrm)
{
	sdrm_of_unbind_regulators(sdrm);
	sdrm_of_unbind_clocks(sdrm);
}

#else /* CONFIG_OF */

void sdrm_of_bootstrap(void)
{
}

int sdrm_of_bind(struct sdrm_device *sdrm)
{
	return 0;
}

void sdrm_of_unbind(struct sdrm_device *sdrm)
{
}

#endif /* CONFIG_OF */
