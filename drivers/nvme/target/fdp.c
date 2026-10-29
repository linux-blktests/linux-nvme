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
#include <linux/sizes.h>

#include "../host/nvme.h"
#include "nvmet.h"

/* Flexible Data Placement feature (FID 1Dh), Command Dword 12 */
#define NVME_FDP_CDW12_FDPE			(1U << 0)
#define NVME_FDP_CDW12_FDPCIDX_MASK		(0xffU << 8)

/*
 * Commands and log pages are staged through a kernel buffer, so bound
 * the host requested transfer size.  The defined responses are small:
 * even a RUH Status with 255 handles is only about 8 KiB.
 */
#define NVMET_FDP_MAX_TRANSFER_SIZE		SZ_1M

static int nvmet_fdp_init_backend(struct nvmet_ns *ns);

int nvmet_fdp_init_ns(struct nvmet_ns *ns)
{
	struct block_device *bdev = ns->bdev;
	struct nvme_ns *backend_ns;
	int ret;

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

	ret = nvmet_fdp_init_backend(ns);
	if (ret)
		nvmet_fdp_exit_ns(ns);

	return ret;
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

struct nvmet_fdp_work {
	struct work_struct work;
	struct nvmet_req *req;
	void (*execute)(struct nvmet_req *req);
};

static void nvmet_fdp_work_fn(struct work_struct *work)
{
	struct nvmet_fdp_work *w = container_of(work, struct nvmet_fdp_work,
						work);

	w->execute(w->req);
	kfree(w);
}

/*
 * lpfc can invoke the execute callbacks in hardirq context, and the FDP
 * handlers allocate memory and wait for backing commands to complete.
 * Defer them to a workqueue, which also keeps the other transports
 * unchanged in behavior.
 */
static void nvmet_fdp_defer(struct nvmet_req *req,
			    void (*execute)(struct nvmet_req *req))
{
	struct nvmet_fdp_work *w;

	w = kmalloc_obj(*w, GFP_ATOMIC);
	if (!w) {
		nvmet_req_complete(req, NVME_SC_INTERNAL);
		return;
	}
	INIT_WORK(&w->work, nvmet_fdp_work_fn);
	w->req = req;
	w->execute = execute;
	queue_work(nvmet_wq, &w->work);
}

static void nvmet_fdp_io_mgmt_recv_work(struct nvmet_req *req)
{
	size_t len = ((u64)le32_to_cpu(req->cmd->imr.numd) + 1) << 2;
	u16 status;

	status = nvmet_fdp_io_mgmt_check(req);
	if (status)
		goto out;

	/* NUMD is 32 bit, do not let the length arithmetic wrap */
	if (len > NVMET_FDP_MAX_TRANSFER_SIZE) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto out;
	}

	if (!nvmet_check_transfer_len(req, len))
		return;

	if (req->cmd->imr.mo != NVME_IO_MGMT_RECV_MO_RUHS) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto out;
	}

	status = nvmet_fdp_io_mgmt_forward(req, true);
out:
	nvmet_req_complete(req, status);
}

void nvmet_execute_io_mgmt_recv(struct nvmet_req *req)
{
	nvmet_fdp_defer(req, nvmet_fdp_io_mgmt_recv_work);
}

static void nvmet_fdp_io_mgmt_send_work(struct nvmet_req *req)
{
	u16 status;

	status = nvmet_fdp_io_mgmt_check(req);
	if (status)
		goto out;

	if (!nvmet_check_transfer_len(req,
			(le16_to_cpu(req->cmd->ims.mos) + 1) * sizeof(__le16)))
		return;

	if (req->cmd->ims.mo != NVME_IO_MGMT_SEND_MO_RUH_UPDATE) {
		status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
		goto out;
	}

	status = nvmet_fdp_io_mgmt_forward(req, false);
out:
	nvmet_req_complete(req, status);
}

void nvmet_execute_io_mgmt_send(struct nvmet_req *req)
{
	nvmet_fdp_defer(req, nvmet_fdp_io_mgmt_send_work);
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

static int nvmet_fdp_backend_configs(struct nvmet_ns *ns,
				     struct nvme_fdp_config_log *log,
				     size_t len)
{
	u32 numd = nvme_bytes_to_numd(len);
	struct nvme_command c = {};

	c.get_log_page.opcode = nvme_admin_get_log_page;
	c.get_log_page.nsid = cpu_to_le32(0);
	c.get_log_page.lid = NVME_LOG_FDP_CONFIGS;
	c.get_log_page.lsi = cpu_to_le16(ns->fdp.backend_endgid);
	c.get_log_page.numdl = cpu_to_le16(numd);
	c.get_log_page.numdu = cpu_to_le16(numd >> 16);

	return nvme_submit_sync_cmd(ns->fdp.backend_ctrl->admin_q, &c, log,
				    len);
}

static int nvmet_fdp_backend_get_feat(struct nvmet_ns *ns,
				      struct nvme_fdp_config *fdp)
{
	return nvme_get_features(ns->fdp.backend_ctrl, NVME_FEAT_FDP,
				 ns->fdp.backend_endgid, NULL, 0, fdp);
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
	struct nvme_fdp_config_desc *desc;
	struct nvme_fdp_config_log *log;
	struct nvme_fdp_ruh_status *st;
	struct nvme_fdp_config fdp;
	struct nvme_id_ns *id;
	size_t dsze, total;
	int ret, i;

	id = kzalloc_obj(*id, GFP_KERNEL);
	if (!id)
		return -ENOMEM;

	ret = nvmet_fdp_backend_identify(ns, id);
	if (ret) {
		pr_err("the ns:%u backing identify failed: %d\n",
		       ns->nsid, ret);
		ret = ret < 0 ? ret : -EIO;
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
		ret = ret < 0 ? ret : -EIO;
		goto free_st;
	}

	if (le16_to_cpu(st->nruhsd) != ns->fdp.nruh) {
		pr_err("the ns:%u backing handle count mismatch\n", ns->nsid);
		ret = -EINVAL;
		goto free_st;
	}

	for (i = 0; i < ns->fdp.nruh; i++) {
		if (le16_to_cpu(st->ruhsd[i].pid) != i) {
			pr_err("the ns:%u backing pids are not identity mapped\n",
			       ns->nsid);
			ret = -EINVAL;
			goto free_st;
		}
	}

	/*
	 * Take the configuration the host is presented with from the
	 * backing device itself: the nominal reclaim unit size is 64 bit
	 * there, and the handle types must not be invented.  The active
	 * configuration is selected by FDPCIDX, not by descriptor order.
	 */
	ret = nvmet_fdp_backend_get_feat(ns, &fdp);
	if (ret) {
		pr_err("the ns:%u backing get FDP feature failed: %d\n",
		       ns->nsid, ret);
		ret = ret < 0 ? ret : -EIO;
		goto free_st;
	}
	if (!(fdp.flags & FDPCFG_FDPE)) {
		pr_err("the ns:%u backing FDP is not enabled\n", ns->nsid);
		ret = -EINVAL;
		goto free_st;
	}

	log = kzalloc(sizeof(*log), GFP_KERNEL);
	if (!log) {
		ret = -ENOMEM;
		goto free_st;
	}
	ret = nvmet_fdp_backend_configs(ns, log, sizeof(*log));
	if (ret) {
		pr_err("the ns:%u backing FDP configurations failed: %d\n",
		       ns->nsid, ret);
		ret = ret < 0 ? ret : -EIO;
		goto free_log;
	}

	total = le32_to_cpu(log->sze);
	kfree(log);
	if (total < sizeof(*log) + sizeof(*desc) ||
	    total > NVMET_FDP_MAX_TRANSFER_SIZE) {
		ret = -EINVAL;
		goto free_st;
	}
	log = kzalloc(total, GFP_KERNEL);
	if (!log) {
		ret = -ENOMEM;
		goto free_st;
	}
	ret = nvmet_fdp_backend_configs(ns, log, total);
	if (ret) {
		pr_err("the ns:%u backing FDP configurations failed: %d\n",
		       ns->nsid, ret);
		ret = ret < 0 ? ret : -EIO;
		goto free_log;
	}

	desc = (void *)(log + 1);
	for (i = 0; i < fdp.fdpcidx; i++) {
		if ((void *)desc + sizeof(*desc) > (void *)log + total) {
			ret = -EINVAL;
			goto free_log;
		}
		if ((void *)desc + le16_to_cpu(desc->dsze) >
		    (void *)log + total) {
			ret = -EINVAL;
			goto free_log;
		}
		desc = (void *)desc + le16_to_cpu(desc->dsze);
	}

	if ((void *)desc + sizeof(*desc) > (void *)log + total) {
		ret = -EINVAL;
		goto free_log;
	}

	dsze = ALIGN(struct_size(desc, ruhs, ns->fdp.nruh), 8);
	if ((void *)desc + dsze > (void *)log + total) {
		ret = -EINVAL;
		goto free_log;
	}
	if (le16_to_cpu(desc->dsze) < dsze ||
	    le16_to_cpu(desc->nruh) != ns->fdp.nruh) {
		pr_err("the ns:%u backing FDP configuration is inconsistent\n",
		       ns->nsid);
		ret = -EINVAL;
		goto free_log;
	}

	ns->fdp.runs = le64_to_cpu(desc->runs);
	for (i = 0; i < ns->fdp.nruh; i++)
		ns->fdp.ruht[i] = desc->ruhs[i].ruht;

free_log:
	kfree(log);
free_st:
	kfree(st);
free_id:
	kfree(id);
	return ret;
}
