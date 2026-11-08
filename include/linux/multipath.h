// SPDX-License-Identifier: GPL-2.0-only
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/srcu.h>

extern const struct block_device_operations mpath_ops;

enum mpath_iopolicy_e {
	MPATH_IOPOLICY_NUMA,
	MPATH_IOPOLICY_RR,
	MPATH_IOPOLICY_QD,
};

enum mpath_access_state {
	MPATH_STATE_OPTIMIZED,
	MPATH_STATE_NONOPTIMIZED,
	MPATH_STATE_OTHER
};

#define MPATH_DEVICE_SYSFS_ATTR_LINK      0

struct mpath_device {
	struct mpath_head	*mpath_head;
	struct list_head	siblings;
	struct gendisk		*disk;
	unsigned long		flags;
	int			numa_node;
	atomic_t		*nr_active;
	enum mpath_access_state access_state;
};

struct mpath_head_template {
	bool (*available_path)(struct mpath_device *);
	void (*remove_head)(struct mpath_head *);
	bool (*is_disabled)(struct mpath_device *);
	bool (*is_optimized)(struct mpath_device *);
	struct bio *(*clone_bio)(struct bio *);
	const struct attribute_group **device_groups;
};

#define MPATH_HEAD_DISK_LIVE 			0
#define MPATH_HEAD_QUEUE_IF_NO_PATH		1

struct mpath_head {
	struct srcu_struct	srcu;
	struct list_head	dev_list;	/* list of all mpath_devs */
	struct mutex		lock;

	refcount_t		refcount;

	enum mpath_iopolicy_e	*iopolicy;

	struct bio_list		requeue_list; /* list for requeing bio */
	spinlock_t		requeue_lock;
	struct work_struct	requeue_work; /* work struct for requeue */

	atomic_long_t		requeue_no_usable_path_cnt;
	atomic_long_t		fail_no_avail_path_cnt;

	struct delayed_work	remove_work;
	unsigned int		delayed_removal_secs;
	struct module		*drv_module;

	unsigned long		flags;
	struct gendisk		*disk;
	struct work_struct	partition_scan_work;
	struct device		*parent;
	const struct attribute_group 		**disk_groups;
	const struct mpath_head_template	*mpdt;
	struct mpath_device __rcu 		*current_path[MAX_NUMNODES];
};

#define REQ_MPATH		REQ_DRV

static inline bool is_mpath_request(struct request *req)
{
	return req->cmd_flags & REQ_MPATH;
}

static inline struct mpath_head *mpath_bd_device_to_head(struct device *dev)
{
	return dev_get_drvdata(dev);
}

static inline struct mpath_head *mpath_gendisk_to_head(struct gendisk *disk)
{
	return mpath_bd_device_to_head(disk_to_dev(disk));
}

static inline enum mpath_iopolicy_e mpath_read_iopolicy(
			struct mpath_head *mpath_head)
{
	return READ_ONCE(*mpath_head->iopolicy);
}
void mpath_synchronize(struct mpath_head *mpath_head);
int mpath_set_iopolicy(const char *str, enum mpath_iopolicy_e *iopolicy);
int mpath_get_iopolicy(char *buf, int iopolicy);
bool mpath_clear_current_path(struct mpath_device *mpath_device);
int mpath_add_device(struct mpath_device *mpath_device, struct gendisk *disk,
		int numa_node, atomic_t *nr_active);
bool mpath_delete_device(struct mpath_device *mpath_device);
bool mpath_head_devices_empty(struct mpath_head *mpath_head);
void mpath_clear_paths(struct mpath_head *mpath_head);
void mpath_revalidate_paths(struct mpath_head *mpath_head);
void mpath_add_sysfs_link(struct mpath_head *mpath_head);
void mpath_remove_sysfs_link(struct mpath_device *mpath_device);
int mpath_get_head(struct mpath_head *mpath_head);
void mpath_put_head(struct mpath_head *mpath_head);
int mpath_head_init(struct mpath_head *mpath_head);
void mpath_head_uninit(struct mpath_head *mpath_head);

void mpath_put_disk(struct mpath_head *mpath_head);
void mpath_remove_disk(struct mpath_head *mpath_head);
int mpath_alloc_head_disk(struct mpath_head *mpath_head,
			struct queue_limits *lim, int numa_node);
void mpath_device_set_live(struct mpath_device *mpath_device);
bool mpath_can_remove_head(struct mpath_head *mpath_head);
ssize_t mpath_delayed_removal_secs_show(struct mpath_head *mpath_head,
			char *buf);
ssize_t mpath_delayed_removal_secs_store(struct mpath_head *mpath_head,
			const char *buf, size_t count);

static inline bool is_mpath_disk(struct gendisk *disk)
{
	#if IS_ENABLED(CONFIG_LIBMULTIPATH)
	return disk->fops == &mpath_ops;
	#else
	return false;
	#endif
}

static inline bool mpath_qd_iopolicy(enum mpath_iopolicy_e *iopolicy)
{
	return READ_ONCE(*iopolicy) == MPATH_IOPOLICY_QD;
}

static inline bool mpath_head_queue_if_no_path(struct mpath_head *mpath_head)
{
	if (test_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags))
		return true;
	return false;
}

static inline void mpath_schedule_requeue_work(struct mpath_head *mpath_head)
{
	kblockd_schedule_work(&mpath_head->requeue_work);
}
#endif // _LIBMULTIPATH_H
