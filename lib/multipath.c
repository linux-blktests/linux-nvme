// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 *
 * Base on NVMe multipath support in drivers/nvme/host/multipath.c
 */
#include <linux/module.h>
#include <linux/multipath.h>
#include <linux/wait_bit.h>
#include <trace/events/block.h>

static struct mpath_device *mpath_find_path(struct mpath_head *mpath_head);

static struct workqueue_struct *mpath_wq;

static const char * const mpath_iopolicy_names[] = {
	[MPATH_IOPOLICY_NUMA]	= "numa",
	[MPATH_IOPOLICY_RR]	= "round-robin",
	[MPATH_IOPOLICY_QD]	= "queue-depth",
};

static int mpath_iopolicy_parse(const char *str)
{
	return __sysfs_match_string(mpath_iopolicy_names,
		ARRAY_SIZE(mpath_iopolicy_names), str);
}

int mpath_set_iopolicy(const char *str, enum mpath_iopolicy_e *iopolicy)
{
	int policy;

	if (!str)
		return -EINVAL;
	policy = mpath_iopolicy_parse(str);
	if (policy < 0)
		return policy;
	WRITE_ONCE(*iopolicy, policy);

	return 0;
}
EXPORT_SYMBOL_GPL(mpath_set_iopolicy);

int mpath_get_iopolicy(char *buf, int iopolicy)
{
	return sprintf(buf, "%s\n", mpath_iopolicy_names[iopolicy]);
}
EXPORT_SYMBOL_GPL(mpath_get_iopolicy);

void mpath_synchronize(struct mpath_head *mpath_head)
{
	synchronize_srcu(&mpath_head->srcu);
}
EXPORT_SYMBOL_GPL(mpath_synchronize);

int mpath_add_device(struct mpath_device *mpath_device, struct gendisk *disk,
		int numa_node, atomic_t *nr_active)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;

	if (!disk || !nr_active)
		return -EINVAL;

	mpath_device->disk = disk;
	mpath_device->numa_node = numa_node;
	mpath_device->nr_active = nr_active;
	mutex_lock(&mpath_head->lock);
	list_add_tail_rcu(&mpath_device->siblings, &mpath_head->dev_list);
	mutex_unlock(&mpath_head->lock);

	if (cancel_delayed_work(&mpath_head->remove_work))
		module_put(mpath_head->drv_module);

	return 0;
}
EXPORT_SYMBOL_GPL(mpath_add_device);

bool mpath_delete_device(struct mpath_device *mpath_device)
{
	bool empty;

	mutex_lock(&mpath_device->mpath_head->lock);
	list_del_rcu(&mpath_device->siblings);
	empty = list_empty(&mpath_device->mpath_head->dev_list);
	mutex_unlock(&mpath_device->mpath_head->lock);

	mpath_synchronize(mpath_device->mpath_head);

	return empty;
}
EXPORT_SYMBOL_GPL(mpath_delete_device);

bool mpath_head_devices_empty(struct mpath_head *mpath_head)
{
	bool empty;

	mutex_lock(&mpath_head->lock);
	empty = list_empty(&mpath_head->dev_list);
	mutex_unlock(&mpath_head->lock);

	return empty;
}
EXPORT_SYMBOL_GPL(mpath_head_devices_empty);

bool mpath_clear_current_path(struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_device->mpath_head;
	bool changed = false;
	int node;

	for_each_node(node) {
		if (mpath_device ==
			rcu_access_pointer(mpath_head->current_path[node])) {
			rcu_assign_pointer(mpath_head->current_path[node],
				NULL);
			changed = true;
		}
	}

	return changed;
}
EXPORT_SYMBOL_GPL(mpath_clear_current_path);

void mpath_clear_paths(struct mpath_head *mpath_head)
{
	int node;

	for_each_node(node)
		rcu_assign_pointer(mpath_head->current_path[node], NULL);
}
EXPORT_SYMBOL_GPL(mpath_clear_paths);

void mpath_revalidate_paths(struct mpath_head *mpath_head)
{
	mpath_clear_paths(mpath_head);
	mpath_schedule_requeue_work(mpath_head);
}
EXPORT_SYMBOL_GPL(mpath_revalidate_paths);

static bool mpath_path_is_disabled(struct mpath_head *mpath_head,
				struct mpath_device *mpath_device)
{
	return mpath_head->mpdt->is_disabled(mpath_device);
}

static struct mpath_device *__mpath_find_path(struct mpath_head *mpath_head,
					int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	struct mpath_device *found = NULL, *fallback = NULL, *mpath_device;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
		srcu_read_lock_held(&mpath_head->srcu)) {
		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;

		if (mpath_device->numa_node != NUMA_NO_NODE &&
		    (mpath_read_iopolicy(mpath_head) ==
			MPATH_IOPOLICY_NUMA))
			distance = node_distance(node,
					mpath_device->numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(mpath_device->access_state) {
		case MPATH_STATE_OPTIMIZED:
		    if (distance < found_distance) {
			    found_distance = distance;
			    found = mpath_device;
		    }
		    break;
		case MPATH_STATE_NONOPTIMIZED:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    fallback = mpath_device;
		    }
		    break;
		default:
		    break;
		}
	}

	if (!found)
		found = fallback;

	if (found)
		rcu_assign_pointer(mpath_head->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_next_dev(struct mpath_head *mpath_head,
				struct mpath_device *mpath_dev)
{
	mpath_dev = list_next_or_null_rcu(&mpath_head->dev_list,
			&mpath_dev->siblings, struct mpath_device,
			siblings);

	if (mpath_dev)
		return mpath_dev;
	return list_first_or_null_rcu(&mpath_head->dev_list,
				struct mpath_device, siblings);
}

static struct mpath_device *mpath_round_robin_path(
				struct mpath_head *mpath_head)
{
	struct mpath_device *mpath_device, *found = NULL;
	int node = numa_node_id();
	enum mpath_access_state access_state_old;
	struct mpath_device *old =
			srcu_dereference(mpath_head->current_path[node],
				&mpath_head->srcu);

	if (unlikely(!old))
		return __mpath_find_path(mpath_head, node);

	if (list_is_singular(&mpath_head->dev_list)) {
		if (mpath_path_is_disabled(mpath_head, old))
			return NULL;
		return old;
	}

	for (mpath_device = mpath_next_dev(mpath_head, old);
	    mpath_device && mpath_device != old;
	    mpath_device = mpath_next_dev(mpath_head, mpath_device)) {

		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;
		if (mpath_device->access_state == MPATH_STATE_OPTIMIZED) {
			found = mpath_device;
			goto out;
		}
		if (mpath_device->access_state == MPATH_STATE_NONOPTIMIZED)
			found = mpath_device;
	}

	/*
	 * The loop above skips the current path for round-robin semantics.
	 * Fall back to the current path if either:
	 *  - no other optimized path found and current is optimized,
	 *  - no other usable path found and current is usable.
	 */
	access_state_old = old->access_state;
	if (!mpath_path_is_disabled(mpath_head, old) &&
	    (access_state_old == MPATH_STATE_OPTIMIZED ||
	    (!found && access_state_old == MPATH_STATE_NONOPTIMIZED)))
		return old;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_head->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_queue_depth_path(
				struct mpath_head *mpath_head)
{
	struct mpath_device *best_opt = NULL, *mpath_device;
	struct mpath_device *best_nonopt = NULL;
	unsigned int min_depth_opt = UINT_MAX, min_depth_nonopt = UINT_MAX;
	unsigned int depth;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {

		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;

		depth = atomic_read(mpath_device->nr_active);

		switch (mpath_device->access_state) {
		case MPATH_STATE_OPTIMIZED:
			if (depth < min_depth_opt) {
				min_depth_opt = depth;
				best_opt = mpath_device;
			}
			break;
		case MPATH_STATE_NONOPTIMIZED:
			if (depth < min_depth_nonopt) {
				min_depth_nonopt = depth;
				best_nonopt = mpath_device;
			}
			break;
		default:
			break;
		}

		if (min_depth_opt == 0)
			return best_opt;
	}

	return best_opt ? best_opt : best_nonopt;
}

static inline bool mpath_path_is_optimized(struct mpath_head *mpath_head,
				struct mpath_device *mpath_device)
{
	return mpath_head->mpdt->is_optimized(mpath_device);
}

static struct mpath_device *mpath_numa_path(struct mpath_head *mpath_head)
{
	int node = numa_node_id();
	struct mpath_device *mpath_device;

	mpath_device = srcu_dereference(mpath_head->current_path[node],
					&mpath_head->srcu);
	if (unlikely(!mpath_device))
		return __mpath_find_path(mpath_head, node);
	if (unlikely(mpath_path_is_disabled(mpath_head, mpath_device)))
		return __mpath_find_path(mpath_head, node);
	if (unlikely(!mpath_path_is_optimized(mpath_head, mpath_device)))
		return __mpath_find_path(mpath_head, node);
	return mpath_device;
}

static struct mpath_device *mpath_find_path(struct mpath_head *mpath_head)
{
	enum mpath_iopolicy_e iopolicy = mpath_read_iopolicy(mpath_head);

	switch (iopolicy) {
	case MPATH_IOPOLICY_QD:
		return mpath_queue_depth_path(mpath_head);
	case MPATH_IOPOLICY_RR:
		return mpath_round_robin_path(mpath_head);
	default:
		return mpath_numa_path(mpath_head);
	}
}

static bool mpath_available_path(struct mpath_head *mpath_head)
{
	struct mpath_device *mpath_device;

	if (!test_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags))
		return false;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (mpath_head->mpdt->available_path(mpath_device))
			return true;
	}

	/*
	 * If "mpath_head->delayed_removal_secs" is set (i.e., non-zero), do
	 * not immediately fail I/O. Instead, requeue the I/O for the configured
	 * duration, anticipating that if there's a transient link failure then
	 * it may recover within this time window. This parameter is exported to
	 * userspace via sysfs, and its default value is zero. It is internally
	 * mapped to MPATH_HEAD_QUEUE_IF_NO_PATH. When delayed_removal_secs is
	 * non-zero, this flag is set to true. When zero, the flag is cleared.
	 */
	return mpath_head_queue_if_no_path(mpath_head);
}

static void mpath_bdev_submit_bio(struct bio *bio)
{
	struct mpath_head *mpath_head = bio->bi_bdev->bd_disk->private_data;
	struct device *dev = mpath_head->parent;
	struct mpath_device *mpath_device;
	int srcu_idx;

	/*
	 * The mpath_device might be going away and the bio might be moved to a
	 * different queue in failover, so we need to use the bio_split
	 * pool from the original queue to allocate the bvecs from.
	 */
	bio = bio_split_to_limits(bio);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (likely(mpath_device)) {
		if (mpath_head->mpdt->clone_bio) {
			struct bio *orig = bio;

			bio = mpath_head->mpdt->clone_bio(bio);
			if (!bio) {
				bio_io_error(orig);
				goto out;
			}
		}

		bio_set_dev(bio, mpath_device->disk->part0);
		/*
		 * Use BIO_REMAPPED to skip bio_check_eod() when this bio
		 * enters submit_bio_noacct() for the per-path device. The EOD
		 * check already passed on the multipath head.
		 */
		bio_set_flag(bio, BIO_REMAPPED);
		bio->bi_opf |= REQ_MPATH;
		trace_block_bio_remap(bio, disk_devt(mpath_head->disk),
				      bio->bi_iter.bi_sector);
		submit_bio_noacct(bio);
	} else if (mpath_available_path(mpath_head)) {
		dev_warn_ratelimited(dev, "no usable path - requeuing I/O\n");

		spin_lock_irq(&mpath_head->requeue_lock);
		bio_list_add(&mpath_head->requeue_list, bio);
		spin_unlock_irq(&mpath_head->requeue_lock);
		atomic_long_inc(&mpath_head->requeue_no_usable_path_cnt);
	} else {
		dev_warn_ratelimited(dev, "no available path - failing I/O\n");

		bio_io_error(bio);
		atomic_long_inc(&mpath_head->fail_no_avail_path_cnt);
	}

out:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}

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
	.submit_bio	= mpath_bdev_submit_bio,
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

static void mpath_requeue_work(struct work_struct *work)
{
	struct mpath_head *mpath_head =
	    container_of(work, struct mpath_head, requeue_work);
	struct bio *bio, *next;

	spin_lock_irq(&mpath_head->requeue_lock);
	next = bio_list_get(&mpath_head->requeue_list);
	spin_unlock_irq(&mpath_head->requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}

bool mpath_can_remove_head(struct mpath_head *mpath_head)
{
	unsigned long delay;
	bool remove = false;

	mutex_lock(&mpath_head->lock);
	/*
	 * Ensure that no one could remove this module while the head
	 * remove work is pending.
	 */
	if (mpath_head_queue_if_no_path(mpath_head) &&
	    !check_mul_overflow(mpath_head->delayed_removal_secs, HZ, &delay) &&
	    try_module_get(mpath_head->drv_module)) {
		mod_delayed_work(mpath_wq, &mpath_head->remove_work, delay);
	} else {
		remove = true;
	}

	mutex_unlock(&mpath_head->lock);
	return remove;
}
EXPORT_SYMBOL_GPL(mpath_can_remove_head);

static void mpath_remove_head_work(struct work_struct *work)
{
	struct mpath_head *mpath_head = container_of(to_delayed_work(work),
			struct mpath_head, remove_work);
	struct module *drv_module = mpath_head->drv_module;

	mpath_head->mpdt->remove_head(mpath_head);
	module_put(drv_module);
}

void mpath_remove_disk(struct mpath_head *mpath_head)
{
	if (test_and_clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		struct gendisk *disk = mpath_head->disk;

		/*
		 * requeue I/O after MPATH_HEAD_DISK_LIVE has been cleared
		 * to allow multipath to fail all I/O.
		 */
		mpath_synchronize(mpath_head);
		mpath_schedule_requeue_work(mpath_head);

		del_gendisk(disk);
	}
}
EXPORT_SYMBOL_GPL(mpath_remove_disk);

void mpath_put_disk(struct mpath_head *mpath_head)
{
	if (!mpath_head->disk)
		return;

	/* make sure all pending bios are cleaned up */
	kblockd_schedule_work(&mpath_head->requeue_work);
	flush_work(&mpath_head->requeue_work);
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

	INIT_DELAYED_WORK(&mpath_head->remove_work, mpath_remove_head_work);
	mpath_head->delayed_removal_secs = 0;

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

	mpath_add_sysfs_link(mpath_head);

	mutex_lock(&mpath_head->lock);
	if (mpath_path_is_optimized(mpath_head, mpath_device)) {
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_head->srcu);
		for_each_online_node(node)
			__mpath_find_path(mpath_head, node);
		srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	}
	mutex_unlock(&mpath_head->lock);

	mpath_synchronize(mpath_head);
	mpath_schedule_requeue_work(mpath_head);
}
EXPORT_SYMBOL_GPL(mpath_device_set_live);

ssize_t mpath_delayed_removal_secs_show(struct mpath_head *mpath_head,
					char *buf)
{
	int ret;

	mutex_lock(&mpath_head->lock);
	ret = sysfs_emit(buf, "%u\n", mpath_head->delayed_removal_secs);
	mutex_unlock(&mpath_head->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mpath_delayed_removal_secs_show);

ssize_t mpath_delayed_removal_secs_store(struct mpath_head *mpath_head,
			const char *buf, size_t count)
{
	unsigned int sec;
	ssize_t ret;

	ret = kstrtouint(buf, 0, &sec);
	if (ret < 0)
		return ret;

	mutex_lock(&mpath_head->lock);
	mpath_head->delayed_removal_secs = sec;
	if (sec)
		set_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags);
	else
		clear_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags);
	mutex_unlock(&mpath_head->lock);

	/*
	 * Ensure that update to MPATH_HEAD_QUEUE_IF_NO_PATH is seen
	 * by its reader.
	 */
	mpath_synchronize(mpath_head);

	return count;
}
EXPORT_SYMBOL_GPL(mpath_delayed_removal_secs_store);

void mpath_add_sysfs_link(struct mpath_head *mpath_head)
{
	struct device *target;
	struct device *source;
	int rc, srcu_idx;
	struct kobject *mpath_gd_kobj;
	struct mpath_device *mpath_device;

	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &mpath_head->disk->state))
		return;

	mpath_gd_kobj = &disk_to_dev(mpath_head->disk)->kobj;
	srcu_idx = srcu_read_lock(&mpath_head->srcu);

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (!test_bit(GD_ADDED, &mpath_device->disk->state))
			continue;

		if (test_and_set_bit(MPATH_DEVICE_SYSFS_ATTR_LINK,
					&mpath_device->flags))
			continue;

		target = disk_to_dev(mpath_device->disk);
		source = disk_to_dev(mpath_head->disk);
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(mpath_gd_kobj, "multipath",
				&target->kobj, dev_name(target));

		if (unlikely(rc)) {
			dev_err(disk_to_dev(mpath_head->disk),
					"failed to create link to %s rc=%d\n",
					dev_name(target), rc);
			clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK,
					&mpath_device->flags);
		} else {
			dev_info(source, "Created multipath sysfs link to %s\n",
					mpath_device->disk->disk_name);
		}
	}

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}
EXPORT_SYMBOL_GPL(mpath_add_sysfs_link);

void mpath_remove_sysfs_link(struct mpath_device *mpath_device)
{
	struct device *target;
	struct kobject *mpath_gd_kobj;
	struct mpath_head *mpath_head = mpath_device->mpath_head;

	if (!test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
		return;

	target = disk_to_dev(mpath_device->disk);
	mpath_gd_kobj = &disk_to_dev(mpath_head->disk)->kobj;

	sysfs_remove_link_from_group(mpath_gd_kobj, "multipath",
			dev_name(target));

	clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
}
EXPORT_SYMBOL_GPL(mpath_remove_sysfs_link);

int mpath_head_init(struct mpath_head *mpath_head)
{
	memset(mpath_head, 0, sizeof(*mpath_head));
	INIT_LIST_HEAD(&mpath_head->dev_list);
	mutex_init(&mpath_head->lock);
	refcount_set(&mpath_head->refcount, 1);

	INIT_WORK(&mpath_head->partition_scan_work,
		multipath_partition_scan_work);
	INIT_WORK(&mpath_head->requeue_work, mpath_requeue_work);
	spin_lock_init(&mpath_head->requeue_lock);
	bio_list_init(&mpath_head->requeue_list);

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
