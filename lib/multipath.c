// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 *
 * Base on NVMe multipath support in drivers/nvme/host/multipath.c
 */
#include <linux/module.h>
#include <linux/multipath.h>
#include <linux/wait_bit.h>

static struct workqueue_struct *mpath_wq;

int mpath_get_head(struct mpath_head *mpath_head)
{
	if (!refcount_inc_not_zero(&mpath_head->refcount))
		return -ENXIO;
	return 0;
}
EXPORT_SYMBOL_GPL(mpath_get_head);

void mpath_put_head(struct mpath_head *mpath_head)
{
	refcount_t *refcount = &mpath_head->refcount;

	if (refcount_dec_and_test(&mpath_head->refcount))
		wake_up_var(refcount);
}
EXPORT_SYMBOL_GPL(mpath_put_head);

void mpath_head_uninit(struct mpath_head *mpath_head)
{
	refcount_t *refcount = &mpath_head->refcount;

	if (!refcount_dec_and_test(refcount))
		wait_var_event(refcount, !refcount_read(refcount));
	cleanup_srcu_struct(&mpath_head->srcu);
}
EXPORT_SYMBOL_GPL(mpath_head_uninit);

static int mpath_bdev_open(struct gendisk *disk, blk_mode_t mode)
{
	struct mpath_head *mpath_head = disk->private_data;

	return mpath_get_head(mpath_head);
}

static void mpath_bdev_release(struct gendisk *disk)
{
	struct mpath_head *mpath_head = disk->private_data;

	mpath_put_head(mpath_head);
}

const struct block_device_operations mpath_ops = {
	.owner          = THIS_MODULE,
	.open		= mpath_bdev_open,
	.release	= mpath_bdev_release,
};
EXPORT_SYMBOL_GPL(mpath_ops);

static void multipath_partition_scan_work(struct work_struct *work)
{
	struct mpath_head *mpath_head =
		container_of(work, struct mpath_head, partition_scan_work);

	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_head->disk->state)))
		return;

	mutex_lock(&mpath_head->disk->open_mutex);
	bdev_disk_changed(mpath_head->disk, false);
	mutex_unlock(&mpath_head->disk->open_mutex);
}

void mpath_remove_disk(struct mpath_head *mpath_head)
{
	if (test_and_clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		struct gendisk *disk = mpath_head->disk;

		del_gendisk(disk);
	}
}
EXPORT_SYMBOL_GPL(mpath_remove_disk);

void mpath_put_disk(struct mpath_head *mpath_head)
{
	if (!mpath_head->disk)
		return;

	/* make sure all pending bios are cleaned up */
	flush_work(&mpath_head->partition_scan_work);
	put_disk(mpath_head->disk);
	mpath_head->disk = NULL;
}
EXPORT_SYMBOL_GPL(mpath_put_disk);

int mpath_alloc_head_disk(struct mpath_head *mpath_head,
			struct queue_limits *lim, int numa_node)
{
	struct gendisk *disk;

	if (mpath_head->disk)
		return -EINVAL;

	disk = blk_alloc_disk(lim, numa_node);
	if (IS_ERR(disk))
		return PTR_ERR(disk);

	mpath_head->disk = disk;
	mpath_head->disk->private_data = mpath_head;
	mpath_head->disk->fops = &mpath_ops;

	set_bit(GD_SUPPRESS_PART_SCAN, &mpath_head->disk->state);

	return 0;
}
EXPORT_SYMBOL_GPL(mpath_alloc_head_disk);

void mpath_device_set_live(struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	int ret;

	if (!mpath_head->disk)
		return;

	if (!test_and_set_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		dev_set_drvdata(disk_to_dev(mpath_head->disk), mpath_head);
		ret = device_add_disk(mpath_head->parent, mpath_head->disk,
				mpath_head->disk_groups);
		if (ret) {
			clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags);
			return;
		}
		queue_work(mpath_wq, &mpath_head->partition_scan_work);
	}
}
EXPORT_SYMBOL_GPL(mpath_device_set_live);

int mpath_head_init(struct mpath_head *mpath_head)
{
	memset(mpath_head, 0, sizeof(*mpath_head));
	INIT_LIST_HEAD(&mpath_head->dev_list);
	mutex_init(&mpath_head->lock);
	refcount_set(&mpath_head->refcount, 1);

	INIT_WORK(&mpath_head->partition_scan_work,
		multipath_partition_scan_work);

	return init_srcu_struct(&mpath_head->srcu);
}
EXPORT_SYMBOL_GPL(mpath_head_init);

static int __init mpath_init(void)
{
	mpath_wq = alloc_workqueue("mpath-wq",
			WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);
	if (!mpath_wq)
		return -ENOMEM;
	return 0;
}

static void __exit mpath_exit(void)
{
	destroy_workqueue(mpath_wq);
}

module_init(mpath_init);
module_exit(mpath_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("libmultipath");
