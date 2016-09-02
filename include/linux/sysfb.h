#ifndef __LINUX_SYSFB_H
#define __LINUX_SYSFB_H

/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>

struct apertures_struct;
struct pci_dev;

enum {
	SYSFB_EVICT_PLATFORM			= (1U <<  0),
	SYSFB_EVICT_FBDEV			= (1U <<  1),
	SYSFB_EVICT_VGACON			= (1U <<  2),
	SYSFB_EVICT_VBE				= (1U <<  3),
};

struct sysfb_evict_ctx {
	struct apertures_struct *ap;
	unsigned int flags;
};

int sysfb_evict_conflicts(struct sysfb_evict_ctx *ctx);
int sysfb_evict_conflicts_firmware(void);
int sysfb_evict_conflicts_pci(struct pci_dev *pdev);

#endif /* __LINUX_SYSFB_H */
