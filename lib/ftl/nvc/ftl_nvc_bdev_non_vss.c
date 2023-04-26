/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "ftl_nv_cache.h"

static int
init(struct spdk_ftl_dev *dev)
{
	int rc;

	rc = ftl_p2l_log_init(dev);
	if (rc) {
		return 0;
	}

	return 0;
}

static void deinit(struct spdk_ftl_dev *dev)
{
	ftl_p2l_log_deinit(dev);
}

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (spdk_bdev_get_md_size(bdev) != 0) {
		/* Bdev's metadata is invalid size */
		return false;
	}

	if (ftl_md_xfer_blocks(dev) * spdk_bdev_get_md_size(bdev) > FTL_ZERO_BUFFER_SIZE) {
		FTL_ERRLOG(dev, "Zero buffer too small for bdev %s metadata transfer\n",
			   spdk_bdev_get_name(bdev));
		return false;
	}

	return true;
}

static void
tune_layout_region(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	/* This device doesn't support VSS, disable it */
	region->vss_blksz = 0;

	if (region->type >= FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN &&
	    region->type <= FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX) {
		region->current.blocks = ftl_p2l_log_get_md_blocks_required(dev, 1,
					 dev->layout.nvc.chunk_data_blocks);
	}
}

static void
p2l_log_cb(struct ftl_io *io)
{
}

static void
on_chunk_open(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk)
{
	assert(NULL == chunk->p2l_log);
	chunk->p2l_log = ftl_p2l_log_acquire(dev, chunk->md->seq_id, p2l_log_cb);
	chunk->md->p2l_log_type = ftl_p2l_log_type(chunk->p2l_log);
}

static void
on_chunk_closed(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->p2l_log);
	ftl_p2l_log_release(dev, chunk->p2l_log);
	chunk->p2l_log = NULL;
}

struct ftl_nv_cache_device_desc nvc_bdev_non_vss = {
	.name = "bdev-non-vss",
	.features = {
		.vss = false,
	},
	.ops = {
		.init = init,
		.deinit = deinit,
		.on_chunk_open = on_chunk_open,
		.on_chunk_closed = on_chunk_closed,
		.is_bdev_compatible = is_bdev_compatible,
		.tune_layout_region = tune_layout_region,
	}
};
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_non_vss)
