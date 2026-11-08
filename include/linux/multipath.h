// SPDX-License-Identifier: GPL-2.0-only
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/blkdev.h>
#include <linux/srcu.h>

struct mpath_device {
	struct list_head	siblings;
	struct gendisk		*disk;
};

struct mpath_head {
	struct srcu_struct	srcu;
	struct list_head	dev_list;	/* list of all mpath_devs */
	struct mutex		lock;

	refcount_t		refcount;

	struct mpath_device __rcu 		*current_path[MAX_NUMNODES];
};

int mpath_get_head(struct mpath_head *mpath_head);
void mpath_put_head(struct mpath_head *mpath_head);
int mpath_head_init(struct mpath_head *mpath_head);
void mpath_head_uninit(struct mpath_head *mpath_head);

#endif // _LIBMULTIPATH_H
