// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics Flexible Data Placement bridge.
 *
 * Copyright (c) 2026 Guixin Liu, Alibaba Group.
 * All rights reserved.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/blkdev.h>
#include <linux/nvme.h>

#include "../host/nvme.h"
#include "nvmet.h"

int nvmet_fdp_init_ns(struct nvmet_ns *ns)
{
	struct block_device *bdev = ns->bdev;
	struct nvme_ns *backend_ns;

	if (!bdev) {
		pr_err("the ns:%u has no block device for FDP\n", ns->nsid);
		return -EINVAL;
	}

	/* the endgid equals the nsid, which is 16 bit on the wire */
	if (ns->nsid > U16_MAX) {
		pr_err("the ns:%u cannot fit an endurance group identifier\n",
		       ns->nsid);
		return -EINVAL;
	}

	ns->fdp.nruh = bdev_max_write_streams(bdev);
	if (!ns->fdp.nruh || ns->fdp.nruh > U8_MAX) {
		pr_err("the ns:%u has no usable backing write streams: %u\n",
		       ns->nsid, ns->fdp.nruh);
		return -EINVAL;
	}

	ns->fdp.runs = bdev_write_stream_granularity(bdev);
	if (!ns->fdp.runs) {
		pr_err("the ns:%u has no backing write streams\n", ns->nsid);
		return -EINVAL;
	}

	/*
	 * The NVMe host driver is the only one that reports write streams
	 * today, so an FDP backing device is an NVMe namespace and the
	 * bridge needs its controller to forward the FDP commands to.
	 */
	if (bdev->bd_disk->fops != &nvme_bdev_ops) {
		pr_err("the ns:%u has no NVMe backing device for FDP\n",
		       ns->nsid);
		return -EINVAL;
	}

	backend_ns = bdev->bd_disk->private_data;
	ns->fdp.backend_q = backend_ns->queue;
	ns->fdp.backend_ctrl = backend_ns->ctrl;

	return 0;
}

void nvmet_fdp_exit_ns(struct nvmet_ns *ns)
{
	ns->fdp.backend_q = NULL;
	ns->fdp.backend_ctrl = NULL;
}
