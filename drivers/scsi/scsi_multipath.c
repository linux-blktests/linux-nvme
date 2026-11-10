// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Oracle Corp
 *
 */

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_multipath.h>

#include "scsi_priv.h"

#define TPGS_MODE_IMPLICIT		0x1

enum {
	SCSI_MULTIPATH_OFF,
	SCSI_MULTIPATH_ON,
	SCSI_MULTIPATH_ALWAYS,
};

static const char *scsi_multipath_modes[] = {
	[SCSI_MULTIPATH_OFF]	= "off",
	[SCSI_MULTIPATH_ON]	= "on",
	[SCSI_MULTIPATH_ALWAYS]	= "always",
};

static int scsi_multipath = SCSI_MULTIPATH_OFF;

static LIST_HEAD(scsi_mpath_heads_list);
static DEFINE_MUTEX(scsi_mpath_heads_lock);
static DEFINE_IDA(scsi_multipath_dev_ida);

static int scsi_multipath_param_set(const char *val, const struct kernel_param *kp)
{
	int mode;

	if (!val)
		return -EINVAL;
	mode = __sysfs_match_string(scsi_multipath_modes,
		ARRAY_SIZE(scsi_multipath_modes), val);

	if (mode < 0)
		return mode;
	scsi_multipath = mode;
	return 0;
}

static int scsi_multipath_param_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", scsi_multipath_modes[scsi_multipath]);
}

static const struct kernel_param_ops multipath_param_ops = {
	.set = scsi_multipath_param_set,
	.get = scsi_multipath_param_get,
};

module_param_cb(multipath, &multipath_param_ops, &scsi_multipath, 0444);
MODULE_PARM_DESC(multipath, "turn on native multipath support, options: on, off, always");

static enum mpath_iopolicy_e iopolicy = MPATH_IOPOLICY_NUMA;

static int scsi_mpath_set_iopolicy_param(const char *val, const struct kernel_param *kp)
{
	return mpath_set_iopolicy(val, &iopolicy);
}

static int scsi_mpath_get_iopolicy_param(char *buf, const struct kernel_param *kp)
{
	return mpath_get_iopolicy(buf, iopolicy);
}

module_param_call(multipath_iopolicy, scsi_mpath_set_iopolicy_param,
		scsi_mpath_get_iopolicy_param, &iopolicy, 0644);
MODULE_PARM_DESC(multipath_iopolicy,
	"Default multipath I/O policy; 'numa' (default), 'round-robin' or 'queue-depth'");

static int scsi_mpath_unique_lun_id(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	int ret;

	ret = scsi_vpd_lun_id(sdev, scsi_mpath_dev->device_id_str,
				SCSI_MPATH_DEVICE_ID_LEN);
	if (ret < 0)
		return ret;
	else if (ret == 0)
		return -EINVAL;
	else if (ret > SCSI_MPATH_DEVICE_ID_LEN)
		return -EINVAL;

	return 0;
}

static void scsi_mpath_head_release(struct device *dev)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);
	struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;

	bioset_exit(&scsi_mpath_head->bio_pool);
	ida_free(&scsi_multipath_dev_ida, scsi_mpath_head->index);
	ida_destroy(&scsi_mpath_head->ida);
	mpath_head_uninit(mpath_head);
	kfree(scsi_mpath_head);
}

static ssize_t scsi_mpath_device_vpd_id_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);

	return sysfs_emit(buf, "%s\n", scsi_mpath_head->vpd_id);
}
static DEVICE_ATTR(vpd_id, S_IRUGO, scsi_mpath_device_vpd_id_show, NULL);

static ssize_t scsi_mpath_device_iopolicy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);
	struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;

	if (!mpath_iopolicy_store(&scsi_mpath_head->iopolicy, buf))
		return -EINVAL;

	mpath_clear_paths(mpath_head);
	mpath_schedule_requeue_work(mpath_head);
	/*
	 * Ensure requeue work completes, as this work could run later when
	 * the mpath_head is gone.
	 */
	flush_work(&mpath_head->requeue_work);
	return count;
}

static ssize_t scsi_mpath_device_iopolicy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(dev, struct scsi_mpath_head, dev);

	return mpath_iopolicy_show(&scsi_mpath_head->iopolicy, buf);
}

static DEVICE_ATTR(iopolicy, S_IRUGO | S_IWUSR,
		scsi_mpath_device_iopolicy_show, scsi_mpath_device_iopolicy_store);

static struct attribute *scsi_mpath_device_attrs[] = {
	&dev_attr_vpd_id.attr,
	&dev_attr_iopolicy.attr,
	NULL
};

static const struct attribute_group scsi_mpath_device_attrs_group = {
	.attrs = scsi_mpath_device_attrs,
};

static bool scsi_multipath_sysfs_group_visible(struct kobject *kobj)
{
	return true;
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(scsi_multipath_sysfs)

static struct attribute dummy_attr = {
	.name = "dummy",
};

static struct attribute *scsi_mpath_attrs[] = {
	&dummy_attr,
	NULL
};

static const struct attribute_group scsi_mpath_attr_group = {
	.name           = "multipath",
	.attrs		= scsi_mpath_attrs,
	.is_visible     = SYSFS_GROUP_VISIBLE(scsi_multipath_sysfs),
};

static const struct attribute_group *scsi_mpath_device_groups[] = {
	&scsi_mpath_device_attrs_group,
	&scsi_mpath_attr_group,
	NULL
};

void scsi_mpath_add_sysfs_link(struct scsi_device *sdev)
{
	struct device *target = &sdev->sdev_gendev;
	struct scsi_mpath_head *scsi_mpath_head =
		sdev->scsi_mpath_dev->scsi_mpath_head;
	struct device *source = &scsi_mpath_head->dev;
	int error;

	error = sysfs_add_link_to_group(&source->kobj, "multipath",
			&target->kobj, dev_name(target));
	if (error) {
		sdev_printk(KERN_INFO, sdev, "Failed to create mpath sysfs link, error=%d\n",
				    error);
	}
}

void scsi_mpath_remove_sysfs_link(struct scsi_device *sdev)
{
	struct device *target = &sdev->sdev_gendev;
	struct scsi_mpath_head *scsi_mpath_head =
		sdev->scsi_mpath_dev->scsi_mpath_head;
	struct device *source = &scsi_mpath_head->dev;

	sysfs_remove_link_from_group(&source->kobj, "multipath",
		dev_name(target));
}

static const struct class scsi_mpath_device_class = {
	.name = "scsi_mpath_device",
	.dev_groups = scsi_mpath_device_groups,
	.dev_release = scsi_mpath_head_release,
};

static int scsi_multipath_sdev_init(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_device *scsi_mpath_dev;
	struct mpath_device *mpath_device;

	scsi_mpath_dev = kzalloc(sizeof(*scsi_mpath_dev), GFP_KERNEL);
	if (!scsi_mpath_dev)
		return -ENOMEM;
	scsi_mpath_dev->sdev = sdev;
	sdev->scsi_mpath_dev = scsi_mpath_dev;

	mpath_device = &scsi_mpath_dev->mpath_device;
	mpath_device->numa_node = dev_to_node(shost->dma_dev);
	mpath_device->access_state = MPATH_STATE_OPTIMIZED;

	return 0;
}

void scsi_mpath_revalidate_paths(struct scsi_mpath_device *scsi_mpath_dev)
{
       struct scsi_mpath_head *scsi_mpath_head = scsi_mpath_dev->scsi_mpath_head;
       struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;

       mpath_revalidate_paths(mpath_head);
}

void scsi_mpath_dev_clear_path(struct scsi_mpath_device *scsi_mpath_dev)
{
       struct mpath_device *mpath_device = &scsi_mpath_dev->mpath_device;
       struct scsi_mpath_head *scsi_mpath_head = scsi_mpath_dev->scsi_mpath_head;
       struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;

       if (mpath_clear_current_path(mpath_device))
               mpath_synchronize(mpath_head);
}

static void scsi_mpath_clone_end_io(struct bio *clone)
{
	struct bio *master_bio = clone->bi_private;

	if (clone->bi_status && blk_path_error(clone->bi_status)) {
		struct mpath_head *mpath_head =
				master_bio->bi_bdev->bd_disk->private_data;
		unsigned long flags;

		spin_lock_irqsave(&mpath_head->requeue_lock, flags);
		bio_list_add(&mpath_head->requeue_list, master_bio);
		spin_unlock_irqrestore(&mpath_head->requeue_lock, flags);
		bio_put(clone);

		mpath_schedule_requeue_work(mpath_head);
		return;
	}

	master_bio->bi_status = clone->bi_status;
	bio_put(clone);
	bio_endio(master_bio);
}

static struct bio *scsi_mpath_clone_bio(struct bio *bio)
{
	struct mpath_head *mpath_head = bio->bi_bdev->bd_disk->private_data;
	struct scsi_mpath_head *scsi_mpath_head = to_scsi_mpath_head(mpath_head);
	struct bio *clone;

	clone = bio_alloc_clone(bio->bi_bdev, bio, GFP_NOIO,
				&scsi_mpath_head->bio_pool);
	if (!clone)
		return NULL;

	clone->bi_end_io = scsi_mpath_clone_end_io;
	clone->bi_private = bio;

	return clone;
}

static bool scsi_mpath_is_disabled(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	unsigned char access_state = READ_ONCE(sdev->access_state) &
					SCSI_ACCESS_STATE_MASK;

	if (READ_ONCE(sdev->sdev_state) != SDEV_RUNNING)
		return true;

	if (access_state == SCSI_ACCESS_STATE_OPTIMAL ||
	    access_state == SCSI_ACCESS_STATE_ACTIVE)
		return false;

	return true;
}

static bool scsi_mpath_is_optimized(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	unsigned char access_state = READ_ONCE(sdev->access_state) &
					SCSI_ACCESS_STATE_MASK;

	if (READ_ONCE(sdev->sdev_state) != SDEV_RUNNING)
		return false;

	return access_state == SCSI_ACCESS_STATE_OPTIMAL;
}

static bool scsi_mpath_available_path(struct mpath_device *mpath_device)
{
	struct scsi_mpath_device *scsi_mpath_dev =
				to_scsi_mpath_device(mpath_device);
	struct scsi_device *sdev = scsi_mpath_dev->sdev;
	enum scsi_device_state sdev_state = READ_ONCE(sdev->sdev_state);

	if (sdev_state == SDEV_RUNNING || sdev_state == SDEV_QUIESCE ||
	    sdev_state == SDEV_BLOCK || sdev_state == SDEV_CREATED_BLOCK)
		return true;

	return false;
}

static int scsi_mpath_remove_head_drv(struct device_driver *drv, void *data)
{
	struct scsi_mpath_head *scsi_mpath_head = data;
	struct scsi_driver *scsi_driver = to_scsi_driver(drv);

	if (scsi_driver->mpath_remove_head)
		scsi_driver->mpath_remove_head(scsi_mpath_head);

	return 0;
}

static void scsi_mpath_remove_head_work(struct mpath_head *mpath_head)
{
	struct scsi_mpath_head *scsi_mpath_head = to_scsi_mpath_head(mpath_head);

	/*
	 * scsi_mpath_remove_head_drv() could drop the final reference to
	 * scsi_mpath_head, meaning that we pass a free'd pointer further
	 * drivers - take a reference so that won't happen.
	 */
	scsi_mpath_get_head(scsi_mpath_head);
	bus_for_each_drv(&scsi_bus_type, NULL, scsi_mpath_head,
		scsi_mpath_remove_head_drv);
	scsi_mpath_put_head(scsi_mpath_head);
}

static struct mpath_head_template smpdt = {
	.remove_head = scsi_mpath_remove_head_work,
	.is_disabled = scsi_mpath_is_disabled,
	.is_optimized = scsi_mpath_is_optimized,
	.available_path = scsi_mpath_available_path,
	.clone_bio = scsi_mpath_clone_bio,
};

static struct scsi_mpath_head *scsi_mpath_alloc_head(char *vpd_id)
{
	struct scsi_mpath_head *scsi_mpath_head;
	int ret;

	scsi_mpath_head = kzalloc(sizeof(*scsi_mpath_head), GFP_KERNEL);
	if (!scsi_mpath_head)
		return NULL;

	ida_init(&scsi_mpath_head->ida);

	if (mpath_head_init(&scsi_mpath_head->mpath_head))
		goto out_free;
	scsi_mpath_head->mpath_head.mpdt = &smpdt;
	scsi_mpath_head->iopolicy = iopolicy;
	scsi_mpath_head->mpath_head.iopolicy = &scsi_mpath_head->iopolicy;

	strscpy(scsi_mpath_head->vpd_id, vpd_id,
		SCSI_MPATH_DEVICE_ID_LEN);

	scsi_mpath_head->index = ida_alloc(&scsi_multipath_dev_ida, GFP_KERNEL);
	if (scsi_mpath_head->index < 0)
		goto out_uninit_head;
	if (bioset_init(&scsi_mpath_head->bio_pool, BIO_POOL_SIZE,
			0, BIOSET_PERCPU_CACHE))
		goto out_ida_free;
	kref_init(&scsi_mpath_head->ref);

	device_initialize(&scsi_mpath_head->dev);
	scsi_mpath_head->dev.class = &scsi_mpath_device_class;
	ret = dev_set_name(&scsi_mpath_head->dev, "scsi_mpath_device%d",
				scsi_mpath_head->index);
	if (ret) {
		put_device(&scsi_mpath_head->dev);
		return NULL;
	}

	ret = device_add(&scsi_mpath_head->dev);
	if (ret) {
		put_device(&scsi_mpath_head->dev);
		return NULL;
	}

	return scsi_mpath_head;

out_ida_free:
	ida_free(&scsi_multipath_dev_ida, scsi_mpath_head->index);
out_uninit_head:
	mpath_head_uninit(&scsi_mpath_head->mpath_head);
out_free:
	kfree(scsi_mpath_head);
	return NULL;
}

static struct scsi_mpath_head *scsi_mpath_find_head(
			struct scsi_mpath_device *scsi_mpath_dev)
{
	struct scsi_mpath_head *scsi_mpath_head;

	list_for_each_entry(scsi_mpath_head, &scsi_mpath_heads_list, entry) {
		if (strncmp(scsi_mpath_head->vpd_id,
			scsi_mpath_dev->device_id_str,
			SCSI_MPATH_DEVICE_ID_LEN) == 0) {
			if (scsi_mpath_try_get_head(scsi_mpath_head))
				continue;
			return scsi_mpath_head;
		}
	}

	return NULL;
}

static void scsi_multipath_sdev_uninit(struct scsi_device *sdev)
{
	kfree(sdev->scsi_mpath_dev);
	sdev->scsi_mpath_dev = NULL;
}

int scsi_mpath_dev_alloc(struct scsi_device *sdev)
{
	struct scsi_mpath_head *scsi_mpath_head;
	int ret;

	if (scsi_multipath == SCSI_MULTIPATH_OFF)
		return 0;

	if (!(scsi_device_tpgs(sdev) & TPGS_MODE_IMPLICIT) &&
	    (scsi_multipath != SCSI_MULTIPATH_ALWAYS)) {
		sdev_printk(KERN_DEBUG, sdev, "IMPLICIT TPGS are required for multipath support\n");
		return 0;
	}

	ret = scsi_multipath_sdev_init(sdev);
	if (ret)
		return ret;

	ret = scsi_mpath_unique_lun_id(sdev);
	if (ret < 0)
		goto out_uninit;

	mutex_lock(&scsi_mpath_heads_lock);
	scsi_mpath_head = scsi_mpath_find_head(sdev->scsi_mpath_dev);
	if (scsi_mpath_head)
		goto found;
	scsi_mpath_head =
		scsi_mpath_alloc_head(sdev->scsi_mpath_dev->device_id_str);
	if (!scsi_mpath_head) {
		sdev_printk(KERN_NOTICE, sdev, "could not allocate multipath head, device multipathing disabled\n");
		mutex_unlock(&scsi_mpath_heads_lock);
		goto out_uninit;
	}

	list_add_tail(&scsi_mpath_head->entry, &scsi_mpath_heads_list);
found:
	mutex_unlock(&scsi_mpath_heads_lock);
	ret = ida_alloc(&scsi_mpath_head->ida, GFP_KERNEL);
	if (ret < 0)
		goto out_put_head;
	sdev->scsi_mpath_dev->index = ret;
	sdev->scsi_mpath_dev->scsi_mpath_head = scsi_mpath_head;
	sdev->scsi_mpath_dev->mpath_device.mpath_head =
				&scsi_mpath_head->mpath_head;
	return 0;
out_put_head:
	scsi_mpath_put_head(scsi_mpath_head);
out_uninit:
	scsi_multipath_sdev_uninit(sdev);
	return 0;
}

static void scsi_mpath_remove_head(struct scsi_mpath_device *scsi_mpath_dev)
{
	scsi_mpath_put_head(scsi_mpath_dev->scsi_mpath_head);
	scsi_mpath_dev->scsi_mpath_head = NULL;
}

void scsi_mpath_remove_device(struct scsi_mpath_device *scsi_mpath_dev)
{
	struct scsi_mpath_head *scsi_mpath_head = scsi_mpath_dev->scsi_mpath_head;

	ida_free(&scsi_mpath_head->ida, scsi_mpath_dev->index);

	scsi_mpath_remove_head(scsi_mpath_dev);
}

void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;

	if (!scsi_mpath_dev)
		return;

	scsi_multipath_sdev_uninit(sdev);
}

void scsi_mpath_get_head(struct scsi_mpath_head *scsi_mpath_head)
{
	kref_get(&scsi_mpath_head->ref);
}
EXPORT_SYMBOL_GPL(scsi_mpath_get_head);

int scsi_mpath_try_get_head(struct scsi_mpath_head *scsi_mpath_head)
{
	if (!kref_get_unless_zero(&scsi_mpath_head->ref))
		return -ENXIO;
	return 0;
}
EXPORT_SYMBOL_GPL(scsi_mpath_try_get_head);

static void scsi_mpath_free_head(struct kref *ref)
{
	struct scsi_mpath_head *scsi_mpath_head =
		container_of(ref, struct scsi_mpath_head, ref);

	/*
	 * If we race with scsi_mpath_find_head(), then that function may
	 * find this scsi_mpath_head in the heads list; however we would fail
	 * to take a reference to this scsi_mpath_head and continue the search.
	 * As such, it is safe to call device_unregister (and free
	 * scsi_mpath_head) after we delete this head from the list.
	 */
	mutex_lock(&scsi_mpath_heads_lock);
	list_del_init(&scsi_mpath_head->entry);
	mutex_unlock(&scsi_mpath_heads_lock);

	device_unregister(&scsi_mpath_head->dev);
}

void scsi_mpath_put_head(struct scsi_mpath_head *scsi_mpath_head)
{
	kref_put(&scsi_mpath_head->ref, scsi_mpath_free_head);
}
EXPORT_SYMBOL_GPL(scsi_mpath_put_head);

void scsi_mpath_start_request(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct scsi_mpath_device *scsi_mpath_dev = sdev->scsi_mpath_dev;
	struct scsi_mpath_head *scsi_mpath_head =
				scsi_mpath_dev->scsi_mpath_head;
	struct bio *clone = req->bio, *master;
	struct mpath_head *mpath_head = &scsi_mpath_head->mpath_head;
	struct gendisk *disk = mpath_head->disk;

	if (mpath_qd_iopolicy(&scsi_mpath_head->iopolicy) &&
	    !(scmd->flags & SCMD_MPATH_CNT_ACTIVE)) {
		struct Scsi_Host *shost = sdev->host;

		atomic_inc(&shost->mpath_nr_active);
		scmd->flags |= SCMD_MPATH_CNT_ACTIVE;
	}

	if (blk_rq_is_passthrough(req) || !blk_queue_io_stat(disk->queue))
		return;

	if (!clone) {
		scmd->flags &= ~SCMD_MPATH_IO_STATS;
		return;
	}

	if (scmd->flags & SCMD_MPATH_IO_STATS)
		return;

	master = clone->bi_private;

	scmd->flags |= SCMD_MPATH_IO_STATS;
	scmd->start_time = bdev_start_io_acct(master->bi_bdev, req_op(req),
				jiffies);
	scmd->start_bytes = blk_rq_bytes(req);
}

bool scsi_mpath_end_request(struct request *req, blk_status_t error,
				unsigned int nr_bytes)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct block_device *bi_bdev = NULL;

	if (scmd->flags & SCMD_MPATH_IO_STATS) {
		struct bio *clone = req->bio, *master = clone->bi_private;

		bi_bdev = master->bi_bdev;
	}

	if (blk_update_request(req, error, nr_bytes))
		return true;

	if (scmd->flags & SCMD_MPATH_CNT_ACTIVE) {
		struct Scsi_Host *shost = sdev->host;

		atomic_dec_if_positive(&shost->mpath_nr_active);
	}

	if (!(scmd->flags & SCMD_MPATH_IO_STATS))
		return false;
	bdev_end_io_acct(bi_bdev, req_op(req),
			 scmd->start_bytes >> SECTOR_SHIFT,
			 scmd->start_time);

	return false;
}

void scsi_mpath_end_request_no_update(struct request *req, blk_status_t error)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct bio *clone = req->bio, *master = clone->bi_private;
	struct block_device *bi_bdev = master->bi_bdev;
	unsigned int nr_bytes;

	/* Don't generate error logs if we are going to try another path */
	if (blk_path_error(error))
		req->rq_flags |= RQF_QUIET;

	if (scmd->flags & SCMD_MPATH_CNT_ACTIVE) {
		struct Scsi_Host *shost = sdev->host;

		atomic_dec_if_positive(&shost->mpath_nr_active);
	}

	if (!(scmd->flags & SCMD_MPATH_IO_STATS))
		return;

	/*
	 * blk_rq_bytes() value should never exceed start_bytes, but check in
	 * case.
	 */
	if (scmd->start_bytes > blk_rq_bytes(req))
		nr_bytes = scmd->start_bytes - blk_rq_bytes(req);
	else
		nr_bytes = 0;

	bdev_end_io_acct(bi_bdev, req_op(req),
			 nr_bytes >> SECTOR_SHIFT,
			 scmd->start_time);
}

int __init scsi_multipath_init(void)
{
	return class_register(&scsi_mpath_device_class);
}

void __exit scsi_multipath_exit(void)
{
	ida_destroy(&scsi_multipath_dev_ida);
	class_unregister(&scsi_mpath_device_class);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_multipath");
