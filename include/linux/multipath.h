// SPDX-License-Identifier: GPL-2.0-only
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/blkdev.h>
#include <linux/srcu.h>

extern const struct block_device_operations mpath_ops;

struct mpath_device {
	struct mpath_head	*mpath_head;
	struct list_head	siblings;
	struct gendisk		*disk;
};

struct mpath_head_template {
};

#define MPATH_HEAD_DISK_LIVE 			0

struct mpath_head {
	struct srcu_struct	srcu;
	struct list_head	dev_list;	/* list of all mpath_devs */
	struct mutex		lock;

	refcount_t		refcount;

	unsigned long		flags;
	struct gendisk		*disk;
	struct work_struct	partition_scan_work;
	struct device		*parent;
	const struct attribute_group 		**disk_groups;
	const struct mpath_head_template	*mpdt;
	struct mpath_device __rcu 		*current_path[MAX_NUMNODES];
};

static inline struct mpath_head *mpath_bd_device_to_head(struct device *dev)
{
	return dev_get_drvdata(dev);
}

static inline struct mpath_head *mpath_gendisk_to_head(struct gendisk *disk)
{
	return mpath_bd_device_to_head(disk_to_dev(disk));
}

int mpath_get_head(struct mpath_head *mpath_head);
void mpath_put_head(struct mpath_head *mpath_head);
int mpath_head_init(struct mpath_head *mpath_head);
void mpath_head_uninit(struct mpath_head *mpath_head);

void mpath_put_disk(struct mpath_head *mpath_head);
void mpath_remove_disk(struct mpath_head *mpath_head);
int mpath_alloc_head_disk(struct mpath_head *mpath_head,
			struct queue_limits *lim, int numa_node);
void mpath_device_set_live(struct mpath_device *mpath_device);

static inline bool is_mpath_disk(struct gendisk *disk)
{
	#if IS_ENABLED(CONFIG_LIBMULTIPATH)
	return disk->fops == &mpath_ops;
	#else
	return false;
	#endif
}
#endif // _LIBMULTIPATH_H
