/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "ftl_nv_cache.h"

static void write_io(struct ftl_io *io);
static void p2l_log_cb(struct ftl_io *io);

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

static void
p2l_log_cb(struct ftl_io *io)
{
	ftl_nv_cache_write_complete(io, true);
}

static void
write_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *ctx)
{
	struct ftl_io *io = ctx;

	ftl_stats_bdev_io_completed(io->dev, FTL_STATS_TYPE_USER, bdev_io);
	spdk_bdev_free_io(bdev_io);

	if (spdk_likely(success)) {
		struct ftl_p2l_log *log = io->nv_cache_chunk->p2l_log;
		ftl_p2l_log_io(log, io);
	} else {
		ftl_nv_cache_write_complete(io, false);
	}
}

static void
write_io_retry(void *ctx)
{
	struct ftl_io *io = ctx;
	write_io(io);
}

static void
write_io(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	int rc;

	rc = spdk_bdev_writev_blocks(nv_cache->bdev_desc, nv_cache->cache_ioch,
				     io->iov, io->iov_cnt,
				     ftl_addr_to_nvc_offset(dev, io->addr), io->num_blocks,
				     write_io_cb, io);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			io->bdev_io_wait.bdev = bdev;
			io->bdev_io_wait.cb_fn = write_io_retry;
			io->bdev_io_wait.cb_arg = io;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &io->bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

static void
process(struct spdk_ftl_dev *dev)
{
	ftl_p2l_log_flush(dev);
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
		.process = process,
		.write = write_io,
	}
};
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_non_vss)
