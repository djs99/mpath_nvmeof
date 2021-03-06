/*
 * NVM Express device driver
 * Copyright (c) 2011-2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/hdreg.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list_sort.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/pr.h>
#include <linux/ptrace.h>
#include <linux/nvme_ioctl.h>
#include <linux/t10-pi.h>
#include <linux/pm_qos.h>
#include <asm/unaligned.h>
#include <linux/kthread.h>

#include "nvme.h"
#include "fabrics.h"

#define NVME_MINORS		(1U << MINORBITS)

unsigned char admin_timeout = 60;
module_param(admin_timeout, byte, 0644);
MODULE_PARM_DESC(admin_timeout, "timeout in seconds for admin commands");
EXPORT_SYMBOL_GPL(admin_timeout);

unsigned char nvme_io_timeout = 30;
module_param_named(io_timeout, nvme_io_timeout, byte, 0644);
MODULE_PARM_DESC(io_timeout, "timeout in seconds for I/O");
EXPORT_SYMBOL_GPL(nvme_io_timeout);

static unsigned char shutdown_timeout = 5;
module_param(shutdown_timeout, byte, 0644);
MODULE_PARM_DESC(shutdown_timeout, "timeout in seconds for controller shutdown");

unsigned char nvme_max_retries = 5;
module_param_named(max_retries, nvme_max_retries, byte, 0644);
MODULE_PARM_DESC(max_retries, "max number of retries a command may have");


unsigned char mpath_io_timeout = 60;
module_param(mpath_io_timeout, byte, 0644);
MODULE_PARM_DESC(mpath_io_timeout, "timeout in seconds for multipath IO");
EXPORT_SYMBOL_GPL(mpath_io_timeout);

unsigned int ns_failover_interval = 60;
module_param_named(failover_interval, ns_failover_interval, uint, 0644);
MODULE_PARM_DESC(failover_interval, "Minimum internval in secs to fallback on same namespace during multipath.");
EXPORT_SYMBOL_GPL(ns_failover_interval);

static int nvme_char_major;
module_param(nvme_char_major, int, 0);

static unsigned long default_ps_max_latency_us = 100000;
module_param(default_ps_max_latency_us, ulong, 0644);
MODULE_PARM_DESC(default_ps_max_latency_us,
		 "max power saving latency for new devices; use PM QOS to change per device");

static bool force_apst;
module_param(force_apst, bool, 0644);
MODULE_PARM_DESC(force_apst, "allow APST for newly enumerated devices even if quirked off");

static bool streams;
module_param(streams, bool, 0644);
MODULE_PARM_DESC(streams, "turn on support for Streams write directives");

struct workqueue_struct *nvme_wq;
EXPORT_SYMBOL_GPL(nvme_wq);

#define NVME_MPATH_NS_AVAIL	0
#define NVME_NO_MPATH_NS_AVAIL	1

static LIST_HEAD(nvme_mpath_ctrl_list);
static LIST_HEAD(nvme_ctrl_list);
static DEFINE_SPINLOCK(dev_list_lock);

static struct task_struct *nvme_mpath_thread;
static wait_queue_head_t nvme_mpath_kthread_wait;
static void nvme_mpath_flush_io_work(struct work_struct *work);

enum {
	/*
	 * Namespace state (Active or Standby) on multipath environment.
	 */
	NVME_NS_STATE_ACTIVE = 1,
	NVME_NS_STATE_STANDBY,
	NVME_NS_STATE_UNDEFINED,
};

static struct class *nvme_class;

struct nvme_mpath_priv {
	struct nvme_ns *ns;
	struct nvme_ns *mpath_ns;
	struct block_device *bi_bdev;
	unsigned long bi_flags;
	sector_t bi_sector;
	unsigned short bi_idx;
	unsigned short bi_vcnt;
	unsigned int bi_phys_segments;
	void  *bi_private;
	size_t nr_bytes;
	struct bio *bio;
	struct bio_vec *bvec;
	bio_end_io_t *bi_end_io;
	int    nr_retries;
	unsigned long start_time;
	struct hd_struct *part;
};

#define NVME_FAILOVER_RETRIES	3
struct nvme_failover_data {
	struct nvme_ns *standby_ns;
	struct nvme_ns *active_ns;
	struct nvme_ns *mpath_ns;
	int retries;
};

static __le32 nvme_get_log_dw10(u8 lid, size_t size)
{
        return cpu_to_le32((((size / 4) - 1) << 16) | lid);
}

int nvme_reset_ctrl(struct nvme_ctrl *ctrl)
{
        if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))
                return -EBUSY;
        if (!queue_work(nvme_wq, &ctrl->reset_work))
                return -EBUSY;
        return 0;
}
EXPORT_SYMBOL_GPL(nvme_reset_ctrl);

static int nvme_reset_ctrl_sync(struct nvme_ctrl *ctrl)
{
    int ret;

    ret = nvme_reset_ctrl(ctrl);
    if (!ret)
        flush_work(&ctrl->reset_work);
    return ret;
}

static blk_status_t nvme_error_status(struct request *req)
{
	switch (nvme_req(req)->status & 0x7ff) {
	case NVME_SC_SUCCESS:
		return BLK_STS_OK;
	case NVME_SC_CAP_EXCEEDED:
		return BLK_STS_NOSPC;
	case NVME_SC_ONCS_NOT_SUPPORTED:
		return BLK_STS_NOTSUPP;
	case NVME_SC_WRITE_FAULT:
	case NVME_SC_READ_ERROR:
	case NVME_SC_UNWRITTEN_BLOCK:
		return BLK_STS_MEDIUM;
	default:
		return BLK_STS_IOERR;
	}
}

static inline bool nvme_req_needs_retry(struct request *req)
{
	if (blk_noretry_request(req))
		return false;
	if (nvme_req(req)->status & NVME_SC_DNR)
		return false;
	if (jiffies - req->start_time >= req->timeout)
		return false;
	if (nvme_req(req)->retries >= nvme_max_retries)
		return false;
	return true;
}

void nvme_complete_rq(struct request *req)
{
	if (unlikely(nvme_req(req)->status && nvme_req_needs_retry(req))) {
		nvme_req(req)->retries++;
		blk_mq_requeue_request(req, true);
		return;
	}

	blk_mq_end_request(req, nvme_error_status(req));
}
EXPORT_SYMBOL_GPL(nvme_complete_rq);

void nvme_cancel_request(struct request *req, void *data, bool reserved)
{
	int status;

	if (!blk_mq_request_started(req))
		return;

	dev_dbg_ratelimited(((struct nvme_ctrl *) data)->device,
				"Cancelling I/O %d", req->tag);

	status = NVME_SC_ABORT_REQ;
	if (blk_queue_dying(req->q))
		status |= NVME_SC_DNR;
	nvme_req(req)->status = status;
	blk_mq_complete_request(req);

}
EXPORT_SYMBOL_GPL(nvme_cancel_request);

bool nvme_change_ctrl_state(struct nvme_ctrl *ctrl,
		enum nvme_ctrl_state new_state)
{
	enum nvme_ctrl_state old_state;
	bool changed = false;

	spin_lock_irq(&ctrl->lock);

	old_state = ctrl->state;
	switch (new_state) {
	case NVME_CTRL_LIVE:
		switch (old_state) {
		case NVME_CTRL_NEW:
		case NVME_CTRL_RESETTING:
		case NVME_CTRL_RECONNECTING:
			changed = true;
			/* FALLTHRU */
		default:
			break;
		}
		break;
	case NVME_CTRL_RESETTING:
		switch (old_state) {
		case NVME_CTRL_NEW:
		case NVME_CTRL_LIVE:

			changed = true;
			/* FALLTHRU */
		default:
			break;
		}
		break;
	case NVME_CTRL_RECONNECTING:
		switch (old_state) {
		case NVME_CTRL_LIVE:
			changed = true;
			/* FALLTHRU */
		default:
			break;
		}
		break;
	case NVME_CTRL_DELETING:
		switch (old_state) {
		case NVME_CTRL_LIVE:
		case NVME_CTRL_RESETTING:
		case NVME_CTRL_RECONNECTING:
			changed = true;
			/* FALLTHRU */
		default:
			break;
		}
		break;
	case NVME_CTRL_DEAD:
		switch (old_state) {
		case NVME_CTRL_DELETING:
			changed = true;
			/* FALLTHRU */
		default:
			break;
		}
		break;
	default:
		break;
	}

	if (changed)
		ctrl->state = new_state;

	spin_unlock_irq(&ctrl->lock);

	return changed;
}
EXPORT_SYMBOL_GPL(nvme_change_ctrl_state);

static void nvme_free_ns(struct kref *kref)
{
	struct nvme_ns *ns = container_of(kref, struct nvme_ns, kref);

	if (ns->ndev)
		nvme_nvm_unregister(ns);

	if (ns->disk) {
		spin_lock(&dev_list_lock);
		ns->disk->private_data = NULL;
		spin_unlock(&dev_list_lock);
	}

	put_disk(ns->disk);
	ida_simple_remove(&ns->ctrl->ns_ida, ns->instance);
	nvme_put_ctrl(ns->ctrl);
	kfree(ns);
}

static void nvme_put_ns(struct nvme_ns *ns)
{
	kref_put(&ns->kref, nvme_free_ns);
}

static struct nvme_ns *nvme_get_ns_from_disk(struct gendisk *disk)
{
	struct nvme_ns *ns;

	spin_lock(&dev_list_lock);
	ns = disk->private_data;
	if (ns) {
		if (!kref_get_unless_zero(&ns->kref))
			goto fail;
		/*ops is not assigned in head-parent controller.
		  So we perform a check for non head controller
		  case of Multipath device.*/
		if (!test_bit(NVME_NS_ROOT, &ns->flags))
			if (!try_module_get(ns->ctrl->ops->module))
				goto fail_put_ns;
	}
	spin_unlock(&dev_list_lock);

	return ns;

fail_put_ns:
	kref_put(&ns->kref, nvme_free_ns);
fail:
	spin_unlock(&dev_list_lock);
	return NULL;
}

struct request *nvme_alloc_request(struct request_queue *q,
		struct nvme_command *cmd, unsigned int flags, int qid)
{
	unsigned op = nvme_is_write(cmd) ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN;
	struct request *req;

	if (qid == NVME_QID_ANY) {
		req = blk_mq_alloc_request(q, op, flags);
	} else {
		req = blk_mq_alloc_request_hctx(q, op, flags,
				qid ? qid - 1 : 0);
	}
	if (IS_ERR(req))
		return req;

	req->cmd_flags |= REQ_FAILFAST_DRIVER;
	nvme_req(req)->cmd = cmd;

	return req;
}
EXPORT_SYMBOL_GPL(nvme_alloc_request);

static int nvme_toggle_streams(struct nvme_ctrl *ctrl, bool enable)
{
	struct nvme_command c;

	memset(&c, 0, sizeof(c));

	c.directive.opcode = nvme_admin_directive_send;
	c.directive.nsid = cpu_to_le32(NVME_NSID_ALL);
	c.directive.doper = NVME_DIR_SND_ID_OP_ENABLE;
	c.directive.dtype = NVME_DIR_IDENTIFY;
	c.directive.tdtype = NVME_DIR_STREAMS;
	c.directive.endir = enable ? NVME_DIR_ENDIR : 0;

	return nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL, 0);
}

static int nvme_disable_streams(struct nvme_ctrl *ctrl)
{
	return nvme_toggle_streams(ctrl, false);
}

static int nvme_enable_streams(struct nvme_ctrl *ctrl)
{
	return nvme_toggle_streams(ctrl, true);
}

static int nvme_get_stream_params(struct nvme_ctrl *ctrl,
				  struct streams_directive_params *s, u32 nsid)
{
	struct nvme_command c;

	memset(&c, 0, sizeof(c));
	memset(s, 0, sizeof(*s));

	c.directive.opcode = nvme_admin_directive_recv;
	c.directive.nsid = cpu_to_le32(nsid);
	c.directive.numd = cpu_to_le32((sizeof(*s) >> 2) - 1);
	c.directive.doper = NVME_DIR_RCV_ST_OP_PARAM;
	c.directive.dtype = NVME_DIR_STREAMS;

	return nvme_submit_sync_cmd(ctrl->admin_q, &c, s, sizeof(*s));
}

static int nvme_configure_directives(struct nvme_ctrl *ctrl)
{
	struct streams_directive_params s;
	int ret;

	if (!(ctrl->oacs & NVME_CTRL_OACS_DIRECTIVES))
		return 0;
	if (!streams)
		return 0;

	ret = nvme_enable_streams(ctrl);
	if (ret)
		return ret;

	ret = nvme_get_stream_params(ctrl, &s, NVME_NSID_ALL);
	if (ret)
		return ret;

	ctrl->nssa = le16_to_cpu(s.nssa);
	if (ctrl->nssa < BLK_MAX_WRITE_HINTS - 1) {
		dev_info(ctrl->device, "too few streams (%u) available\n",
					ctrl->nssa);
		nvme_disable_streams(ctrl);
		return 0;
	}

	ctrl->nr_streams = min_t(unsigned, ctrl->nssa, BLK_MAX_WRITE_HINTS - 1);
	dev_info(ctrl->device, "Using %u streams\n", ctrl->nr_streams);
	return 0;
}

/*
 * Check if 'req' has a write hint associated with it. If it does, assign
 * a valid namespace stream to the write.
 */
static void nvme_assign_write_stream(struct nvme_ctrl *ctrl,
				     struct request *req, u16 *control,
				     u32 *dsmgmt)
{
	enum rw_hint streamid = req->write_hint;

	if (streamid == WRITE_LIFE_NOT_SET || streamid == WRITE_LIFE_NONE)
		streamid = 0;
	else {
		streamid--;
		if (WARN_ON_ONCE(streamid > ctrl->nr_streams))
			return;

		*control |= NVME_RW_DTYPE_STREAMS;
		*dsmgmt |= streamid << 16;
	}

	if (streamid < ARRAY_SIZE(req->q->write_hints))
		req->q->write_hints[streamid] += blk_rq_bytes(req) >> 9;
}

static inline void nvme_setup_flush(struct nvme_ns *ns,
		struct nvme_command *cmnd)
{
	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->common.opcode = nvme_cmd_flush;
	cmnd->common.nsid = cpu_to_le32(ns->ns_id);
}

static blk_status_t nvme_setup_discard(struct nvme_ns *ns, struct request *req,
		struct nvme_command *cmnd)
{
	unsigned short segments = blk_rq_nr_discard_segments(req), n = 0;
	struct nvme_dsm_range *range;
	struct bio *bio;

	range = kmalloc_array(segments, sizeof(*range), GFP_ATOMIC);
	if (!range)
		return BLK_STS_RESOURCE;

	__rq_for_each_bio(bio, req) {
		u64 slba = nvme_block_nr(ns, bio->bi_iter.bi_sector);
		u32 nlb = bio->bi_iter.bi_size >> ns->lba_shift;

		range[n].cattr = cpu_to_le32(0);
		range[n].nlb = cpu_to_le32(nlb);
		range[n].slba = cpu_to_le64(slba);
		n++;
	}

	if (WARN_ON_ONCE(n != segments)) {
		kfree(range);
		return BLK_STS_IOERR;
	}

	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->dsm.opcode = nvme_cmd_dsm;
	cmnd->dsm.nsid = cpu_to_le32(ns->ns_id);
	cmnd->dsm.nr = cpu_to_le32(segments - 1);
	cmnd->dsm.attributes = cpu_to_le32(NVME_DSMGMT_AD);

	req->special_vec.bv_page = virt_to_page(range);
	req->special_vec.bv_offset = offset_in_page(range);
	req->special_vec.bv_len = sizeof(*range) * segments;
	req->rq_flags |= RQF_SPECIAL_PAYLOAD;

	return BLK_STS_OK;
}

static inline blk_status_t nvme_setup_rw(struct nvme_ns *ns,
		struct request *req, struct nvme_command *cmnd)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	u16 control = 0;
	u32 dsmgmt = 0;

	/*
	 * If formated with metadata, require the block layer provide a buffer
	 * unless this namespace is formated such that the metadata can be
	 * stripped/generated by the controller with PRACT=1.
	 */
	if (ns && ns->ms &&
	    (!ns->pi_type || ns->ms != sizeof(struct t10_pi_tuple)) &&
	    !blk_integrity_rq(req) && !blk_rq_is_passthrough(req))
		return BLK_STS_NOTSUPP;

	if (req->cmd_flags & REQ_FUA)
		control |= NVME_RW_FUA;
	if (req->cmd_flags & (REQ_FAILFAST_DEV | REQ_RAHEAD))
		control |= NVME_RW_LR;

	if (req->cmd_flags & REQ_RAHEAD)
		dsmgmt |= NVME_RW_DSM_FREQ_PREFETCH;

	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->rw.opcode = (rq_data_dir(req) ? nvme_cmd_write : nvme_cmd_read);
	cmnd->rw.nsid = cpu_to_le32(ns->ns_id);
	cmnd->rw.slba = cpu_to_le64(nvme_block_nr(ns, blk_rq_pos(req)));
	cmnd->rw.length = cpu_to_le16((blk_rq_bytes(req) >> ns->lba_shift) - 1);

	if (req_op(req) == REQ_OP_WRITE && ctrl->nr_streams)
		nvme_assign_write_stream(ctrl, req, &control, &dsmgmt);

	if (ns->ms) {
		switch (ns->pi_type) {
		case NVME_NS_DPS_PI_TYPE3:
			control |= NVME_RW_PRINFO_PRCHK_GUARD;
			break;
		case NVME_NS_DPS_PI_TYPE1:
		case NVME_NS_DPS_PI_TYPE2:
			control |= NVME_RW_PRINFO_PRCHK_GUARD |
					NVME_RW_PRINFO_PRCHK_REF;
			cmnd->rw.reftag = cpu_to_le32(
					nvme_block_nr(ns, blk_rq_pos(req)));
			break;
		}
		if (!blk_integrity_rq(req))
			control |= NVME_RW_PRINFO_PRACT;
	}

	cmnd->rw.control = cpu_to_le16(control);
	cmnd->rw.dsmgmt = cpu_to_le32(dsmgmt);
	return 0;
}

blk_status_t nvme_setup_cmd(struct nvme_ns *ns, struct request *req,
		struct nvme_command *cmd)
{
	blk_status_t ret = BLK_STS_OK;

	if (!(req->rq_flags & RQF_DONTPREP)) {
		nvme_req(req)->retries = 0;
		nvme_req(req)->flags = 0;
		req->rq_flags |= RQF_DONTPREP;
	}

	switch (req_op(req)) {
	case REQ_OP_DRV_IN:
	case REQ_OP_DRV_OUT:
		memcpy(cmd, nvme_req(req)->cmd, sizeof(*cmd));
		break;
	case REQ_OP_FLUSH:
		nvme_setup_flush(ns, cmd);
		break;
	case REQ_OP_WRITE_ZEROES:
		/* currently only aliased to deallocate for a few ctrls: */
	case REQ_OP_DISCARD:
		ret = nvme_setup_discard(ns, req, cmd);
		break;
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		ret = nvme_setup_rw(ns, req, cmd);
		break;
	default:
		WARN_ON_ONCE(1);
		return BLK_STS_IOERR;
	}

	cmd->common.command_id = req->tag;
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_setup_cmd);

/*
 * Returns 0 on success.  If the result is negative, it's a Linux error code;
 * if the result is positive, it's an NVM Express status code
 */
int __nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		union nvme_result *result, void *buffer, unsigned bufflen,
		unsigned timeout, int qid, int at_head, int flags)
{
	struct request *req;
	int ret;

	req = nvme_alloc_request(q, cmd, flags, qid);
	if (IS_ERR(req))
		return PTR_ERR(req);

	req->timeout = timeout ? timeout : ADMIN_TIMEOUT;

	if (buffer && bufflen) {
		ret = blk_rq_map_kern(q, req, buffer, bufflen, GFP_KERNEL);
		if (ret)
			goto out;
	}

	blk_execute_rq(req->q, NULL, req, at_head);
	if (result)
		*result = nvme_req(req)->result;
	if (nvme_req(req)->flags & NVME_REQ_CANCELLED)
		ret = -EINTR;
	else
		ret = nvme_req(req)->status;
 out:
	blk_mq_free_request(req);
	return ret;
}
EXPORT_SYMBOL_GPL(__nvme_submit_sync_cmd);

int nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		void *buffer, unsigned bufflen)
{
	return __nvme_submit_sync_cmd(q, cmd, NULL, buffer, bufflen, 0,
			NVME_QID_ANY, 0, 0);
}
EXPORT_SYMBOL_GPL(nvme_submit_sync_cmd);

int __nvme_submit_user_cmd(struct request_queue *q, struct nvme_command *cmd,
		void __user *ubuffer, unsigned bufflen,
		void __user *meta_buffer, unsigned meta_len, u32 meta_seed,
		u32 *result, unsigned timeout)
{
	bool write = nvme_is_write(cmd);
	struct nvme_ns *ns = q->queuedata;
	struct gendisk *disk = ns ? ns->disk : NULL;
	struct request *req;
	struct bio *bio = NULL;
	void *meta = NULL;
	int ret;

	req = nvme_alloc_request(q, cmd, 0, NVME_QID_ANY);
	if (IS_ERR(req))
		return PTR_ERR(req);

	req->timeout = timeout ? timeout : ADMIN_TIMEOUT;

	if (ubuffer && bufflen) {
		ret = blk_rq_map_user(q, req, NULL, ubuffer, bufflen,
				GFP_KERNEL);
		if (ret)
			goto out;
		bio = req->bio;

		if (!disk)
			goto submit;
		bio->bi_bdev = bdget_disk(disk, 0);
		if (!bio->bi_bdev) {
			ret = -ENODEV;
			goto out_unmap;
		}

		if (meta_buffer && meta_len) {
			struct bio_integrity_payload *bip;

			meta = kmalloc(meta_len, GFP_KERNEL);
			if (!meta) {
				ret = -ENOMEM;
				goto out_unmap;
			}

			if (write) {
				if (copy_from_user(meta, meta_buffer,
						meta_len)) {
					ret = -EFAULT;
					goto out_free_meta;
				}
			}

			bip = bio_integrity_alloc(bio, GFP_KERNEL, 1);
			if (IS_ERR(bip)) {
				ret = PTR_ERR(bip);
				goto out_free_meta;
			}

			bip->bip_iter.bi_size = meta_len;
			bip->bip_iter.bi_sector = meta_seed;

			ret = bio_integrity_add_page(bio, virt_to_page(meta),
					meta_len, offset_in_page(meta));
			if (ret != meta_len) {
				ret = -ENOMEM;
				goto out_free_meta;
			}
		}
	}
 submit:
	blk_execute_rq(req->q, disk, req, 0);
	if (nvme_req(req)->flags & NVME_REQ_CANCELLED)
		ret = -EINTR;
	else
		ret = nvme_req(req)->status;
	if (result)
		*result = le32_to_cpu(nvme_req(req)->result.u32);
	if (meta && !ret && !write) {
		if (copy_to_user(meta_buffer, meta, meta_len))
			ret = -EFAULT;
	}
 out_free_meta:
	if (meta)
		kfree(meta);
 out_unmap:
	if (bio) {
		if (disk && bio->bi_bdev)
			bdput(bio->bi_bdev);
		blk_rq_unmap_user(bio);
	}
 out:
	blk_mq_free_request(req);
	return ret;
}

int nvme_submit_user_cmd(struct request_queue *q, struct nvme_command *cmd,
		void __user *ubuffer, unsigned bufflen, u32 *result,
		unsigned timeout)
{
	return __nvme_submit_user_cmd(q, cmd, ubuffer, bufflen, NULL, 0, 0,
			result, timeout);
}

static void nvme_keep_alive_end_io(struct request *rq, blk_status_t status)
{
	struct nvme_ctrl *ctrl = rq->end_io_data;

	blk_mq_free_request(rq);

	if (status) {
		dev_err(ctrl->device,
			"failed nvme_keep_alive_end_io error=%d\n",
				status);
		schedule_work(&ctrl->failover_work);
		return;
	}

	schedule_delayed_work(&ctrl->ka_work, ctrl->kato * HZ);
}

static int nvme_keep_alive(struct nvme_ctrl *ctrl)
{
	struct nvme_command c;
	struct request *rq;

	memset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_keep_alive;

	rq = nvme_alloc_request(ctrl->admin_q, &c, BLK_MQ_REQ_RESERVED,
			NVME_QID_ANY);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	rq->timeout = ctrl->kato * HZ;
	rq->end_io_data = ctrl;

	blk_execute_rq_nowait(rq->q, NULL, rq, 0, nvme_keep_alive_end_io);

	return 0;
}

static void nvme_keep_alive_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvme_ctrl, ka_work);

	if (nvme_keep_alive(ctrl)) {
		/* allocation failure, reset the controller */
		dev_err(ctrl->device, "keep-alive failed\n");
		nvme_reset_ctrl(ctrl);
		return;
	}
}

/*
 * Returns non-zero value if operation is write, zero otherwise.
 */
static inline int nvme_mpath_bio_is_write(struct bio *bio)
{

	return op_is_write(bio_op(bio)) ? 1 : 0;

}

/*
 * Stats accounting for IO requests on multipath volume.
 * Same code for stand-alone volume can not be reused since it works on
 * struct request. Multipath volume does not maintain its own request,
 * rather it simply redirects IO requests to the active volume.
 */
static void nvme_mpath_blk_account_io_done(struct bio *bio,
					   struct nvme_ns *mpath_ns,
					   struct nvme_mpath_priv *priv)
{
	int rw;
	int cpu;
	struct hd_struct *part;
	unsigned long flags;
	unsigned long duration;

	spin_lock_irqsave(mpath_ns->queue->queue_lock, flags);

	duration = jiffies - priv->start_time;
	cpu = part_stat_lock();

	rw = nvme_mpath_bio_is_write(bio);
	cpu  = part_stat_lock();
	part = priv->part;

	part_stat_inc(cpu, part, ios[rw]);
	part_stat_add(cpu, part, ticks[rw], duration);
	part_round_stats(cpu, part);
	part_stat_add(cpu, part, sectors[rw], priv->nr_bytes >> 9);
	part_dec_in_flight(part, rw);
	part_stat_unlock();

	spin_unlock_irqrestore(mpath_ns->queue->queue_lock, flags);
}

static void nvme_mpath_cancel_ios(struct nvme_ns *mpath_ns)
{
	struct nvme_mpath_priv *priv;
	struct bio *bio;
	struct bio_list bios;
	unsigned long flags;

	mutex_lock(&mpath_ns->ctrl->namespaces_mutex);
	spin_lock_irqsave(&mpath_ns->ctrl->lock, flags);
	if (bio_list_empty(&mpath_ns->fq_cong)) {
		spin_unlock_irqrestore(&mpath_ns->ctrl->lock, flags);
		goto biolist_empty;
	}

	bio_list_init(&bios);
	bio_list_merge(&bios, &mpath_ns->fq_cong);

	bio_list_init(&mpath_ns->fq_cong);
	remove_wait_queue(&mpath_ns->fq_full, &mpath_ns->fq_cong_wait);
	spin_unlock_irqrestore(&mpath_ns->ctrl->lock, flags);

	while (bio_list_peek(&bios)) {
		bio = bio_list_pop(&bios);
		priv = bio->bi_private;

		bio->bi_status = BLK_STS_IOERR;
		bio->bi_bdev = priv->bi_bdev;
		bio->bi_end_io = priv->bi_end_io;
		bio->bi_private = priv->bi_private;
        nvme_mpath_blk_account_io_done(bio, mpath_ns, priv);
		bio_endio(bio);

		mempool_free(priv, mpath_ns->ctrl->mpath_req_pool);
	}
biolist_empty:
	mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
}

static void nvme_mpath_flush_io_work(struct work_struct *work)
{
    struct nvme_ctrl *mpath_ctrl = container_of(to_delayed_work(work),
            struct nvme_ctrl, cu_work);
    struct nvme_ns *mpath_ns = NULL;
    struct nvme_ns *next;

    list_for_each_entry_safe(mpath_ns, next, &mpath_ctrl->mpath_namespace, list) {
        if (mpath_ns)
            break;
    }

    if (!mpath_ns) {
        dev_err(mpath_ctrl->device,"No Multipath namespace found.\n");
        return;
    }

    if (test_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags))
        goto exit;

    if (test_bit(NVME_NS_ROOT, &mpath_ns->flags)) {
        printk("Cancelling all pending IOs\n");
        nvme_mpath_cancel_ios(mpath_ns);
    }

    return;
exit:
    schedule_delayed_work(&mpath_ctrl->cu_work, nvme_io_timeout*HZ);
}


void nvme_start_keep_alive(struct nvme_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	INIT_DELAYED_WORK(&ctrl->ka_work, nvme_keep_alive_work);
	schedule_delayed_work(&ctrl->ka_work, ctrl->kato * HZ);
}
EXPORT_SYMBOL_GPL(nvme_start_keep_alive);

void nvme_stop_keep_alive(struct nvme_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	cancel_delayed_work_sync(&ctrl->ka_work);
}
EXPORT_SYMBOL_GPL(nvme_stop_keep_alive);

static int nvme_identify_ctrl(struct nvme_ctrl *dev, struct nvme_id_ctrl **id)
{
	struct nvme_command c = { };
	int error;

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = NVME_ID_CNS_CTRL;

	*id = kmalloc(sizeof(struct nvme_id_ctrl), GFP_KERNEL);
	if (!*id)
		return -ENOMEM;

	error = nvme_submit_sync_cmd(dev->admin_q, &c, *id,
			sizeof(struct nvme_id_ctrl));
	if (error)
		kfree(*id);
	return error;
}

static int nvme_identify_ns_descs(struct nvme_ns *ns, unsigned nsid)
{
	struct nvme_command c = { };
	int status;
	void *data;
	int pos;
	int len;

	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = cpu_to_le32(nsid);
	c.identify.cns = NVME_ID_CNS_NS_DESC_LIST;

	data = kzalloc(NVME_IDENTIFY_DATA_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	status = nvme_submit_sync_cmd(ns->ctrl->admin_q, &c, data,
				      NVME_IDENTIFY_DATA_SIZE);
	if (status)
		goto free_data;

	for (pos = 0; pos < NVME_IDENTIFY_DATA_SIZE; pos += len) {
		struct nvme_ns_id_desc *cur = data + pos;

		if (cur->nidl == 0)
			break;

		switch (cur->nidt) {
		case NVME_NIDT_EUI64:
			if (cur->nidl != NVME_NIDT_EUI64_LEN) {
				dev_warn(ns->ctrl->device,
					 "ctrl returned bogus length: %d for NVME_NIDT_EUI64\n",
					 cur->nidl);
				goto free_data;
			}
			len = NVME_NIDT_EUI64_LEN;
			memcpy(ns->eui, data + pos + sizeof(*cur), len);
			break;
		case NVME_NIDT_NGUID:
			if (cur->nidl != NVME_NIDT_NGUID_LEN) {
				dev_warn(ns->ctrl->device,
					 "ctrl returned bogus length: %d for NVME_NIDT_NGUID\n",
					 cur->nidl);
				goto free_data;
			}
			len = NVME_NIDT_NGUID_LEN;
			memcpy(ns->nguid, data + pos + sizeof(*cur), len);
			break;
		case NVME_NIDT_UUID:
			if (cur->nidl != NVME_NIDT_UUID_LEN) {
				dev_warn(ns->ctrl->device,
					 "ctrl returned bogus length: %d for NVME_NIDT_UUID\n",
					 cur->nidl);
				goto free_data;
			}
			len = NVME_NIDT_UUID_LEN;
			uuid_copy(&ns->uuid, data + pos + sizeof(*cur));
			break;
		default:
			/* Skip unnkown types */
			len = cur->nidl;
			break;
		}

		len += sizeof(*cur);
	}
free_data:
	kfree(data);
	return status;
}

static int nvme_identify_ns_list(struct nvme_ctrl *dev, unsigned nsid, __le32 *ns_list)
{
	struct nvme_command c = { };

	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = NVME_ID_CNS_NS_ACTIVE_LIST;
	c.identify.nsid = cpu_to_le32(nsid);
	return nvme_submit_sync_cmd(dev->admin_q, &c, ns_list, 0x1000);
}

static int nvme_identify_ns(struct nvme_ctrl *dev, unsigned nsid,
		struct nvme_id_ns **id)
{
	struct nvme_command c = { };
	int error;

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = cpu_to_le32(nsid);
	c.identify.cns = NVME_ID_CNS_NS;

	*id = kmalloc(sizeof(struct nvme_id_ns), GFP_KERNEL);
	if (!*id)
		return -ENOMEM;

	error = nvme_submit_sync_cmd(dev->admin_q, &c, *id,
			sizeof(struct nvme_id_ns));
	if (error)
		kfree(*id);
	return error;
}

static void nvme_ns_active_end_io(struct request *rq, blk_status_t error)
{
	struct nvme_ctrl *ctrl;
	struct nvme_ns *standby_ns, *mpath_ns;
	struct nvme_failover_data *priv = rq->end_io_data;
	standby_ns = priv->standby_ns;
	mpath_ns =  priv->mpath_ns;
	ctrl = standby_ns->ctrl;

	blk_mq_free_request(rq);


	if (error) {
		dev_err(ctrl->device,
			"Failed to set nvme%dn%d active with error=%d\n",
			ctrl->instance, standby_ns->instance, error);
	} else {
		standby_ns->active = 1;
		standby_ns->mpath_ctrl->cleanup_done = 1;
		dev_info(ctrl->device,
			"New active ns nvme%dn%d \n",ctrl->instance,
			standby_ns->instance);
	}
	test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);

	if(error)
		schedule_delayed_work(&standby_ns->mpath_ctrl->cu_work, HZ);

	kfree(priv);
}

int nvme_set_ns_active(struct nvme_ns *standby_ns, struct nvme_ns *mpath_ns,
		int retry_cnt)
{
	struct nvme_command c = { };
	struct request *rq;
	struct nvme_failover_data *priv;

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = 0xFE;
	c.identify.nsid = cpu_to_le32(standby_ns->ns_id);
	dev_info(standby_ns->ctrl->device, "Set active ns nvme%dn%d \n",
	standby_ns->ctrl->instance, standby_ns->instance);

	priv = kmalloc(sizeof(struct nvme_failover_data), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mpath_ns = mpath_ns;
	priv->standby_ns = standby_ns;
	priv->retries = retry_cnt;
	rq = nvme_alloc_request(standby_ns->ctrl->admin_q, &c,
		BLK_MQ_REQ_RESERVED, NVME_QID_ANY);
	if (IS_ERR(rq)) {
		kfree(priv);
		return PTR_ERR(rq);
	}

	rq->timeout = standby_ns->ctrl->kato * HZ * NVME_NS_ACTIVE_TIMEOUT;
	rq->end_io_data = priv;
	blk_execute_rq_nowait(rq->q, NULL, rq, 0, nvme_ns_active_end_io);

	return 0;
}

static int nvme_get_mpath_nguid(struct nvme_ctrl *dev, unsigned nsid,
		char **nguid)
{
	struct nvme_command c = { };
	c.identify.opcode = 0xFC;
	c.identify.nsid = cpu_to_le32(nsid);

	*nguid = kzalloc(1024, GFP_KERNEL);
	if (!*nguid)
		return -ENOMEM;

	return nvme_submit_sync_cmd(dev->admin_q, &c, (*nguid), 1024);
}

int nvme_get_features(struct nvme_ctrl *dev, unsigned fid, unsigned nsid,
		dma_addr_t dma_addr, u32 *result)
{
	struct nvme_command c;
	union nvme_result res;
	int ret;

	memset(&c, 0, sizeof(c));
	c.features.opcode = nvme_admin_get_features;
	c.features.nsid = cpu_to_le32(nsid);
	c.features.dptr.prp1 = cpu_to_le64(dma_addr);
	c.features.fid = cpu_to_le32(fid);

	ret = __nvme_submit_sync_cmd(dev->admin_q, &c, &res, NULL, 0, 0,
			NVME_QID_ANY, 0, 0);
	if (ret >= 0)
		*result = le32_to_cpu(res.u32);
	return ret;
}

static int nvme_set_features(struct nvme_ctrl *dev, unsigned fid, unsigned dword11,
                      void *buffer, size_t buflen, u32 *result)
{
	struct nvme_command c;
	union nvme_result res;
	int ret;

	memset(&c, 0, sizeof(c));
	c.features.opcode = nvme_admin_set_features;
	c.features.fid = cpu_to_le32(fid);
	c.features.dword11 = cpu_to_le32(dword11);

	ret = __nvme_submit_sync_cmd(dev->admin_q, &c, &res,
			buffer, buflen, 0, NVME_QID_ANY, 0, 0);
	if (ret >= 0 && result)
		*result = le32_to_cpu(res.u32);
	return ret;
}

int nvme_set_queue_count(struct nvme_ctrl *ctrl, int *count)
{
	u32 q_count = (*count - 1) | ((*count - 1) << 16);
	u32 result;
	int status, nr_io_queues;

	status = nvme_set_features(ctrl, NVME_FEAT_NUM_QUEUES, q_count, NULL, 0,
			&result);
	if (status < 0)
		return status;

	/*
	 * Degraded controllers might return an error when setting the queue
	 * count.  We still want to be able to bring them online and offer
	 * access to the admin queue, as that might be only way to fix them up.
	 */
	if (status > 0) {
		dev_err(ctrl->device, "Could not set queue count (%d)\n", status);
		*count = 0;
	} else {
		nr_io_queues = min(result & 0xffff, result >> 16) + 1;
		*count = min(*count, nr_io_queues);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(nvme_set_queue_count);

static struct nvme_ns *nvme_get_active_ns_for_mpath_ns(struct nvme_ns *mpath_ns)
{
	struct nvme_ns *ns = NULL, *next;

	/*Only get active Namespace if given Namespace is head or parent
	 of Multipath group otherwise just return the same namespace.*/
	if (test_bit(NVME_NS_ROOT, &mpath_ns->flags)) {
		mutex_lock(&mpath_ns->ctrl->namespaces_mutex);
		list_for_each_entry_safe(ns, next, &mpath_ns->ctrl->namespaces, mpathlist) {
			if (ns->active) {
				mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
				return ns;
			}
		}
		mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);

		/* did not find active */
		ns = NULL;
		printk("%s: No active ns found for mpath ns mpnvme%dn%d\n", __FUNCTION__, mpath_ns->ctrl->instance, mpath_ns->instance);
		return ns;
	}
	return mpath_ns;
}

static struct nvme_ns *nvme_get_ns_for_mpath_ns(struct nvme_ns *mpath_ns)
{
	struct nvme_ns *ns = NULL, *next;
	if (test_bit(NVME_NS_ROOT, &mpath_ns->flags)) {
		mutex_lock(&mpath_ns->ctrl->namespaces_mutex);
		list_for_each_entry_safe(ns, next, &mpath_ns->ctrl->namespaces, mpathlist) {
			if (ns) {
				mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
				return ns;
			}
		}
		mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
		printk("%s: No mpath group device found for mpath ns mpnvme%dn%d\n", __FUNCTION__, mpath_ns->ctrl->instance, mpath_ns->instance);
	}
	return mpath_ns;
}

static int nvme_submit_io(struct nvme_ns *ns, struct nvme_user_io __user *uio)
{
	struct nvme_user_io io;
	struct nvme_command c;
	unsigned length, meta_len;
	void __user *metadata;

	if (copy_from_user(&io, uio, sizeof(io)))
		return -EFAULT;
	if (io.flags)
		return -EINVAL;

	switch (io.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
	case nvme_cmd_compare:
		break;
	default:
		return -EINVAL;
	}

	length = (io.nblocks + 1) << ns->lba_shift;
	meta_len = (io.nblocks + 1) * ns->ms;
	metadata = (void __user *)(uintptr_t)io.metadata;

	if (ns->ext) {
		length += meta_len;
		meta_len = 0;
	} else if (meta_len) {
		if ((io.metadata & 3) || !io.metadata)
			return -EINVAL;
	}

	memset(&c, 0, sizeof(c));
	c.rw.opcode = io.opcode;
	c.rw.flags = io.flags;
	c.rw.nsid = cpu_to_le32(ns->ns_id);
	c.rw.slba = cpu_to_le64(io.slba);
	c.rw.length = cpu_to_le16(io.nblocks);
	c.rw.control = cpu_to_le16(io.control);
	c.rw.dsmgmt = cpu_to_le32(io.dsmgmt);
	c.rw.reftag = cpu_to_le32(io.reftag);
	c.rw.apptag = cpu_to_le16(io.apptag);
	c.rw.appmask = cpu_to_le16(io.appmask);

	return __nvme_submit_user_cmd(ns->queue, &c,
			(void __user *)(uintptr_t)io.addr, length,
			metadata, meta_len, io.slba, NULL, 0);
}

static int nvme_user_cmd(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			struct nvme_passthru_cmd __user *ucmd)
{
	struct nvme_passthru_cmd cmd;
	struct nvme_command c;
	unsigned timeout = 0;
	int status;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;

	memset(&c, 0, sizeof(c));
	c.common.opcode = cmd.opcode;
	c.common.flags = cmd.flags;
	c.common.nsid = cpu_to_le32(cmd.nsid);
	c.common.cdw2[0] = cpu_to_le32(cmd.cdw2);
	c.common.cdw2[1] = cpu_to_le32(cmd.cdw3);
	c.common.cdw10[0] = cpu_to_le32(cmd.cdw10);
	c.common.cdw10[1] = cpu_to_le32(cmd.cdw11);
	c.common.cdw10[2] = cpu_to_le32(cmd.cdw12);
	c.common.cdw10[3] = cpu_to_le32(cmd.cdw13);
	c.common.cdw10[4] = cpu_to_le32(cmd.cdw14);
	c.common.cdw10[5] = cpu_to_le32(cmd.cdw15);

	if (cmd.timeout_ms)
		timeout = msecs_to_jiffies(cmd.timeout_ms);

	status = nvme_submit_user_cmd(ns ? ns->queue : ctrl->admin_q, &c,
			(void __user *)(uintptr_t)cmd.addr, cmd.data_len,
			&cmd.result, timeout);
	if (status >= 0) {
		if (put_user(cmd.result, &ucmd->result))
			return -EFAULT;
	}

	return status;
}

/*
 * Starts the io accounting for given io request. Again, code from stand alone
 * volume io accounting can not be shared since it operates on struct request.
 */
static void nvme_mpath_blk_account_io_start(struct bio *bio,
		struct nvme_ns *mpath_ns, struct nvme_mpath_priv *priv)
{
	int rw;
	int cpu;
	struct hd_struct *part;

	rw = nvme_mpath_bio_is_write(bio);
	cpu = part_stat_lock();

	part = disk_map_sector_rcu(mpath_ns->disk, priv->bi_sector);
	part_round_stats(cpu, part);
	part_inc_in_flight(part, rw);

	part_stat_unlock();
	priv->part = part;
}

int get_ns_state(struct nvme_ns *ns) {
	if(ns->active && ns->ctrl->state == NVME_CTRL_LIVE)
		return NVME_NS_STATE_ACTIVE; /* active */

	if(!ns->active && ns->ctrl->state == NVME_CTRL_LIVE)
		return NVME_NS_STATE_STANDBY; /* standby */

	return NVME_NS_STATE_UNDEFINED; /* state undefined */
}

struct nvme_ns* get_ns_active (struct nvme_ns *mpath_ns) {
	struct nvme_ns *ns = NULL,*next;
	struct nvme_ns *tmp = NULL;
	list_for_each_entry_safe(tmp, next, &mpath_ns->ctrl->namespaces, mpathlist) {
		if((get_ns_state(tmp) == NVME_NS_STATE_ACTIVE) && test_bit(NVME_NS_MULTIPATH, &tmp->flags)) {
			ns = tmp;
			break;
		}
	}
	return ns;
}
static void nvme_mpath_resubmit_bios(struct nvme_ns *mpath_ns)
{
	struct nvme_mpath_priv *priv;
	struct nvme_ns *ns = NULL;
	struct bio *bio;
	struct bio_vec *bvec;
	struct bio_list bios;
	unsigned long flags;
	struct blk_plug plug;

	/*Get active namespace before resending the IO*/

	mutex_lock(&mpath_ns->ctrl->namespaces_mutex);
	if (test_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags)) {
		goto namespaces_mutex_unlock;
        }
	if (list_empty(&mpath_ns->ctrl->namespaces)) {
		goto namespaces_mutex_unlock;
	}

	ns = get_ns_active(mpath_ns);

	if (!ns) {
		goto namespaces_mutex_unlock;
	}

	if (test_bit(NVME_NS_REMOVING, &ns->flags) ||  !mpath_ns->ctrl->cleanup_done) {
		goto namespaces_mutex_unlock;
	}

	if (test_bit(NVME_NS_REMOVING, &mpath_ns->flags)) {
		goto namespaces_mutex_unlock;
	}

	spin_lock_irqsave(&mpath_ns->ctrl->lock, flags);
	if (bio_list_empty(&mpath_ns->fq_cong)) {
		spin_unlock_irqrestore(&mpath_ns->ctrl->lock, flags);
		goto namespaces_mutex_unlock;
	}

	bio_list_init(&bios);
	bio_list_merge(&bios, &mpath_ns->fq_cong);

	bio_list_init(&mpath_ns->fq_cong);
	remove_wait_queue(&mpath_ns->fq_full, &mpath_ns->fq_cong_wait);
	spin_unlock_irqrestore(&mpath_ns->ctrl->lock, flags);

	blk_start_plug(&plug);

	while (bio_list_peek(&bios)) {
		bio = bio_list_pop(&bios);
		priv = bio->bi_private;
		bvec = &priv->bio->bi_io_vec[0];
		priv->ns = ns;
		bio->bi_bdev = ns->bdev;
		bvec = &bio->bi_io_vec[0];
		bio->bi_status = 0;
		bio->bi_flags = priv->bi_flags;
		bio->bi_iter.bi_idx = 0;
		bio->bi_iter.bi_bvec_done = 0;
		bio->bi_iter.bi_sector = priv->bi_sector;
		bio->bi_iter.bi_size = priv->nr_bytes;
		bio->bi_vcnt = priv->bi_vcnt;
		bio->bi_phys_segments = priv->bi_phys_segments;
		bio->bi_seg_front_size = 0;
		bio->bi_seg_back_size = 0;
		atomic_set(&bio->__bi_remaining, 1);
		generic_make_request(bio);
	}
	blk_finish_plug(&plug);

namespaces_mutex_unlock:
	mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
}

/*This logic get executed during IO errors on head or parent Multipath device.*/
static int nvme_mpath_kthread(void *data)
{
	struct nvme_ns *mpath_ns, *next;
	struct nvme_ctrl *mpath_ctrl, *next_ctrl;

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		list_for_each_entry_safe(mpath_ctrl, next_ctrl, &nvme_mpath_ctrl_list, node) {
			list_for_each_entry_safe(mpath_ns, next, &mpath_ctrl->mpath_namespace, list) {
				if (!mpath_ns)
					continue;
				rcu_read_lock();
				if (waitqueue_active(&mpath_ns->fq_full))
					nvme_mpath_resubmit_bios(mpath_ns);
				rcu_read_unlock();
			}
		}
		schedule_timeout(round_jiffies_relative(HZ));
	}
	return 0;
}


static bool nvme_mpath_retry_bio(struct bio *bio)
{
	unsigned long flags;

	struct nvme_mpath_priv *priv = bio->bi_private;
	struct nvme_ns *mpath_ns = priv->mpath_ns;

	spin_lock_irqsave(&mpath_ns->ctrl->lock, flags);
	if (!waitqueue_active(&mpath_ns->fq_full))
		add_wait_queue(&mpath_ns->fq_full, &mpath_ns->fq_cong_wait);

	bio_list_add(&mpath_ns->fq_cong, bio);

	spin_unlock_irqrestore(&mpath_ns->ctrl->lock, flags);
	return true;
}

static inline int nvme_mpath_bio_has_error(struct bio *bio)
{
	return ((bio->bi_status != 0) ? 1:0);
}


static void nvme_mpath_endio(struct bio *bio)
{
	int ret;
	struct nvme_ns *mpath_ns;
	struct nvme_mpath_priv *priv;

	priv = bio->bi_private;
	mpath_ns = priv->mpath_ns;

	ret = nvme_mpath_bio_has_error(bio);
	if (ret) {
		if (!test_bit(NVME_NS_REMOVING, &mpath_ns->flags)) {
			if (priv->nr_retries > 0) {
				priv->nr_retries--;
				ret = nvme_mpath_retry_bio(bio);
				if (ret)
					return;
			}
		}
	} else {
		nvme_mpath_blk_account_io_done(bio, mpath_ns, priv);
	}

	mpath_ns =  priv->mpath_ns;
	bio->bi_bdev = priv->bi_bdev;
	bio->bi_end_io = priv->bi_end_io;
	bio->bi_private = priv->bi_private;
	bio_endio(bio);

	mempool_free(priv, mpath_ns->ctrl->mpath_req_pool);
}

static void nvme_mpath_priv_bio(struct nvme_mpath_priv *priv, struct bio *bio, 
struct nvme_ns *ns, struct nvme_ns *mpath_ns)
{
	priv->bi_bdev = bio->bi_bdev;
	priv->bi_end_io = bio->bi_end_io;
	priv->bi_private = bio->bi_private;
	priv->bi_flags = bio->bi_flags;
	priv->bi_sector = bio->bi_iter.bi_sector;
	priv->nr_bytes = bio->bi_iter.bi_size;
	priv->bio = bio;
	priv->bi_vcnt = bio->bi_vcnt;
	priv->bi_phys_segments = bio->bi_phys_segments;
	priv->bvec = &bio->bi_io_vec[0];
	/*Count for two connections, so twice the retry logic.*/
	priv->nr_retries = nvme_max_retries;
	priv->start_time = jiffies;
	priv->ns = ns;
	priv->mpath_ns = mpath_ns;
	bio->bi_opf |= REQ_FAILFAST_TRANSPORT;
	bio->bi_private = priv;
	bio->bi_end_io = nvme_mpath_endio;
	bio->bi_bdev = ns->bdev;
}

static blk_qc_t nvme_mpath_make_request(struct request_queue *q, struct bio *bio)

{
	struct nvme_mpath_priv *priv = NULL;
	static struct nvme_ns *ns = NULL;
	struct nvme_ns *mpath_ns = q->queuedata;

	if (test_bit(NVME_NS_REMOVING, &mpath_ns->flags)) {
		goto out_exit_mpath_request;
	}

	priv = mempool_alloc(mpath_ns->ctrl->mpath_req_pool, GFP_ATOMIC);
	if (unlikely(!priv)) {
		dev_err(mpath_ns->ctrl->device, "failed allocating mpath priv request\n");
		goto out_exit_mpath_request;
	}

mpath_retry:
	mutex_lock(&mpath_ns->ctrl->namespaces_mutex);

	list_for_each_entry(ns, &mpath_ns->ctrl->namespaces, mpathlist) {
		if (test_bit(NVME_NS_REMOVING, &ns->flags))
			continue;

		if (test_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags)) {
            		break;
        	}
		if(get_ns_state(ns) == NVME_NS_STATE_ACTIVE) {
			if (ns->mpath_ctrl != mpath_ns->ctrl) {
				mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
				dev_err(mpath_ns->ctrl->device, "Incorrect namespace parent child combination.\n");
				goto mpath_retry;
			}
			nvme_mpath_priv_bio(priv, bio, ns, mpath_ns);
			nvme_mpath_blk_account_io_start(bio, mpath_ns, priv);
			generic_make_request(bio);
			mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
			goto out_mpath_return;
		}
	}

	list_for_each_entry(ns, &mpath_ns->ctrl->namespaces, mpathlist) {
		if(get_ns_state(ns) == NVME_NS_STATE_STANDBY) {
			nvme_mpath_priv_bio(priv, bio, ns, mpath_ns);
			nvme_mpath_blk_account_io_start(bio, mpath_ns, priv);
			mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
			goto out_exit_mpath_request;
		}
	}

	mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
	printk_ratelimited("%s:No devices found nvme%dn%d\n",
			__FUNCTION__, mpath_ns->ctrl->instance, mpath_ns->instance);

out_exit_mpath_request:
	bio->bi_status = BLK_STS_IOERR;
	bio_endio(bio);

out_mpath_return:
	return BLK_QC_T_NONE;
}

static int nvme_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct nvme_ns *ns = bdev->bd_disk->private_data;
	struct nvme_ns *mpath_ns = ns;

	ns = nvme_get_active_ns_for_mpath_ns(mpath_ns);
	if (!ns) {
		/* fail IOCTL if no active ns found for mpath */
		return -ENOTTY;
	}

	if (test_bit(NVME_NS_REMOVING, &ns->flags) || (ns->ctrl->state != NVME_CTRL_LIVE)) {
		return -ENOTTY;
	}


	switch (cmd) {
	case NVME_IOCTL_ID:
		force_successful_syscall_return();
		return ns->ns_id;
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ns->ctrl, NULL, (void __user *)arg);
	case NVME_IOCTL_IO_CMD:
		return nvme_user_cmd(ns->ctrl, ns, (void __user *)arg);
	case NVME_IOCTL_SUBMIT_IO:
		return nvme_submit_io(ns, (void __user *)arg);
	default:
#ifdef CONFIG_NVM
		if (ns->ndev)
			return nvme_nvm_ioctl(ns, cmd, arg);
#endif
		if (is_sed_ioctl(cmd))
			return sed_ioctl(ns->ctrl->opal_dev, cmd,
					 (void __user *) arg);
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
static int nvme_compat_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	return nvme_ioctl(bdev, mode, cmd, arg);
}
#else
#define nvme_compat_ioctl	NULL
#endif

static int nvme_open(struct block_device *bdev, fmode_t mode)
{
	return nvme_get_ns_from_disk(bdev->bd_disk) ? 0 : -ENXIO;
}

static void nvme_release(struct gendisk *disk, fmode_t mode)
{
	struct nvme_ns *ns = disk->private_data;

	if (!test_bit(NVME_NS_ROOT, &ns->flags))
		module_put(ns->ctrl->ops->module);
	nvme_put_ns(ns);
}

static int nvme_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	/* some standard values */
	geo->heads = 1 << 6;
	geo->sectors = 1 << 5;
	geo->cylinders = get_capacity(bdev->bd_disk) >> 11;
	return 0;
}

#ifdef CONFIG_BLK_DEV_INTEGRITY
static void nvme_prep_integrity(struct gendisk *disk, struct nvme_id_ns *id,
		u16 bs)
{
	struct nvme_ns *ns = disk->private_data;
	u16 old_ms = ns->ms;
	u8 pi_type = 0;

	ns->ms = le16_to_cpu(id->lbaf[id->flbas & NVME_NS_FLBAS_LBA_MASK].ms);
	ns->ext = ns->ms && (id->flbas & NVME_NS_FLBAS_META_EXT);

	/* PI implementation requires metadata equal t10 pi tuple size */
	if (ns->ms == sizeof(struct t10_pi_tuple))
		pi_type = id->dps & NVME_NS_DPS_PI_MASK;

	if (blk_get_integrity(disk) &&
	    (ns->pi_type != pi_type || ns->ms != old_ms ||
	     bs != queue_logical_block_size(disk->queue) ||
	     (ns->ms && ns->ext)))
		blk_integrity_unregister(disk);

	ns->pi_type = pi_type;
}

static void nvme_init_integrity(struct nvme_ns *ns)
{
	struct blk_integrity integrity;

	memset(&integrity, 0, sizeof(integrity));
	switch (ns->pi_type) {
	case NVME_NS_DPS_PI_TYPE3:
		integrity.profile = &t10_pi_type3_crc;
		integrity.tag_size = sizeof(u16) + sizeof(u32);
		integrity.flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
		break;
	case NVME_NS_DPS_PI_TYPE1:
	case NVME_NS_DPS_PI_TYPE2:
		integrity.profile = &t10_pi_type1_crc;
		integrity.tag_size = sizeof(u16);
		integrity.flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
		break;
	default:
		integrity.profile = NULL;
		break;
	}
	integrity.tuple_size = ns->ms;
	blk_integrity_register(ns->disk, &integrity);
	blk_queue_max_integrity_segments(ns->queue, 1);
}
#else
static void nvme_prep_integrity(struct gendisk *disk, struct nvme_id_ns *id,
		u16 bs)
{
}
static void nvme_init_integrity(struct nvme_ns *ns)
{
}
#endif /* CONFIG_BLK_DEV_INTEGRITY */

static void nvme_set_chunk_size(struct nvme_ns *ns)
{
	u32 chunk_size = (((u32)ns->noiob) << (ns->lba_shift - 9));
	blk_queue_chunk_sectors(ns->queue, rounddown_pow_of_two(chunk_size));
}

static void nvme_config_discard(struct nvme_ns *ns)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	u32 logical_block_size = queue_logical_block_size(ns->queue);

	BUILD_BUG_ON(PAGE_SIZE / sizeof(struct nvme_dsm_range) <
			NVME_DSM_MAX_RANGES);

	if (ctrl->nr_streams && ns->sws && ns->sgs) {
		unsigned int sz = logical_block_size * ns->sws * ns->sgs;

		ns->queue->limits.discard_alignment = sz;
		ns->queue->limits.discard_granularity = sz;
	} else {
		ns->queue->limits.discard_alignment = logical_block_size;
		ns->queue->limits.discard_granularity = logical_block_size;
	}
	blk_queue_max_discard_sectors(ns->queue, UINT_MAX);
	blk_queue_max_discard_segments(ns->queue, NVME_DSM_MAX_RANGES);
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, ns->queue);

	if (ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES)
		blk_queue_max_write_zeroes_sectors(ns->queue, UINT_MAX);
}

static int nvme_revalidate_ns(struct nvme_ns *ns, struct nvme_id_ns **id)
{
    int res = 0;
    char *buf;
	if (nvme_identify_ns(ns->ctrl, ns->ns_id, id)) {
		dev_warn(ns->ctrl->device, "Identify namespace failed\n");
		return -ENODEV;
	}

	if ((*id)->ncap == 0) {
		kfree(*id);
		return -ENODEV;
	}

	if (ns->ctrl->vs >= NVME_VS(1, 1, 0))
		memcpy(ns->eui, (*id)->eui64, sizeof(ns->eui));
	if (ns->ctrl->vs >= NVME_VS(1, 2, 0))
		memcpy(ns->nguid, (*id)->nguid, sizeof(ns->nguid));
	if (ns->ctrl->vs >= NVME_VS(1, 3, 0)) {
		 /* Don't treat error as fatal we potentially
		  * already have a NGUID or EUI-64
		  */
		if (nvme_identify_ns_descs(ns, ns->ns_id))
			dev_warn(ns->ctrl->device,
				 "%s: Identify Descriptors failed\n", __func__);
	}

	/*Retrieve NGUID or UUID from target device as it needs to be persistent across boot.*/
	if(!test_bit(NVME_NS_ROOT, &ns->flags)) {
		res = nvme_get_mpath_nguid(ns->ctrl, ns->ns_id, &buf);
		if (res) {
			dev_warn(ns->ctrl->dev, "%s: Failed to get NGUID\n", __func__);
		} else {
			memcpy(ns->mpath_nguid, buf, NVME_NIDT_NGUID_LEN);
			kfree(buf);
		}
	}

	return 0;
}

static void __nvme_revalidate_disk(struct gendisk *disk, struct nvme_id_ns *id)
{
	struct nvme_ns *ns = disk->private_data;
	struct nvme_ctrl *ctrl = ns->ctrl;
	u16 bs;

	/*For device to shared,  Bit 0 is set nmic.
	We use this to make device part of multipath group.*/
	ns->nmic = id->nmic;
	/*
	 * If identify namespace failed, use default 512 byte block size so
	 * block layer can use before failing read/write for 0 capacity.
	 */
	ns->lba_shift = id->lbaf[id->flbas & NVME_NS_FLBAS_LBA_MASK].ds;
	if (ns->lba_shift == 0)
		ns->lba_shift = 9;
	bs = 1 << ns->lba_shift;
	ns->noiob = le16_to_cpu(id->noiob);

	blk_mq_freeze_queue(disk->queue);

	if (ctrl->ops->flags & NVME_F_METADATA_SUPPORTED)
		nvme_prep_integrity(disk, id, bs);
	blk_queue_logical_block_size(ns->queue, bs);
	if (ns->noiob)
		nvme_set_chunk_size(ns);
	if (ns->ms && !blk_get_integrity(disk) && !ns->ext)
		nvme_init_integrity(ns);
	if (ns->ms && !(ns->ms == 8 && ns->pi_type) && !blk_get_integrity(disk))
		set_capacity(disk, 0);
	else
		set_capacity(disk, le64_to_cpup(&id->nsze) << (ns->lba_shift - 9));

	if (ctrl->oncs & NVME_CTRL_ONCS_DSM)
		nvme_config_discard(ns);
	blk_mq_unfreeze_queue(disk->queue);
}

static int nvme_revalidate_disk(struct gendisk *disk)
{
	struct nvme_ns *mpath_ns;
	struct nvme_ns *ns = disk->private_data;
	struct nvme_id_ns *id = NULL;
	int ret;
	mpath_ns = ns;

	if (test_bit(NVME_NS_DEAD, &ns->flags)) {
		set_capacity(disk, 0);
		return -ENODEV;
	}

	ns = nvme_get_ns_for_mpath_ns(mpath_ns);

	ret = nvme_revalidate_ns(ns, &id);
	if (ret)
		return ret;

	__nvme_revalidate_disk(disk, id);
	kfree(id);

	return 0;
}

static char nvme_pr_type(enum pr_type type)
{
	switch (type) {
	case PR_WRITE_EXCLUSIVE:
		return 1;
	case PR_EXCLUSIVE_ACCESS:
		return 2;
	case PR_WRITE_EXCLUSIVE_REG_ONLY:
		return 3;
	case PR_EXCLUSIVE_ACCESS_REG_ONLY:
		return 4;
	case PR_WRITE_EXCLUSIVE_ALL_REGS:
		return 5;
	case PR_EXCLUSIVE_ACCESS_ALL_REGS:
		return 6;
	default:
		return 0;
	}
};

static int nvme_pr_command(struct block_device *bdev, u32 cdw10,
				u64 key, u64 sa_key, u8 op)
{
	struct nvme_ns *ns = bdev->bd_disk->private_data;
	struct nvme_command c;
	u8 data[16] = { 0, };

	put_unaligned_le64(key, &data[0]);
	put_unaligned_le64(sa_key, &data[8]);

	memset(&c, 0, sizeof(c));
	c.common.opcode = op;
	c.common.nsid = cpu_to_le32(ns->ns_id);
	c.common.cdw10[0] = cpu_to_le32(cdw10);

	return nvme_submit_sync_cmd(ns->queue, &c, data, 16);
}

static int nvme_pr_register(struct block_device *bdev, u64 old,
		u64 new, unsigned flags)
{
	u32 cdw10;

	if (flags & ~PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;

	cdw10 = old ? 2 : 0;
	cdw10 |= (flags & PR_FL_IGNORE_KEY) ? 1 << 3 : 0;
	cdw10 |= (1 << 30) | (1 << 31); /* PTPL=1 */
	return nvme_pr_command(bdev, cdw10, old, new, nvme_cmd_resv_register);
}

static int nvme_pr_reserve(struct block_device *bdev, u64 key,
		enum pr_type type, unsigned flags)
{
	u32 cdw10;

	if (flags & ~PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;

	cdw10 = nvme_pr_type(type) << 8;
	cdw10 |= ((flags & PR_FL_IGNORE_KEY) ? 1 << 3 : 0);
	return nvme_pr_command(bdev, cdw10, key, 0, nvme_cmd_resv_acquire);
}

static int nvme_pr_preempt(struct block_device *bdev, u64 old, u64 new,
		enum pr_type type, bool abort)
{
	u32 cdw10 = nvme_pr_type(type) << 8 | abort ? 2 : 1;
	return nvme_pr_command(bdev, cdw10, old, new, nvme_cmd_resv_acquire);
}

static int nvme_pr_clear(struct block_device *bdev, u64 key)
{
	u32 cdw10 = 1 | (key ? 1 << 3 : 0);
	return nvme_pr_command(bdev, cdw10, key, 0, nvme_cmd_resv_register);
}

static int nvme_pr_release(struct block_device *bdev, u64 key, enum pr_type type)
{
	u32 cdw10 = nvme_pr_type(type) << 8 | key ? 1 << 3 : 0;
	return nvme_pr_command(bdev, cdw10, key, 0, nvme_cmd_resv_release);
}

static const struct pr_ops nvme_pr_ops = {
	.pr_register	= nvme_pr_register,
	.pr_reserve	= nvme_pr_reserve,
	.pr_release	= nvme_pr_release,
	.pr_preempt	= nvme_pr_preempt,
	.pr_clear	= nvme_pr_clear,
};

#ifdef CONFIG_BLK_SED_OPAL
int nvme_sec_submit(void *data, u16 spsp, u8 secp, void *buffer, size_t len,
		bool send)
{
	struct nvme_ctrl *ctrl = data;
	struct nvme_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	if (send)
		cmd.common.opcode = nvme_admin_security_send;
	else
		cmd.common.opcode = nvme_admin_security_recv;
	cmd.common.nsid = 0;
	cmd.common.cdw10[0] = cpu_to_le32(((u32)secp) << 24 | ((u32)spsp) << 8);
	cmd.common.cdw10[1] = cpu_to_le32(len);

	return __nvme_submit_sync_cmd(ctrl->admin_q, &cmd, NULL, buffer, len,
				      ADMIN_TIMEOUT, NVME_QID_ANY, 1, 0);
}
EXPORT_SYMBOL_GPL(nvme_sec_submit);
#endif /* CONFIG_BLK_SED_OPAL */

static const struct block_device_operations nvme_fops = {
	.owner		= THIS_MODULE,
	.ioctl		= nvme_ioctl,
	.compat_ioctl	= nvme_compat_ioctl,
	.open		= nvme_open,
	.release	= nvme_release,
	.getgeo		= nvme_getgeo,
	.revalidate_disk= nvme_revalidate_disk,
	.pr_ops		= &nvme_pr_ops,
};

static int nvme_wait_ready(struct nvme_ctrl *ctrl, u64 cap, bool enabled)
{
	unsigned long timeout =
		((NVME_CAP_TIMEOUT(cap) + 1) * HZ / 2) + jiffies;
	u32 csts, bit = enabled ? NVME_CSTS_RDY : 0;
	int ret;

	while ((ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts)) == 0) {
		if (csts == ~0)
			return -ENODEV;
		if ((csts & NVME_CSTS_RDY) == bit)
			break;

		msleep(100);
		if (fatal_signal_pending(current))
			return -EINTR;
		if (time_after(jiffies, timeout)) {
			dev_err(ctrl->device,
				"Device not ready; aborting %s\n", enabled ?
						"initialisation" : "reset");
			return -ENODEV;
		}
	}

	return ret;
}

/*
 * If the device has been passed off to us in an enabled state, just clear
 * the enabled bit.  The spec says we should set the 'shutdown notification
 * bits', but doing so may cause the device to complete commands to the
 * admin queue ... and we don't know what memory that might be pointing at!
 */
int nvme_disable_ctrl(struct nvme_ctrl *ctrl, u64 cap)
{
	int ret;

	ctrl->ctrl_config &= ~NVME_CC_SHN_MASK;
	ctrl->ctrl_config &= ~NVME_CC_ENABLE;

	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);
	if (ret)
		return ret;

	if (ctrl->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY)
		msleep(NVME_QUIRK_DELAY_AMOUNT);

	return nvme_wait_ready(ctrl, cap, false);
}
EXPORT_SYMBOL_GPL(nvme_disable_ctrl);

int nvme_enable_ctrl(struct nvme_ctrl *ctrl, u64 cap)
{
	/*
	 * Default to a 4K page size, with the intention to update this
	 * path in the future to accomodate architectures with differing
	 * kernel and IO page sizes.
	 */
	unsigned dev_page_min = NVME_CAP_MPSMIN(cap) + 12, page_shift = 12;
	int ret;

	if (page_shift < dev_page_min) {
		dev_err(ctrl->device,
			"Minimum device page size %u too large for host (%u)\n",
			1 << dev_page_min, 1 << page_shift);
		return -ENODEV;
	}

	ctrl->page_size = 1 << page_shift;

	ctrl->ctrl_config = NVME_CC_CSS_NVM;
	ctrl->ctrl_config |= (page_shift - 12) << NVME_CC_MPS_SHIFT;
	ctrl->ctrl_config |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
	ctrl->ctrl_config |= NVME_CC_IOSQES | NVME_CC_IOCQES;
	ctrl->ctrl_config |= NVME_CC_ENABLE;

	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);
	if (ret)
		return ret;
	return nvme_wait_ready(ctrl, cap, true);
}
EXPORT_SYMBOL_GPL(nvme_enable_ctrl);

int nvme_shutdown_ctrl(struct nvme_ctrl *ctrl)
{
	unsigned long timeout = jiffies + (shutdown_timeout * HZ);
	u32 csts;
	int ret;

	ctrl->ctrl_config &= ~NVME_CC_SHN_MASK;
	ctrl->ctrl_config |= NVME_CC_SHN_NORMAL;

	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);
	if (ret)
		return ret;

	while ((ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts)) == 0) {
		if ((csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_CMPLT)
			break;

		msleep(100);
		if (fatal_signal_pending(current))
			return -EINTR;
		if (time_after(jiffies, timeout)) {
			dev_err(ctrl->device,
				"Device shutdown incomplete; abort shutdown\n");
			return -ENODEV;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(nvme_shutdown_ctrl);

static void nvme_set_queue_limits(struct nvme_ctrl *ctrl,
		struct request_queue *q)
{
	bool vwc = false;

	if (ctrl->max_hw_sectors) {
		u32 max_segments =
			(ctrl->max_hw_sectors / (ctrl->page_size >> 9)) + 1;

		blk_queue_max_hw_sectors(q, ctrl->max_hw_sectors);
		blk_queue_max_segments(q, min_t(u32, max_segments, USHRT_MAX));
	}
	if (ctrl->quirks & NVME_QUIRK_STRIPE_SIZE)
		blk_queue_chunk_sectors(q, ctrl->max_hw_sectors);
	blk_queue_virt_boundary(q, ctrl->page_size - 1);
	if (ctrl->vwc & NVME_CTRL_VWC_PRESENT)
		vwc = true;
	blk_queue_write_cache(q, vwc, vwc);
}

static int nvme_configure_timestamp(struct nvme_ctrl *ctrl)
{
	__le64 ts;
	int ret;

	if (!(ctrl->oncs & NVME_CTRL_ONCS_TIMESTAMP))
		return 0;

	ts = cpu_to_le64(ktime_to_ms(ktime_get_real()));
	ret = nvme_set_features(ctrl, NVME_FEAT_TIMESTAMP, 0, &ts, sizeof(ts),
			NULL);
	if (ret)
		dev_warn_once(ctrl->device,
			"could not set timestamp (%d)\n", ret);
	return ret;
}

static int nvme_configure_apst(struct nvme_ctrl *ctrl)
{
	/*
	 * APST (Autonomous Power State Transition) lets us program a
	 * table of power state transitions that the controller will
	 * perform automatically.  We configure it with a simple
	 * heuristic: we are willing to spend at most 2% of the time
	 * transitioning between power states.  Therefore, when running
	 * in any given state, we will enter the next lower-power
	 * non-operational state after waiting 50 * (enlat + exlat)
	 * microseconds, as long as that state's exit latency is under
	 * the requested maximum latency.
	 *
	 * We will not autonomously enter any non-operational state for
	 * which the total latency exceeds ps_max_latency_us.  Users
	 * can set ps_max_latency_us to zero to turn off APST.
	 */

	unsigned apste;
	struct nvme_feat_auto_pst *table;
	u64 max_lat_us = 0;
	int max_ps = -1;
	int ret;

	/*
	 * If APST isn't supported or if we haven't been initialized yet,
	 * then don't do anything.
	 */
	if (!ctrl->apsta)
		return 0;

	if (ctrl->npss > 31) {
		dev_warn(ctrl->device, "NPSS is invalid; not using APST\n");
		return 0;
	}

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return 0;

	if (!ctrl->apst_enabled || ctrl->ps_max_latency_us == 0) {
		/* Turn off APST. */
		apste = 0;
		dev_dbg(ctrl->device, "APST disabled\n");
	} else {
		__le64 target = cpu_to_le64(0);
		int state;

		/*
		 * Walk through all states from lowest- to highest-power.
		 * According to the spec, lower-numbered states use more
		 * power.  NPSS, despite the name, is the index of the
		 * lowest-power state, not the number of states.
		 */
		for (state = (int)ctrl->npss; state >= 0; state--) {
			u64 total_latency_us, exit_latency_us, transition_ms;

			if (target)
				table->entries[state] = target;

			/*
			 * Don't allow transitions to the deepest state
			 * if it's quirked off.
			 */
			if (state == ctrl->npss &&
			    (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS))
				continue;

			/*
			 * Is this state a useful non-operational state for
			 * higher-power states to autonomously transition to?
			 */
			if (!(ctrl->psd[state].flags &
			      NVME_PS_FLAGS_NON_OP_STATE))
				continue;

			exit_latency_us =
				(u64)le32_to_cpu(ctrl->psd[state].exit_lat);
			if (exit_latency_us > ctrl->ps_max_latency_us)
				continue;

			total_latency_us =
				exit_latency_us +
				le32_to_cpu(ctrl->psd[state].entry_lat);

			/*
			 * This state is good.  Use it as the APST idle
			 * target for higher power states.
			 */
			transition_ms = total_latency_us + 19;
			do_div(transition_ms, 20);
			if (transition_ms > (1 << 24) - 1)
				transition_ms = (1 << 24) - 1;

			target = cpu_to_le64((state << 3) |
					     (transition_ms << 8));

			if (max_ps == -1)
				max_ps = state;

			if (total_latency_us > max_lat_us)
				max_lat_us = total_latency_us;
		}

		apste = 1;

		if (max_ps == -1) {
			dev_dbg(ctrl->device, "APST enabled but no non-operational states are available\n");
		} else {
			dev_dbg(ctrl->device, "APST enabled: max PS = %d, max round-trip latency = %lluus, table = %*phN\n",
				max_ps, max_lat_us, (int)sizeof(*table), table);
		}
	}

	ret = nvme_set_features(ctrl, NVME_FEAT_AUTO_PST, apste,
				table, sizeof(*table), NULL);
	if (ret)
		dev_err(ctrl->device, "failed to set APST feature (%d)\n", ret);

	kfree(table);
	return ret;
}

static void nvme_set_latency_tolerance(struct device *dev, s32 val)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	u64 latency;

	switch (val) {
	case PM_QOS_LATENCY_TOLERANCE_NO_CONSTRAINT:
	case PM_QOS_LATENCY_ANY:
		latency = U64_MAX;
		break;

	default:
		latency = val;
	}

	if (ctrl->ps_max_latency_us != latency) {
		ctrl->ps_max_latency_us = latency;
		nvme_configure_apst(ctrl);
	}
}

struct nvme_core_quirk_entry {
	/*
	 * NVMe model and firmware strings are padded with spaces.  For
	 * simplicity, strings in the quirk table are padded with NULLs
	 * instead.
	 */
	u16 vid;
	const char *mn;
	const char *fr;
	unsigned long quirks;
};

static const struct nvme_core_quirk_entry core_quirks[] = {
	{
		/*
		 * This Toshiba device seems to die using any APST states.  See:
		 * https://bugs.launchpad.net/ubuntu/+source/linux/+bug/1678184/comments/11
		 */
		.vid = 0x1179,
		.mn = "THNSF5256GPUK TOSHIBA",
		.quirks = NVME_QUIRK_NO_APST,
	}
};

/* match is null-terminated but idstr is space-padded. */
static bool string_matches(const char *idstr, const char *match, size_t len)
{
	size_t matchlen;

	if (!match)
		return true;

	matchlen = strlen(match);
	WARN_ON_ONCE(matchlen > len);

	if (memcmp(idstr, match, matchlen))
		return false;

	for (; matchlen < len; matchlen++)
		if (idstr[matchlen] != ' ')
			return false;

	return true;
}

static bool quirk_matches(const struct nvme_id_ctrl *id,
			  const struct nvme_core_quirk_entry *q)
{
	return q->vid == le16_to_cpu(id->vid) &&
		string_matches(id->mn, q->mn, sizeof(id->mn)) &&
		string_matches(id->fr, q->fr, sizeof(id->fr));
}

static void nvme_init_subnqn(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	size_t nqnlen;
	int off;

	nqnlen = strnlen(id->subnqn, NVMF_NQN_SIZE);
	if (nqnlen > 0 && nqnlen < NVMF_NQN_SIZE) {
		strcpy(ctrl->subnqn, id->subnqn);
		return;
	}

	if (ctrl->vs >= NVME_VS(1, 2, 1))
		dev_warn(ctrl->device, "missing or invalid SUBNQN field.\n");

	/* Generate a "fake" NQN per Figure 254 in NVMe 1.3 + ECN 001 */
	off = snprintf(ctrl->subnqn, NVMF_NQN_SIZE,
			"nqn.2014.08.org.nvmexpress:%4x%4x",
			le16_to_cpu(id->vid), le16_to_cpu(id->ssvid));
	memcpy(ctrl->subnqn + off, id->sn, sizeof(id->sn));
	off += sizeof(id->sn);
	memcpy(ctrl->subnqn + off, id->mn, sizeof(id->mn));
	off += sizeof(id->mn);
	memset(ctrl->subnqn + off, 0, sizeof(ctrl->subnqn) - off);
}

/*
 * Initialize the cached copies of the Identify data and various controller
 * register in our nvme_ctrl structure.  This should be called as soon as
 * the admin queue is fully up and running.
 */
int nvme_init_identify(struct nvme_ctrl *ctrl)
{
	struct nvme_id_ctrl *id;
	u64 cap;
	int ret, page_shift;
	u32 max_hw_sectors;
	bool prev_apst_enabled;

	ret = ctrl->ops->reg_read32(ctrl, NVME_REG_VS, &ctrl->vs);
	if (ret) {
		dev_err(ctrl->device, "Reading VS failed (%d)\n", ret);
		return ret;
	}

	ret = ctrl->ops->reg_read64(ctrl, NVME_REG_CAP, &cap);
	if (ret) {
		dev_err(ctrl->device, "Reading CAP failed (%d)\n", ret);
		return ret;
	}
	page_shift = NVME_CAP_MPSMIN(cap) + 12;

	if (ctrl->vs >= NVME_VS(1, 1, 0))
		ctrl->subsystem = NVME_CAP_NSSRC(cap);

	ret = nvme_identify_ctrl(ctrl, &id);
	if (ret) {
		dev_err(ctrl->device, "Identify Controller failed (%d)\n", ret);
		return -EIO;
	}

	nvme_init_subnqn(ctrl, id);

	if (!ctrl->identified) {
		/*
		 * Check for quirks.  Quirk can depend on firmware version,
		 * so, in principle, the set of quirks present can change
		 * across a reset.  As a possible future enhancement, we
		 * could re-scan for quirks every time we reinitialize
		 * the device, but we'd have to make sure that the driver
		 * behaves intelligently if the quirks change.
		 */

		int i;

		for (i = 0; i < ARRAY_SIZE(core_quirks); i++) {
			if (quirk_matches(id, &core_quirks[i]))
				ctrl->quirks |= core_quirks[i].quirks;
		}
	}

	if (force_apst && (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS)) {
		dev_warn(ctrl->device, "forcibly allowing all power states due to nvme_core.force_apst -- use at your own risk\n");
		ctrl->quirks &= ~NVME_QUIRK_NO_DEEPEST_PS;
	}

	ctrl->oacs = le16_to_cpu(id->oacs);
	ctrl->vid = le16_to_cpu(id->vid);
	ctrl->oncs = le16_to_cpup(&id->oncs);
	atomic_set(&ctrl->abort_limit, id->acl + 1);
	ctrl->vwc = id->vwc;
	ctrl->cntlid = le16_to_cpup(&id->cntlid);
	memcpy(ctrl->serial, id->sn, sizeof(id->sn));
	memcpy(ctrl->model, id->mn, sizeof(id->mn));
	memcpy(ctrl->firmware_rev, id->fr, sizeof(id->fr));
	if (id->mdts)
		max_hw_sectors = 1 << (id->mdts + page_shift - 9);
	else
		max_hw_sectors = UINT_MAX;
	ctrl->max_hw_sectors =
		min_not_zero(ctrl->max_hw_sectors, max_hw_sectors);

	nvme_set_queue_limits(ctrl, ctrl->admin_q);
	ctrl->sgls = le32_to_cpu(id->sgls);
	ctrl->kas = le16_to_cpu(id->kas);

	ctrl->npss = id->npss;
	ctrl->apsta = id->apsta;
	prev_apst_enabled = ctrl->apst_enabled;
	if (ctrl->quirks & NVME_QUIRK_NO_APST) {
		if (force_apst && id->apsta) {
			dev_warn(ctrl->device, "forcibly allowing APST due to nvme_core.force_apst -- use at your own risk\n");
			ctrl->apst_enabled = true;
		} else {
			ctrl->apst_enabled = false;
		}
	} else {
		ctrl->apst_enabled = id->apsta;
	}
	memcpy(ctrl->psd, id->psd, sizeof(ctrl->psd));

	if (ctrl->ops->flags & NVME_F_FABRICS) {
		ctrl->icdoff = le16_to_cpu(id->icdoff);
		ctrl->ioccsz = le32_to_cpu(id->ioccsz);
		ctrl->iorcsz = le32_to_cpu(id->iorcsz);
		ctrl->maxcmd = le16_to_cpu(id->maxcmd);

		/*
		 * In fabrics we need to verify the cntlid matches the
		 * admin connect
		 */
		if (ctrl->cntlid != le16_to_cpu(id->cntlid)) {
			ret = -EINVAL;
			goto out_free;
		}

		if (!ctrl->opts->discovery_nqn && !ctrl->kas) {
			dev_err(ctrl->device,
				"keep-alive support is mandatory for fabrics\n");
			ret = -EINVAL;
			goto out_free;
		}
	} else {
		ctrl->cntlid = le16_to_cpu(id->cntlid);
		ctrl->hmpre = le32_to_cpu(id->hmpre);
		ctrl->hmmin = le32_to_cpu(id->hmmin);
	}

	kfree(id);

	if (ctrl->apst_enabled && !prev_apst_enabled)
		dev_pm_qos_expose_latency_tolerance(ctrl->device);
	else if (!ctrl->apst_enabled && prev_apst_enabled)
		dev_pm_qos_hide_latency_tolerance(ctrl->device);

	ret = nvme_configure_apst(ctrl);
	if (ret < 0)
		return ret;

	ret = nvme_configure_timestamp(ctrl);
	if (ret < 0)
		return ret;

	ret = nvme_configure_directives(ctrl);
	if (ret < 0)
		return ret;

	ctrl->identified = true;

	return 0;

out_free:
	kfree(id);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_init_identify);

static int nvme_dev_open(struct inode *inode, struct file *file)
{
	struct nvme_ctrl *ctrl;
	int instance = iminor(inode);
	int ret = -ENODEV;

	spin_lock(&dev_list_lock);
	list_for_each_entry(ctrl, &nvme_ctrl_list, node) {
		if (ctrl->instance != instance)
			continue;

		if (!ctrl->admin_q) {
			ret = -EWOULDBLOCK;
			break;
		}
		if (!kref_get_unless_zero(&ctrl->kref))
			break;
		file->private_data = ctrl;
		ret = 0;
		break;
	}
	spin_unlock(&dev_list_lock);

	return ret;
}

static int nvme_dev_release(struct inode *inode, struct file *file)
{
	nvme_put_ctrl(file->private_data);
	return 0;
}

static int nvme_dev_user_cmd(struct nvme_ctrl *ctrl, void __user *argp)
{
	struct nvme_ns *ns;
	int ret;

	mutex_lock(&ctrl->namespaces_mutex);
	if (list_empty(&ctrl->namespaces)) {
		ret = -ENOTTY;
		goto out_unlock;
	}

	ns = list_first_entry(&ctrl->namespaces, struct nvme_ns, list);
	if (ns != list_last_entry(&ctrl->namespaces, struct nvme_ns, list)) {
		dev_warn(ctrl->device,
			"NVME_IOCTL_IO_CMD not supported when multiple namespaces present!\n");
		ret = -EINVAL;
		goto out_unlock;
	}

	dev_warn(ctrl->device,
		"using deprecated NVME_IOCTL_IO_CMD ioctl on the char device!\n");
	kref_get(&ns->kref);
	mutex_unlock(&ctrl->namespaces_mutex);

	ret = nvme_user_cmd(ctrl, ns, argp);
	nvme_put_ns(ns);
	return ret;

out_unlock:
	mutex_unlock(&ctrl->namespaces_mutex);
	return ret;
}

static long nvme_dev_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct nvme_ctrl *ctrl = file->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ctrl, NULL, argp);
	case NVME_IOCTL_IO_CMD:
		return nvme_dev_user_cmd(ctrl, argp);
	case NVME_IOCTL_RESET:
		dev_warn(ctrl->device, "resetting controller\n");
		return nvme_reset_ctrl_sync(ctrl);
	case NVME_IOCTL_SUBSYS_RESET:
		return nvme_reset_subsystem(ctrl);
	case NVME_IOCTL_RESCAN:
		nvme_queue_scan(ctrl);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations nvme_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= nvme_dev_open,
	.release	= nvme_dev_release,
	.unlocked_ioctl	= nvme_dev_ioctl,
	.compat_ioctl	= nvme_dev_ioctl,
};

static ssize_t nvme_sysfs_reset(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	int ret;

	ret = nvme_reset_ctrl_sync(ctrl);
	if (ret < 0)
		return ret;
	return count;
}
static DEVICE_ATTR(reset_controller, S_IWUSR, NULL, nvme_sysfs_reset);

static ssize_t nvme_sysfs_rescan(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);

	nvme_queue_scan(ctrl);
	return count;
}
static DEVICE_ATTR(rescan_controller, S_IWUSR, NULL, nvme_sysfs_rescan);

static ssize_t wwid_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);
	struct nvme_ctrl *ctrl = ns->ctrl;
	int serial_len = sizeof(ctrl->serial);
	int model_len = sizeof(ctrl->model);

	if (!uuid_is_null(&ns->uuid))
		return sprintf(buf, "uuid.%pU\n", &ns->uuid);

	if (memchr_inv(ns->nguid, 0, sizeof(ns->nguid)))
		return sprintf(buf, "eui.%16phN\n", ns->nguid);

	if (memchr_inv(ns->eui, 0, sizeof(ns->eui)))
		return sprintf(buf, "eui.%8phN\n", ns->eui);

	while (serial_len > 0 && (ctrl->serial[serial_len - 1] == ' ' ||
				  ctrl->serial[serial_len - 1] == '\0'))
		serial_len--;
	while (model_len > 0 && (ctrl->model[model_len - 1] == ' ' ||
				 ctrl->model[model_len - 1] == '\0'))
		model_len--;

	return sprintf(buf, "nvme.%04x-%*phN-%*phN-%08x\n", ctrl->vid,
		serial_len, ctrl->serial, model_len, ctrl->model, ns->ns_id);
}
static DEVICE_ATTR(wwid, S_IRUGO, wwid_show, NULL);

static ssize_t nguid_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);
	return sprintf(buf, "%pU\n", ns->nguid);
}
static DEVICE_ATTR(nguid, S_IRUGO, nguid_show, NULL);

static ssize_t uuid_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);

	/* For backward compatibility expose the NGUID to userspace if
	 * we have no UUID set
	 */
	if (uuid_is_null(&ns->uuid)) {
		printk_ratelimited(KERN_WARNING
				   "No UUID available providing old NGUID\n");
		return sprintf(buf, "%pU\n", ns->nguid);
	}
	return sprintf(buf, "%pU\n", &ns->uuid);
}
static DEVICE_ATTR(uuid, S_IRUGO, uuid_show, NULL);

static ssize_t eui_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);
	return sprintf(buf, "%8phd\n", ns->eui);
}
static DEVICE_ATTR(eui, S_IRUGO, eui_show, NULL);

static ssize_t active_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);
	int ret = 0;
	if (!test_bit(NVME_NS_ROOT, &ns->flags)) {
		ret = sprintf(buf, "%d\n", ns->active);
	}
	return ret;
}
static DEVICE_ATTR(active, S_IRUGO, active_show, NULL);

static ssize_t active_path_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nvme_ns *mpath_ns = nvme_get_ns_from_dev(dev);
	struct nvme_ns *nsa;
	int ret = 0;
	if (test_bit(NVME_NS_ROOT, &mpath_ns->flags)) {
		mutex_lock(&mpath_ns->ctrl->namespaces_mutex);
		list_for_each_entry(nsa, &mpath_ns->ctrl->namespaces, mpathlist) {
			if(nsa->active) {
				ret = sprintf(buf, "nvme%dn%d\n", nsa->ctrl->instance, nsa->instance);
				break;
			}
		}
		mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
	}
	return ret;
}
static DEVICE_ATTR(active_path, S_IRUGO, active_path_show, NULL);

static ssize_t nsid_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);
	return sprintf(buf, "%d\n", ns->ns_id);
}
static DEVICE_ATTR(nsid, S_IRUGO, nsid_show, NULL);

static ssize_t mpath_nguid_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);
	return sprintf(buf, "%pU\n", ns->mpath_nguid);
}
static DEVICE_ATTR(mpath_nguid, S_IRUGO, mpath_nguid_show, NULL);

static struct attribute *nvme_ns_attrs[] = {
	&dev_attr_wwid.attr,
	&dev_attr_uuid.attr,
	&dev_attr_nguid.attr,
	&dev_attr_eui.attr,
	&dev_attr_nsid.attr,
	&dev_attr_active.attr,
	&dev_attr_active_path.attr,
	&dev_attr_mpath_nguid.attr,
	NULL,
};

static umode_t nvme_ns_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);

	if (a == &dev_attr_uuid.attr) {
		if (uuid_is_null(&ns->uuid) ||
		    !memchr_inv(ns->nguid, 0, sizeof(ns->nguid)))
			return 0;
	}
	if (a == &dev_attr_nguid.attr) {
		if (!memchr_inv(ns->nguid, 0, sizeof(ns->nguid)))
			return 0;
	}
	if (a == &dev_attr_eui.attr) {
		if (!memchr_inv(ns->eui, 0, sizeof(ns->eui)))
			return 0;
	}
	return a->mode;
}

static const struct attribute_group nvme_ns_attr_group = {
	.attrs		= nvme_ns_attrs,
	.is_visible	= nvme_ns_attrs_are_visible,
};

#define nvme_show_str_function(field)						\
static ssize_t  field##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)		\
{										\
        struct nvme_ctrl *ctrl = dev_get_drvdata(dev);				\
        return sprintf(buf, "%.*s\n", (int)sizeof(ctrl->field), ctrl->field);	\
}										\
static DEVICE_ATTR(field, S_IRUGO, field##_show, NULL);

#define nvme_show_int_function(field)						\
static ssize_t  field##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)		\
{										\
        struct nvme_ctrl *ctrl = dev_get_drvdata(dev);				\
        return sprintf(buf, "%d\n", ctrl->field);	\
}										\
static DEVICE_ATTR(field, S_IRUGO, field##_show, NULL);

nvme_show_str_function(model);
nvme_show_str_function(serial);
nvme_show_str_function(firmware_rev);
nvme_show_int_function(cntlid);

static ssize_t nvme_sysfs_delete(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);

	if (test_bit(NVME_CTRL_MULTIPATH, &ctrl->flags))
		return 0;

	ctrl->delete_cmd = 1;
	if (device_remove_file_self(dev, attr))
		ctrl->ops->delete_ctrl(ctrl);
	return count;
}
static DEVICE_ATTR(delete_controller, S_IWUSR, NULL, nvme_sysfs_delete);

static ssize_t nvme_sysfs_show_transport(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n", ctrl->ops->name);
}
static DEVICE_ATTR(transport, S_IRUGO, nvme_sysfs_show_transport, NULL);

static ssize_t nvme_sysfs_show_state(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	static const char *const state_name[] = {
		[NVME_CTRL_NEW]		= "new",
		[NVME_CTRL_LIVE]	= "live",
		[NVME_CTRL_RESETTING]	= "resetting",
		[NVME_CTRL_RECONNECTING]= "reconnecting",
		[NVME_CTRL_DELETING]	= "deleting",
		[NVME_CTRL_DEAD]	= "dead",
	};

	if ((unsigned)ctrl->state < ARRAY_SIZE(state_name) &&
	    state_name[ctrl->state])
		return sprintf(buf, "%s\n", state_name[ctrl->state]);

	return sprintf(buf, "unknown state\n");
}

static DEVICE_ATTR(state, S_IRUGO, nvme_sysfs_show_state, NULL);

static ssize_t nvme_sysfs_show_subsysnqn(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	struct nvme_ns *nsa = NULL;
	ssize_t ret = 0;
	if (test_bit(NVME_CTRL_MULTIPATH, &ctrl->flags)) {
		/* mpath ctrl, iterate and send it to nsa->ctrl */
		mutex_lock(&ctrl->namespaces_mutex);
		list_for_each_entry(nsa, &ctrl->namespaces, mpathlist) {
			if (nsa->ctrl && nsa->ctrl->ops) {
				ret = snprintf(buf, PAGE_SIZE, "%s\n", nsa->ctrl->subnqn);
				break;
			}
		}
		mutex_unlock(&ctrl->namespaces_mutex);
	} else {
		ret = snprintf(buf, PAGE_SIZE, "%s\n", ctrl->subnqn);
	}
	return ret;
}
static DEVICE_ATTR(subsysnqn, S_IRUGO, nvme_sysfs_show_subsysnqn, NULL);

static ssize_t nvme_sysfs_show_address(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);

	return ctrl->ops->get_address(ctrl, buf, PAGE_SIZE);
}
static DEVICE_ATTR(address, S_IRUGO, nvme_sysfs_show_address, NULL);

static struct attribute *nvme_dev_attrs[] = {
	&dev_attr_reset_controller.attr,
	&dev_attr_rescan_controller.attr,
	&dev_attr_model.attr,
	&dev_attr_serial.attr,
	&dev_attr_firmware_rev.attr,
	&dev_attr_cntlid.attr,
	&dev_attr_delete_controller.attr,
	&dev_attr_transport.attr,
	&dev_attr_subsysnqn.attr,
	&dev_attr_address.attr,
	&dev_attr_state.attr,
	NULL
};

static umode_t nvme_dev_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);

	if (a == &dev_attr_delete_controller.attr && !ctrl->ops->delete_ctrl)
		return 0;
	if (a == &dev_attr_address.attr && !ctrl->ops->get_address)
		return 0;

	return a->mode;
}

static struct attribute_group nvme_dev_attrs_group = {
	.attrs		= nvme_dev_attrs,
	.is_visible	= nvme_dev_attrs_are_visible,
};

static const struct attribute_group *nvme_dev_attr_groups[] = {
	&nvme_dev_attrs_group,
	NULL,
};

static int ns_cmp(void *priv, struct list_head *a, struct list_head *b)
{
	struct nvme_ns *nsa = container_of(a, struct nvme_ns, list);
	struct nvme_ns *nsb = container_of(b, struct nvme_ns, list);

	return nsa->ns_id - nsb->ns_id;
}

static struct nvme_ns *nvme_find_get_ns(struct nvme_ctrl *ctrl, unsigned nsid)
{
	struct nvme_ns *ns, *ret = NULL;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		if (ns->ns_id == nsid) {
			kref_get(&ns->kref);
			ret = ns;
			break;
		}
		if (ns->ns_id > nsid)
			break;
	}
	mutex_unlock(&ctrl->namespaces_mutex);
	return ret;
}

static int nvme_setup_streams_ns(struct nvme_ctrl *ctrl, struct nvme_ns *ns)
{
	struct streams_directive_params s;
	int ret;

	if (!ctrl->nr_streams)
		return 0;

	ret = nvme_get_stream_params(ctrl, &s, ns->ns_id);
	if (ret)
		return ret;

	ns->sws = le32_to_cpu(s.sws);
	ns->sgs = le16_to_cpu(s.sgs);

	if (ns->sws) {
		unsigned int bs = 1 << ns->lba_shift;

		blk_queue_io_min(ns->queue, bs * ns->sws);
		if (ns->sgs)
			blk_queue_io_opt(ns->queue, bs * ns->sws * ns->sgs);
	}

	return 0;
}

static struct nvme_ns *nvme_find_get_mpath_ns(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns = NULL;
	mutex_lock(&ctrl->namespaces_mutex);
    list_for_each_entry(ns, &ctrl->mpath_namespace, list) {
		if (ns)
			break;
	}
	mutex_unlock(&ctrl->namespaces_mutex);
	return ns;
}

/*Adding namespace to multipath list under multipath controller*/
static void nvme_add_ns_mpath_ctrl(struct nvme_ns *ns)
{
	struct nvme_ns *mpath_ns = NULL;
	mpath_ns = nvme_find_get_mpath_ns(ns->mpath_ctrl);
	mutex_lock(&ns->mpath_ctrl->namespaces_mutex);
	list_add_tail(&ns->mpathlist, &ns->mpath_ctrl->namespaces);
	test_and_set_bit(NVME_CTRL_MPATH_CHILD, &ns->ctrl->flags);
	test_and_set_bit(NVME_NS_MULTIPATH, &ns->flags);
	mutex_unlock(&ns->mpath_ctrl->namespaces_mutex);
	kref_get(&mpath_ns->kref);
}

/*Deleting namespace from multipath list under multipath controller*/
static int nvme_del_ns_mpath_ctrl(struct nvme_ns *ns)
{

	struct nvme_ns *mpath_ns = NULL, *nsa = NULL, *next;

	if (!ns->mpath_ctrl)
		return NVME_NO_MPATH_NS_AVAIL;
	mpath_ns = nvme_find_get_mpath_ns(ns->mpath_ctrl);
	mutex_lock(&mpath_ns->ctrl->namespaces_mutex);
	test_and_clear_bit(NVME_NS_MULTIPATH, &ns->flags);
	list_del_init(&ns->mpathlist);
	list_for_each_entry_safe(nsa, next, &mpath_ns->ctrl->namespaces, mpathlist) {
		if (nsa == ns) {
			list_del_init(&ns->mpathlist);
			continue;
		}
	}
	mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);

	/*Check if we were the last device to a given head or parent device.
	If last device then remove head device also.*/
	if (mpath_ns == nvme_get_ns_for_mpath_ns(mpath_ns)) {
		nvme_put_ns(mpath_ns);
		nvme_mpath_ns_remove(mpath_ns);
		/*cancel delayed work as we are the last device */
		cancel_delayed_work_sync(&ns->mpath_ctrl->cu_work);
		return NVME_NO_MPATH_NS_AVAIL;
	} else {
		blk_mq_freeze_queue(ns->disk->queue);
		set_capacity(ns->disk, 0);
		blk_mq_unfreeze_queue(ns->disk->queue);
		revalidate_disk(ns->disk);
		nvme_put_ns(mpath_ns);
		return NVME_MPATH_NS_AVAIL;
	}
}


static struct nvme_ns *nvme_alloc_mpath_ns(struct nvme_ns *nsa)
{
	struct gendisk *disk;
	struct nvme_id_ns *id;
	char disk_name[DISK_NAME_LEN];
	char devpath[DISK_NAME_LEN+4];
	struct nvme_ctrl *ctrl = NULL;
	struct nvme_ns *ns = NULL;
	int node;

	printk("%s:(%d)\n", __FUNCTION__,__LINE__);
	ctrl = nvme_init_mpath_ctrl(nsa->ctrl);
	if (!ctrl)
		return NULL;

	node = dev_to_node(ctrl->dev);
	ns = kzalloc_node(sizeof(*ns), GFP_KERNEL, node);
	if (!ns)
		goto out_free_ctrl;
	ns->ctrl = ctrl;
	ns->instance = ida_simple_get(&ns->ctrl->ns_ida, 1, 0, GFP_KERNEL);
	if (ns->instance < 0)
		goto out_free_ns;

	ns->queue = blk_alloc_queue(GFP_KERNEL);
	if (IS_ERR(ns->queue))
		goto out_release_instance;

	blk_queue_make_request(ns->queue, nvme_mpath_make_request);

	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, ns->queue);
	ns->queue->queuedata = ns;
	kref_init(&ns->kref);
	ns->ns_id = nsa->ns_id;
	ns->lba_shift = 9; /* set to a default value for 512 until disk is validated */

	test_and_set_bit(NVME_NS_ROOT, &ns->flags);
	blk_queue_logical_block_size(ns->queue, 1 << ns->lba_shift);
	nvme_set_queue_limits(ctrl, ns->queue);
	blk_queue_rq_timeout(ns->queue, mpath_io_timeout * HZ);
	sprintf(disk_name, "mpnvme%dn%d", ctrl->instance, ns->instance);
	sprintf(devpath, "/dev/mpnvme%dn%d", ctrl->instance, ns->instance);
	if (nvme_revalidate_ns(nsa, &id))
		goto out_free_queue;

	disk = alloc_disk_node(0, node);
	if (!disk)
		goto out_free_id;

	disk->fops = &nvme_fops;
	disk->private_data = ns;
	disk->queue = ns->queue;
	disk->flags = GENHD_FL_EXT_DEVT;
	memcpy(disk->disk_name, disk_name, DISK_NAME_LEN);
	ns->disk = disk;
	__nvme_revalidate_disk(disk, id);
	init_waitqueue_head(&ns->fq_full);
	init_waitqueue_entry(&ns->fq_cong_wait, nvme_mpath_thread);
	bio_list_init(&ns->fq_cong);
	nsa->mpath_ctrl = ns->ctrl;
	nsa->ctrl->mpath_ctrl = (void *)ns->ctrl;
	mutex_lock(&ctrl->namespaces_mutex);
	list_add_tail(&ns->list, &ctrl->mpath_namespace);
	mutex_unlock(&ctrl->namespaces_mutex);
	nvme_add_ns_mpath_ctrl(nsa);

	memcpy(&ns->mpath_nguid, &nsa->mpath_nguid, NVME_NIDT_NGUID_LEN);
	kref_get(&ns->ctrl->kref);

	device_add_disk(ctrl->device, ns->disk);


	if (sysfs_create_group(&disk_to_dev(ns->disk)->kobj,
		&nvme_ns_attr_group)) {
		pr_warn("%s: failed to create sysfs group for identification\n",
			ns->disk->disk_name);
		goto out_del_gendisk;
	}

	ns->bdev = blkdev_get_by_path(devpath,
                                FMODE_READ | FMODE_WRITE , NULL);
	if (IS_ERR(ns->bdev)) {
		pr_warn("%s: failed to get block device\n",
                        ns->disk->disk_name);
		goto out_sysfs_remove_group;
	}

	kfree(id);

	if (nvme_set_ns_active(nsa, ns, NVME_FAILOVER_RETRIES)) {
		pr_info("%s:%d Failed to set active Namespace nvme%dn%d\n", __FUNCTION__, __LINE__, nsa->ctrl->instance, nsa->instance);
	}

    /* init delayed work for IO cleanup when both iface are down */
    INIT_DELAYED_WORK(&ctrl->cu_work, nvme_mpath_flush_io_work);
    return ns;

 out_sysfs_remove_group:
	sysfs_remove_group(&disk_to_dev(ns->disk)->kobj,
			&nvme_ns_attr_group);
 out_del_gendisk:
	del_gendisk(ns->disk);
	mutex_lock(&ctrl->namespaces_mutex);
	test_and_clear_bit(NVME_NS_MULTIPATH, &nsa->flags);
	list_del_init(&nsa->mpathlist);
	mutex_unlock(&ctrl->namespaces_mutex);
	nsa->mpath_ctrl = NULL;
	nsa->ctrl->mpath_ctrl = NULL;
 out_free_id:
	kfree(id);
 out_free_queue:
 	blk_cleanup_queue(ns->queue);
 out_release_instance:
	ida_simple_remove(&ctrl->ns_ida, ns->instance);
 out_free_ns:
	kfree(ns);
 out_free_ctrl:
	device_destroy(nvme_class, MKDEV(nvme_char_major, ctrl->instance));
	spin_lock(&dev_list_lock);
	list_del(&ctrl->node);
	spin_unlock(&dev_list_lock);
	nvme_put_ctrl(ctrl);
	return NULL;
}

static void nvme_shared_ns(struct nvme_ns *shared_ns)
{
	struct nvme_ctrl *ctrl = NULL;
	struct nvme_ns *ns, *ret = NULL;
	/*
	  Check if the namespace is shared and another namespace with same
	  serial number exist amount the controllers.
	*/

	spin_lock(&dev_list_lock);
	list_for_each_entry(ctrl, &nvme_ctrl_list, node) {
		list_for_each_entry(ns, &ctrl->namespaces, list) {
			if (ns == shared_ns)
				continue;
			/*
			 * Revalidating a dead namespace sets capacity to 0. This will
			 * end buffered writers dirtying pages that can't be synced.
			 */
			if (!ns->disk || test_bit(NVME_NS_DEAD, &ns->flags))
				continue;

			if (!strncmp(ns->nguid, shared_ns->nguid, NVME_NIDT_NGUID_LEN)) {
				if (test_bit(NVME_NS_MULTIPATH, &ns->flags)) {
					shared_ns->mpath_ctrl = ns->mpath_ctrl;
					shared_ns->ctrl->mpath_ctrl = (void *)ns->mpath_ctrl;
					ret = shared_ns;
				} else {
					ret = ns;
				}
				goto found_ns;
			}
		}
	}
	spin_unlock(&dev_list_lock);

	if (shared_ns->nmic & 0x1) {
		shared_ns->active = 1;
		nvme_alloc_mpath_ns(shared_ns);
	}
	return;
found_ns:
	spin_unlock(&dev_list_lock);
	if (ret == shared_ns)
		nvme_add_ns_mpath_ctrl(shared_ns);
}

static void nvme_trigger_failover_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =
		container_of(work, struct nvme_ctrl, failover_work);

	printk("%s: nvme%d\n", __FUNCTION__, ctrl->instance);

	nvme_trigger_failover(ctrl);
}

/*
 This function will try to get an active namespace 
 when both the interface is down
 it will return -1 if no active NS found
 it will return 0 if active NS found but identify not successful
 it will return 1 if active NS found and identify successful
 */

static int nvme_update_active(struct nvme_ns *mpath_ns) {
    struct nvme_ns *ns = NULL, *next;
    u8 found_active = 0;
    if(!mpath_ns) {
        test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);
        return -1;
    }
    list_for_each_entry_safe(ns, next, &mpath_ns->ctrl->namespaces, mpathlist) {
        if(!ns->active && ns->ctrl->state != NVME_CTRL_RECONNECTING) {
            /* state change happened,will set this ns as new active */
            found_active = 1;
            break;
        }
    }
    if(!found_active) {
        pr_info("No namespace with Multipath support found.\n");
        test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);
        return -1;
    }

    /* set ns as next active namespace */
    if (nvme_set_ns_active(ns, mpath_ns, NVME_FAILOVER_RETRIES)) {
        pr_info("%s:%d Failed to set active Namespace nvme%dn%d\n", __FUNCTION__, __LINE__, ns->ctrl->instance, ns->instance);
        test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);
        return 0;
    }
    return 1;
}

/*
 This will only be called in IO Failure scenario
 or on device removal
 or on device disconnect.
*/
void nvme_trigger_failover(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *active_ns = NULL;
	struct nvme_ns *standby_ns = NULL;
	struct nvme_ns *ns = NULL,*tmp, *next;
	struct nvme_ctrl *mpath_ctrl = NULL;
	struct nvme_ns *mpath_ns = NULL;
	printk("%s:(%d) nvme%d\n", __FUNCTION__, __LINE__, ctrl->instance);


	if(test_bit(NVME_CTRL_MPATH_CHILD, &ctrl->flags)) {
		list_for_each_entry_safe(tmp, next, &ctrl->namespaces, list) {
			mpath_ctrl = tmp->mpath_ctrl;
			ns = tmp;
			if(ns->active)
				break;
		}
	} else {
		/*We are not part of multipath group.*/
		return;
	}

	if (!mpath_ctrl) {
		pr_info("No namespace with Multipath support found.\n");
		return;
	}

	if (ns && !ns->active && mpath_ctrl->cleanup_done) {
		pr_info("No Failover. Namespace nvme%dn%d not active.\n",ctrl->instance, ns->instance);
		return;
	}

	/*Find the namespace for above multipath controller.
	  There is only one namespace per given multipath controller.
	  We just use same mechanism of list even if we have single
	  mulipath namespace.*/
	list_for_each_entry_safe(mpath_ns, next, &mpath_ctrl->mpath_namespace, list) {
		if (mpath_ns)
			break;
	}
	if (!mpath_ns) {
		pr_info("No Multipath namespace found.\n");
		return;
	}
	if (test_and_set_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags))  {
		return;
	}
	if (!mpath_ctrl->cleanup_done) {
		int ret = nvme_update_active(mpath_ns);
		if(!ret)
			schedule_delayed_work(&mpath_ctrl->cu_work, HZ);
		return;
	}

	/* Iterate through all namespaces related to Multipath controller.
	   Find a different one from the one in use. This will be the
	   namespace we will failover to.*/
	pr_info("%s: flags=0x%lx nsid=%d\n",__FUNCTION__, mpath_ns->flags, mpath_ns->ns_id);
	if (test_bit(NVME_NS_ROOT, &mpath_ns->flags)) {
		mutex_lock(&mpath_ns->ctrl->namespaces_mutex);
		list_for_each_entry_safe(ns, next, &mpath_ns->ctrl->namespaces, mpathlist) {
			if (ns) {
				if(ns->active)
					active_ns = ns;
				else
					standby_ns = ns;
			}
			if (active_ns && standby_ns) {
				if (active_ns == standby_ns) {
					test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);
					break;
				}
				if ((jiffies - standby_ns->start_time) < (ns_failover_interval * HZ)) {
					pr_info("Failover failed due unmet time interval between consecuting failover on same volume.\n");
					test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);
					schedule_delayed_work(&mpath_ctrl->cu_work, HZ);
					break;
				}
				active_ns->mpath_ctrl->cleanup_done = 0;
				active_ns->active = 0;
				active_ns->start_time = jiffies;
				if (nvme_set_ns_active(standby_ns, mpath_ns, NVME_FAILOVER_RETRIES)) {
					pr_info("%s:%d Failed to set active Namespace nvme%dn%d\n", __FUNCTION__, __LINE__, standby_ns->ctrl->instance, standby_ns->instance);
					test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);
					schedule_delayed_work(&mpath_ctrl->cu_work, HZ);
				}
				break;
			}
		}

		if (active_ns && !standby_ns) {
			test_and_clear_bit(NVME_NS_FO_IN_PROGRESS, &mpath_ns->flags);
		}
		mutex_unlock(&mpath_ns->ctrl->namespaces_mutex);
	}
}
EXPORT_SYMBOL_GPL(nvme_trigger_failover);

static struct nvme_ns *nvme_alloc_ns(struct nvme_ctrl *ctrl, unsigned nsid)
{
	struct nvme_ns *ns;
	struct gendisk *disk;
	struct nvme_id_ns *id;
	char disk_name[DISK_NAME_LEN];
	char devpath[DISK_NAME_LEN+4];
	int node = dev_to_node(ctrl->dev);

	static char *_claim_ptr = "I belong to mpath device";
	ns = kzalloc_node(sizeof(*ns), GFP_KERNEL, node);
	if (!ns)
		return NULL;

	ns->instance = ida_simple_get(&ctrl->ns_ida, 1, 0, GFP_KERNEL);
	if (ns->instance < 0)
		goto out_free_ns;

	ns->queue = blk_mq_init_queue(ctrl->tagset);
	if (IS_ERR(ns->queue))
		goto out_release_instance;
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, ns->queue);
	ns->queue->queuedata = ns;
	ns->ctrl = ctrl;
	ns->start_time = 0;

	kref_init(&ns->kref);
	ns->ns_id = nsid;
	ns->lba_shift = 9; /* set to a default value for 512 until disk is validated */

	blk_queue_logical_block_size(ns->queue, 1 << ns->lba_shift);
	nvme_set_queue_limits(ctrl, ns->queue);
	nvme_setup_streams_ns(ctrl, ns);
	blk_queue_rq_timeout(ns->queue, nvme_io_timeout * HZ);

	sprintf(disk_name, "nvme%dn%d", ctrl->instance, ns->instance);
	sprintf(devpath, "/dev/nvme%dn%d", ctrl->instance, ns->instance);

	if (nvme_revalidate_ns(ns, &id))
		goto out_free_queue;

	if (nvme_nvm_ns_supported(ns, id) &&
				nvme_nvm_register(ns, disk_name, node)) {
		dev_warn(ctrl->device, "%s: LightNVM init failure\n", __func__);
		goto out_free_id;
	}

	disk = alloc_disk_node(0, node);
	if (!disk)
		goto out_free_id;

	disk->fops = &nvme_fops;
	disk->private_data = ns;
	disk->queue = ns->queue;
	disk->flags = GENHD_FL_EXT_DEVT;
	memcpy(disk->disk_name, disk_name, DISK_NAME_LEN);
	ns->disk = disk;

	__nvme_revalidate_disk(disk, id);

	mutex_lock(&ctrl->namespaces_mutex);
	list_add_tail(&ns->list, &ctrl->namespaces);
	mutex_unlock(&ctrl->namespaces_mutex);

	kref_get(&ctrl->kref);

	device_add_disk(ctrl->device, ns->disk);
	if (sysfs_create_group(&disk_to_dev(ns->disk)->kobj,
					&nvme_ns_attr_group)) {
		pr_warn("%s: failed to create sysfs group for identification\n",
			ns->disk->disk_name);
		goto out_del_gendisk;
	}

	if (ns->ndev && nvme_nvm_register_sysfs(ns))
		pr_warn("%s: failed to register lightnvm sysfs group for identification\n",
			ns->disk->disk_name);

	if (ns->nmic &  0x1) {
		ns->bdev = blkdev_get_by_path(devpath,
                                FMODE_READ | FMODE_WRITE | FMODE_EXCL, _claim_ptr);
		if (IS_ERR(ns->bdev)) {
			goto out_sysfs_remove_group;
		}
	}

	kfree(id);

	return ns;
 out_sysfs_remove_group:
	pr_err("%s: failed to get block device handle %p\n",
			ns->disk->disk_name, ns->bdev);
	sysfs_remove_group(&disk_to_dev(ns->disk)->kobj,
			&nvme_ns_attr_group);
 out_del_gendisk:
	del_gendisk(ns->disk);
 out_free_id:
	kfree(id);
 out_free_queue:
	blk_cleanup_queue(ns->queue);
 out_release_instance:
	ida_simple_remove(&ctrl->ns_ida, ns->instance);
 out_free_ns:
	kfree(ns);
	return NULL;
}

static void nvme_ns_remove(struct nvme_ns *ns)
{
	struct nvme_ctrl *mpath_ctrl = NULL;
	if (test_and_set_bit(NVME_NS_REMOVING, &ns->flags))
		return;

	if (test_bit(NVME_NS_ROOT, &ns->flags))
		nvme_mpath_cancel_ios(ns);

	if (ns->active)
		nvme_trigger_failover(ns->ctrl);
	if (ns->mpath_ctrl) {
		mpath_ctrl = ns->mpath_ctrl;
		if (nvme_del_ns_mpath_ctrl(ns) == NVME_NO_MPATH_NS_AVAIL) {
			mpath_ctrl = NULL;
		}
		if (ns->disk && ns->disk->flags & GENHD_FL_UP) {
			if (blk_get_integrity(ns->disk))
				blk_integrity_unregister(ns->disk);
			sysfs_remove_group(&disk_to_dev(ns->disk)->kobj,
						&nvme_ns_attr_group);
			if (ns->nmic &  0x1)
				blkdev_put(ns->bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
			del_gendisk(ns->disk);
			blk_cleanup_queue(ns->queue);
		}

	} else {
		if (ns->disk && ns->disk->flags & GENHD_FL_UP) {
			if (blk_get_integrity(ns->disk))
				blk_integrity_unregister(ns->disk);

			sysfs_remove_group(&disk_to_dev(ns->disk)->kobj,
				&nvme_ns_attr_group);
			if (ns->ndev)
				nvme_nvm_unregister_sysfs(ns);
			if (ns->bdev)
				blkdev_put(ns->bdev, FMODE_READ | FMODE_WRITE );
			del_gendisk(ns->disk);
			blk_cleanup_queue(ns->queue);
		}
	}

	mutex_lock(&ns->ctrl->namespaces_mutex);
	list_del_init(&ns->list);
	mutex_unlock(&ns->ctrl->namespaces_mutex);

	nvme_put_ns(ns);
	if (mpath_ctrl) {
		mpath_ctrl->cleanup_done = 1;
	}
}

void nvme_mpath_ns_remove(struct nvme_ns *ns)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	nvme_ns_remove(ns);
	device_destroy(nvme_class, MKDEV(nvme_char_major, ctrl->instance));
	spin_lock(&dev_list_lock);
	list_del(&ctrl->node);
	spin_unlock(&dev_list_lock);
	nvme_put_ctrl(ctrl);
}

static void nvme_validate_ns(struct nvme_ctrl *ctrl, unsigned nsid)
{
	struct nvme_ns *ns;

	ns = nvme_find_get_ns(ctrl, nsid);
	if (ns) {
		if (ns->disk && revalidate_disk(ns->disk))
			nvme_ns_remove(ns);
		nvme_put_ns(ns);
	} else {
		ns = nvme_alloc_ns(ctrl, nsid);
		if (ns && (ns->nmic &  0x1)) {
			if (ns->bdev->bd_part == NULL) {
				pr_err("%s(%d): bd_part NOT FOUND nvme%dn%d\n", __FUNCTION__, __LINE__, ns->ctrl->instance, ns->instance);
			} else {
				nvme_shared_ns(ns);
			}
		}
	}
}

static void nvme_remove_invalid_namespaces(struct nvme_ctrl *ctrl,
					unsigned nsid)
{
	struct nvme_ns *ns, *next;

	list_for_each_entry_safe(ns, next, &ctrl->namespaces, list) {
		if (ns->ns_id > nsid)
			nvme_ns_remove(ns);
	}
}

static int nvme_scan_ns_list(struct nvme_ctrl *ctrl, unsigned nn)
{
	struct nvme_ns *ns;
	__le32 *ns_list;
	unsigned i, j, nsid, prev = 0, num_lists = DIV_ROUND_UP(nn, 1024);
	int ret = 0;

	ns_list = kzalloc(0x1000, GFP_KERNEL);
	if (!ns_list)
		return -ENOMEM;

	for (i = 0; i < num_lists; i++) {
		ret = nvme_identify_ns_list(ctrl, prev, ns_list);
		if (ret)
			goto free;

		for (j = 0; j < min(nn, 1024U); j++) {
			nsid = le32_to_cpu(ns_list[j]);
			if (!nsid)
				goto out;

			nvme_validate_ns(ctrl, nsid);

			while (++prev < nsid) {
				ns = nvme_find_get_ns(ctrl, prev);
				if (ns) {
					nvme_ns_remove(ns);
					nvme_put_ns(ns);
				}
			}
		}
		nn -= j;
	}
 out:
	nvme_remove_invalid_namespaces(ctrl, prev);
 free:
	kfree(ns_list);
	return ret;
}

static void nvme_scan_ns_sequential(struct nvme_ctrl *ctrl, unsigned nn)
{
	unsigned i;

	for (i = 1; i <= nn; i++)
		nvme_validate_ns(ctrl, i);

	nvme_remove_invalid_namespaces(ctrl, nn);
}

static void nvme_scan_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =
		container_of(work, struct nvme_ctrl, scan_work);
	struct nvme_id_ctrl *id;
	unsigned nn;

	if (ctrl->state != NVME_CTRL_LIVE)
		return;

	if (nvme_identify_ctrl(ctrl, &id))
		return;

	nn = le32_to_cpu(id->nn);
	if (ctrl->vs >= NVME_VS(1, 1, 0) &&
	    !(ctrl->quirks & NVME_QUIRK_IDENTIFY_CNS)) {
		if (!nvme_scan_ns_list(ctrl, nn))
			goto done;
	}
	nvme_scan_ns_sequential(ctrl, nn);
 done:
	mutex_lock(&ctrl->namespaces_mutex);
	list_sort(NULL, &ctrl->namespaces, ns_cmp);
	mutex_unlock(&ctrl->namespaces_mutex);
	kfree(id);
}

void nvme_queue_scan(struct nvme_ctrl *ctrl)
{
	/*
	 * Do not queue new scan work when a controller is reset during
	 * removal.
	 */
	if (ctrl->state == NVME_CTRL_LIVE)
		queue_work(nvme_wq, &ctrl->scan_work);
}
EXPORT_SYMBOL_GPL(nvme_queue_scan);

/*
 * This function iterates the namespace list unlocked to allow recovery from
 * controller failure. It is up to the caller to ensure the namespace list is
 * not modified by scan work while this function is executing.
 */
void nvme_remove_namespaces(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns, *next;

	/*
	 * The dead states indicates the controller was not gracefully
	 * disconnected. In that case, we won't be able to flush any data while
	 * removing the namespaces' disks; fail all the queues now to avoid
	 * potentially having to clean up the failed sync later.
	 */
	if (ctrl->state == NVME_CTRL_DEAD)
		nvme_kill_queues(ctrl);

	list_for_each_entry_safe(ns, next, &ctrl->namespaces, list)
		nvme_ns_remove(ns);
}
EXPORT_SYMBOL_GPL(nvme_remove_namespaces);

static void nvme_async_event_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =
		container_of(work, struct nvme_ctrl, async_event_work);

	spin_lock_irq(&ctrl->lock);
	while (ctrl->event_limit > 0) {
		int aer_idx = --ctrl->event_limit;

		spin_unlock_irq(&ctrl->lock);
		ctrl->ops->submit_async_event(ctrl, aer_idx);
		spin_lock_irq(&ctrl->lock);
	}
	spin_unlock_irq(&ctrl->lock);
}

static bool nvme_ctrl_pp_status(struct nvme_ctrl *ctrl)
{

	u32 csts;

	if (ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts))
		return false;

	if (csts == ~0)
		return false;

	return ((ctrl->ctrl_config & NVME_CC_ENABLE) && (csts & NVME_CSTS_PP));
}

static void nvme_get_fw_slot_info(struct nvme_ctrl *ctrl)
{
	struct nvme_command c = { };
	struct nvme_fw_slot_info_log *log;

	log = kmalloc(sizeof(*log), GFP_KERNEL);
	if (!log)
		return;

	c.common.opcode = nvme_admin_get_log_page;
	c.common.nsid = cpu_to_le32(NVME_NSID_ALL);
	c.common.cdw10[0] = nvme_get_log_dw10(NVME_LOG_FW_SLOT, sizeof(*log));

	if (!nvme_submit_sync_cmd(ctrl->admin_q, &c, log, sizeof(*log)))
		dev_warn(ctrl->device,
				"Get FW SLOT INFO log error\n");
	kfree(log);
}

static void nvme_fw_act_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(work,
				struct nvme_ctrl, fw_act_work);
	unsigned long fw_act_timeout;

	if (ctrl->mtfa)
		fw_act_timeout = jiffies +
				msecs_to_jiffies(ctrl->mtfa * 100);
	else
		fw_act_timeout = jiffies +
				msecs_to_jiffies(admin_timeout * 1000);

	nvme_stop_queues(ctrl);
	while (nvme_ctrl_pp_status(ctrl)) {
		if (time_after(jiffies, fw_act_timeout)) {
			dev_warn(ctrl->device,
				"Fw activation timeout, reset controller\n");
			nvme_reset_ctrl(ctrl);
			break;
		}
		msleep(100);
	}

	if (ctrl->state != NVME_CTRL_LIVE)
		return;

	nvme_start_queues(ctrl);
	/* read FW slot informationi to clear the AER*/
	nvme_get_fw_slot_info(ctrl);
}

void nvme_complete_async_event(struct nvme_ctrl *ctrl, __le16 status,
		union nvme_result *res)
{
	u32 result = le32_to_cpu(res->u32);
	bool done = true;

	switch (le16_to_cpu(status) >> 1) {
	case NVME_SC_SUCCESS:
		done = false;
		/*FALLTHRU*/
	case NVME_SC_ABORT_REQ:
		++ctrl->event_limit;
		queue_work(nvme_wq, &ctrl->async_event_work);
		break;
	default:
		break;
	}

	if (done)
		return;

	switch (result & 0xff07) {
	case NVME_AER_NOTICE_NS_CHANGED:
		dev_info(ctrl->device, "rescanning\n");
		nvme_queue_scan(ctrl);
		break;
	case NVME_AER_NOTICE_FW_ACT_STARTING:
		schedule_work(&ctrl->fw_act_work);
		break;
	default:
		dev_warn(ctrl->device, "async event result %08x\n", result);
	}
}
EXPORT_SYMBOL_GPL(nvme_complete_async_event);

void nvme_queue_async_events(struct nvme_ctrl *ctrl)
{
	ctrl->event_limit = NVME_NR_AERS;
	queue_work(nvme_wq, &ctrl->async_event_work);
}
EXPORT_SYMBOL_GPL(nvme_queue_async_events);

static DEFINE_IDA(nvme_instance_ida);

static int nvme_set_instance(struct nvme_ctrl *ctrl)
{
	int instance, error;

	do {
		if (!ida_pre_get(&nvme_instance_ida, GFP_KERNEL))
			return -ENODEV;

		spin_lock(&dev_list_lock);
		error = ida_get_new(&nvme_instance_ida, &instance);
		spin_unlock(&dev_list_lock);
	} while (error == -EAGAIN);

	if (error)
		return -ENODEV;

	ctrl->instance = instance;
	return 0;
}

static void nvme_release_instance(struct nvme_ctrl *ctrl)
{
	spin_lock(&dev_list_lock);
	ida_remove(&nvme_instance_ida, ctrl->instance);
	spin_unlock(&dev_list_lock);
}

void nvme_stop_ctrl(struct nvme_ctrl *ctrl)
{
	if (!test_bit(NVME_CTRL_MULTIPATH, &ctrl->flags)) {
	nvme_stop_keep_alive(ctrl);


	flush_work(&ctrl->async_event_work);
	flush_work(&ctrl->scan_work);
	cancel_work_sync(&ctrl->fw_act_work);
	}
}
EXPORT_SYMBOL_GPL(nvme_stop_ctrl);

void nvme_start_ctrl(struct nvme_ctrl *ctrl)
{
	if (ctrl->kato)
		nvme_start_keep_alive(ctrl);

	if (ctrl->queue_count > 1) {
		nvme_queue_scan(ctrl);
		nvme_queue_async_events(ctrl);
		nvme_start_queues(ctrl);
	}
}
EXPORT_SYMBOL_GPL(nvme_start_ctrl);

void nvme_uninit_ctrl(struct nvme_ctrl *ctrl)
{
	struct task_struct *tmp = NULL;
	device_destroy(nvme_class, MKDEV(nvme_char_major, ctrl->instance));

	spin_lock(&dev_list_lock);
	list_del(&ctrl->node);
	if (list_empty(&nvme_mpath_ctrl_list) && !IS_ERR_OR_NULL(nvme_mpath_thread)) {
		tmp = nvme_mpath_thread;
		nvme_mpath_thread = NULL;
	}
	spin_unlock(&dev_list_lock);
	if (tmp) {
		kthread_stop(tmp);
	}
}
EXPORT_SYMBOL_GPL(nvme_uninit_ctrl);

static void nvme_free_ctrl(struct kref *kref)
{
	struct nvme_ctrl *ctrl = container_of(kref, struct nvme_ctrl, kref);

	put_device(ctrl->device);
	nvme_release_instance(ctrl);
	ida_destroy(&ctrl->ns_ida);

	if (test_bit(NVME_CTRL_MULTIPATH, &ctrl->flags)) {
		if (ctrl->mpath_req_pool) {
			mempool_destroy(ctrl->mpath_req_pool);
			kmem_cache_destroy(ctrl->mpath_req_slab);
		}
		kfree(ctrl);
	} else {
		ctrl->ops->free_ctrl(ctrl);
	}
}

void nvme_put_ctrl(struct nvme_ctrl *ctrl)
{
	kref_put(&ctrl->kref, nvme_free_ctrl);
}
EXPORT_SYMBOL_GPL(nvme_put_ctrl);

/*
 * Initialize a NVMe controller structures.  This needs to be called during
 * earliest initialization so that we have the initialized structured around
 * during probing.
 */
int nvme_init_ctrl(struct nvme_ctrl *ctrl, struct device *dev,
		const struct nvme_ctrl_ops *ops, unsigned long quirks)
{
	int ret;
	dev_t nvme_dev;

	ctrl->state = NVME_CTRL_NEW;
	spin_lock_init(&ctrl->lock);
	INIT_LIST_HEAD(&ctrl->namespaces);
	mutex_init(&ctrl->namespaces_mutex);
	kref_init(&ctrl->kref);
	ctrl->dev = dev;
	ctrl->ops = ops;
	ctrl->quirks = quirks;
	INIT_WORK(&ctrl->failover_work, nvme_trigger_failover_work);
	INIT_WORK(&ctrl->scan_work, nvme_scan_work);
	INIT_WORK(&ctrl->async_event_work, nvme_async_event_work);
	INIT_WORK(&ctrl->fw_act_work, nvme_fw_act_work);

	ret = nvme_set_instance(ctrl);
	if (ret)
		goto out;

	nvme_dev = MKDEV(nvme_char_major, ctrl->instance);
	nvme_char_major = MAJOR(nvme_dev);

	ctrl->device = device_create_with_groups(nvme_class, ctrl->dev,
				MKDEV(nvme_char_major, ctrl->instance),
				ctrl, nvme_dev_attr_groups,
				"nvme%d", ctrl->instance);

	if (IS_ERR(ctrl->device)) {
		ret = PTR_ERR(ctrl->device);
		goto out_release_instance;
	}
	get_device(ctrl->device);
	ida_init(&ctrl->ns_ida);

	spin_lock(&dev_list_lock);
	list_add_tail(&ctrl->node, &nvme_ctrl_list);
	spin_unlock(&dev_list_lock);

	/*
	 * Initialize latency tolerance controls.  The sysfs files won't
	 * be visible to userspace unless the device actually supports APST.
	 */
	ctrl->device->power.set_latency_tolerance = nvme_set_latency_tolerance;
	dev_pm_qos_update_user_latency_tolerance(ctrl->device,
		min(default_ps_max_latency_us, (unsigned long)S32_MAX));

	return 0;
out_release_instance:
	nvme_release_instance(ctrl);
out:
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_init_ctrl);

struct nvme_ctrl *nvme_init_mpath_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_ctrl *mpath_ctrl;
	bool changed;
	int ret;
	bool start_thread = false;

	mpath_ctrl = kzalloc(sizeof(*mpath_ctrl), GFP_KERNEL);
	if (!mpath_ctrl)
		return ERR_PTR(-ENOMEM);

	set_bit(NVME_CTRL_MULTIPATH, &mpath_ctrl->flags);
	mpath_ctrl->state = NVME_CTRL_NEW;
	mpath_ctrl->cleanup_done = 1;
	spin_lock_init(&mpath_ctrl->lock);
	INIT_LIST_HEAD(&mpath_ctrl->namespaces);
	INIT_LIST_HEAD(&mpath_ctrl->mpath_namespace);
	mutex_init(&mpath_ctrl->namespaces_mutex);
	kref_init(&mpath_ctrl->kref);
	mpath_ctrl->dev = ctrl->dev;
	mpath_ctrl->ops = ctrl->ops;

	ret = nvme_set_instance(mpath_ctrl);
	if (ret)
		goto out;

	mpath_ctrl->device = device_create_with_groups(nvme_class, mpath_ctrl->dev,
		MKDEV(nvme_char_major, mpath_ctrl->instance),
		mpath_ctrl, nvme_dev_attr_groups,
		"nvme%d", mpath_ctrl->instance);

	if (IS_ERR(mpath_ctrl->device)) {
		goto out_release_instance;
	}

	printk(" mpath_ctrl->dev=%p  mpath_ctrl->device=%p\n", mpath_ctrl->dev,  mpath_ctrl->device);
	get_device(mpath_ctrl->device);
	ida_init(&mpath_ctrl->ns_ida);

	if (list_empty(&nvme_mpath_ctrl_list) && IS_ERR_OR_NULL(nvme_mpath_thread)) {
		start_thread = true;
		nvme_mpath_thread = NULL;
	}
	spin_lock(&dev_list_lock);
	list_add_tail(&mpath_ctrl->node, &nvme_mpath_ctrl_list);
	spin_unlock(&dev_list_lock);

	changed = nvme_change_ctrl_state(mpath_ctrl, NVME_CTRL_LIVE);

	sprintf(mpath_ctrl->mpath_req_cache_name, "mpath_req%d", mpath_ctrl->instance);

	/* allocate a slab cache */

	mpath_ctrl->mpath_req_slab = kmem_cache_create(
		mpath_ctrl->mpath_req_cache_name, sizeof(struct nvme_mpath_priv), 0,
		SLAB_HWCACHE_ALIGN, NULL);

	if (mpath_ctrl->mpath_req_slab == NULL) {
		dev_err(mpath_ctrl->device,
			"failed allocating mpath request cache\n");
		goto out_release_instance;
	}

	/* allocate a memory pool which uses the slab cache */
	mpath_ctrl->mpath_req_pool = mempool_create(4096,
		mempool_alloc_slab,
		mempool_free_slab,
		mpath_ctrl->mpath_req_slab);
	if (mpath_ctrl->mpath_req_pool == NULL) {
		dev_err(mpath_ctrl->device,
			"failed allocating mpath request pool\n");
		goto out_release_req_slab;
	}

	if (start_thread) {
		nvme_mpath_thread = kthread_run(nvme_mpath_kthread, NULL, "nvme_mpath");
	} else
		wait_event_killable(nvme_mpath_kthread_wait, nvme_mpath_thread);

	if (IS_ERR_OR_NULL(nvme_mpath_thread)) {
		ret = nvme_mpath_thread ? PTR_ERR(nvme_mpath_thread) : -EINTR;
		goto out_release_req_pool;
	}
	dev_info(mpath_ctrl->device,"multipath request pool allocated\n");

	return mpath_ctrl;
out_release_req_pool:
	mempool_destroy(mpath_ctrl->mpath_req_pool);
 out_release_req_slab:
	kmem_cache_destroy(mpath_ctrl->mpath_req_slab);
	mpath_ctrl->mpath_req_slab = NULL;
 out_release_instance:
	nvme_release_instance(mpath_ctrl);
 out:
	kfree(mpath_ctrl);
	return NULL;
}
EXPORT_SYMBOL_GPL(nvme_init_mpath_ctrl);

/**
 * nvme_kill_queues(): Ends all namespace queues
 * @ctrl: the dead controller that needs to end
 *
 * Call this function when the driver determines it is unable to get the
 * controller in a state capable of servicing IO.
 */
void nvme_kill_queues(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);

	/* Forcibly unquiesce queues to avoid blocking dispatch */
	if (ctrl->admin_q)
		blk_mq_unquiesce_queue(ctrl->admin_q);

	list_for_each_entry(ns, &ctrl->namespaces, list) {
		/*
		 * Revalidating a dead namespace sets capacity to 0. This will
		 * end buffered writers dirtying pages that can't be synced.
		 */
		if (!ns->disk || test_and_set_bit(NVME_NS_DEAD, &ns->flags))
			continue;
		revalidate_disk(ns->disk);
		blk_set_queue_dying(ns->queue);

		/* Forcibly unquiesce queues to avoid blocking dispatch */
		blk_mq_unquiesce_queue(ns->queue);
	}
	mutex_unlock(&ctrl->namespaces_mutex);
}
EXPORT_SYMBOL_GPL(nvme_kill_queues);

void nvme_unfreeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_mq_unfreeze_queue(ns->queue);
	mutex_unlock(&ctrl->namespaces_mutex);
}
EXPORT_SYMBOL_GPL(nvme_unfreeze);

void nvme_wait_freeze_timeout(struct nvme_ctrl *ctrl, long timeout)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		timeout = blk_mq_freeze_queue_wait_timeout(ns->queue, timeout);
		if (timeout <= 0)
			break;
	}
	mutex_unlock(&ctrl->namespaces_mutex);
}
EXPORT_SYMBOL_GPL(nvme_wait_freeze_timeout);

void nvme_wait_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_mq_freeze_queue_wait(ns->queue);
	mutex_unlock(&ctrl->namespaces_mutex);
}
EXPORT_SYMBOL_GPL(nvme_wait_freeze);

void nvme_start_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_freeze_queue_start(ns->queue);
	mutex_unlock(&ctrl->namespaces_mutex);
}
EXPORT_SYMBOL_GPL(nvme_start_freeze);

void nvme_stop_queues(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_mq_quiesce_queue(ns->queue);
	mutex_unlock(&ctrl->namespaces_mutex);
}
EXPORT_SYMBOL_GPL(nvme_stop_queues);

void nvme_start_queues(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_mq_unquiesce_queue(ns->queue);
	mutex_unlock(&ctrl->namespaces_mutex);
}
EXPORT_SYMBOL_GPL(nvme_start_queues);

int __init nvme_core_init(void)
{
	int result;

	init_waitqueue_head(&nvme_mpath_kthread_wait);
	nvme_wq = alloc_workqueue("nvme-wq",
			WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);
	if (!nvme_wq)
		return -ENOMEM;

	result = __register_chrdev(nvme_char_major, 0, NVME_MINORS, "nvme",
							&nvme_dev_fops);
	if (result < 0)
		goto destroy_wq;
	else if (result > 0)
		nvme_char_major = result;

	nvme_class = class_create(THIS_MODULE, "nvme");
	if (IS_ERR(nvme_class)) {
		result = PTR_ERR(nvme_class);
		goto unregister_chrdev;
	}

	return 0;

unregister_chrdev:
	__unregister_chrdev(nvme_char_major, 0, NVME_MINORS, "nvme");
destroy_wq:
	destroy_workqueue(nvme_wq);
	return result;
}

void nvme_core_exit(void)
{
	class_destroy(nvme_class);
	__unregister_chrdev(nvme_char_major, 0, NVME_MINORS, "nvme");
	destroy_workqueue(nvme_wq);
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
module_init(nvme_core_init);
module_exit(nvme_core_exit);
