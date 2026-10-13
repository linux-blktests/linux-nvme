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

bool nvmet_fdp_subsys_configured(struct nvmet_subsys *subsys)
{
	struct nvmet_ns *ns;
	unsigned long idx;
	bool ret = false;

	mutex_lock(&subsys->lock);
	nvmet_for_each_enabled_ns(&subsys->namespaces, idx, ns) {
		if (nvmet_fdp_enabled(ns)) {
			ret = true;
			break;
		}
	}
	mutex_unlock(&subsys->lock);

	return ret;
}

static int nvmet_fdp_init_backend(struct nvmet_ns *ns);

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
	ns->fdp.backend_nsid = backend_ns->head->ns_id;

	return nvmet_fdp_init_backend(ns);
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
static struct nvmet_ns *nvmet_fdp_lookup_ns(struct nvmet_req *req, u32 endgid)
{
	struct nvmet_subsys *subsys = nvmet_req_subsys(req);
	struct nvmet_ns *ns;

	ns = xa_load(&subsys->namespaces, endgid);
	if (!ns || !ns->enabled || !nvmet_fdp_enabled(ns))
		return NULL;

	if (!percpu_ref_tryget_live(&ns->ref))
		return NULL;

	return ns;
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
				 le32_to_cpu(req->cmd->features.dword11));
	if (!ns)
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;

	if (cdw12 & ~(NVME_FDP_CDW12_FDPCIDX_MASK | NVME_FDP_CDW12_FDPE)) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
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
				 le32_to_cpu(req->cmd->features.dword11));
	if (!ns)
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;

	/* the namespace is up, so FDP is enabled on the backing device */
	nvmet_set_result(req, FDPCFG_FDPE);

	percpu_ref_put(&ns->ref);
	return 0;
}


static u16 nvmet_fdp_io_mgmt_check(struct nvmet_req *req)
{
	if (!nvmet_fdp_enabled(req->ns))
		return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;

	return 0;
}

/*
 * The backend submission path takes a linear kernel buffer, so the
 * request data is staged through one.
 */
static u16 nvmet_fdp_io_mgmt_forward(struct nvmet_req *req, bool recv)
{
	struct nvme_command c = {};
	u16 status = 0;
	void *buf;
	int ret;

	buf = kzalloc(req->transfer_len, GFP_KERNEL);
	if (!buf)
		return NVME_SC_INTERNAL;

	if (!recv) {
		status = nvmet_copy_from_sgl(req, 0, buf, req->transfer_len);
		if (status)
			goto free;
	}

	c.common = req->cmd->common;
	c.common.nsid = cpu_to_le32(req->ns->fdp.backend_nsid);
	ret = nvme_submit_sync_cmd(req->ns->fdp.backend_q, &c, buf,
				   req->transfer_len);
	if (ret) {
		status = ret > 0 ? ret : NVME_SC_INTERNAL;
		goto free;
	}

	if (recv)
		status = nvmet_copy_to_sgl(req, 0, buf, req->transfer_len);
free:
	kfree(buf);
	return status;
}

void nvmet_execute_io_mgmt_recv(struct nvmet_req *req)
{
	u16 status;

	status = nvmet_fdp_io_mgmt_check(req);
	if (status)
		goto out;

	if (req->cmd->imr.mo != NVME_IO_MGMT_RECV_MO_RUHS) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto out;
	}

	status = nvmet_fdp_io_mgmt_forward(req, true);
out:
	nvmet_req_complete(req, status);
}

void nvmet_execute_io_mgmt_send(struct nvmet_req *req)
{
	u16 status;

	status = nvmet_fdp_io_mgmt_check(req);
	if (status)
		goto out;

	if (req->cmd->ims.mo != NVME_IO_MGMT_SEND_MO_RUH_UPDATE) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto out;
	}

	status = nvmet_fdp_io_mgmt_forward(req, false);
out:
	nvmet_req_complete(req, status);
}

static int nvmet_fdp_backend_identify(struct nvmet_ns *ns,
		struct nvme_id_ns *id)
{
	struct nvme_command c = {};

	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = cpu_to_le32(ns->fdp.backend_nsid);
	c.identify.cns = NVME_ID_CNS_NS;

	return nvme_submit_sync_cmd(ns->fdp.backend_ctrl->admin_q, &c, id,
				    sizeof(*id));
}

static int nvmet_fdp_backend_ruhs(struct nvmet_ns *ns,
				  struct nvme_fdp_ruh_status *st)
{
	size_t len = struct_size(st, ruhsd, ns->fdp.nruh);
	struct nvme_command c = {};

	c.imr.opcode = nvme_cmd_io_mgmt_recv;
	c.imr.nsid = cpu_to_le32(ns->fdp.backend_nsid);
	c.imr.mo = NVME_IO_MGMT_RECV_MO_RUHS;
	c.imr.numd = cpu_to_le32(nvme_bytes_to_numd(len));

	return nvme_submit_sync_cmd(ns->fdp.backend_q, &c, st, len);
}

/*
 * Learn the FDP geometry of the backing NVMe namespace: the
 * Endurance Group Identifier its FDP state is addressed by, and the
 * placement identifier list of the Reclaim Unit Handle Status.  The
 * placement translation relies on the identity mapping, so the
 * enable fails when the backing geometry deviates from it.
 */
static int nvmet_fdp_init_backend(struct nvmet_ns *ns)
{
	struct nvme_fdp_ruh_status *st;
	struct nvme_id_ns *id;
	int ret, i;

	id = kzalloc_obj(*id, GFP_KERNEL);
	if (!id)
		return -ENOMEM;

	ret = nvmet_fdp_backend_identify(ns, id);
	if (ret) {
		pr_err("the ns:%u backing identify failed: %d\n",
		       ns->nsid, ret);
		goto free_id;
	}

	ns->fdp.backend_endgid = le16_to_cpu(id->endgid);

	st = kzalloc(struct_size(st, ruhsd, ns->fdp.nruh), GFP_KERNEL);
	if (!st) {
		ret = -ENOMEM;
		goto free_id;
	}

	ret = nvmet_fdp_backend_ruhs(ns, st);
	if (ret) {
		pr_err("the ns:%u backing reclaim unit handle status failed: %d\n",
		       ns->nsid, ret);
		goto free_st;
	}

	if (le16_to_cpu(st->nruhsd) < ns->fdp.nruh) {
		pr_err("the ns:%u backing reports fewer handles than write streams\n",
		       ns->nsid);
		ret = -EINVAL;
		goto free_st;
	}

	for (i = 0; i < ns->fdp.nruh; i++) {
		if (le16_to_cpu(st->ruhsd[i].pid) != i) {
			pr_err("the ns:%u backing placement identifiers are not identity mapped\n",
			       ns->nsid);
			ret = -EINVAL;
			goto free_st;
		}
	}

free_st:
	kfree(st);
free_id:
	kfree(id);
	return ret;
}

