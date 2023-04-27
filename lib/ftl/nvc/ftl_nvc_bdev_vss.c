/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (!spdk_bdev_is_md_separate(bdev)) {
		/* It doesn't support separate metadata buffer IO */
		return false;
	}

	if (spdk_bdev_get_md_size(bdev) != sizeof(union ftl_md_vss)) {
		/* Bdev's metadata is invalid size */
		return false;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		/* Unsupported DIF type used by bdev */
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
ftl_nv_cache_submit_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;

	ftl_stats_bdev_io_completed(io->dev, FTL_STATS_TYPE_USER, bdev_io);

	spdk_bdev_free_io(bdev_io);

	ftl_mempool_put(nv_cache->md_pool, io->md);

	ftl_nv_cache_write_complete(io, success);
}

static void nvc_vss_write(struct ftl_io *io);

static void
_nvc_vss_write(void *io)
{
	nvc_vss_write(io);
}

static void
nvc_vss_write(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	int rc;

	io->md = ftl_mempool_get(dev->nv_cache.md_pool);
	if (spdk_unlikely(!io->md)) {
		ftl_abort();
	}

	ftl_nv_cache_fill_md(io);

	rc = spdk_bdev_writev_blocks_with_md(nv_cache->bdev_desc, nv_cache->cache_ioch,
					     io->iov, io->iov_cnt, io->md,
					     ftl_addr_to_nvc_offset(dev, io->addr), io->num_blocks,
					     ftl_nv_cache_submit_cb, io);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			ftl_mempool_put(nv_cache->md_pool, io->md);
			io->md = NULL;

			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			io->bdev_io_wait.bdev = bdev;
			io->bdev_io_wait.cb_fn = _nvc_vss_write;
			io->bdev_io_wait.cb_arg = io;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &io->bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

struct ftl_nv_cache_device_desc nvc_bdev_vss = {
	.name = "bdev",
	.features = {
		.vss = true,
	},
	.ops = {
		.is_bdev_compatible = is_bdev_compatible,
		.write = nvc_vss_write,
	}
};
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_vss)
