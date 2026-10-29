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

/* Flexible Data Placement feature (FID 1Dh), Command Dword 12 */
#define NVME_FDP_CDW12_FDPE			(1U << 0)
#define NVME_FDP_CDW12_FDPCIDX_MASK		(0xffU << 8)

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


/*
 * Each namespace is its own endurance group, so the Endurance Group
 * Identifier in Command Dword 11, and in the Log Specific Identifier
 * of the FDP log pages, directly names the namespace.  Returns the
 * namespace with a reference held.
 */
static struct nvmet_ns *nvmet_fdp_lookup_ns(struct nvmet_req *req,
		u32 endgid, bool check_fdp)
{
	struct nvmet_subsys *subsys = nvmet_req_subsys(req);
	struct nvmet_ns *ns;

	rcu_read_lock();
	ns = xa_load(&subsys->namespaces, endgid);
	if (!ns || !test_bit(NVMET_NS_IO_LIVE, &ns->flags) ||
	    (check_fdp && !nvmet_fdp_enabled(ns)))
		goto out_unlock;

	if (!percpu_ref_tryget_live_rcu(&ns->ref))
		goto out_unlock;

	rcu_read_unlock();
	return ns;

out_unlock:
	rcu_read_unlock();
	return NULL;
}

/*
 * The bridge runs the one configuration that the backing device is
 * already in, so setting the feature to that same value is the only
 * transition that can be served.  The spec aborts any other value
 * change of a feature whose endurance group has namespaces with
 * Command Sequence Error, which also covers disabling: pulling FDP
 * from under a live namespace would strand the write streams already
 * handed out to hosts.
 */
u16 nvmet_set_feat_fdp(struct nvmet_req *req)
{
	u32 cdw12 = le32_to_cpu(req->cmd->features.dword12);
	struct nvmet_ns *ns;
	u16 status;

	ns = nvmet_fdp_lookup_ns(req,
				 le32_to_cpu(req->cmd->features.dword11),
				 false);
	if (!ns) {
		req->error_loc = offsetof(struct nvme_features, dword11);
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	}

	if (cdw12 & ~(NVME_FDP_CDW12_FDPCIDX_MASK | NVME_FDP_CDW12_FDPE)) {
		req->error_loc = offsetof(struct nvme_features, dword12);
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	} else if (!nvmet_fdp_enabled(ns)) {
		/*
		 * A namespace without FDP reports FDPE clear, so setting
		 * that same disabled configuration is a no-op; enabling
		 * it is a configuration change the bridge cannot serve.
		 */
		if (cdw12) {
			status = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
		} else {
			nvmet_set_result(req, 0);
			status = 0;
		}
	} else if ((cdw12 & NVME_FDP_CDW12_FDPE) &&
		   !(cdw12 & NVME_FDP_CDW12_FDPCIDX_MASK)) {
		nvmet_set_result(req, FDPCFG_FDPE);
		status = 0;
	} else {
		status = NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
	}

	percpu_ref_put(&ns->ref);
	return status;
}

u16 nvmet_get_feat_fdp(struct nvmet_req *req)
{
	struct nvmet_ns *ns;

	ns = nvmet_fdp_lookup_ns(req,
				 le32_to_cpu(req->cmd->features.dword11),
				 false);
	if (!ns) {
		req->error_loc = offsetof(struct nvme_features, dword11);
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	}

	/*
	 * The controller may serve namespaces without FDP next to bridged
	 * ones; report FDP as disabled for those instead of failing the
	 * Get Features command the host issues for every namespace.
	 */
	if (nvmet_fdp_enabled(ns))
		/* the namespace is up, so FDP is enabled on the backing device */
		nvmet_set_result(req, FDPCFG_FDPE);
	else
		nvmet_set_result(req, 0);

	percpu_ref_put(&ns->ref);
	return 0;
}
