/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _GOOGLE_ICC_H
#define _GOOGLE_ICC_H

#include <linux/interconnect-provider.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#include "google_icc_internal.h"
#include "google_irm.h"

#define to_google_provider(_provider) \
	container_of(_provider, struct google_icc_provider, provider)

#define MAX_LINKS 8

enum {
	TYPE_NORMAL = 0,
	TYPE_SOURCE,
	TYPE_GMC,
	TYPE_GSLC,
	TYPE_QBOX,
};

struct google_icc_provider {
	struct icc_provider provider;
	struct device *dev;
	struct irm_dev *irm_dev;
};

struct google_icc_node {
	const char *name;
	u16 type;
	u16 links[MAX_LINKS];
	u16 id;
	u16 num_links;
	u32 attr;
	struct icc_vote vote;
};

#define DEFINE_GNODE(_name, _id, _type, _attr, ...)                \
	static struct google_icc_node _name = {                    \
		.id = _id,                                         \
		.name = #_name,                                    \
		.type = _type,                                     \
		.attr = _attr,                                     \
		.num_links = ARRAY_SIZE(((int[]){ __VA_ARGS__ })), \
		.links = { __VA_ARGS__ },                          \
	}

struct google_icc_desc {
	struct google_icc_node *const *nodes;
	size_t num_nodes;
};

int google_icc_platform_probe(struct platform_device *pdev);
int google_icc_platform_remove(struct platform_device *pdev);

#endif /* _GOOGLE_ICC_H */
