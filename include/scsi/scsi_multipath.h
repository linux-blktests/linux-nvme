/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCSI_SCSI_MULTIPATH_H
#define _SCSI_SCSI_MULTIPATH_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/blk-mq.h>
#include <linux/multipath.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_devinfo.h>
#include <scsi/scsi_driver.h>

#ifdef CONFIG_SCSI_MULTIPATH
#define SCSI_MPATH_DEVICE_ID_LEN 256

struct scsi_mpath_head {
	struct mpath_head	mpath_head;
	char			vpd_id[SCSI_MPATH_DEVICE_ID_LEN];
	struct list_head	entry;
	struct ida		ida;
	struct kref		ref;
	enum mpath_iopolicy_e	iopolicy;
	struct bio_set		bio_pool;
	struct device		dev;
	int			index;
};

struct scsi_mpath_device {
	struct mpath_device	mpath_device;
	struct scsi_device 	*sdev;
	int			index;
	struct scsi_mpath_head	*scsi_mpath_head;

	char			device_id_str[SCSI_MPATH_DEVICE_ID_LEN];
};

#define to_scsi_mpath_device(d) \
	container_of(d, struct scsi_mpath_device, mpath_device)
#define to_scsi_mpath_head(d) \
	container_of(d, struct scsi_mpath_head, mpath_head)

int scsi_mpath_dev_alloc(struct scsi_device *sdev);
void scsi_mpath_dev_release(struct scsi_device *sdev);
int scsi_multipath_init(void);
void scsi_multipath_exit(void);
void scsi_mpath_remove_device(struct scsi_mpath_device *scsi_mpath_dev);
void scsi_mpath_get_head(struct scsi_mpath_head *scsi_mpath_head);
int scsi_mpath_try_get_head(struct scsi_mpath_head *scsi_mpath_head);
void scsi_mpath_put_head(struct scsi_mpath_head *scsi_mpath_head);
void scsi_mpath_add_sysfs_link(struct scsi_device *sdev);
void scsi_mpath_remove_sysfs_link(struct scsi_device *sdev);
#else /* CONFIG_SCSI_MULTIPATH */

struct scsi_mpath_head {
};
struct scsi_mpath_device {
};

static inline int scsi_mpath_dev_alloc(struct scsi_device *sdev)
{
	return 0;
}
static inline void scsi_mpath_dev_release(struct scsi_device *sdev)
{
}
static inline int scsi_multipath_init(void)
{
	return 0;
}
static inline void scsi_multipath_exit(void)
{
}
static inline
void scsi_mpath_remove_device(struct scsi_mpath_device *scsi_mpath_dev)
{
}
static inline
void scsi_mpath_get_head(struct scsi_mpath_head *scsi_mpath_head)
{
}
static inline
int scsi_mpath_try_get_head(struct scsi_mpath_head *scsi_mpath_head)
{
	return 0;
}
static inline
void scsi_mpath_put_head(struct scsi_mpath_head *scsi_mpath_head)
{
}
static inline void scsi_mpath_add_sysfs_link(struct scsi_device *sdev)
{
}
static inline void scsi_mpath_remove_sysfs_link(struct scsi_device *sdev)
{
}
#endif /* CONFIG_SCSI_MULTIPATH */
#endif /* _SCSI_SCSI_MULTIPATH_H */
