/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/log.h"

#include "ftl_nvc_dev.h"
#include "utils/ftl_defs.h"

struct ftl_nv_cache_device_type_entry {
	const struct ftl_nv_cache_device_desc *desc;
	TAILQ_ENTRY(ftl_nv_cache_device_type_entry) entry;
};

static TAILQ_HEAD(, ftl_nv_cache_device_type_entry) g_devs = TAILQ_HEAD_INITIALIZER(g_devs);
static pthread_mutex_t g_devs_mutex = PTHREAD_MUTEX_INITIALIZER;

static const struct ftl_nv_cache_device_desc *
ftl_nv_cache_device_type_get_desc(const char *name)
{
	struct ftl_nv_cache_device_type_entry *entry = NULL;

	TAILQ_FOREACH(entry, &g_devs, entry) {
		if (0 == strcmp(entry->desc->name, name)) {
			return entry->desc;
		}
	}

	return NULL;
}

static bool
ftl_nv_cache_device_valid(const struct ftl_nv_cache_device_desc *desc)
{
	if (!desc || !desc->name) {
		return false;
	}

	if (!strlen(desc->name)) {
		return false;
	}

	return true;
}

void
ftl_nv_cache_device_register(const struct ftl_nv_cache_device_desc *desc)
{
	if (!ftl_nv_cache_device_valid(desc)) {
		SPDK_ERRLOG("[FTL] NV cache device descriptor is invalid invalid\n");
		ftl_abort();
	}

	pthread_mutex_lock(&g_devs_mutex);
	if (!ftl_nv_cache_device_type_get_desc(desc->name)) {
		struct ftl_nv_cache_device_type_entry *entry = calloc(1, sizeof(*entry));

		if (!entry) {
			SPDK_ERRLOG("[FTL] Cannot NV cache device, out of memory, name: %s\n", desc->name);
			ftl_abort();
		}

		entry->desc = desc;
		TAILQ_INSERT_TAIL(&g_devs, entry, entry);

		SPDK_NOTICELOG("[FTL] Registered NV cache device, name: %s\n", desc->name);
	} else {
		SPDK_ERRLOG("[FTL] Cannot register NV cache device, already exist, name: %s\n", desc->name);
		ftl_abort();
	}

	pthread_mutex_unlock(&g_devs_mutex);
}

const struct ftl_nv_cache_device_desc *
ftl_nv_cache_device_get_desc_by_bdev(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	pthread_mutex_lock(&g_devs_mutex);
	struct ftl_nv_cache_device_type_entry *entry;
	const struct ftl_nv_cache_device_desc *desc = NULL;

	TAILQ_FOREACH(entry, &g_devs, entry) {
		if (entry->desc->ops.is_bdev_compatible) {
			if (entry->desc->ops.is_bdev_compatible(dev, bdev)) {
				desc = entry->desc;
				break;
			}
		}
	}

	pthread_mutex_unlock(&g_devs_mutex);

	return desc;
}
