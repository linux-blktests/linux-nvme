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

int mpath_head_init(struct mpath_head *mpath_head)
{
	memset(mpath_head, 0, sizeof(*mpath_head));
	INIT_LIST_HEAD(&mpath_head->dev_list);
	mutex_init(&mpath_head->lock);
	refcount_set(&mpath_head->refcount, 1);

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
