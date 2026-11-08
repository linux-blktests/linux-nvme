// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 *
 * Base on NVMe multipath support in drivers/nvme/host/multipath.c
 */
#include <linux/module.h>
#include <linux/multipath.h>
#include <linux/wait_bit.h>

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

__maybe_unused
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

		mpath_synchronize(mpath_head);
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
