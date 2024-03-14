// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver
 * Copyright (c) 2011-2014, Intel Corporation.
 */

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blk-integrity.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/hdreg.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/pr.h>
#include <linux/ptrace.h>
#include <linux/nvme_ioctl.h>
#include <linux/pm_qos.h>
#include <asm/unaligned.h>

#include "nvme.h"
#include "fabrics.h"
#include <linux/nvme-auth.h>

#define CREATE_TRACE_POINTS
#include "trace.h"

#define NVME_MINORS		(1U << MINORBITS)

struct nvme_ns_info {
	struct nvme_ns_ids ids;
	u32 nsid;
	__le32 anagrpid;
	bool is_shared;
	bool is_readonly;
	bool is_ready;
	bool is_removed;
};

unsigned int admin_timeout = 60;
module_param(admin_timeout, uint, 0644);
MODULE_PARM_DESC(admin_timeout, "timeout in seconds for admin commands");
EXPORT_SYMBOL_GPL(admin_timeout);

unsigned int nvme_io_timeout = 30;
module_param_named(io_timeout, nvme_io_timeout, uint, 0644);
MODULE_PARM_DESC(io_timeout, "timeout in seconds for I/O");
EXPORT_SYMBOL_GPL(nvme_io_timeout);

static unsigned char shutdown_timeout = 5;
module_param(shutdown_timeout, byte, 0644);
MODULE_PARM_DESC(shutdown_timeout, "timeout in seconds for controller shutdown");

static u8 nvme_max_retries = 5;
module_param_named(max_retries, nvme_max_retries, byte, 0644);
MODULE_PARM_DESC(max_retries, "max number of retries a command may have");

static unsigned long default_ps_max_latency_us = 100000;
module_param(default_ps_max_latency_us, ulong, 0644);
MODULE_PARM_DESC(default_ps_max_latency_us,
		 "max power saving latency for new devices; use PM QOS to change per device");

static bool force_apst;
module_param(force_apst, bool, 0644);
MODULE_PARM_DESC(force_apst, "allow APST for newly enumerated devices even if quirked off");

static unsigned long apst_primary_timeout_ms = 100;
module_param(apst_primary_timeout_ms, ulong, 0644);
MODULE_PARM_DESC(apst_primary_timeout_ms,
	"primary APST timeout in ms");

static unsigned long apst_secondary_timeout_ms = 2000;
module_param(apst_secondary_timeout_ms, ulong, 0644);
MODULE_PARM_DESC(apst_secondary_timeout_ms,
	"secondary APST timeout in ms");

static unsigned long apst_primary_latency_tol_us = 15000;
module_param(apst_primary_latency_tol_us, ulong, 0644);
MODULE_PARM_DESC(apst_primary_latency_tol_us,
	"primary APST latency tolerance in us");

static unsigned long apst_secondary_latency_tol_us = 100000;
module_param(apst_secondary_latency_tol_us, ulong, 0644);
MODULE_PARM_DESC(apst_secondary_latency_tol_us,
	"secondary APST latency tolerance in us");

/*
 * nvme_wq - hosts nvme related works that are not reset or delete
 * nvme_reset_wq - hosts nvme reset works
 * nvme_delete_wq - hosts nvme delete works
 *
 * nvme_wq will host works such as scan, aen handling, fw activation,
 * keep-alive, periodic reconnects etc. nvme_reset_wq
 * runs reset works which also flush works hosted on nvme_wq for
 * serialization purposes. nvme_delete_wq host controller deletion
 * works which flush reset works for serialization.
 */
struct workqueue_struct *nvme_wq;
EXPORT_SYMBOL_GPL(nvme_wq);

struct workqueue_struct *nvme_reset_wq;
EXPORT_SYMBOL_GPL(nvme_reset_wq);

struct workqueue_struct *nvme_delete_wq;
EXPORT_SYMBOL_GPL(nvme_delete_wq);

static LIST_HEAD(nvme_subsystems);
static DEFINE_MUTEX(nvme_subsystems_lock);

static DEFINE_IDA(nvme_instance_ida);
static dev_t nvme_ctrl_base_chr_devt;
static struct class *nvme_class;
static struct class *nvme_subsys_class;

static DEFINE_IDA(nvme_ns_chr_minor_ida);
static dev_t nvme_ns_chr_devt;
static struct class *nvme_ns_chr_class;

static void nvme_put_subsystem(struct nvme_subsystem *subsys);
static void nvme_remove_invalid_namespaces(struct nvme_ctrl *ctrl,
					   unsigned nsid);
static void nvme_update_keep_alive(struct nvme_ctrl *ctrl,
				   struct nvme_command *cmd);

void nvme_queue_scan(struct nvme_ctrl *ctrl)
{
	/*
	 * Only new queue scan work when admin and IO queues are both alive
	 */
	if (ctrl->state == NVME_CTRL_LIVE && ctrl->tagset)
		queue_work(nvme_wq, &ctrl->scan_work);
}

/*
 * Use this function to proceed with scheduling reset_work for a controller
 * that had previously been set to the resetting state. This is intended for
 * code paths that can't be interrupted by other reset attempts. A hot removal
 * may prevent this from succeeding.
 */
int nvme_try_sched_reset(struct nvme_ctrl *ctrl)
{
	if (ctrl->state != NVME_CTRL_RESETTING)
		return -EBUSY;
	if (!queue_work(nvme_reset_wq, &ctrl->reset_work))
		return -EBUSY;
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_try_sched_reset);

static void nvme_failfast_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvme_ctrl, failfast_work);

	if (ctrl->state != NVME_CTRL_CONNECTING)
		return;

	set_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);
	dev_info(ctrl->device, "failfast expired\n");
	nvme_kick_requeue_lists(ctrl);
}

static inline void nvme_start_failfast_work(struct nvme_ctrl *ctrl)
{
	if (!ctrl->opts || ctrl->opts->fast_io_fail_tmo == -1)
		return;

	schedule_delayed_work(&ctrl->failfast_work,
			      ctrl->opts->fast_io_fail_tmo * HZ);
}

static inline void nvme_stop_failfast_work(struct nvme_ctrl *ctrl)
{
	if (!ctrl->opts)
		return;

	cancel_delayed_work_sync(&ctrl->failfast_work);
	clear_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);
}


int nvme_reset_ctrl(struct nvme_ctrl *ctrl)
{
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))
		return -EBUSY;
	if (!queue_work(nvme_reset_wq, &ctrl->reset_work))
		return -EBUSY;
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_reset_ctrl);

int nvme_reset_ctrl_sync(struct nvme_ctrl *ctrl)
{
	int ret;

	ret = nvme_reset_ctrl(ctrl);
	if (!ret) {
		flush_work(&ctrl->reset_work);
		if (ctrl->state != NVME_CTRL_LIVE)
			ret = -ENETRESET;
	}

	return ret;
}

static void nvme_do_delete_ctrl(struct nvme_ctrl *ctrl)
{
	dev_info(ctrl->device,
		 "Removing ctrl: NQN \"%s\"\n", nvmf_ctrl_subsysnqn(ctrl));

	flush_work(&ctrl->reset_work);
	nvme_stop_ctrl(ctrl);
	nvme_remove_namespaces(ctrl);
	ctrl->ops->delete_ctrl(ctrl);
	nvme_uninit_ctrl(ctrl);
}

static void nvme_delete_ctrl_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =
		container_of(work, struct nvme_ctrl, delete_work);

	nvme_do_delete_ctrl(ctrl);
}

int nvme_delete_ctrl(struct nvme_ctrl *ctrl)
{
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING))
		return -EBUSY;
	if (!queue_work(nvme_delete_wq, &ctrl->delete_work))
		return -EBUSY;
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_delete_ctrl);

void nvme_delete_ctrl_sync(struct nvme_ctrl *ctrl)
{
	/*
	 * Keep a reference until nvme_do_delete_ctrl() complete,
	 * since ->delete_ctrl can free the controller.
	 */
	nvme_get_ctrl(ctrl);
	if (nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING))
		nvme_do_delete_ctrl(ctrl);
	nvme_put_ctrl(ctrl);
}

static blk_status_t nvme_error_status(u16 status)
{
	switch (status & 0x7ff) {
	case NVME_SC_SUCCESS:
		return BLK_STS_OK;
	case NVME_SC_CAP_EXCEEDED:
		return BLK_STS_NOSPC;
	case NVME_SC_LBA_RANGE:
	case NVME_SC_CMD_INTERRUPTED:
	case NVME_SC_NS_NOT_READY:
		return BLK_STS_TARGET;
	case NVME_SC_BAD_ATTRIBUTES:
	case NVME_SC_ONCS_NOT_SUPPORTED:
	case NVME_SC_INVALID_OPCODE:
	case NVME_SC_INVALID_FIELD:
	case NVME_SC_INVALID_NS:
		return BLK_STS_NOTSUPP;
	case NVME_SC_WRITE_FAULT:
	case NVME_SC_READ_ERROR:
	case NVME_SC_UNWRITTEN_BLOCK:
	case NVME_SC_ACCESS_DENIED:
	case NVME_SC_READ_ONLY:
	case NVME_SC_COMPARE_FAILED:
		return BLK_STS_MEDIUM;
	case NVME_SC_GUARD_CHECK:
	case NVME_SC_APPTAG_CHECK:
	case NVME_SC_REFTAG_CHECK:
	case NVME_SC_INVALID_PI:
		return BLK_STS_PROTECTION;
	case NVME_SC_RESERVATION_CONFLICT:
		return BLK_STS_RESV_CONFLICT;
	case NVME_SC_HOST_PATH_ERROR:
		return BLK_STS_TRANSPORT;
	case NVME_SC_ZONE_TOO_MANY_ACTIVE:
		return BLK_STS_ZONE_ACTIVE_RESOURCE;
	case NVME_SC_ZONE_TOO_MANY_OPEN:
		return BLK_STS_ZONE_OPEN_RESOURCE;
	default:
		return BLK_STS_IOERR;
	}
}

static void nvme_retry_req(struct request *req)
{
	unsigned long delay = 0;
	u16 crd;

	/* The mask and shift result must be <= 3 */
	crd = (nvme_req(req)->status & NVME_SC_CRD) >> 11;
	if (crd)
		delay = nvme_req(req)->ctrl->crdt[crd - 1] * 100;

	nvme_req(req)->retries++;
	blk_mq_requeue_request(req, false);
	blk_mq_delay_kick_requeue_list(req->q, delay);
}

static void nvme_log_error(struct request *req)
{
	struct nvme_ns *ns = req->q->queuedata;
	struct nvme_request *nr = nvme_req(req);

	if (ns) {
		pr_err_ratelimited("%s: %s(0x%x) @ LBA %llu, %llu blocks, %s (sct 0x%x / sc 0x%x) %s%s\n",
		       ns->disk ? ns->disk->disk_name : "?",
		       nvme_get_opcode_str(nr->cmd->common.opcode),
		       nr->cmd->common.opcode,
		       (unsigned long long)nvme_sect_to_lba(ns, blk_rq_pos(req)),
		       (unsigned long long)blk_rq_bytes(req) >> ns->lba_shift,
		       nvme_get_error_status_str(nr->status),
		       nr->status >> 8 & 7,	/* Status Code Type */
		       nr->status & 0xff,	/* Status Code */
		       nr->status & NVME_SC_MORE ? "MORE " : "",
		       nr->status & NVME_SC_DNR  ? "DNR "  : "");
		return;
	}

	pr_err_ratelimited("%s: %s(0x%x), %s (sct 0x%x / sc 0x%x) %s%s\n",
			   dev_name(nr->ctrl->device),
			   nvme_get_admin_opcode_str(nr->cmd->common.opcode),
			   nr->cmd->common.opcode,
			   nvme_get_error_status_str(nr->status),
			   nr->status >> 8 & 7,	/* Status Code Type */
			   nr->status & 0xff,	/* Status Code */
			   nr->status & NVME_SC_MORE ? "MORE " : "",
			   nr->status & NVME_SC_DNR  ? "DNR "  : "");
}

enum nvme_disposition {
	COMPLETE,
	RETRY,
	FAILOVER,
	AUTHENTICATE,
};

static inline enum nvme_disposition nvme_decide_disposition(struct request *req)
{
	if (likely(nvme_req(req)->status == 0))
		return COMPLETE;

	if ((nvme_req(req)->status & 0x7ff) == NVME_SC_AUTH_REQUIRED)
		return AUTHENTICATE;

	if (blk_noretry_request(req) ||
	    (nvme_req(req)->status & NVME_SC_DNR) ||
	    nvme_req(req)->retries >= nvme_max_retries)
		return COMPLETE;

	if (req->cmd_flags & REQ_NVME_MPATH) {
		if (nvme_is_path_error(nvme_req(req)->status) ||
		    blk_queue_dying(req->q))
			return FAILOVER;
	} else {
		if (blk_queue_dying(req->q))
			return COMPLETE;
	}

	return RETRY;
}

static inline void nvme_end_req_zoned(struct request *req)
{
	if (IS_ENABLED(CONFIG_BLK_DEV_ZONED) &&
	    req_op(req) == REQ_OP_ZONE_APPEND)
		req->__sector = nvme_lba_to_sect(req->q->queuedata,
			le64_to_cpu(nvme_req(req)->result.u64));
}

static inline void nvme_end_req(struct request *req)
{
	blk_status_t status = nvme_error_status(nvme_req(req)->status);

	if (unlikely(nvme_req(req)->status && !(req->rq_flags & RQF_QUIET)))
		nvme_log_error(req);
	nvme_end_req_zoned(req);
	nvme_trace_bio_complete(req);
	if (req->cmd_flags & REQ_NVME_MPATH)
		nvme_mpath_end_request(req);
	blk_mq_end_request(req, status);
}

void nvme_complete_rq(struct request *req)
{
	struct nvme_ctrl *ctrl = nvme_req(req)->ctrl;

	trace_nvme_complete_rq(req);
	nvme_cleanup_cmd(req);

	/*
	 * Completions of long-running commands should not be able to
	 * defer sending of periodic keep alives, since the controller
	 * may have completed processing such commands a long time ago
	 * (arbitrarily close to command submission time).
	 * req->deadline - req->timeout is the command submission time
	 * in jiffies.
	 */
	if (ctrl->kas &&
	    req->deadline - req->timeout >= ctrl->ka_last_check_time)
		ctrl->comp_seen = true;

	switch (nvme_decide_disposition(req)) {
	case COMPLETE:
		nvme_end_req(req);
		return;
	case RETRY:
		nvme_retry_req(req);
		return;
	case FAILOVER:
		nvme_failover_req(req);
		return;
	case AUTHENTICATE:
#ifdef CONFIG_NVME_AUTH
		queue_work(nvme_wq, &ctrl->dhchap_auth_work);
		nvme_retry_req(req);
#else
		nvme_end_req(req);
#endif
		return;
	}
}
EXPORT_SYMBOL_GPL(nvme_complete_rq);

void nvme_complete_batch_req(struct request *req)
{
	trace_nvme_complete_rq(req);
	nvme_cleanup_cmd(req);
	nvme_end_req_zoned(req);
}
EXPORT_SYMBOL_GPL(nvme_complete_batch_req);

/*
 * Called to unwind from ->queue_rq on a failed command submission so that the
 * multipathing code gets called to potentially failover to another path.
 * The caller needs to unwind all transport specific resource allocations and
 * must return propagate the return value.
 */
blk_status_t nvme_host_path_error(struct request *req)
{
	nvme_req(req)->status = NVME_SC_HOST_PATH_ERROR;
	blk_mq_set_request_complete(req);
	nvme_complete_rq(req);
	return BLK_STS_OK;
}
EXPORT_SYMBOL_GPL(nvme_host_path_error);

bool nvme_cancel_request(struct request *req, void *data)
{
	dev_dbg_ratelimited(((struct nvme_ctrl *) data)->device,
				"Cancelling I/O %d", req->tag);

	/* don't abort one completed or idle request */
	if (blk_mq_rq_state(req) != MQ_RQ_IN_FLIGHT)
		return true;

	nvme_req(req)->status = NVME_SC_HOST_ABORTED_CMD;
	nvme_req(req)->flags |= NVME_REQ_CANCELLED;
	blk_mq_complete_request(req);
	return true;
}
EXPORT_SYMBOL_GPL(nvme_cancel_request);

void nvme_cancel_tagset(struct nvme_ctrl *ctrl)
{
	if (ctrl->tagset) {
		blk_mq_tagset_busy_iter(ctrl->tagset,
				nvme_cancel_request, ctrl);
		blk_mq_tagset_wait_completed_request(ctrl->tagset);
	}
}
EXPORT_SYMBOL_GPL(nvme_cancel_tagset);

void nvme_cancel_admin_tagset(struct nvme_ctrl *ctrl)
{
	if (ctrl->admin_tagset) {
		blk_mq_tagset_busy_iter(ctrl->admin_tagset,
				nvme_cancel_request, ctrl);
		blk_mq_tagset_wait_completed_request(ctrl->admin_tagset);
	}
}
EXPORT_SYMBOL_GPL(nvme_cancel_admin_tagset);

bool nvme_change_ctrl_state(struct nvme_ctrl *ctrl,
		enum nvme_ctrl_state new_state)
{
	enum nvme_ctrl_state old_state;
	unsigned long flags;
	bool changed = false;

	spin_lock_irqsave(&ctrl->lock, flags);

	old_state = ctrl->state;
	switch (new_state) {
	case NVME_CTRL_LIVE:
		switch (old_state) {
		case NVME_CTRL_NEW:
		case NVME_CTRL_RESETTING:
		case NVME_CTRL_CONNECTING:
			changed = true;
			fallthrough;
		default:
			break;
		}
		break;
	case NVME_CTRL_RESETTING:
		switch (old_state) {
		case NVME_CTRL_NEW:
		case NVME_CTRL_LIVE:
			changed = true;
			fallthrough;
		default:
			break;
		}
		break;
	case NVME_CTRL_CONNECTING:
		switch (old_state) {
		case NVME_CTRL_NEW:
		case NVME_CTRL_RESETTING:
			changed = true;
			fallthrough;
		default:
			break;
		}
		break;
	case NVME_CTRL_DELETING:
		switch (old_state) {
		case NVME_CTRL_LIVE:
		case NVME_CTRL_RESETTING:
		case NVME_CTRL_CONNECTING:
			changed = true;
			fallthrough;
		default:
			break;
		}
		break;
	case NVME_CTRL_DELETING_NOIO:
		switch (old_state) {
		case NVME_CTRL_DELETING:
		case NVME_CTRL_DEAD:
			changed = true;
			fallthrough;
		default:
			break;
		}
		break;
	case NVME_CTRL_DEAD:
		switch (old_state) {
		case NVME_CTRL_DELETING:
			changed = true;
			fallthrough;
		default:
			break;
		}
		break;
	default:
		break;
	}

	if (changed) {
		ctrl->state = new_state;
		wake_up_all(&ctrl->state_wq);
	}

	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (!changed)
		return false;

	if (ctrl->state == NVME_CTRL_LIVE) {
		if (old_state == NVME_CTRL_CONNECTING)
			nvme_stop_failfast_work(ctrl);
		nvme_kick_requeue_lists(ctrl);
	} else if (ctrl->state == NVME_CTRL_CONNECTING &&
		old_state == NVME_CTRL_RESETTING) {
		nvme_start_failfast_work(ctrl);
	}
	return changed;
}
EXPORT_SYMBOL_GPL(nvme_change_ctrl_state);

/*
 * Returns true for sink states that can't ever transition back to live.
 */
static bool nvme_state_terminal(struct nvme_ctrl *ctrl)
{
	switch (ctrl->state) {
	case NVME_CTRL_NEW:
	case NVME_CTRL_LIVE:
	case NVME_CTRL_RESETTING:
	case NVME_CTRL_CONNECTING:
		return false;
	case NVME_CTRL_DELETING:
	case NVME_CTRL_DELETING_NOIO:
	case NVME_CTRL_DEAD:
		return true;
	default:
		WARN_ONCE(1, "Unhandled ctrl state:%d", ctrl->state);
		return true;
	}
}

/*
 * Waits for the controller state to be resetting, or returns false if it is
 * not possible to ever transition to that state.
 */
bool nvme_wait_reset(struct nvme_ctrl *ctrl)
{
	wait_event(ctrl->state_wq,
		   nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING) ||
		   nvme_state_terminal(ctrl));
	return ctrl->state == NVME_CTRL_RESETTING;
}
EXPORT_SYMBOL_GPL(nvme_wait_reset);

static void nvme_free_ns_head(struct kref *ref)
{
	struct nvme_ns_head *head =
		container_of(ref, struct nvme_ns_head, ref);

	nvme_mpath_remove_disk(head);
	ida_free(&head->subsys->ns_ida, head->instance);
	cleanup_srcu_struct(&head->srcu);
	nvme_put_subsystem(head->subsys);
	kfree(head);
}

bool nvme_tryget_ns_head(struct nvme_ns_head *head)
{
	return kref_get_unless_zero(&head->ref);
}

void nvme_put_ns_head(struct nvme_ns_head *head)
{
	kref_put(&head->ref, nvme_free_ns_head);
}

static void nvme_free_ns(struct kref *kref)
{
	struct nvme_ns *ns = container_of(kref, struct nvme_ns, kref);

	put_disk(ns->disk);
	nvme_put_ns_head(ns->head);
	nvme_put_ctrl(ns->ctrl);
	kfree(ns);
}

static inline bool nvme_get_ns(struct nvme_ns *ns)
{
	return kref_get_unless_zero(&ns->kref);
}

void nvme_put_ns(struct nvme_ns *ns)
{
	kref_put(&ns->kref, nvme_free_ns);
}
EXPORT_SYMBOL_NS_GPL(nvme_put_ns, NVME_TARGET_PASSTHRU);

static inline void nvme_clear_nvme_request(struct request *req)
{
	nvme_req(req)->status = 0;
	nvme_req(req)->retries = 0;
	nvme_req(req)->flags = 0;
	req->rq_flags |= RQF_DONTPREP;
}

/* initialize a passthrough request */
void nvme_init_request(struct request *req, struct nvme_command *cmd)
{
	if (req->q->queuedata)
		req->timeout = NVME_IO_TIMEOUT;
	else /* no queuedata implies admin queue */
		req->timeout = NVME_ADMIN_TIMEOUT;

	/* passthru commands should let the driver set the SGL flags */
	cmd->common.flags &= ~NVME_CMD_SGL_ALL;

	req->cmd_flags |= REQ_FAILFAST_DRIVER;
	if (req->mq_hctx->type == HCTX_TYPE_POLL)
		req->cmd_flags |= REQ_POLLED;
	nvme_clear_nvme_request(req);
	req->rq_flags |= RQF_QUIET;
	memcpy(nvme_req(req)->cmd, cmd, sizeof(*cmd));
}
EXPORT_SYMBOL_GPL(nvme_init_request);

/*
 * For something we're not in a state to send to the device the default action
 * is to busy it and retry it after the controller state is recovered.  However,
 * if the controller is deleting or if anything is marked for failfast or
 * nvme multipath it is immediately failed.
 *
 * Note: commands used to initialize the controller will be marked for failfast.
 * Note: nvme cli/ioctl commands are marked for failfast.
 */
blk_status_t nvme_fail_nonready_command(struct nvme_ctrl *ctrl,
		struct request *rq)
{
	if (ctrl->state != NVME_CTRL_DELETING_NOIO &&
	    ctrl->state != NVME_CTRL_DELETING &&
	    ctrl->state != NVME_CTRL_DEAD &&
	    !test_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags) &&
	    !blk_noretry_request(rq) && !(rq->cmd_flags & REQ_NVME_MPATH))
		return BLK_STS_RESOURCE;
	return nvme_host_path_error(rq);
}
EXPORT_SYMBOL_GPL(nvme_fail_nonready_command);

bool __nvme_check_ready(struct nvme_ctrl *ctrl, struct request *rq,
		bool queue_live)
{
	struct nvme_request *req = nvme_req(rq);

	/*
	 * currently we have a problem sending passthru commands
	 * on the admin_q if the controller is not LIVE because we can't
	 * make sure that they are going out after the admin connect,
	 * controller enable and/or other commands in the initialization
	 * sequence. until the controller will be LIVE, fail with
	 * BLK_STS_RESOURCE so that they will be rescheduled.
	 */
	if (rq->q == ctrl->admin_q && (req->flags & NVME_REQ_USERCMD))
		return false;

	if (ctrl->ops->flags & NVME_F_FABRICS) {
		/*
		 * Only allow commands on a live queue, except for the connect
		 * command, which is require to set the queue live in the
		 * appropinquate states.
		 */
		switch (ctrl->state) {
		case NVME_CTRL_CONNECTING:
			if (blk_rq_is_passthrough(rq) && nvme_is_fabrics(req->cmd) &&
			    (req->cmd->fabrics.fctype == nvme_fabrics_type_connect ||
			     req->cmd->fabrics.fctype == nvme_fabrics_type_auth_send ||
			     req->cmd->fabrics.fctype == nvme_fabrics_type_auth_receive))
				return true;
			break;
		default:
			break;
		case NVME_CTRL_DEAD:
			return false;
		}
	}

	return queue_live;
}
EXPORT_SYMBOL_GPL(__nvme_check_ready);

static inline void nvme_setup_flush(struct nvme_ns *ns,
		struct nvme_command *cmnd)
{
	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->common.opcode = nvme_cmd_flush;
	cmnd->common.nsid = cpu_to_le32(ns->head->ns_id);
}

static blk_status_t nvme_setup_discard(struct nvme_ns *ns, struct request *req,
		struct nvme_command *cmnd)
{
	unsigned short segments = blk_rq_nr_discard_segments(req), n = 0;
	struct nvme_dsm_range *range;
	struct bio *bio;

	/*
	 * Some devices do not consider the DSM 'Number of Ranges' field when
	 * determining how much data to DMA. Always allocate memory for maximum
	 * number of segments to prevent device reading beyond end of buffer.
	 */
	static const size_t alloc_size = sizeof(*range) * NVME_DSM_MAX_RANGES;

	range = kzalloc(alloc_size, GFP_ATOMIC | __GFP_NOWARN);
	if (!range) {
		/*
		 * If we fail allocation our range, fallback to the controller
		 * discard page. If that's also busy, it's safe to return
		 * busy, as we know we can make progress once that's freed.
		 */
		if (test_and_set_bit_lock(0, &ns->ctrl->discard_page_busy))
			return BLK_STS_RESOURCE;

		range = page_address(ns->ctrl->discard_page);
	}

	if (queue_max_discard_segments(req->q) == 1) {
		u64 slba = nvme_sect_to_lba(ns, blk_rq_pos(req));
		u32 nlb = blk_rq_sectors(req) >> (ns->lba_shift - 9);

		range[0].cattr = cpu_to_le32(0);
		range[0].nlb = cpu_to_le32(nlb);
		range[0].slba = cpu_to_le64(slba);
		n = 1;
	} else {
		__rq_for_each_bio(bio, req) {
			u64 slba = nvme_sect_to_lba(ns, bio->bi_iter.bi_sector);
			u32 nlb = bio->bi_iter.bi_size >> ns->lba_shift;

			if (n < segments) {
				range[n].cattr = cpu_to_le32(0);
				range[n].nlb = cpu_to_le32(nlb);
				range[n].slba = cpu_to_le64(slba);
			}
			n++;
		}
	}

	if (WARN_ON_ONCE(n != segments)) {
		if (virt_to_page(range) == ns->ctrl->discard_page)
			clear_bit_unlock(0, &ns->ctrl->discard_page_busy);
		else
			kfree(range);
		return BLK_STS_IOERR;
	}

	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->dsm.opcode = nvme_cmd_dsm;
	cmnd->dsm.nsid = cpu_to_le32(ns->head->ns_id);
	cmnd->dsm.nr = cpu_to_le32(segments - 1);
	cmnd->dsm.attributes = cpu_to_le32(NVME_DSMGMT_AD);

	bvec_set_virt(&req->special_vec, range, alloc_size);
	req->rq_flags |= RQF_SPECIAL_PAYLOAD;

	return BLK_STS_OK;
}

static void nvme_set_ref_tag(struct nvme_ns *ns, struct nvme_command *cmnd,
			      struct request *req)
{
	u32 upper, lower;
	u64 ref48;

	/* both rw and write zeroes share the same reftag format */
	switch (ns->guard_type) {
	case NVME_NVM_NS_16B_GUARD:
		cmnd->rw.reftag = cpu_to_le32(t10_pi_ref_tag(req));
		break;
	case NVME_NVM_NS_64B_GUARD:
		ref48 = ext_pi_ref_tag(req);
		lower = lower_32_bits(ref48);
		upper = upper_32_bits(ref48);

		cmnd->rw.reftag = cpu_to_le32(lower);
		cmnd->rw.cdw3 = cpu_to_le32(upper);
		break;
	default:
		break;
	}
}

static inline blk_status_t nvme_setup_write_zeroes(struct nvme_ns *ns,
		struct request *req, struct nvme_command *cmnd)
{
	memset(cmnd, 0, sizeof(*cmnd));

	if (ns->ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES)
		return nvme_setup_discard(ns, req, cmnd);

	cmnd->write_zeroes.opcode = nvme_cmd_write_zeroes;
	cmnd->write_zeroes.nsid = cpu_to_le32(ns->head->ns_id);
	cmnd->write_zeroes.slba =
		cpu_to_le64(nvme_sect_to_lba(ns, blk_rq_pos(req)));
	cmnd->write_zeroes.length =
		cpu_to_le16((blk_rq_bytes(req) >> ns->lba_shift) - 1);

	if (!(req->cmd_flags & REQ_NOUNMAP) && (ns->features & NVME_NS_DEAC))
		cmnd->write_zeroes.control |= cpu_to_le16(NVME_WZ_DEAC);

	if (nvme_ns_has_pi(ns)) {
		cmnd->write_zeroes.control |= cpu_to_le16(NVME_RW_PRINFO_PRACT);

		switch (ns->pi_type) {
		case NVME_NS_DPS_PI_TYPE1:
		case NVME_NS_DPS_PI_TYPE2:
			nvme_set_ref_tag(ns, cmnd, req);
			break;
		}
	}

	return BLK_STS_OK;
}

static inline blk_status_t nvme_setup_rw(struct nvme_ns *ns,
		struct request *req, struct nvme_command *cmnd,
		enum nvme_opcode op)
{
	u16 control = 0;
	u32 dsmgmt = 0;

	if (req->cmd_flags & REQ_FUA)
		control |= NVME_RW_FUA;
	if (req->cmd_flags & (REQ_FAILFAST_DEV | REQ_RAHEAD))
		control |= NVME_RW_LR;

	if (req->cmd_flags & REQ_RAHEAD)
		dsmgmt |= NVME_RW_DSM_FREQ_PREFETCH;

	cmnd->rw.opcode = op;
	cmnd->rw.flags = 0;
	cmnd->rw.nsid = cpu_to_le32(ns->head->ns_id);
	cmnd->rw.cdw2 = 0;
	cmnd->rw.cdw3 = 0;
	cmnd->rw.metadata = 0;
	cmnd->rw.slba = cpu_to_le64(nvme_sect_to_lba(ns, blk_rq_pos(req)));
	cmnd->rw.length = cpu_to_le16((blk_rq_bytes(req) >> ns->lba_shift) - 1);
	cmnd->rw.reftag = 0;
	cmnd->rw.apptag = 0;
	cmnd->rw.appmask = 0;

	if (ns->ms) {
		/*
		 * If formated with metadata, the block layer always provides a
		 * metadata buffer if CONFIG_BLK_DEV_INTEGRITY is enabled.  Else
		 * we enable the PRACT bit for protection information or set the
		 * namespace capacity to zero to prevent any I/O.
		 */
		if (!blk_integrity_rq(req)) {
			if (WARN_ON_ONCE(!nvme_ns_has_pi(ns)))
				return BLK_STS_NOTSUPP;
			control |= NVME_RW_PRINFO_PRACT;
		}

		switch (ns->pi_type) {
		case NVME_NS_DPS_PI_TYPE3:
			control |= NVME_RW_PRINFO_PRCHK_GUARD;
			break;
		case NVME_NS_DPS_PI_TYPE1:
		case NVME_NS_DPS_PI_TYPE2:
			control |= NVME_RW_PRINFO_PRCHK_GUARD |
					NVME_RW_PRINFO_PRCHK_REF;
			if (op == nvme_cmd_zone_append)
				control |= NVME_RW_APPEND_PIREMAP;
			nvme_set_ref_tag(ns, cmnd, req);
			break;
		}
	}

	cmnd->rw.control = cpu_to_le16(control);
	cmnd->rw.dsmgmt = cpu_to_le32(dsmgmt);
	return 0;
}

void nvme_cleanup_cmd(struct request *req)
{
	if (req->rq_flags & RQF_SPECIAL_PAYLOAD) {
		struct nvme_ctrl *ctrl = nvme_req(req)->ctrl;

		if (req->special_vec.bv_page == ctrl->discard_page)
			clear_bit_unlock(0, &ctrl->discard_page_busy);
		else
			kfree(bvec_virt(&req->special_vec));
	}
}
EXPORT_SYMBOL_GPL(nvme_cleanup_cmd);

blk_status_t nvme_setup_cmd(struct nvme_ns *ns, struct request *req)
{
	struct nvme_command *cmd = nvme_req(req)->cmd;
	blk_status_t ret = BLK_STS_OK;

	if (!(req->rq_flags & RQF_DONTPREP))
		nvme_clear_nvme_request(req);

	switch (req_op(req)) {
	case REQ_OP_DRV_IN:
	case REQ_OP_DRV_OUT:
		/* these are setup prior to execution in nvme_init_request() */
		break;
	case REQ_OP_FLUSH:
		nvme_setup_flush(ns, cmd);
		break;
	case REQ_OP_ZONE_RESET_ALL:
	case REQ_OP_ZONE_RESET:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_RESET);
		break;
	case REQ_OP_ZONE_OPEN:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_OPEN);
		break;
	case REQ_OP_ZONE_CLOSE:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_CLOSE);
		break;
	case REQ_OP_ZONE_FINISH:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_FINISH);
		break;
	case REQ_OP_WRITE_ZEROES:
		ret = nvme_setup_write_zeroes(ns, req, cmd);
		break;
	case REQ_OP_DISCARD:
		ret = nvme_setup_discard(ns, req, cmd);
		break;
	case REQ_OP_READ:
		ret = nvme_setup_rw(ns, req, cmd, nvme_cmd_read);
		break;
	case REQ_OP_WRITE:
		ret = nvme_setup_rw(ns, req, cmd, nvme_cmd_write);
		break;
	case REQ_OP_ZONE_APPEND:
		ret = nvme_setup_rw(ns, req, cmd, nvme_cmd_zone_append);
		break;
	default:
		WARN_ON_ONCE(1);
		return BLK_STS_IOERR;
	}

	cmd->common.command_id = nvme_cid(req);
	trace_nvme_setup_cmd(req, cmd);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_setup_cmd);

/*
 * Return values:
 * 0:  success
 * >0: nvme controller's cqe status response
 * <0: kernel error in lieu of controller response
 */
int nvme_execute_rq(struct request *rq, bool at_head)
{
	blk_status_t status;

	status = blk_execute_rq(rq, at_head);
	if (nvme_req(rq)->flags & NVME_REQ_CANCELLED)
		return -EINTR;
	if (nvme_req(rq)->status)
		return nvme_req(rq)->status;
	return blk_status_to_errno(status);
}
EXPORT_SYMBOL_NS_GPL(nvme_execute_rq, NVME_TARGET_PASSTHRU);

/*
 * Returns 0 on success.  If the result is negative, it's a Linux error code;
 * if the result is positive, it's an NVM Express status code
 */
int __nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		union nvme_result *result, void *buffer, unsigned bufflen,
		int qid, int at_head, blk_mq_req_flags_t flags)
{
	struct request *req;
	int ret;

	if (qid == NVME_QID_ANY)
		req = blk_mq_alloc_request(q, nvme_req_op(cmd), flags);
	else
		req = blk_mq_alloc_request_hctx(q, nvme_req_op(cmd), flags,
						qid - 1);

	if (IS_ERR(req))
		return PTR_ERR(req);
	nvme_init_request(req, cmd);

	if (buffer && bufflen) {
		ret = blk_rq_map_kern(q, req, buffer, bufflen, GFP_KERNEL);
		if (ret)
			goto out;
	}

	ret = nvme_execute_rq(req, at_head);
	if (result && ret >= 0)
		*result = nvme_req(req)->result;
 out:
	blk_mq_free_request(req);
	return ret;
}
EXPORT_SYMBOL_GPL(__nvme_submit_sync_cmd);

int nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		void *buffer, unsigned bufflen)
{
	return __nvme_submit_sync_cmd(q, cmd, NULL, buffer, bufflen,
			NVME_QID_ANY, 0, 0);
}
EXPORT_SYMBOL_GPL(nvme_submit_sync_cmd);

u32 nvme_command_effects(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u8 opcode)
{
	u32 effects = 0;

	if (ns) {
		effects = le32_to_cpu(ns->head->effects->iocs[opcode]);
		if (effects & ~(NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC))
			dev_warn_once(ctrl->device,
				"IO command:%02x has unusual effects:%08x\n",
				opcode, effects);

		/*
		 * NVME_CMD_EFFECTS_CSE_MASK causes a freeze all I/O queues,
		 * which would deadlock when done on an I/O command.  Note that
		 * We already warn about an unusual effect above.
		 */
		effects &= ~NVME_CMD_EFFECTS_CSE_MASK;
	} else {
		effects = le32_to_cpu(ctrl->effects->acs[opcode]);
	}

	return effects;
}
EXPORT_SYMBOL_NS_GPL(nvme_command_effects, NVME_TARGET_PASSTHRU);

u32 nvme_passthru_start(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u8 opcode)
{
	u32 effects = nvme_command_effects(ctrl, ns, opcode);

	/*
	 * For simplicity, IO to all namespaces is quiesced even if the command
	 * effects say only one namespace is affected.
	 */
	if (effects & NVME_CMD_EFFECTS_CSE_MASK) {
		mutex_lock(&ctrl->scan_lock);
		mutex_lock(&ctrl->subsys->lock);
		nvme_mpath_start_freeze(ctrl->subsys);
		nvme_mpath_wait_freeze(ctrl->subsys);
		nvme_start_freeze(ctrl);
		nvme_wait_freeze(ctrl);
	}
	return effects;
}
EXPORT_SYMBOL_NS_GPL(nvme_passthru_start, NVME_TARGET_PASSTHRU);

void nvme_passthru_end(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u32 effects,
		       struct nvme_command *cmd, int status)
{
	if (effects & NVME_CMD_EFFECTS_CSE_MASK) {
		nvme_unfreeze(ctrl);
		nvme_mpath_unfreeze(ctrl->subsys);
		mutex_unlock(&ctrl->subsys->lock);
		mutex_unlock(&ctrl->scan_lock);
	}
	if (effects & NVME_CMD_EFFECTS_CCC) {
		if (!test_and_set_bit(NVME_CTRL_DIRTY_CAPABILITY,
				      &ctrl->flags)) {
			dev_info(ctrl->device,
"controller capabilities changed, reset may be required to take effect.\n");
		}
	}
	if (effects & (NVME_CMD_EFFECTS_NIC | NVME_CMD_EFFECTS_NCC)) {
		nvme_queue_scan(ctrl);
		flush_work(&ctrl->scan_work);
	}
	if (ns)
		return;

	switch (cmd->common.opcode) {
	case nvme_admin_set_features:
		switch (le32_to_cpu(cmd->common.cdw10) & 0xFF) {
		case NVME_FEAT_KATO:
			/*
			 * Keep alive commands interval on the host should be
			 * updated when KATO is modified by Set Features
			 * commands.
			 */
			if (!status)
				nvme_update_keep_alive(ctrl, cmd);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL_NS_GPL(nvme_passthru_end, NVME_TARGET_PASSTHRU);

/*
 * Recommended frequency for KATO commands per NVMe 1.4 section 7.12.1:
 * 
 *   The host should send Keep Alive commands at half of the Keep Alive Timeout
 *   accounting for transport roundtrip times [..].
 */
static unsigned long nvme_keep_alive_work_period(struct nvme_ctrl *ctrl)
{
	unsigned long delay = ctrl->kato * HZ / 2;

	/*
	 * When using Traffic Based Keep Alive, we need to run
	 * nvme_keep_alive_work at twice the normal frequency, as one
	 * command completion can postpone sending a keep alive command
	 * by up to twice the delay between runs.
	 */
	if (ctrl->ctratt & NVME_CTRL_ATTR_TBKAS)
		delay /= 2;
	return delay;
}

static void nvme_queue_keep_alive_work(struct nvme_ctrl *ctrl)
{
	queue_delayed_work(nvme_wq, &ctrl->ka_work,
			   nvme_keep_alive_work_period(ctrl));
}

static enum rq_end_io_ret nvme_keep_alive_end_io(struct request *rq,
						 blk_status_t status)
{
	struct nvme_ctrl *ctrl = rq->end_io_data;
	unsigned long flags;
	bool startka = false;
	unsigned long rtt = jiffies - (rq->deadline - rq->timeout);
	unsigned long delay = nvme_keep_alive_work_period(ctrl);

	/*
	 * Subtract off the keepalive RTT so nvme_keep_alive_work runs
	 * at the desired frequency.
	 */
	if (rtt <= delay) {
		delay -= rtt;
	} else {
		dev_warn(ctrl->device, "long keepalive RTT (%u ms)\n",
			 jiffies_to_msecs(rtt));
		delay = 0;
	}

	blk_mq_free_request(rq);

	if (status) {
		dev_err(ctrl->device,
			"failed nvme_keep_alive_end_io error=%d\n",
				status);
		return RQ_END_IO_NONE;
	}

	ctrl->ka_last_check_time = jiffies;
	ctrl->comp_seen = false;
	spin_lock_irqsave(&ctrl->lock, flags);
	if (ctrl->state == NVME_CTRL_LIVE ||
	    ctrl->state == NVME_CTRL_CONNECTING)
		startka = true;
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (startka)
		queue_delayed_work(nvme_wq, &ctrl->ka_work, delay);
	return RQ_END_IO_NONE;
}

static void nvme_keep_alive_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvme_ctrl, ka_work);
	bool comp_seen = ctrl->comp_seen;
	struct request *rq;

	ctrl->ka_last_check_time = jiffies;

	if ((ctrl->ctratt & NVME_CTRL_ATTR_TBKAS) && comp_seen) {
		dev_dbg(ctrl->device,
			"reschedule traffic based keep-alive timer\n");
		ctrl->comp_seen = false;
		nvme_queue_keep_alive_work(ctrl);
		return;
	}

	rq = blk_mq_alloc_request(ctrl->admin_q, nvme_req_op(&ctrl->ka_cmd),
				  BLK_MQ_REQ_RESERVED | BLK_MQ_REQ_NOWAIT);
	if (IS_ERR(rq)) {
		/* allocation failure, reset the controller */
		dev_err(ctrl->device, "keep-alive failed: %ld\n", PTR_ERR(rq));
		nvme_reset_ctrl(ctrl);
		return;
	}
	nvme_init_request(rq, &ctrl->ka_cmd);

	rq->timeout = ctrl->kato * HZ;
	rq->end_io = nvme_keep_alive_end_io;
	rq->end_io_data = ctrl;
	blk_execute_rq_nowait(rq, false);
}

static void nvme_start_keep_alive(struct nvme_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	nvme_queue_keep_alive_work(ctrl);
}

void nvme_stop_keep_alive(struct nvme_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	cancel_delayed_work_sync(&ctrl->ka_work);
}
EXPORT_SYMBOL_GPL(nvme_stop_keep_alive);

static void nvme_update_keep_alive(struct nvme_ctrl *ctrl,
				   struct nvme_command *cmd)
{
	unsigned int new_kato =
		DIV_ROUND_UP(le32_to_cpu(cmd->common.cdw11), 1000);

	dev_info(ctrl->device,
		 "keep alive interval updated from %u ms to %u ms\n",
		 ctrl->kato * 1000 / 2, new_kato * 1000 / 2);

	nvme_stop_keep_alive(ctrl);
	ctrl->kato = new_kato;
	nvme_start_keep_alive(ctrl);
}

/*
 * In NVMe 1.0 the CNS field was just a binary controller or namespace
 * flag, thus sending any new CNS opcodes has a big chance of not working.
 * Qemu unfortunately had that bug after reporting a 1.1 version compliance
 * (but not for any later version).
 */
static bool nvme_ctrl_limited_cns(struct nvme_ctrl *ctrl)
{
	if (ctrl->quirks & NVME_QUIRK_IDENTIFY_CNS)
		return ctrl->vs < NVME_VS(1, 2, 0);
	return ctrl->vs < NVME_VS(1, 1, 0);
}

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

static int nvme_process_ns_desc(struct nvme_ctrl *ctrl, struct nvme_ns_ids *ids,
		struct nvme_ns_id_desc *cur, bool *csi_seen)
{
	const char *warn_str = "ctrl returned bogus length:";
	void *data = cur;

	switch (cur->nidt) {
	case NVME_NIDT_EUI64:
		if (cur->nidl != NVME_NIDT_EUI64_LEN) {
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_EUI64\n",
				 warn_str, cur->nidl);
			return -1;
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_EUI64_LEN;
		memcpy(ids->eui64, data + sizeof(*cur), NVME_NIDT_EUI64_LEN);
		return NVME_NIDT_EUI64_LEN;
	case NVME_NIDT_NGUID:
		if (cur->nidl != NVME_NIDT_NGUID_LEN) {
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_NGUID\n",
				 warn_str, cur->nidl);
			return -1;
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_NGUID_LEN;
		memcpy(ids->nguid, data + sizeof(*cur), NVME_NIDT_NGUID_LEN);
		return NVME_NIDT_NGUID_LEN;
	case NVME_NIDT_UUID:
		if (cur->nidl != NVME_NIDT_UUID_LEN) {
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_UUID\n",
				 warn_str, cur->nidl);
			return -1;
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_UUID_LEN;
		uuid_copy(&ids->uuid, data + sizeof(*cur));
		return NVME_NIDT_UUID_LEN;
	case NVME_NIDT_CSI:
		if (cur->nidl != NVME_NIDT_CSI_LEN) {
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_CSI\n",
				 warn_str, cur->nidl);
			return -1;
		}
		memcpy(&ids->csi, data + sizeof(*cur), NVME_NIDT_CSI_LEN);
		*csi_seen = true;
		return NVME_NIDT_CSI_LEN;
	default:
		/* Skip unknown types */
		return cur->nidl;
	}
}

static int nvme_identify_ns_descs(struct nvme_ctrl *ctrl,
		struct nvme_ns_info *info)
{
	struct nvme_command c = { };
	bool csi_seen = false;
	int status, pos, len;
	void *data;

	if (ctrl->vs < NVME_VS(1, 3, 0) && !nvme_multi_css(ctrl))
		return 0;
	if (ctrl->quirks & NVME_QUIRK_NO_NS_DESC_LIST)
		return 0;

	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = cpu_to_le32(info->nsid);
	c.identify.cns = NVME_ID_CNS_NS_DESC_LIST;

	data = kzalloc(NVME_IDENTIFY_DATA_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	status = nvme_submit_sync_cmd(ctrl->admin_q, &c, data,
				      NVME_IDENTIFY_DATA_SIZE);
	if (status) {
		dev_warn(ctrl->device,
			"Identify Descriptors failed (nsid=%u, status=0x%x)\n",
			info->nsid, status);
		goto free_data;
	}

	for (pos = 0; pos < NVME_IDENTIFY_DATA_SIZE; pos += len) {
		struct nvme_ns_id_desc *cur = data + pos;

		if (cur->nidl == 0)
			break;

		len = nvme_process_ns_desc(ctrl, &info->ids, cur, &csi_seen);
		if (len < 0)
			break;

		len += sizeof(*cur);
	}

	if (nvme_multi_css(ctrl) && !csi_seen) {
		dev_warn(ctrl->device, "Command set not reported for nsid:%d\n",
			 info->nsid);
		status = -EINVAL;
	}

free_data:
	kfree(data);
	return status;
}

static int nvme_identify_ns(struct nvme_ctrl *ctrl, unsigned nsid,
			struct nvme_id_ns **id)
{
	struct nvme_command c = { };
	int error;

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = cpu_to_le32(nsid);
	c.identify.cns = NVME_ID_CNS_NS;

	*id = kmalloc(sizeof(**id), GFP_KERNEL);
	if (!*id)
		return -ENOMEM;

	error = nvme_submit_sync_cmd(ctrl->admin_q, &c, *id, sizeof(**id));
	if (error) {
		dev_warn(ctrl->device, "Identify namespace failed (%d)\n", error);
		kfree(*id);
	}
	return error;
}

static int nvme_ns_info_from_identify(struct nvme_ctrl *ctrl,
		struct nvme_ns_info *info)
{
	struct nvme_ns_ids *ids = &info->ids;
	struct nvme_id_ns *id;
	int ret;

	ret = nvme_identify_ns(ctrl, info->nsid, &id);
	if (ret)
		return ret;

	if (id->ncap == 0) {
		/* namespace not allocated or attached */
		info->is_removed = true;
		return -ENODEV;
	}

	info->anagrpid = id->anagrpid;
	info->is_shared = id->nmic & NVME_NS_NMIC_SHARED;
	info->is_readonly = id->nsattr & NVME_NS_ATTR_RO;
	info->is_ready = true;
	if (ctrl->quirks & NVME_QUIRK_BOGUS_NID) {
		dev_info(ctrl->device,
			 "Ignoring bogus Namespace Identifiers\n");
	} else {
		if (ctrl->vs >= NVME_VS(1, 1, 0) &&
		    !memchr_inv(ids->eui64, 0, sizeof(ids->eui64)))
			memcpy(ids->eui64, id->eui64, sizeof(ids->eui64));
		if (ctrl->vs >= NVME_VS(1, 2, 0) &&
		    !memchr_inv(ids->nguid, 0, sizeof(ids->nguid)))
			memcpy(ids->nguid, id->nguid, sizeof(ids->nguid));
	}
	kfree(id);
	return 0;
}

static int nvme_ns_info_from_id_cs_indep(struct nvme_ctrl *ctrl,
		struct nvme_ns_info *info)
{
	struct nvme_id_ns_cs_indep *id;
	struct nvme_command c = {
		.identify.opcode	= nvme_admin_identify,
		.identify.nsid		= cpu_to_le32(info->nsid),
		.identify.cns		= NVME_ID_CNS_NS_CS_INDEP,
	};
	int ret;

	id = kmalloc(sizeof(*id), GFP_KERNEL);
	if (!id)
		return -ENOMEM;

	ret = nvme_submit_sync_cmd(ctrl->admin_q, &c, id, sizeof(*id));
	if (!ret) {
		info->anagrpid = id->anagrpid;
		info->is_shared = id->nmic & NVME_NS_NMIC_SHARED;
		info->is_readonly = id->nsattr & NVME_NS_ATTR_RO;
		info->is_ready = id->nstat & NVME_NSTAT_NRDY;
	}
	kfree(id);
	return ret;
}

static int nvme_features(struct nvme_ctrl *dev, u8 op, unsigned int fid,
		unsigned int dword11, void *buffer, size_t buflen, u32 *result)
{
	union nvme_result res = { 0 };
	struct nvme_command c = { };
	int ret;

	c.features.opcode = op;
	c.features.fid = cpu_to_le32(fid);
	c.features.dword11 = cpu_to_le32(dword11);

	ret = __nvme_submit_sync_cmd(dev->admin_q, &c, &res,
			buffer, buflen, NVME_QID_ANY, 0, 0);
	if (ret >= 0 && result)
		*result = le32_to_cpu(res.u32);
	return ret;
}

int nvme_set_features(struct nvme_ctrl *dev, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      u32 *result)
{
	return nvme_features(dev, nvme_admin_set_features, fid, dword11, buffer,
			     buflen, result);
}
EXPORT_SYMBOL_GPL(nvme_set_features);

int nvme_get_features(struct nvme_ctrl *dev, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      u32 *result)
{
	return nvme_features(dev, nvme_admin_get_features, fid, dword11, buffer,
			     buflen, result);
}
EXPORT_SYMBOL_GPL(nvme_get_features);

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

#define NVME_AEN_SUPPORTED \
	(NVME_AEN_CFG_NS_ATTR | NVME_AEN_CFG_FW_ACT | \
	 NVME_AEN_CFG_ANA_CHANGE | NVME_AEN_CFG_DISC_CHANGE)

static void nvme_enable_aen(struct nvme_ctrl *ctrl)
{
	u32 result, supported_aens = ctrl->oaes & NVME_AEN_SUPPORTED;
	int status;

	if (!supported_aens)
		return;

	status = nvme_set_features(ctrl, NVME_FEAT_ASYNC_EVENT, supported_aens,
			NULL, 0, &result);
	if (status)
		dev_warn(ctrl->device, "Failed to configure AEN (cfg %x)\n",
			 supported_aens);

	queue_work(nvme_wq, &ctrl->async_event_work);
}

static int nvme_ns_open(struct nvme_ns *ns)
{

	/* should never be called due to GENHD_FL_HIDDEN */
	if (WARN_ON_ONCE(nvme_ns_head_multipath(ns->head)))
		goto fail;
	if (!nvme_get_ns(ns))
		goto fail;
	if (!try_module_get(ns->ctrl->ops->module))
		goto fail_put_ns;

	return 0;

fail_put_ns:
	nvme_put_ns(ns);
fail:
	return -ENXIO;
}

static void nvme_ns_release(struct nvme_ns *ns)
{

	module_put(ns->ctrl->ops->module);
	nvme_put_ns(ns);
}

static int nvme_open(struct gendisk *disk, blk_mode_t mode)
{
	return nvme_ns_open(disk->private_data);
}

static void nvme_release(struct gendisk *disk)
{
	nvme_ns_release(disk->private_data);
}

int nvme_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	/* some standard values */
	geo->heads = 1 << 6;
	geo->sectors = 1 << 5;
	geo->cylinders = get_capacity(bdev->bd_disk) >> 11;
	return 0;
}

#ifdef CONFIG_BLK_DEV_INTEGRITY
static void nvme_init_integrity(struct gendisk *disk, struct nvme_ns *ns,
				u32 max_integrity_segments)
{
	struct blk_integrity integrity = { };

	switch (ns->pi_type) {
	case NVME_NS_DPS_PI_TYPE3:
		switch (ns->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			integrity.profile = &t10_pi_type3_crc;
			integrity.tag_size = sizeof(u16) + sizeof(u32);
			integrity.flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
			break;
		case NVME_NVM_NS_64B_GUARD:
			integrity.profile = &ext_pi_type3_crc64;
			integrity.tag_size = sizeof(u16) + 6;
			integrity.flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
			break;
		default:
			integrity.profile = NULL;
			break;
		}
		break;
	case NVME_NS_DPS_PI_TYPE1:
	case NVME_NS_DPS_PI_TYPE2:
		switch (ns->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			integrity.profile = &t10_pi_type1_crc;
			integrity.tag_size = sizeof(u16);
			integrity.flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
			break;
		case NVME_NVM_NS_64B_GUARD:
			integrity.profile = &ext_pi_type1_crc64;
			integrity.tag_size = sizeof(u16);
			integrity.flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
			break;
		default:
			integrity.profile = NULL;
			break;
		}
		break;
	default:
		integrity.profile = NULL;
		break;
	}

	integrity.tuple_size = ns->ms;
	blk_integrity_register(disk, &integrity);
	blk_queue_max_integrity_segments(disk->queue, max_integrity_segments);
}
#else
static void nvme_init_integrity(struct gendisk *disk, struct nvme_ns *ns,
				u32 max_integrity_segments)
{
}
#endif /* CONFIG_BLK_DEV_INTEGRITY */

static void nvme_config_discard(struct gendisk *disk, struct nvme_ns *ns)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct request_queue *queue = disk->queue;
	u32 size = queue_logical_block_size(queue);

	if (ctrl->dmrsl && ctrl->dmrsl <= nvme_sect_to_lba(ns, UINT_MAX))
		ctrl->max_discard_sectors = nvme_lba_to_sect(ns, ctrl->dmrsl);

	if (ctrl->max_discard_sectors == 0) {
		blk_queue_max_discard_sectors(queue, 0);
		return;
	}

	BUILD_BUG_ON(PAGE_SIZE / sizeof(struct nvme_dsm_range) <
			NVME_DSM_MAX_RANGES);

	queue->limits.discard_granularity = size;

	/* If discard is already enabled, don't reset queue limits */
	if (queue->limits.max_discard_sectors)
		return;

	blk_queue_max_discard_sectors(queue, ctrl->max_discard_sectors);
	blk_queue_max_discard_segments(queue, ctrl->max_discard_segments);

	if (ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES)
		blk_queue_max_write_zeroes_sectors(queue, UINT_MAX);
}

static bool nvme_ns_ids_equal(struct nvme_ns_ids *a, struct nvme_ns_ids *b)
{
	return uuid_equal(&a->uuid, &b->uuid) &&
		memcmp(&a->nguid, &b->nguid, sizeof(a->nguid)) == 0 &&
		memcmp(&a->eui64, &b->eui64, sizeof(a->eui64)) == 0 &&
		a->csi == b->csi;
}

static int nvme_init_ms(struct nvme_ns *ns, struct nvme_id_ns *id)
{
	bool first = id->dps & NVME_NS_DPS_PI_FIRST;
	unsigned lbaf = nvme_lbaf_index(id->flbas);
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_command c = { };
	struct nvme_id_ns_nvm *nvm;
	int ret = 0;
	u32 elbaf;

	ns->pi_size = 0;
	ns->ms = le16_to_cpu(id->lbaf[lbaf].ms);
	if (!(ctrl->ctratt & NVME_CTRL_ATTR_ELBAS)) {
		ns->pi_size = sizeof(struct t10_pi_tuple);
		ns->guard_type = NVME_NVM_NS_16B_GUARD;
		goto set_pi;
	}

	nvm = kzalloc(sizeof(*nvm), GFP_KERNEL);
	if (!nvm)
		return -ENOMEM;

	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = cpu_to_le32(ns->head->ns_id);
	c.identify.cns = NVME_ID_CNS_CS_NS;
	c.identify.csi = NVME_CSI_NVM;

	ret = nvme_submit_sync_cmd(ns->ctrl->admin_q, &c, nvm, sizeof(*nvm));
	if (ret)
		goto free_data;

	elbaf = le32_to_cpu(nvm->elbaf[lbaf]);

	/* no support for storage tag formats right now */
	if (nvme_elbaf_sts(elbaf))
		goto free_data;

	ns->guard_type = nvme_elbaf_guard_type(elbaf);
	switch (ns->guard_type) {
	case NVME_NVM_NS_64B_GUARD:
		ns->pi_size = sizeof(struct crc64_pi_tuple);
		break;
	case NVME_NVM_NS_16B_GUARD:
		ns->pi_size = sizeof(struct t10_pi_tuple);
		break;
	default:
		break;
	}

free_data:
	kfree(nvm);
set_pi:
	if (ns->pi_size && (first || ns->ms == ns->pi_size))
		ns->pi_type = id->dps & NVME_NS_DPS_PI_MASK;
	else
		ns->pi_type = 0;

	return ret;
}

static void nvme_configure_metadata(struct nvme_ns *ns, struct nvme_id_ns *id)
{
	struct nvme_ctrl *ctrl = ns->ctrl;

	if (nvme_init_ms(ns, id))
		return;

	ns->features &= ~(NVME_NS_METADATA_SUPPORTED | NVME_NS_EXT_LBAS);
	if (!ns->ms || !(ctrl->ops->flags & NVME_F_METADATA_SUPPORTED))
		return;

	if (ctrl->ops->flags & NVME_F_FABRICS) {
		/*
		 * The NVMe over Fabrics specification only supports metadata as
		 * part of the extended data LBA.  We rely on HCA/HBA support to
		 * remap the separate metadata buffer from the block layer.
		 */
		if (WARN_ON_ONCE(!(id->flbas & NVME_NS_FLBAS_META_EXT)))
			return;

		ns->features |= NVME_NS_EXT_LBAS;

		/*
		 * The current fabrics transport drivers support namespace
		 * metadata formats only if nvme_ns_has_pi() returns true.
		 * Suppress support for all other formats so the namespace will
		 * have a 0 capacity and not be usable through the block stack.
		 *
		 * Note, this check will need to be modified if any drivers
		 * gain the ability to use other metadata formats.
		 */
		if (ctrl->max_integrity_segments && nvme_ns_has_pi(ns))
			ns->features |= NVME_NS_METADATA_SUPPORTED;
	} else {
		/*
		 * For PCIe controllers, we can't easily remap the separate
		 * metadata buffer from the block layer and thus require a
		 * separate metadata buffer for block layer metadata/PI support.
		 * We allow extended LBAs for the passthrough interface, though.
		 */
		if (id->flbas & NVME_NS_FLBAS_META_EXT)
			ns->features |= NVME_NS_EXT_LBAS;
		else
			ns->features |= NVME_NS_METADATA_SUPPORTED;
	}
}

static void nvme_set_queue_limits(struct nvme_ctrl *ctrl,
		struct request_queue *q)
{
	bool vwc = ctrl->vwc & NVME_CTRL_VWC_PRESENT;

	if (ctrl->max_hw_sectors) {
		u32 max_segments =
			(ctrl->max_hw_sectors / (NVME_CTRL_PAGE_SIZE >> 9)) + 1;

		max_segments = min_not_zero(max_segments, ctrl->max_segments);
		blk_queue_max_hw_sectors(q, ctrl->max_hw_sectors);
		blk_queue_max_segments(q, min_t(u32, max_segments, USHRT_MAX));
	}
	blk_queue_virt_boundary(q, NVME_CTRL_PAGE_SIZE - 1);
	blk_queue_dma_alignment(q, 3);
	blk_queue_write_cache(q, vwc, vwc);
}

static void nvme_update_disk_info(struct gendisk *disk,
		struct nvme_ns *ns, struct nvme_id_ns *id)
{
	sector_t capacity = nvme_lba_to_sect(ns, le64_to_cpu(id->nsze));
	u32 bs = 1U << ns->lba_shift;
	u32 atomic_bs, phys_bs, io_opt = 0;

	/*
	 * The block layer can't support LBA sizes larger than the page size
	 * yet, so catch this early and don't allow block I/O.
	 */
	if (ns->lba_shift > PAGE_SHIFT) {
		capacity = 0;
		bs = (1 << 9);
	}

	blk_integrity_unregister(disk);

	atomic_bs = phys_bs = bs;
	if (id->nabo == 0) {
		/*
		 * Bit 1 indicates whether NAWUPF is defined for this namespace
		 * and whether it should be used instead of AWUPF. If NAWUPF ==
		 * 0 then AWUPF must be used instead.
		 */
		if (id->nsfeat & NVME_NS_FEAT_ATOMICS && id->nawupf)
			atomic_bs = (1 + le16_to_cpu(id->nawupf)) * bs;
		else
			atomic_bs = (1 + ns->ctrl->subsys->awupf) * bs;
	}

	if (id->nsfeat & NVME_NS_FEAT_IO_OPT) {
		/* NPWG = Namespace Preferred Write Granularity */
		phys_bs = bs * (1 + le16_to_cpu(id->npwg));
		/* NOWS = Namespace Optimal Write Size */
		io_opt = bs * (1 + le16_to_cpu(id->nows));
	}

	blk_queue_logical_block_size(disk->queue, bs);
	/*
	 * Linux filesystems assume writing a single physical block is
	 * an atomic operation. Hence limit the physical block size to the
	 * value of the Atomic Write Unit Power Fail parameter.
	 */
	blk_queue_physical_block_size(disk->queue, min(phys_bs, atomic_bs));
	blk_queue_io_min(disk->queue, phys_bs);
	blk_queue_io_opt(disk->queue, io_opt);

	/*
	 * Register a metadata profile for PI, or the plain non-integrity NVMe
	 * metadata masquerading as Type 0 if supported, otherwise reject block
	 * I/O to namespaces with metadata except when the namespace supports
	 * PI, as it can strip/insert in that case.
	 */
	if (ns->ms) {
		if (IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY) &&
		    (ns->features & NVME_NS_METADATA_SUPPORTED))
			nvme_init_integrity(disk, ns,
					    ns->ctrl->max_integrity_segments);
		else if (!nvme_ns_has_pi(ns))
			capacity = 0;
	}

	set_capacity_and_notify(disk, capacity);

	nvme_config_discard(disk, ns);
	blk_queue_max_write_zeroes_sectors(disk->queue,
					   ns->ctrl->max_zeroes_sectors);
}

static bool nvme_ns_is_readonly(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	return info->is_readonly || test_bit(NVME_NS_FORCE_RO, &ns->flags);
}

static inline bool nvme_first_scan(struct gendisk *disk)
{
	/* nvme_alloc_ns() scans the disk prior to adding it */
	return !disk_live(disk);
}

static void nvme_set_chunk_sectors(struct nvme_ns *ns, struct nvme_id_ns *id)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	u32 iob;

	if ((ctrl->quirks & NVME_QUIRK_STRIPE_SIZE) &&
	    is_power_of_2(ctrl->max_hw_sectors))
		iob = ctrl->max_hw_sectors;
	else
		iob = nvme_lba_to_sect(ns, le16_to_cpu(id->noiob));

	if (!iob)
		return;

	if (!is_power_of_2(iob)) {
		if (nvme_first_scan(ns->disk))
			pr_warn("%s: ignoring unaligned IO boundary:%u\n",
				ns->disk->disk_name, iob);
		return;
	}

	if (blk_queue_is_zoned(ns->disk->queue)) {
		if (nvme_first_scan(ns->disk))
			pr_warn("%s: ignoring zoned namespace IO boundary\n",
				ns->disk->disk_name);
		return;
	}

	blk_queue_chunk_sectors(ns->queue, iob);
}

static int nvme_update_ns_info_generic(struct nvme_ns *ns,
		struct nvme_ns_info *info)
{
	blk_mq_freeze_queue(ns->disk->queue);
	nvme_set_queue_limits(ns->ctrl, ns->queue);
	set_disk_ro(ns->disk, nvme_ns_is_readonly(ns, info));
	blk_mq_unfreeze_queue(ns->disk->queue);

	if (nvme_ns_head_multipath(ns->head)) {
		blk_mq_freeze_queue(ns->head->disk->queue);
		set_disk_ro(ns->head->disk, nvme_ns_is_readonly(ns, info));
		nvme_mpath_revalidate_paths(ns);
		blk_stack_limits(&ns->head->disk->queue->limits,
				 &ns->queue->limits, 0);
		ns->head->disk->flags |= GENHD_FL_HIDDEN;
		blk_mq_unfreeze_queue(ns->head->disk->queue);
	}

	/* Hide the block-interface for these devices */
	ns->disk->flags |= GENHD_FL_HIDDEN;
	set_bit(NVME_NS_READY, &ns->flags);

	return 0;
}

static int nvme_update_ns_info_block(struct nvme_ns *ns,
		struct nvme_ns_info *info)
{
	struct nvme_id_ns *id;
	unsigned lbaf;
	int ret;

	ret = nvme_identify_ns(ns->ctrl, info->nsid, &id);
	if (ret)
		return ret;

	blk_mq_freeze_queue(ns->disk->queue);
	lbaf = nvme_lbaf_index(id->flbas);
	ns->lba_shift = id->lbaf[lbaf].ds;
	nvme_set_queue_limits(ns->ctrl, ns->queue);

	nvme_configure_metadata(ns, id);
	nvme_set_chunk_sectors(ns, id);
	nvme_update_disk_info(ns->disk, ns, id);

	if (ns->head->ids.csi == NVME_CSI_ZNS) {
		ret = nvme_update_zone_info(ns, lbaf);
		if (ret) {
			blk_mq_unfreeze_queue(ns->disk->queue);
			goto out;
		}
	}

	/*
	 * Only set the DEAC bit if the device guarantees that reads from
	 * deallocated data return zeroes.  While the DEAC bit does not
	 * require that, it must be a no-op if reads from deallocated data
	 * do not return zeroes.
	 */
	if ((id->dlfeat & 0x7) == 0x1 && (id->dlfeat & (1 << 3)))
		ns->features |= NVME_NS_DEAC;
	set_disk_ro(ns->disk, nvme_ns_is_readonly(ns, info));
	set_bit(NVME_NS_READY, &ns->flags);
	blk_mq_unfreeze_queue(ns->disk->queue);

	if (blk_queue_is_zoned(ns->queue)) {
		ret = nvme_revalidate_zones(ns);
		if (ret && !nvme_first_scan(ns->disk))
			goto out;
	}

	if (nvme_ns_head_multipath(ns->head)) {
		blk_mq_freeze_queue(ns->head->disk->queue);
		nvme_update_disk_info(ns->head->disk, ns, id);
		set_disk_ro(ns->head->disk, nvme_ns_is_readonly(ns, info));
		nvme_mpath_revalidate_paths(ns);
		blk_stack_limits(&ns->head->disk->queue->limits,
				 &ns->queue->limits, 0);
		disk_update_readahead(ns->head->disk);
		blk_mq_unfreeze_queue(ns->head->disk->queue);
	}

	ret = 0;
out:
	/*
	 * If probing fails due an unsupported feature, hide the block device,
	 * but still allow other access.
	 */
	if (ret == -ENODEV) {
		ns->disk->flags |= GENHD_FL_HIDDEN;
		set_bit(NVME_NS_READY, &ns->flags);
		ret = 0;
	}
	kfree(id);
	return ret;
}

static int nvme_update_ns_info(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	switch (info->ids.csi) {
	case NVME_CSI_ZNS:
		if (!IS_ENABLED(CONFIG_BLK_DEV_ZONED)) {
			dev_info(ns->ctrl->device,
	"block device for nsid %u not supported without CONFIG_BLK_DEV_ZONED\n",
				info->nsid);
			return nvme_update_ns_info_generic(ns, info);
		}
		return nvme_update_ns_info_block(ns, info);
	case NVME_CSI_NVM:
		return nvme_update_ns_info_block(ns, info);
	default:
		dev_info(ns->ctrl->device,
			"block device for nsid %u not supported (csi %u)\n",
			info->nsid, info->ids.csi);
		return nvme_update_ns_info_generic(ns, info);
	}
}

#ifdef CONFIG_BLK_SED_OPAL
static int nvme_sec_submit(void *data, u16 spsp, u8 secp, void *buffer, size_t len,
		bool send)
{
	struct nvme_ctrl *ctrl = data;
	struct nvme_command cmd = { };

	if (send)
		cmd.common.opcode = nvme_admin_security_send;
	else
		cmd.common.opcode = nvme_admin_security_recv;
	cmd.common.nsid = 0;
	cmd.common.cdw10 = cpu_to_le32(((u32)secp) << 24 | ((u32)spsp) << 8);
	cmd.common.cdw11 = cpu_to_le32(len);

	return __nvme_submit_sync_cmd(ctrl->admin_q, &cmd, NULL, buffer, len,
			NVME_QID_ANY, 1, 0);
}

static void nvme_configure_opal(struct nvme_ctrl *ctrl, bool was_suspended)
{
	if (ctrl->oacs & NVME_CTRL_OACS_SEC_SUPP) {
		if (!ctrl->opal_dev)
			ctrl->opal_dev = init_opal_dev(ctrl, &nvme_sec_submit);
		else if (was_suspended)
			opal_unlock_from_suspend(ctrl->opal_dev);
	} else {
		free_opal_dev(ctrl->opal_dev);
		ctrl->opal_dev = NULL;
	}
}
#else
static void nvme_configure_opal(struct nvme_ctrl *ctrl, bool was_suspended)
{
}
#endif /* CONFIG_BLK_SED_OPAL */

#ifdef CONFIG_BLK_DEV_ZONED
static int nvme_report_zones(struct gendisk *disk, sector_t sector,
		unsigned int nr_zones, report_zones_cb cb, void *data)
{
	return nvme_ns_report_zones(disk->private_data, sector, nr_zones, cb,
			data);
}
#else
#define nvme_report_zones	NULL
#endif /* CONFIG_BLK_DEV_ZONED */

const struct block_device_operations nvme_bdev_ops = {
	.owner		= THIS_MODULE,
	.ioctl		= nvme_ioctl,
	.compat_ioctl	= blkdev_compat_ptr_ioctl,
	.open		= nvme_open,
	.release	= nvme_release,
	.getgeo		= nvme_getgeo,
	.report_zones	= nvme_report_zones,
	.pr_ops		= &nvme_pr_ops,
};

static int nvme_wait_ready(struct nvme_ctrl *ctrl, u32 mask, u32 val,
		u32 timeout, const char *op)
{
	unsigned long timeout_jiffies = jiffies + timeout * HZ;
	u32 csts;
	int ret;

	while ((ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts)) == 0) {
		if (csts == ~0)
			return -ENODEV;
		if ((csts & mask) == val)
			break;

		usleep_range(1000, 2000);
		if (fatal_signal_pending(current))
			return -EINTR;
		if (time_after(jiffies, timeout_jiffies)) {
			dev_err(ctrl->device,
				"Device not ready; aborting %s, CSTS=0x%x\n",
				op, csts);
			return -ENODEV;
		}
	}

	return ret;
}

int nvme_disable_ctrl(struct nvme_ctrl *ctrl, bool shutdown)
{
	int ret;

	ctrl->ctrl_config &= ~NVME_CC_SHN_MASK;
	if (shutdown)
		ctrl->ctrl_config |= NVME_CC_SHN_NORMAL;
	else
		ctrl->ctrl_config &= ~NVME_CC_ENABLE;

	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);
	if (ret)
		return ret;

	if (shutdown) {
		return nvme_wait_ready(ctrl, NVME_CSTS_SHST_MASK,
				       NVME_CSTS_SHST_CMPLT,
				       ctrl->shutdown_timeout, "shutdown");
	}
	if (ctrl->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY)
		msleep(NVME_QUIRK_DELAY_AMOUNT);
	return nvme_wait_ready(ctrl, NVME_CSTS_RDY, 0,
			       (NVME_CAP_TIMEOUT(ctrl->cap) + 1) / 2, "reset");
}
EXPORT_SYMBOL_GPL(nvme_disable_ctrl);

int nvme_enable_ctrl(struct nvme_ctrl *ctrl)
{
	unsigned dev_page_min;
	u32 timeout;
	int ret;

	ret = ctrl->ops->reg_read64(ctrl, NVME_REG_CAP, &ctrl->cap);
	if (ret) {
		dev_err(ctrl->device, "Reading CAP failed (%d)\n", ret);
		return ret;
	}
	dev_page_min = NVME_CAP_MPSMIN(ctrl->cap) + 12;

	if (NVME_CTRL_PAGE_SHIFT < dev_page_min) {
		dev_err(ctrl->device,
			"Minimum device page size %u too large for host (%u)\n",
			1 << dev_page_min, 1 << NVME_CTRL_PAGE_SHIFT);
		return -ENODEV;
	}

	if (NVME_CAP_CSS(ctrl->cap) & NVME_CAP_CSS_CSI)
		ctrl->ctrl_config = NVME_CC_CSS_CSI;
	else
		ctrl->ctrl_config = NVME_CC_CSS_NVM;

	if (ctrl->cap & NVME_CAP_CRMS_CRWMS && ctrl->cap & NVME_CAP_CRMS_CRIMS)
		ctrl->ctrl_config |= NVME_CC_CRIME;

	ctrl->ctrl_config |= (NVME_CTRL_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT;
	ctrl->ctrl_config |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
	ctrl->ctrl_config |= NVME_CC_IOSQES | NVME_CC_IOCQES;
	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);
	if (ret)
		return ret;

	/* Flush write to device (required if transport is PCI) */
	ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CC, &ctrl->ctrl_config);
	if (ret)
		return ret;

	/* CAP value may change after initial CC write */
	ret = ctrl->ops->reg_read64(ctrl, NVME_REG_CAP, &ctrl->cap);
	if (ret)
		return ret;

	timeout = NVME_CAP_TIMEOUT(ctrl->cap);
	if (ctrl->cap & NVME_CAP_CRMS_CRWMS) {
		u32 crto, ready_timeout;

		ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CRTO, &crto);
		if (ret) {
			dev_err(ctrl->device, "Reading CRTO failed (%d)\n",
				ret);
			return ret;
		}

		/*
		 * CRTO should always be greater or equal to CAP.TO, but some
		 * devices are known to get this wrong. Use the larger of the
		 * two values.
		 */
		if (ctrl->ctrl_config & NVME_CC_CRIME)
			ready_timeout = NVME_CRTO_CRIMT(crto);
		else
			ready_timeout = NVME_CRTO_CRWMT(crto);

		if (ready_timeout < timeout)
			dev_warn_once(ctrl->device, "bad crto:%x cap:%llx\n",
				      crto, ctrl->cap);
		else
			timeout = ready_timeout;
	}

	ctrl->ctrl_config |= NVME_CC_ENABLE;
	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);
	if (ret)
		return ret;
	return nvme_wait_ready(ctrl, NVME_CSTS_RDY, NVME_CSTS_RDY,
			       (timeout + 1) / 2, "initialisation");
}
EXPORT_SYMBOL_GPL(nvme_enable_ctrl);

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

static int nvme_configure_host_options(struct nvme_ctrl *ctrl)
{
	struct nvme_feat_host_behavior *host;
	u8 acre = 0, lbafee = 0;
	int ret;

	/* Don't bother enabling the feature if retry delay is not reported */
	if (ctrl->crdt[0])
		acre = NVME_ENABLE_ACRE;
	if (ctrl->ctratt & NVME_CTRL_ATTR_ELBAS)
		lbafee = NVME_ENABLE_LBAFEE;

	if (!acre && !lbafee)
		return 0;

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return 0;

	host->acre = acre;
	host->lbafee = lbafee;
	ret = nvme_set_features(ctrl, NVME_FEAT_HOST_BEHAVIOR, 0,
				host, sizeof(*host), NULL);
	kfree(host);
	return ret;
}

/*
 * The function checks whether the given total (exlat + enlat) latency of
 * a power state allows the latter to be used as an APST transition target.
 * It does so by comparing the latency to the primary and secondary latency
 * tolerances defined by module params. If there's a match, the corresponding
 * timeout value is returned and the matching tolerance index (1 or 2) is
 * reported.
 */
static bool nvme_apst_get_transition_time(u64 total_latency,
		u64 *transition_time, unsigned *last_index)
{
	if (total_latency <= apst_primary_latency_tol_us) {
		if (*last_index == 1)
			return false;
		*last_index = 1;
		*transition_time = apst_primary_timeout_ms;
		return true;
	}
	if (apst_secondary_timeout_ms &&
		total_latency <= apst_secondary_latency_tol_us) {
		if (*last_index <= 2)
			return false;
		*last_index = 2;
		*transition_time = apst_secondary_timeout_ms;
		return true;
	}
	return false;
}

/*
 * APST (Autonomous Power State Transition) lets us program a table of power
 * state transitions that the controller will perform automatically.
 *
 * Depending on module params, one of the two supported techniques will be used:
 *
 * - If the parameters provide explicit timeouts and tolerances, they will be
 *   used to build a table with up to 2 non-operational states to transition to.
 *   The default parameter values were selected based on the values used by
 *   Microsoft's and Intel's NVMe drivers. Yet, since we don't implement dynamic
 *   regeneration of the APST table in the event of switching between external
 *   and battery power, the timeouts and tolerances reflect a compromise
 *   between values used by Microsoft for AC and battery scenarios.
 * - If not, we'll configure the table with a simple heuristic: we are willing
 *   to spend at most 2% of the time transitioning between power states.
 *   Therefore, when running in any given state, we will enter the next
 *   lower-power non-operational state after waiting 50 * (enlat + exlat)
 *   microseconds, as long as that state's exit latency is under the requested
 *   maximum latency.
 *
 * We will not autonomously enter any non-operational state for which the total
 * latency exceeds ps_max_latency_us.
 *
 * Users can set ps_max_latency_us to zero to turn off APST.
 */
static int nvme_configure_apst(struct nvme_ctrl *ctrl)
{
	struct nvme_feat_auto_pst *table;
	unsigned apste = 0;
	u64 max_lat_us = 0;
	__le64 target = 0;
	int max_ps = -1;
	int state;
	int ret;
	unsigned last_lt_index = UINT_MAX;

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
		dev_dbg(ctrl->device, "APST disabled\n");
		goto done;
	}

	/*
	 * Walk through all states from lowest- to highest-power.
	 * According to the spec, lower-numbered states use more power.  NPSS,
	 * despite the name, is the index of the lowest-power state, not the
	 * number of states.
	 */
	for (state = (int)ctrl->npss; state >= 0; state--) {
		u64 total_latency_us, exit_latency_us, transition_ms;

		if (target)
			table->entries[state] = target;

		/*
		 * Don't allow transitions to the deepest state if it's quirked
		 * off.
		 */
		if (state == ctrl->npss &&
		    (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS))
			continue;

		/*
		 * Is this state a useful non-operational state for higher-power
		 * states to autonomously transition to?
		 */
		if (!(ctrl->psd[state].flags & NVME_PS_FLAGS_NON_OP_STATE))
			continue;

		exit_latency_us = (u64)le32_to_cpu(ctrl->psd[state].exit_lat);
		if (exit_latency_us > ctrl->ps_max_latency_us)
			continue;

		total_latency_us = exit_latency_us +
			le32_to_cpu(ctrl->psd[state].entry_lat);

		/*
		 * This state is good. It can be used as the APST idle target
		 * for higher power states.
		 */
		if (apst_primary_timeout_ms && apst_primary_latency_tol_us) {
			if (!nvme_apst_get_transition_time(total_latency_us,
					&transition_ms, &last_lt_index))
				continue;
		} else {
			transition_ms = total_latency_us + 19;
			do_div(transition_ms, 20);
			if (transition_ms > (1 << 24) - 1)
				transition_ms = (1 << 24) - 1;
		}

		target = cpu_to_le64((state << 3) | (transition_ms << 8));
		if (max_ps == -1)
			max_ps = state;
		if (total_latency_us > max_lat_us)
			max_lat_us = total_latency_us;
	}

	if (max_ps == -1)
		dev_dbg(ctrl->device, "APST enabled but no non-operational states are available\n");
	else
		dev_dbg(ctrl->device, "APST enabled: max PS = %d, max round-trip latency = %lluus, table = %*phN\n",
			max_ps, max_lat_us, (int)sizeof(*table), table);
	apste = 1;

done:
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
		if (ctrl->state == NVME_CTRL_LIVE)
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
	},
	{
		/*
		 * This LiteON CL1-3D*-Q11 firmware version has a race
		 * condition associated with actions related to suspend to idle
		 * LiteON has resolved the problem in future firmware
		 */
		.vid = 0x14a4,
		.fr = "22301111",
		.quirks = NVME_QUIRK_SIMPLE_SUSPEND,
	},
	{
		/*
		 * This Kioxia CD6-V Series / HPE PE8030 device times out and
		 * aborts I/O during any load, but more easily reproducible
		 * with discards (fstrim).
		 *
		 * The device is left in a state where it is also not possible
		 * to use "nvme set-feature" to disable APST, but booting with
		 * nvme_core.default_ps_max_latency=0 works.
		 */
		.vid = 0x1e0f,
		.mn = "KCD6XVUL6T40",
		.quirks = NVME_QUIRK_NO_APST,
	},
	{
		/*
		 * The external Samsung X5 SSD fails initialization without a
		 * delay before checking if it is ready and has a whole set of
		 * other problems.  To make this even more interesting, it
		 * shares the PCI ID with internal Samsung 970 Evo Plus that
		 * does not need or want these quirks.
		 */
		.vid = 0x144d,
		.mn = "Samsung Portable SSD X5",
		.quirks = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |
			  NVME_QUIRK_NO_DEEPEST_PS |
			  NVME_QUIRK_IGNORE_DEV_SUBNQN,
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

static void nvme_init_subnqn(struct nvme_subsystem *subsys, struct nvme_ctrl *ctrl,
		struct nvme_id_ctrl *id)
{
	size_t nqnlen;
	int off;

	if(!(ctrl->quirks & NVME_QUIRK_IGNORE_DEV_SUBNQN)) {
		nqnlen = strnlen(id->subnqn, NVMF_NQN_SIZE);
		if (nqnlen > 0 && nqnlen < NVMF_NQN_SIZE) {
			strscpy(subsys->subnqn, id->subnqn, NVMF_NQN_SIZE);
			return;
		}

		if (ctrl->vs >= NVME_VS(1, 2, 1))
			dev_warn(ctrl->device, "missing or invalid SUBNQN field.\n");
	}

	/*
	 * Generate a "fake" NQN similar to the one in Section 4.5 of the NVMe
	 * Base Specification 2.0.  It is slightly different from the format
	 * specified there due to historic reasons, and we can't change it now.
	 */
	off = snprintf(subsys->subnqn, NVMF_NQN_SIZE,
			"nqn.2014.08.org.nvmexpress:%04x%04x",
			le16_to_cpu(id->vid), le16_to_cpu(id->ssvid));
	memcpy(subsys->subnqn + off, id->sn, sizeof(id->sn));
	off += sizeof(id->sn);
	memcpy(subsys->subnqn + off, id->mn, sizeof(id->mn));
	off += sizeof(id->mn);
	memset(subsys->subnqn + off, 0, sizeof(subsys->subnqn) - off);
}

static void nvme_release_subsystem(struct device *dev)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);

	if (subsys->instance >= 0)
		ida_free(&nvme_instance_ida, subsys->instance);
	kfree(subsys);
}

static void nvme_destroy_subsystem(struct kref *ref)
{
	struct nvme_subsystem *subsys =
			container_of(ref, struct nvme_subsystem, ref);

	mutex_lock(&nvme_subsystems_lock);
	list_del(&subsys->entry);
	mutex_unlock(&nvme_subsystems_lock);

	ida_destroy(&subsys->ns_ida);
	device_del(&subsys->dev);
	put_device(&subsys->dev);
}

static void nvme_put_subsystem(struct nvme_subsystem *subsys)
{
	kref_put(&subsys->ref, nvme_destroy_subsystem);
}

static struct nvme_subsystem *__nvme_find_get_subsystem(const char *subsysnqn)
{
	struct nvme_subsystem *subsys;

	lockdep_assert_held(&nvme_subsystems_lock);

	/*
	 * Fail matches for discovery subsystems. This results
	 * in each discovery controller bound to a unique subsystem.
	 * This avoids issues with validating controller values
	 * that can only be true when there is a single unique subsystem.
	 * There may be multiple and completely independent entities
	 * that provide discovery controllers.
	 */
	if (!strcmp(subsysnqn, NVME_DISC_SUBSYS_NAME))
		return NULL;

	list_for_each_entry(subsys, &nvme_subsystems, entry) {
		if (strcmp(subsys->subnqn, subsysnqn))
			continue;
		if (!kref_get_unless_zero(&subsys->ref))
			continue;
		return subsys;
	}

	return NULL;
}

static inline bool nvme_discovery_ctrl(struct nvme_ctrl *ctrl)
{
	return ctrl->opts && ctrl->opts->discovery_nqn;
}

static bool nvme_validate_cntlid(struct nvme_subsystem *subsys,
		struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	struct nvme_ctrl *tmp;

	lockdep_assert_held(&nvme_subsystems_lock);

	list_for_each_entry(tmp, &subsys->ctrls, subsys_entry) {
		if (nvme_state_terminal(tmp))
			continue;

		if (tmp->cntlid == ctrl->cntlid) {
			dev_err(ctrl->device,
				"Duplicate cntlid %u with %s, subsys %s, rejecting\n",
				ctrl->cntlid, dev_name(tmp->device),
				subsys->subnqn);
			return false;
		}

		if ((id->cmic & NVME_CTRL_CMIC_MULTI_CTRL) ||
		    nvme_discovery_ctrl(ctrl))
			continue;

		dev_err(ctrl->device,
			"Subsystem does not support multiple controllers\n");
		return false;
	}

	return true;
}

static int nvme_init_subsystem(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	struct nvme_subsystem *subsys, *found;
	int ret;

	subsys = kzalloc(sizeof(*subsys), GFP_KERNEL);
	if (!subsys)
		return -ENOMEM;

	subsys->instance = -1;
	mutex_init(&subsys->lock);
	kref_init(&subsys->ref);
	INIT_LIST_HEAD(&subsys->ctrls);
	INIT_LIST_HEAD(&subsys->nsheads);
	nvme_init_subnqn(subsys, ctrl, id);
	memcpy(subsys->serial, id->sn, sizeof(subsys->serial));
	memcpy(subsys->model, id->mn, sizeof(subsys->model));
	subsys->vendor_id = le16_to_cpu(id->vid);
	subsys->cmic = id->cmic;

	/* Versions prior to 1.4 don't necessarily report a valid type */
	if (id->cntrltype == NVME_CTRL_DISC ||
	    !strcmp(subsys->subnqn, NVME_DISC_SUBSYS_NAME))
		subsys->subtype = NVME_NQN_DISC;
	else
		subsys->subtype = NVME_NQN_NVME;

	if (nvme_discovery_ctrl(ctrl) && subsys->subtype != NVME_NQN_DISC) {
		dev_err(ctrl->device,
			"Subsystem %s is not a discovery controller",
			subsys->subnqn);
		kfree(subsys);
		return -EINVAL;
	}
	subsys->awupf = le16_to_cpu(id->awupf);
	nvme_mpath_default_iopolicy(subsys);

	subsys->dev.class = nvme_subsys_class;
	subsys->dev.release = nvme_release_subsystem;
	subsys->dev.groups = nvme_subsys_attrs_groups;
	dev_set_name(&subsys->dev, "nvme-subsys%d", ctrl->instance);
	device_initialize(&subsys->dev);

	mutex_lock(&nvme_subsystems_lock);
	found = __nvme_find_get_subsystem(subsys->subnqn);
	if (found) {
		put_device(&subsys->dev);
		subsys = found;

		if (!nvme_validate_cntlid(subsys, ctrl, id)) {
			ret = -EINVAL;
			goto out_put_subsystem;
		}
	} else {
		ret = device_add(&subsys->dev);
		if (ret) {
			dev_err(ctrl->device,
				"failed to register subsystem device.\n");
			put_device(&subsys->dev);
			goto out_unlock;
		}
		ida_init(&subsys->ns_ida);
		list_add_tail(&subsys->entry, &nvme_subsystems);
	}

	ret = sysfs_create_link(&subsys->dev.kobj, &ctrl->device->kobj,
				dev_name(ctrl->device));
	if (ret) {
		dev_err(ctrl->device,
			"failed to create sysfs link from subsystem.\n");
		goto out_put_subsystem;
	}

	if (!found)
		subsys->instance = ctrl->instance;
	ctrl->subsys = subsys;
	list_add_tail(&ctrl->subsys_entry, &subsys->ctrls);
	mutex_unlock(&nvme_subsystems_lock);
	return 0;

out_put_subsystem:
	nvme_put_subsystem(subsys);
out_unlock:
	mutex_unlock(&nvme_subsystems_lock);
	return ret;
}

int nvme_get_log(struct nvme_ctrl *ctrl, u32 nsid, u8 log_page, u8 lsp, u8 csi,
		void *log, size_t size, u64 offset)
{
	struct nvme_command c = { };
	u32 dwlen = nvme_bytes_to_numd(size);

	c.get_log_page.opcode = nvme_admin_get_log_page;
	c.get_log_page.nsid = cpu_to_le32(nsid);
	c.get_log_page.lid = log_page;
	c.get_log_page.lsp = lsp;
	c.get_log_page.numdl = cpu_to_le16(dwlen & ((1 << 16) - 1));
	c.get_log_page.numdu = cpu_to_le16(dwlen >> 16);
	c.get_log_page.lpol = cpu_to_le32(lower_32_bits(offset));
	c.get_log_page.lpou = cpu_to_le32(upper_32_bits(offset));
	c.get_log_page.csi = csi;

	return nvme_submit_sync_cmd(ctrl->admin_q, &c, log, size);
}

static int nvme_get_effects_log(struct nvme_ctrl *ctrl, u8 csi,
				struct nvme_effects_log **log)
{
	struct nvme_effects_log	*cel = xa_load(&ctrl->cels, csi);
	int ret;

	if (cel)
		goto out;

	cel = kzalloc(sizeof(*cel), GFP_KERNEL);
	if (!cel)
		return -ENOMEM;

	ret = nvme_get_log(ctrl, 0x00, NVME_LOG_CMD_EFFECTS, 0, csi,
			cel, sizeof(*cel), 0);
	if (ret) {
		kfree(cel);
		return ret;
	}

	xa_store(&ctrl->cels, csi, cel, GFP_KERNEL);
out:
	*log = cel;
	return 0;
}

static inline u32 nvme_mps_to_sectors(struct nvme_ctrl *ctrl, u32 units)
{
	u32 page_shift = NVME_CAP_MPSMIN(ctrl->cap) + 12, val;

	if (check_shl_overflow(1U, units + page_shift - 9, &val))
		return UINT_MAX;
	return val;
}

static int nvme_init_non_mdts_limits(struct nvme_ctrl *ctrl)
{
	struct nvme_command c = { };
	struct nvme_id_ctrl_nvm *id;
	int ret;

	if (ctrl->oncs & NVME_CTRL_ONCS_DSM) {
		ctrl->max_discard_sectors = UINT_MAX;
		ctrl->max_discard_segments = NVME_DSM_MAX_RANGES;
	} else {
		ctrl->max_discard_sectors = 0;
		ctrl->max_discard_segments = 0;
	}

	/*
	 * Even though NVMe spec explicitly states that MDTS is not applicable
	 * to the write-zeroes, we are cautious and limit the size to the
	 * controllers max_hw_sectors value, which is based on the MDTS field
	 * and possibly other limiting factors.
	 */
	if ((ctrl->oncs & NVME_CTRL_ONCS_WRITE_ZEROES) &&
	    !(ctrl->quirks & NVME_QUIRK_DISABLE_WRITE_ZEROES))
		ctrl->max_zeroes_sectors = ctrl->max_hw_sectors;
	else
		ctrl->max_zeroes_sectors = 0;

	if (ctrl->subsys->subtype != NVME_NQN_NVME ||
	    nvme_ctrl_limited_cns(ctrl) ||
	    test_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags))
		return 0;

	id = kzalloc(sizeof(*id), GFP_KERNEL);
	if (!id)
		return -ENOMEM;

	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = NVME_ID_CNS_CS_CTRL;
	c.identify.csi = NVME_CSI_NVM;

	ret = nvme_submit_sync_cmd(ctrl->admin_q, &c, id, sizeof(*id));
	if (ret)
		goto free_data;

	if (id->dmrl)
		ctrl->max_discard_segments = id->dmrl;
	ctrl->dmrsl = le32_to_cpu(id->dmrsl);
	if (id->wzsl)
		ctrl->max_zeroes_sectors = nvme_mps_to_sectors(ctrl, id->wzsl);

free_data:
	if (ret > 0)
		set_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags);
	kfree(id);
	return ret;
}

static void nvme_init_known_nvm_effects(struct nvme_ctrl *ctrl)
{
	struct nvme_effects_log	*log = ctrl->effects;

	log->acs[nvme_admin_format_nvm] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC |
						NVME_CMD_EFFECTS_NCC |
						NVME_CMD_EFFECTS_CSE_MASK);
	log->acs[nvme_admin_sanitize_nvm] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC |
						NVME_CMD_EFFECTS_CSE_MASK);

	/*
	 * The spec says the result of a security receive command depends on
	 * the previous security send command. As such, many vendors log this
	 * command as one to submitted only when no other commands to the same
	 * namespace are outstanding. The intention is to tell the host to
	 * prevent mixing security send and receive.
	 *
	 * This driver can only enforce such exclusive access against IO
	 * queues, though. We are not readily able to enforce such a rule for
	 * two commands to the admin queue, which is the only queue that
	 * matters for this command.
	 *
	 * Rather than blindly freezing the IO queues for this effect that
	 * doesn't even apply to IO, mask it off.
	 */
	log->acs[nvme_admin_security_recv] &= cpu_to_le32(~NVME_CMD_EFFECTS_CSE_MASK);

	log->iocs[nvme_cmd_write] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC);
	log->iocs[nvme_cmd_write_zeroes] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC);
	log->iocs[nvme_cmd_write_uncor] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC);
}

static int nvme_init_effects(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	int ret = 0;

	if (ctrl->effects)
		return 0;

	if (id->lpa & NVME_CTRL_LPA_CMD_EFFECTS_LOG) {
		ret = nvme_get_effects_log(ctrl, NVME_CSI_NVM, &ctrl->effects);
		if (ret < 0)
			return ret;
	}

	if (!ctrl->effects) {
		ctrl->effects = kzalloc(sizeof(*ctrl->effects), GFP_KERNEL);
		if (!ctrl->effects)
			return -ENOMEM;
		xa_store(&ctrl->cels, NVME_CSI_NVM, ctrl->effects, GFP_KERNEL);
	}

	nvme_init_known_nvm_effects(ctrl);
	return 0;
}

static int nvme_init_identify(struct nvme_ctrl *ctrl)
{
	struct nvme_id_ctrl *id;
	u32 max_hw_sectors;
	bool prev_apst_enabled;
	int ret;

	ret = nvme_identify_ctrl(ctrl, &id);
	if (ret) {
		dev_err(ctrl->device, "Identify Controller failed (%d)\n", ret);
		return -EIO;
	}

	if (!(ctrl->ops->flags & NVME_F_FABRICS))
		ctrl->cntlid = le16_to_cpu(id->cntlid);

	if (!ctrl->identified) {
		unsigned int i;

		/*
		 * Check for quirks.  Quirk can depend on firmware version,
		 * so, in principle, the set of quirks present can change
		 * across a reset.  As a possible future enhancement, we
		 * could re-scan for quirks every time we reinitialize
		 * the device, but we'd have to make sure that the driver
		 * behaves intelligently if the quirks change.
		 */
		for (i = 0; i < ARRAY_SIZE(core_quirks); i++) {
			if (quirk_matches(id, &core_quirks[i]))
				ctrl->quirks |= core_quirks[i].quirks;
		}

		ret = nvme_init_subsystem(ctrl, id);
		if (ret)
			goto out_free;

		ret = nvme_init_effects(ctrl, id);
		if (ret)
			goto out_free;
	}
	memcpy(ctrl->subsys->firmware_rev, id->fr,
	       sizeof(ctrl->subsys->firmware_rev));

	if (force_apst && (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS)) {
		dev_warn(ctrl->device, "forcibly allowing all power states due to nvme_core.force_apst -- use at your own risk\n");
		ctrl->quirks &= ~NVME_QUIRK_NO_DEEPEST_PS;
	}

	ctrl->crdt[0] = le16_to_cpu(id->crdt1);
	ctrl->crdt[1] = le16_to_cpu(id->crdt2);
	ctrl->crdt[2] = le16_to_cpu(id->crdt3);

	ctrl->oacs = le16_to_cpu(id->oacs);
	ctrl->oncs = le16_to_cpu(id->oncs);
	ctrl->mtfa = le16_to_cpu(id->mtfa);
	ctrl->oaes = le32_to_cpu(id->oaes);
	ctrl->wctemp = le16_to_cpu(id->wctemp);
	ctrl->cctemp = le16_to_cpu(id->cctemp);

	atomic_set(&ctrl->abort_limit, id->acl + 1);
	ctrl->vwc = id->vwc;
	if (id->mdts)
		max_hw_sectors = nvme_mps_to_sectors(ctrl, id->mdts);
	else
		max_hw_sectors = UINT_MAX;
	ctrl->max_hw_sectors =
		min_not_zero(ctrl->max_hw_sectors, max_hw_sectors);

	nvme_set_queue_limits(ctrl, ctrl->admin_q);
	ctrl->sgls = le32_to_cpu(id->sgls);
	ctrl->kas = le16_to_cpu(id->kas);
	ctrl->max_namespaces = le32_to_cpu(id->mnan);
	ctrl->ctratt = le32_to_cpu(id->ctratt);

	ctrl->cntrltype = id->cntrltype;
	ctrl->dctype = id->dctype;

	if (id->rtd3e) {
		/* us -> s */
		u32 transition_time = le32_to_cpu(id->rtd3e) / USEC_PER_SEC;

		ctrl->shutdown_timeout = clamp_t(unsigned int, transition_time,
						 shutdown_timeout, 60);

		if (ctrl->shutdown_timeout != shutdown_timeout)
			dev_info(ctrl->device,
				 "Shutdown timeout set to %u seconds\n",
				 ctrl->shutdown_timeout);
	} else
		ctrl->shutdown_timeout = shutdown_timeout;

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
			dev_err(ctrl->device,
				"Mismatching cntlid: Connect %u vs Identify "
				"%u, rejecting\n",
				ctrl->cntlid, le16_to_cpu(id->cntlid));
			ret = -EINVAL;
			goto out_free;
		}

		if (!nvme_discovery_ctrl(ctrl) && !ctrl->kas) {
			dev_err(ctrl->device,
				"keep-alive support is mandatory for fabrics\n");
			ret = -EINVAL;
			goto out_free;
		}
	} else {
		ctrl->hmpre = le32_to_cpu(id->hmpre);
		ctrl->hmmin = le32_to_cpu(id->hmmin);
		ctrl->hmminds = le32_to_cpu(id->hmminds);
		ctrl->hmmaxd = le16_to_cpu(id->hmmaxd);
	}

	ret = nvme_mpath_init_identify(ctrl, id);
	if (ret < 0)
		goto out_free;

	if (ctrl->apst_enabled && !prev_apst_enabled)
		dev_pm_qos_expose_latency_tolerance(ctrl->device);
	else if (!ctrl->apst_enabled && prev_apst_enabled)
		dev_pm_qos_hide_latency_tolerance(ctrl->device);

out_free:
	kfree(id);
	return ret;
}

/*
 * Initialize the cached copies of the Identify data and various controller
 * register in our nvme_ctrl structure.  This should be called as soon as
 * the admin queue is fully up and running.
 */
int nvme_init_ctrl_finish(struct nvme_ctrl *ctrl, bool was_suspended)
{
	int ret;

	ret = ctrl->ops->reg_read32(ctrl, NVME_REG_VS, &ctrl->vs);
	if (ret) {
		dev_err(ctrl->device, "Reading VS failed (%d)\n", ret);
		return ret;
	}

	ctrl->sqsize = min_t(u16, NVME_CAP_MQES(ctrl->cap), ctrl->sqsize);

	if (ctrl->vs >= NVME_VS(1, 1, 0))
		ctrl->subsystem = NVME_CAP_NSSRC(ctrl->cap);

	ret = nvme_init_identify(ctrl);
	if (ret)
		return ret;

	ret = nvme_configure_apst(ctrl);
	if (ret < 0)
		return ret;

	ret = nvme_configure_timestamp(ctrl);
	if (ret < 0)
		return ret;

	ret = nvme_configure_host_options(ctrl);
	if (ret < 0)
		return ret;

	nvme_configure_opal(ctrl, was_suspended);

	if (!ctrl->identified && !nvme_discovery_ctrl(ctrl)) {
		/*
		 * Do not return errors unless we are in a controller reset,
		 * the controller works perfectly fine without hwmon.
		 */
		ret = nvme_hwmon_init(ctrl);
		if (ret == -EINTR)
			return ret;
	}

	clear_bit(NVME_CTRL_DIRTY_CAPABILITY, &ctrl->flags);
	ctrl->identified = true;

	return 0;
}
EXPORT_SYMBOL_GPL(nvme_init_ctrl_finish);

static int nvme_dev_open(struct inode *inode, struct file *file)
{
	struct nvme_ctrl *ctrl =
		container_of(inode->i_cdev, struct nvme_ctrl, cdev);

	switch (ctrl->state) {
	case NVME_CTRL_LIVE:
		break;
	default:
		return -EWOULDBLOCK;
	}

	nvme_get_ctrl(ctrl);
	if (!try_module_get(ctrl->ops->module)) {
		nvme_put_ctrl(ctrl);
		return -EINVAL;
	}

	file->private_data = ctrl;
	return 0;
}

static int nvme_dev_release(struct inode *inode, struct file *file)
{
	struct nvme_ctrl *ctrl =
		container_of(inode->i_cdev, struct nvme_ctrl, cdev);

	module_put(ctrl->ops->module);
	nvme_put_ctrl(ctrl);
	return 0;
}

static const struct file_operations nvme_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= nvme_dev_open,
	.release	= nvme_dev_release,
	.unlocked_ioctl	= nvme_dev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.uring_cmd	= nvme_dev_uring_cmd,
};

static struct nvme_ns_head *nvme_find_ns_head(struct nvme_ctrl *ctrl,
		unsigned nsid)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&ctrl->subsys->lock);

	list_for_each_entry(h, &ctrl->subsys->nsheads, entry) {
		/*
		 * Private namespaces can share NSIDs under some conditions.
		 * In that case we can't use the same ns_head for namespaces
		 * with the same NSID.
		 */
		if (h->ns_id != nsid || !nvme_is_unique_nsid(ctrl, h))
			continue;
		if (!list_empty(&h->list) && nvme_tryget_ns_head(h))
			return h;
	}

	return NULL;
}

static int nvme_subsys_check_duplicate_ids(struct nvme_subsystem *subsys,
		struct nvme_ns_ids *ids)
{
	bool has_uuid = !uuid_is_null(&ids->uuid);
	bool has_nguid = memchr_inv(ids->nguid, 0, sizeof(ids->nguid));
	bool has_eui64 = memchr_inv(ids->eui64, 0, sizeof(ids->eui64));
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);

	list_for_each_entry(h, &subsys->nsheads, entry) {
		if (has_uuid && uuid_equal(&ids->uuid, &h->ids.uuid))
			return -EINVAL;
		if (has_nguid &&
		    memcmp(&ids->nguid, &h->ids.nguid, sizeof(ids->nguid)) == 0)
			return -EINVAL;
		if (has_eui64 &&
		    memcmp(&ids->eui64, &h->ids.eui64, sizeof(ids->eui64)) == 0)
			return -EINVAL;
	}

	return 0;
}

static void nvme_cdev_rel(struct device *dev)
{
	ida_free(&nvme_ns_chr_minor_ida, MINOR(dev->devt));
}

void nvme_cdev_del(struct cdev *cdev, struct device *cdev_device)
{
	cdev_device_del(cdev, cdev_device);
	put_device(cdev_device);
}

int nvme_cdev_add(struct cdev *cdev, struct device *cdev_device,
		const struct file_operations *fops, struct module *owner)
{
	int minor, ret;

	minor = ida_alloc(&nvme_ns_chr_minor_ida, GFP_KERNEL);
	if (minor < 0)
		return minor;
	cdev_device->devt = MKDEV(MAJOR(nvme_ns_chr_devt), minor);
	cdev_device->class = nvme_ns_chr_class;
	cdev_device->release = nvme_cdev_rel;
	device_initialize(cdev_device);
	cdev_init(cdev, fops);
	cdev->owner = owner;
	ret = cdev_device_add(cdev, cdev_device);
	if (ret)
		put_device(cdev_device);

	return ret;
}

static int nvme_ns_chr_open(struct inode *inode, struct file *file)
{
	return nvme_ns_open(container_of(inode->i_cdev, struct nvme_ns, cdev));
}

static int nvme_ns_chr_release(struct inode *inode, struct file *file)
{
	nvme_ns_release(container_of(inode->i_cdev, struct nvme_ns, cdev));
	return 0;
}

static const struct file_operations nvme_ns_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= nvme_ns_chr_open,
	.release	= nvme_ns_chr_release,
	.unlocked_ioctl	= nvme_ns_chr_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.uring_cmd	= nvme_ns_chr_uring_cmd,
	.uring_cmd_iopoll = nvme_ns_chr_uring_cmd_iopoll,
};

static int nvme_add_ns_cdev(struct nvme_ns *ns)
{
	int ret;

	ns->cdev_device.parent = ns->ctrl->device;
	ret = dev_set_name(&ns->cdev_device, "ng%dn%d",
			   ns->ctrl->instance, ns->head->instance);
	if (ret)
		return ret;

	return nvme_cdev_add(&ns->cdev, &ns->cdev_device, &nvme_ns_chr_fops,
			     ns->ctrl->ops->module);
}

static struct nvme_ns_head *nvme_alloc_ns_head(struct nvme_ctrl *ctrl,
		struct nvme_ns_info *info)
{
	struct nvme_ns_head *head;
	size_t size = sizeof(*head);
	int ret = -ENOMEM;

#ifdef CONFIG_NVME_MULTIPATH
	size += num_possible_nodes() * sizeof(struct nvme_ns *);
#endif

	head = kzalloc(size, GFP_KERNEL);
	if (!head)
		goto out;
	ret = ida_alloc_min(&ctrl->subsys->ns_ida, 1, GFP_KERNEL);
	if (ret < 0)
		goto out_free_head;
	head->instance = ret;
	INIT_LIST_HEAD(&head->list);
	ret = init_srcu_struct(&head->srcu);
	if (ret)
		goto out_ida_remove;
	head->subsys = ctrl->subsys;
	head->ns_id = info->nsid;
	head->ids = info->ids;
	head->shared = info->is_shared;
	kref_init(&head->ref);

	if (head->ids.csi) {
		ret = nvme_get_effects_log(ctrl, head->ids.csi, &head->effects);
		if (ret)
			goto out_cleanup_srcu;
	} else
		head->effects = ctrl->effects;

	ret = nvme_mpath_alloc_disk(ctrl, head);
	if (ret)
		goto out_cleanup_srcu;

	list_add_tail(&head->entry, &ctrl->subsys->nsheads);

	kref_get(&ctrl->subsys->ref);

	return head;
out_cleanup_srcu:
	cleanup_srcu_struct(&head->srcu);
out_ida_remove:
	ida_free(&ctrl->subsys->ns_ida, head->instance);
out_free_head:
	kfree(head);
out:
	if (ret > 0)
		ret = blk_status_to_errno(nvme_error_status(ret));
	return ERR_PTR(ret);
}

static int nvme_global_check_duplicate_ids(struct nvme_subsystem *this,
		struct nvme_ns_ids *ids)
{
	struct nvme_subsystem *s;
	int ret = 0;

	/*
	 * Note that this check is racy as we try to avoid holding the global
	 * lock over the whole ns_head creation.  But it is only intended as
	 * a sanity check anyway.
	 */
	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(s, &nvme_subsystems, entry) {
		if (s == this)
			continue;
		mutex_lock(&s->lock);
		ret = nvme_subsys_check_duplicate_ids(s, ids);
		mutex_unlock(&s->lock);
		if (ret)
			break;
	}
	mutex_unlock(&nvme_subsystems_lock);

	return ret;
}

static int nvme_init_ns_head(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_ns_head *head = NULL;
	int ret;

	ret = nvme_global_check_duplicate_ids(ctrl->subsys, &info->ids);
	if (ret) {
		/*
		 * We've found two different namespaces on two different
		 * subsystems that report the same ID.  This is pretty nasty
		 * for anything that actually requires unique device
		 * identification.  In the kernel we need this for multipathing,
		 * and in user space the /dev/disk/by-id/ links rely on it.
		 *
		 * If the device also claims to be multi-path capable back off
		 * here now and refuse the probe the second device as this is a
		 * recipe for data corruption.  If not this is probably a
		 * cheap consumer device if on the PCIe bus, so let the user
		 * proceed and use the shiny toy, but warn that with changing
		 * probing order (which due to our async probing could just be
		 * device taking longer to startup) the other device could show
		 * up at any time.
		 */
		nvme_print_device_info(ctrl);
		if ((ns->ctrl->ops->flags & NVME_F_FABRICS) || /* !PCIe */
		    ((ns->ctrl->subsys->cmic & NVME_CTRL_CMIC_MULTI_CTRL) &&
		     info->is_shared)) {
			dev_err(ctrl->device,
				"ignoring nsid %d because of duplicate IDs\n",
				info->nsid);
			return ret;
		}

		dev_err(ctrl->device,
			"clearing duplicate IDs for nsid %d\n", info->nsid);
		dev_err(ctrl->device,
			"use of /dev/disk/by-id/ may cause data corruption\n");
		memset(&info->ids.nguid, 0, sizeof(info->ids.nguid));
		memset(&info->ids.uuid, 0, sizeof(info->ids.uuid));
		memset(&info->ids.eui64, 0, sizeof(info->ids.eui64));
		ctrl->quirks |= NVME_QUIRK_BOGUS_NID;
	}

	mutex_lock(&ctrl->subsys->lock);
	head = nvme_find_ns_head(ctrl, info->nsid);
	if (!head) {
		ret = nvme_subsys_check_duplicate_ids(ctrl->subsys, &info->ids);
		if (ret) {
			dev_err(ctrl->device,
				"duplicate IDs in subsystem for nsid %d\n",
				info->nsid);
			goto out_unlock;
		}
		head = nvme_alloc_ns_head(ctrl, info);
		if (IS_ERR(head)) {
			ret = PTR_ERR(head);
			goto out_unlock;
		}
	} else {
		ret = -EINVAL;
		if (!info->is_shared || !head->shared) {
			dev_err(ctrl->device,
				"Duplicate unshared namespace %d\n",
				info->nsid);
			goto out_put_ns_head;
		}
		if (!nvme_ns_ids_equal(&head->ids, &info->ids)) {
			dev_err(ctrl->device,
				"IDs don't match for shared namespace %d\n",
					info->nsid);
			goto out_put_ns_head;
		}

		if (!multipath) {
			dev_warn(ctrl->device,
				"Found shared namespace %d, but multipathing not supported.\n",
				info->nsid);
			dev_warn_once(ctrl->device,
				"Support for shared namespaces without CONFIG_NVME_MULTIPATH is deprecated and will be removed in Linux 6.0\n.");
		}
	}

	list_add_tail_rcu(&ns->siblings, &head->list);
	ns->head = head;
	mutex_unlock(&ctrl->subsys->lock);
	return 0;

out_put_ns_head:
	nvme_put_ns_head(head);
out_unlock:
	mutex_unlock(&ctrl->subsys->lock);
	return ret;
}

struct nvme_ns *nvme_find_get_ns(struct nvme_ctrl *ctrl, unsigned nsid)
{
	struct nvme_ns *ns, *ret = NULL;

	down_read(&ctrl->namespaces_rwsem);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		if (ns->head->ns_id == nsid) {
			if (!nvme_get_ns(ns))
				continue;
			ret = ns;
			break;
		}
		if (ns->head->ns_id > nsid)
			break;
	}
	up_read(&ctrl->namespaces_rwsem);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(nvme_find_get_ns, NVME_TARGET_PASSTHRU);

/*
 * Add the namespace to the controller list while keeping the list ordered.
 */
static void nvme_ns_add_to_ctrl_list(struct nvme_ns *ns)
{
	struct nvme_ns *tmp;

	list_for_each_entry_reverse(tmp, &ns->ctrl->namespaces, list) {
		if (tmp->head->ns_id < ns->head->ns_id) {
			list_add(&ns->list, &tmp->list);
			return;
		}
	}
	list_add(&ns->list, &ns->ctrl->namespaces);
}

static void nvme_alloc_ns(struct nvme_ctrl *ctrl, struct nvme_ns_info *info)
{
	struct nvme_ns *ns;
	struct gendisk *disk;
	int node = ctrl->numa_node;

	ns = kzalloc_node(sizeof(*ns), GFP_KERNEL, node);
	if (!ns)
		return;

	disk = blk_mq_alloc_disk(ctrl->tagset, ns);
	if (IS_ERR(disk))
		goto out_free_ns;
	disk->fops = &nvme_bdev_ops;
	disk->private_data = ns;

	ns->disk = disk;
	ns->queue = disk->queue;

	if (ctrl->opts && ctrl->opts->data_digest)
		blk_queue_flag_set(QUEUE_FLAG_STABLE_WRITES, ns->queue);

	blk_queue_flag_set(QUEUE_FLAG_NONROT, ns->queue);
	if (ctrl->ops->supports_pci_p2pdma &&
	    ctrl->ops->supports_pci_p2pdma(ctrl))
		blk_queue_flag_set(QUEUE_FLAG_PCI_P2PDMA, ns->queue);

	ns->ctrl = ctrl;
	kref_init(&ns->kref);

	if (nvme_init_ns_head(ns, info))
		goto out_cleanup_disk;

	/*
	 * If multipathing is enabled, the device name for all disks and not
	 * just those that represent shared namespaces needs to be based on the
	 * subsystem instance.  Using the controller instance for private
	 * namespaces could lead to naming collisions between shared and private
	 * namespaces if they don't use a common numbering scheme.
	 *
	 * If multipathing is not enabled, disk names must use the controller
	 * instance as shared namespaces will show up as multiple block
	 * devices.
	 */
	if (nvme_ns_head_multipath(ns->head)) {
		sprintf(disk->disk_name, "nvme%dc%dn%d", ctrl->subsys->instance,
			ctrl->instance, ns->head->instance);
		disk->flags |= GENHD_FL_HIDDEN;
	} else if (multipath) {
		sprintf(disk->disk_name, "nvme%dn%d", ctrl->subsys->instance,
			ns->head->instance);
	} else {
		sprintf(disk->disk_name, "nvme%dn%d", ctrl->instance,
			ns->head->instance);
	}

	if (nvme_update_ns_info(ns, info))
		goto out_unlink_ns;

	down_write(&ctrl->namespaces_rwsem);
	nvme_ns_add_to_ctrl_list(ns);
	up_write(&ctrl->namespaces_rwsem);
	nvme_get_ctrl(ctrl);

	if (device_add_disk(ctrl->device, ns->disk, nvme_ns_id_attr_groups))
		goto out_cleanup_ns_from_list;

	if (!nvme_ns_head_multipath(ns->head))
		nvme_add_ns_cdev(ns);

	nvme_mpath_add_disk(ns, info->anagrpid);
	nvme_fault_inject_init(&ns->fault_inject, ns->disk->disk_name);

	return;

 out_cleanup_ns_from_list:
	nvme_put_ctrl(ctrl);
	down_write(&ctrl->namespaces_rwsem);
	list_del_init(&ns->list);
	up_write(&ctrl->namespaces_rwsem);
 out_unlink_ns:
	mutex_lock(&ctrl->subsys->lock);
	list_del_rcu(&ns->siblings);
	if (list_empty(&ns->head->list))
		list_del_init(&ns->head->entry);
	mutex_unlock(&ctrl->subsys->lock);
	nvme_put_ns_head(ns->head);
 out_cleanup_disk:
	put_disk(disk);
 out_free_ns:
	kfree(ns);
}

static void nvme_ns_remove(struct nvme_ns *ns)
{
	bool last_path = false;

	if (test_and_set_bit(NVME_NS_REMOVING, &ns->flags))
		return;

	clear_bit(NVME_NS_READY, &ns->flags);
	set_capacity(ns->disk, 0);
	nvme_fault_inject_fini(&ns->fault_inject);

	/*
	 * Ensure that !NVME_NS_READY is seen by other threads to prevent
	 * this ns going back into current_path.
	 */
	synchronize_srcu(&ns->head->srcu);

	/* wait for concurrent submissions */
	if (nvme_mpath_clear_current_path(ns))
		synchronize_srcu(&ns->head->srcu);

	mutex_lock(&ns->ctrl->subsys->lock);
	list_del_rcu(&ns->siblings);
	if (list_empty(&ns->head->list)) {
		list_del_init(&ns->head->entry);
		last_path = true;
	}
	mutex_unlock(&ns->ctrl->subsys->lock);

	/* guarantee not available in head->list */
	synchronize_srcu(&ns->head->srcu);

	if (!nvme_ns_head_multipath(ns->head))
		nvme_cdev_del(&ns->cdev, &ns->cdev_device);
	del_gendisk(ns->disk);

	down_write(&ns->ctrl->namespaces_rwsem);
	list_del_init(&ns->list);
	up_write(&ns->ctrl->namespaces_rwsem);

	if (last_path)
		nvme_mpath_shutdown_disk(ns->head);
	nvme_put_ns(ns);
}

static void nvme_ns_remove_by_nsid(struct nvme_ctrl *ctrl, u32 nsid)
{
	struct nvme_ns *ns = nvme_find_get_ns(ctrl, nsid);

	if (ns) {
		nvme_ns_remove(ns);
		nvme_put_ns(ns);
	}
}

static void nvme_validate_ns(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	int ret = NVME_SC_INVALID_NS | NVME_SC_DNR;

	if (!nvme_ns_ids_equal(&ns->head->ids, &info->ids)) {
		dev_err(ns->ctrl->device,
			"identifiers changed for nsid %d\n", ns->head->ns_id);
		goto out;
	}

	ret = nvme_update_ns_info(ns, info);
out:
	/*
	 * Only remove the namespace if we got a fatal error back from the
	 * device, otherwise ignore the error and just move on.
	 *
	 * TODO: we should probably schedule a delayed retry here.
	 */
	if (ret > 0 && (ret & NVME_SC_DNR))
		nvme_ns_remove(ns);
}

static void nvme_scan_ns(struct nvme_ctrl *ctrl, unsigned nsid)
{
	struct nvme_ns_info info = { .nsid = nsid };
	struct nvme_ns *ns;
	int ret;

	if (nvme_identify_ns_descs(ctrl, &info))
		return;

	if (info.ids.csi != NVME_CSI_NVM && !nvme_multi_css(ctrl)) {
		dev_warn(ctrl->device,
			"command set not reported for nsid: %d\n", nsid);
		return;
	}

	/*
	 * If available try to use the Command Set Idependent Identify Namespace
	 * data structure to find all the generic information that is needed to
	 * set up a namespace.  If not fall back to the legacy version.
	 */
	if ((ctrl->cap & NVME_CAP_CRMS_CRIMS) ||
	    (info.ids.csi != NVME_CSI_NVM && info.ids.csi != NVME_CSI_ZNS))
		ret = nvme_ns_info_from_id_cs_indep(ctrl, &info);
	else
		ret = nvme_ns_info_from_identify(ctrl, &info);

	if (info.is_removed)
		nvme_ns_remove_by_nsid(ctrl, nsid);

	/*
	 * Ignore the namespace if it is not ready. We will get an AEN once it
	 * becomes ready and restart the scan.
	 */
	if (ret || !info.is_ready)
		return;

	ns = nvme_find_get_ns(ctrl, nsid);
	if (ns) {
		nvme_validate_ns(ns, &info);
		nvme_put_ns(ns);
	} else {
		nvme_alloc_ns(ctrl, &info);
	}
}

static void nvme_remove_invalid_namespaces(struct nvme_ctrl *ctrl,
					unsigned nsid)
{
	struct nvme_ns *ns, *next;
	LIST_HEAD(rm_list);

	down_write(&ctrl->namespaces_rwsem);
	list_for_each_entry_safe(ns, next, &ctrl->namespaces, list) {
		if (ns->head->ns_id > nsid)
			list_move_tail(&ns->list, &rm_list);
	}
	up_write(&ctrl->namespaces_rwsem);

	list_for_each_entry_safe(ns, next, &rm_list, list)
		nvme_ns_remove(ns);

}

static int nvme_scan_ns_list(struct nvme_ctrl *ctrl)
{
	const int nr_entries = NVME_IDENTIFY_DATA_SIZE / sizeof(__le32);
	__le32 *ns_list;
	u32 prev = 0;
	int ret = 0, i;

	ns_list = kzalloc(NVME_IDENTIFY_DATA_SIZE, GFP_KERNEL);
	if (!ns_list)
		return -ENOMEM;

	for (;;) {
		struct nvme_command cmd = {
			.identify.opcode	= nvme_admin_identify,
			.identify.cns		= NVME_ID_CNS_NS_ACTIVE_LIST,
			.identify.nsid		= cpu_to_le32(prev),
		};

		ret = nvme_submit_sync_cmd(ctrl->admin_q, &cmd, ns_list,
					    NVME_IDENTIFY_DATA_SIZE);
		if (ret) {
			dev_warn(ctrl->device,
				"Identify NS List failed (status=0x%x)\n", ret);
			goto free;
		}

		for (i = 0; i < nr_entries; i++) {
			u32 nsid = le32_to_cpu(ns_list[i]);

			if (!nsid)	/* end of the list? */
				goto out;
			nvme_scan_ns(ctrl, nsid);
			while (++prev < nsid)
				nvme_ns_remove_by_nsid(ctrl, prev);
		}
	}
 out:
	nvme_remove_invalid_namespaces(ctrl, prev);
 free:
	kfree(ns_list);
	return ret;
}

static void nvme_scan_ns_sequential(struct nvme_ctrl *ctrl)
{
	struct nvme_id_ctrl *id;
	u32 nn, i;

	if (nvme_identify_ctrl(ctrl, &id))
		return;
	nn = le32_to_cpu(id->nn);
	kfree(id);

	for (i = 1; i <= nn; i++)
		nvme_scan_ns(ctrl, i);

	nvme_remove_invalid_namespaces(ctrl, nn);
}

static void nvme_clear_changed_ns_log(struct nvme_ctrl *ctrl)
{
	size_t log_size = NVME_MAX_CHANGED_NAMESPACES * sizeof(__le32);
	__le32 *log;
	int error;

	log = kzalloc(log_size, GFP_KERNEL);
	if (!log)
		return;

	/*
	 * We need to read the log to clear the AEN, but we don't want to rely
	 * on it for the changed namespace information as userspace could have
	 * raced with us in reading the log page, which could cause us to miss
	 * updates.
	 */
	error = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_CHANGED_NS, 0,
			NVME_CSI_NVM, log, log_size, 0);
	if (error)
		dev_warn(ctrl->device,
			"reading changed ns log failed: %d\n", error);

	kfree(log);
}

static void nvme_scan_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =
		container_of(work, struct nvme_ctrl, scan_work);
	int ret;

	/* No tagset on a live ctrl means IO queues could not created */
	if (ctrl->state != NVME_CTRL_LIVE || !ctrl->tagset)
		return;

	/*
	 * Identify controller limits can change at controller reset due to
	 * new firmware download, even though it is not common we cannot ignore
	 * such scenario. Controller's non-mdts limits are reported in the unit
	 * of logical blocks that is dependent on the format of attached
	 * namespace. Hence re-read the limits at the time of ns allocation.
	 */
	ret = nvme_init_non_mdts_limits(ctrl);
	if (ret < 0) {
		dev_warn(ctrl->device,
			"reading non-mdts-limits failed: %d\n", ret);
		return;
	}

	if (test_and_clear_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events)) {
		dev_info(ctrl->device, "rescanning namespaces.\n");
		nvme_clear_changed_ns_log(ctrl);
	}

	mutex_lock(&ctrl->scan_lock);
	if (nvme_ctrl_limited_cns(ctrl)) {
		nvme_scan_ns_sequential(ctrl);
	} else {
		/*
		 * Fall back to sequential scan if DNR is set to handle broken
		 * devices which should support Identify NS List (as per the VS
		 * they report) but don't actually support it.
		 */
		ret = nvme_scan_ns_list(ctrl);
		if (ret > 0 && ret & NVME_SC_DNR)
			nvme_scan_ns_sequential(ctrl);
	}
	mutex_unlock(&ctrl->scan_lock);
}

/*
 * This function iterates the namespace list unlocked to allow recovery from
 * controller failure. It is up to the caller to ensure the namespace list is
 * not modified by scan work while this function is executing.
 */
void nvme_remove_namespaces(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns, *next;
	LIST_HEAD(ns_list);

	/*
	 * make sure to requeue I/O to all namespaces as these
	 * might result from the scan itself and must complete
	 * for the scan_work to make progress
	 */
	nvme_mpath_clear_ctrl_paths(ctrl);

	/*
	 * Unquiesce io queues so any pending IO won't hang, especially
	 * those submitted from scan work
	 */
	nvme_unquiesce_io_queues(ctrl);

	/* prevent racing with ns scanning */
	flush_work(&ctrl->scan_work);

	/*
	 * The dead states indicates the controller was not gracefully
	 * disconnected. In that case, we won't be able to flush any data while
	 * removing the namespaces' disks; fail all the queues now to avoid
	 * potentially having to clean up the failed sync later.
	 */
	if (ctrl->state == NVME_CTRL_DEAD)
		nvme_mark_namespaces_dead(ctrl);

	/* this is a no-op when called from the controller reset handler */
	nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING_NOIO);

	down_write(&ctrl->namespaces_rwsem);
	list_splice_init(&ctrl->namespaces, &ns_list);
	up_write(&ctrl->namespaces_rwsem);

	list_for_each_entry_safe(ns, next, &ns_list, list)
		nvme_ns_remove(ns);
}
EXPORT_SYMBOL_GPL(nvme_remove_namespaces);

static int nvme_class_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct nvme_ctrl *ctrl =
		container_of(dev, struct nvme_ctrl, ctrl_device);
	struct nvmf_ctrl_options *opts = ctrl->opts;
	int ret;

	ret = add_uevent_var(env, "NVME_TRTYPE=%s", ctrl->ops->name);
	if (ret)
		return ret;

	if (opts) {
		ret = add_uevent_var(env, "NVME_TRADDR=%s", opts->traddr);
		if (ret)
			return ret;

		ret = add_uevent_var(env, "NVME_TRSVCID=%s",
				opts->trsvcid ?: "none");
		if (ret)
			return ret;

		ret = add_uevent_var(env, "NVME_HOST_TRADDR=%s",
				opts->host_traddr ?: "none");
		if (ret)
			return ret;

		ret = add_uevent_var(env, "NVME_HOST_IFACE=%s",
				opts->host_iface ?: "none");
	}
	return ret;
}

static void nvme_change_uevent(struct nvme_ctrl *ctrl, char *envdata)
{
	char *envp[2] = { envdata, NULL };

	kobject_uevent_env(&ctrl->device->kobj, KOBJ_CHANGE, envp);
}

static void nvme_aen_uevent(struct nvme_ctrl *ctrl)
{
	char *envp[2] = { NULL, NULL };
	u32 aen_result = ctrl->aen_result;

	ctrl->aen_result = 0;
	if (!aen_result)
		return;

	envp[0] = kasprintf(GFP_KERNEL, "NVME_AEN=%#08x", aen_result);
	if (!envp[0])
		return;
	kobject_uevent_env(&ctrl->device->kobj, KOBJ_CHANGE, envp);
	kfree(envp[0]);
}

static void nvme_async_event_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =
		container_of(work, struct nvme_ctrl, async_event_work);

	nvme_aen_uevent(ctrl);

	/*
	 * The transport drivers must guarantee AER submission here is safe by
	 * flushing ctrl async_event_work after changing the controller state
	 * from LIVE and before freeing the admin queue.
	*/
	if (ctrl->state == NVME_CTRL_LIVE)
		ctrl->ops->submit_async_event(ctrl);
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
	struct nvme_fw_slot_info_log *log;

	log = kmalloc(sizeof(*log), GFP_KERNEL);
	if (!log)
		return;

	if (nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_FW_SLOT, 0, NVME_CSI_NVM,
			log, sizeof(*log), 0))
		dev_warn(ctrl->device, "Get FW SLOT INFO log error\n");
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

	nvme_quiesce_io_queues(ctrl);
	while (nvme_ctrl_pp_status(ctrl)) {
		if (time_after(jiffies, fw_act_timeout)) {
			dev_warn(ctrl->device,
				"Fw activation timeout, reset controller\n");
			nvme_try_sched_reset(ctrl);
			return;
		}
		msleep(100);
	}

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE))
		return;

	nvme_unquiesce_io_queues(ctrl);
	/* read FW slot information to clear the AER */
	nvme_get_fw_slot_info(ctrl);

	queue_work(nvme_wq, &ctrl->async_event_work);
}

static u32 nvme_aer_type(u32 result)
{
	return result & 0x7;
}

static u32 nvme_aer_subtype(u32 result)
{
	return (result & 0xff00) >> 8;
}

static bool nvme_handle_aen_notice(struct nvme_ctrl *ctrl, u32 result)
{
	u32 aer_notice_type = nvme_aer_subtype(result);
	bool requeue = true;

	switch (aer_notice_type) {
	case NVME_AER_NOTICE_NS_CHANGED:
		set_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events);
		nvme_queue_scan(ctrl);
		break;
	case NVME_AER_NOTICE_FW_ACT_STARTING:
		/*
		 * We are (ab)using the RESETTING state to prevent subsequent
		 * recovery actions from interfering with the controller's
		 * firmware activation.
		 */
		if (nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING)) {
			nvme_auth_stop(ctrl);
			requeue = false;
			queue_work(nvme_wq, &ctrl->fw_act_work);
		}
		break;
#ifdef CONFIG_NVME_MULTIPATH
	case NVME_AER_NOTICE_ANA:
		if (!ctrl->ana_log_buf)
			break;
		queue_work(nvme_wq, &ctrl->ana_work);
		break;
#endif
	case NVME_AER_NOTICE_DISC_CHANGED:
		ctrl->aen_result = result;
		break;
	default:
		dev_warn(ctrl->device, "async event result %08x\n", result);
	}
	return requeue;
}

static void nvme_handle_aer_persistent_error(struct nvme_ctrl *ctrl)
{
	dev_warn(ctrl->device, "resetting controller due to AER\n");
	nvme_reset_ctrl(ctrl);
}

void nvme_complete_async_event(struct nvme_ctrl *ctrl, __le16 status,
		volatile union nvme_result *res)
{
	u32 result = le32_to_cpu(res->u32);
	u32 aer_type = nvme_aer_type(result);
	u32 aer_subtype = nvme_aer_subtype(result);
	bool requeue = true;

	if (le16_to_cpu(status) >> 1 != NVME_SC_SUCCESS)
		return;

	trace_nvme_async_event(ctrl, result);
	switch (aer_type) {
	case NVME_AER_NOTICE:
		requeue = nvme_handle_aen_notice(ctrl, result);
		break;
	case NVME_AER_ERROR:
		/*
		 * For a persistent internal error, don't run async_event_work
		 * to submit a new AER. The controller reset will do it.
		 */
		if (aer_subtype == NVME_AER_ERROR_PERSIST_INT_ERR) {
			nvme_handle_aer_persistent_error(ctrl);
			return;
		}
		fallthrough;
	case NVME_AER_SMART:
	case NVME_AER_CSS:
	case NVME_AER_VS:
		ctrl->aen_result = result;
		break;
	default:
		break;
	}

	if (requeue)
		queue_work(nvme_wq, &ctrl->async_event_work);
}
EXPORT_SYMBOL_GPL(nvme_complete_async_event);

int nvme_alloc_admin_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,
		const struct blk_mq_ops *ops, unsigned int cmd_size)
{
	int ret;

	memset(set, 0, sizeof(*set));
	set->ops = ops;
	set->queue_depth = NVME_AQ_MQ_TAG_DEPTH;
	if (ctrl->ops->flags & NVME_F_FABRICS)
		set->reserved_tags = NVMF_RESERVED_TAGS;
	set->numa_node = ctrl->numa_node;
	set->flags = BLK_MQ_F_NO_SCHED;
	if (ctrl->ops->flags & NVME_F_BLOCKING)
		set->flags |= BLK_MQ_F_BLOCKING;
	set->cmd_size = cmd_size;
	set->driver_data = ctrl;
	set->nr_hw_queues = 1;
	set->timeout = NVME_ADMIN_TIMEOUT;
	ret = blk_mq_alloc_tag_set(set);
	if (ret)
		return ret;

	ctrl->admin_q = blk_mq_init_queue(set);
	if (IS_ERR(ctrl->admin_q)) {
		ret = PTR_ERR(ctrl->admin_q);
		goto out_free_tagset;
	}

	if (ctrl->ops->flags & NVME_F_FABRICS) {
		ctrl->fabrics_q = blk_mq_init_queue(set);
		if (IS_ERR(ctrl->fabrics_q)) {
			ret = PTR_ERR(ctrl->fabrics_q);
			goto out_cleanup_admin_q;
		}
	}

	ctrl->admin_tagset = set;
	return 0;

out_cleanup_admin_q:
	blk_mq_destroy_queue(ctrl->admin_q);
	blk_put_queue(ctrl->admin_q);
out_free_tagset:
	blk_mq_free_tag_set(set);
	ctrl->admin_q = NULL;
	ctrl->fabrics_q = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_alloc_admin_tag_set);

void nvme_remove_admin_tag_set(struct nvme_ctrl *ctrl)
{
	blk_mq_destroy_queue(ctrl->admin_q);
	blk_put_queue(ctrl->admin_q);
	if (ctrl->ops->flags & NVME_F_FABRICS) {
		blk_mq_destroy_queue(ctrl->fabrics_q);
		blk_put_queue(ctrl->fabrics_q);
	}
	blk_mq_free_tag_set(ctrl->admin_tagset);
}
EXPORT_SYMBOL_GPL(nvme_remove_admin_tag_set);

int nvme_alloc_io_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,
		const struct blk_mq_ops *ops, unsigned int nr_maps,
		unsigned int cmd_size)
{
	int ret;

	memset(set, 0, sizeof(*set));
	set->ops = ops;
	set->queue_depth = min_t(unsigned, ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1);
	/*
	 * Some Apple controllers requires tags to be unique across admin and
	 * the (only) I/O queue, so reserve the first 32 tags of the I/O queue.
	 */
	if (ctrl->quirks & NVME_QUIRK_SHARED_TAGS)
		set->reserved_tags = NVME_AQ_DEPTH;
	else if (ctrl->ops->flags & NVME_F_FABRICS)
		set->reserved_tags = NVMF_RESERVED_TAGS;
	set->numa_node = ctrl->numa_node;
	set->flags = BLK_MQ_F_SHOULD_MERGE;
	if (ctrl->ops->flags & NVME_F_BLOCKING)
		set->flags |= BLK_MQ_F_BLOCKING;
	set->cmd_size = cmd_size,
	set->driver_data = ctrl;
	set->nr_hw_queues = ctrl->queue_count - 1;
	set->timeout = NVME_IO_TIMEOUT;
	set->nr_maps = nr_maps;
	ret = blk_mq_alloc_tag_set(set);
	if (ret)
		return ret;

	if (ctrl->ops->flags & NVME_F_FABRICS) {
		ctrl->connect_q = blk_mq_init_queue(set);
        	if (IS_ERR(ctrl->connect_q)) {
			ret = PTR_ERR(ctrl->connect_q);
			goto out_free_tag_set;
		}
		blk_queue_flag_set(QUEUE_FLAG_SKIP_TAGSET_QUIESCE,
				   ctrl->connect_q);
	}

	ctrl->tagset = set;
	return 0;

out_free_tag_set:
	blk_mq_free_tag_set(set);
	ctrl->connect_q = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_alloc_io_tag_set);

void nvme_remove_io_tag_set(struct nvme_ctrl *ctrl)
{
	if (ctrl->ops->flags & NVME_F_FABRICS) {
		blk_mq_destroy_queue(ctrl->connect_q);
		blk_put_queue(ctrl->connect_q);
	}
	blk_mq_free_tag_set(ctrl->tagset);
}
EXPORT_SYMBOL_GPL(nvme_remove_io_tag_set);

void nvme_stop_ctrl(struct nvme_ctrl *ctrl)
{
	nvme_mpath_stop(ctrl);
	nvme_auth_stop(ctrl);
	nvme_stop_keep_alive(ctrl);
	nvme_stop_failfast_work(ctrl);
	flush_work(&ctrl->async_event_work);
	cancel_work_sync(&ctrl->fw_act_work);
	if (ctrl->ops->stop_ctrl)
		ctrl->ops->stop_ctrl(ctrl);
}
EXPORT_SYMBOL_GPL(nvme_stop_ctrl);

void nvme_start_ctrl(struct nvme_ctrl *ctrl)
{
	nvme_start_keep_alive(ctrl);

	nvme_enable_aen(ctrl);

	/*
	 * persistent discovery controllers need to send indication to userspace
	 * to re-read the discovery log page to learn about possible changes
	 * that were missed. We identify persistent discovery controllers by
	 * checking that they started once before, hence are reconnecting back.
	 */
	if (test_bit(NVME_CTRL_STARTED_ONCE, &ctrl->flags) &&
	    nvme_discovery_ctrl(ctrl))
		nvme_change_uevent(ctrl, "NVME_EVENT=rediscover");

	if (ctrl->queue_count > 1) {
		nvme_queue_scan(ctrl);
		nvme_unquiesce_io_queues(ctrl);
		nvme_mpath_update(ctrl);
	}

	nvme_change_uevent(ctrl, "NVME_EVENT=connected");
	set_bit(NVME_CTRL_STARTED_ONCE, &ctrl->flags);
}
EXPORT_SYMBOL_GPL(nvme_start_ctrl);

void nvme_uninit_ctrl(struct nvme_ctrl *ctrl)
{
	nvme_hwmon_exit(ctrl);
	nvme_fault_inject_fini(&ctrl->fault_inject);
	dev_pm_qos_hide_latency_tolerance(ctrl->device);
	cdev_device_del(&ctrl->cdev, ctrl->device);
	nvme_put_ctrl(ctrl);
}
EXPORT_SYMBOL_GPL(nvme_uninit_ctrl);

static void nvme_free_cels(struct nvme_ctrl *ctrl)
{
	struct nvme_effects_log	*cel;
	unsigned long i;

	xa_for_each(&ctrl->cels, i, cel) {
		xa_erase(&ctrl->cels, i);
		kfree(cel);
	}

	xa_destroy(&ctrl->cels);
}

static void nvme_free_ctrl(struct device *dev)
{
	struct nvme_ctrl *ctrl =
		container_of(dev, struct nvme_ctrl, ctrl_device);
	struct nvme_subsystem *subsys = ctrl->subsys;

	if (!subsys || ctrl->instance != subsys->instance)
		ida_free(&nvme_instance_ida, ctrl->instance);

	nvme_free_cels(ctrl);
	nvme_mpath_uninit(ctrl);
	nvme_auth_stop(ctrl);
	nvme_auth_free(ctrl);
	__free_page(ctrl->discard_page);
	free_opal_dev(ctrl->opal_dev);

	if (subsys) {
		mutex_lock(&nvme_subsystems_lock);
		list_del(&ctrl->subsys_entry);
		sysfs_remove_link(&subsys->dev.kobj, dev_name(ctrl->device));
		mutex_unlock(&nvme_subsystems_lock);
	}

	ctrl->ops->free_ctrl(ctrl);

	if (subsys)
		nvme_put_subsystem(subsys);
}

/*
 * Initialize a NVMe controller structures.  This needs to be called during
 * earliest initialization so that we have the initialized structured around
 * during probing.
 */
int nvme_init_ctrl(struct nvme_ctrl *ctrl, struct device *dev,
		const struct nvme_ctrl_ops *ops, unsigned long quirks)
{
	int ret;

	ctrl->state = NVME_CTRL_NEW;
	clear_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);
	spin_lock_init(&ctrl->lock);
	mutex_init(&ctrl->scan_lock);
	INIT_LIST_HEAD(&ctrl->namespaces);
	xa_init(&ctrl->cels);
	init_rwsem(&ctrl->namespaces_rwsem);
	ctrl->dev = dev;
	ctrl->ops = ops;
	ctrl->quirks = quirks;
	ctrl->numa_node = NUMA_NO_NODE;
	INIT_WORK(&ctrl->scan_work, nvme_scan_work);
	INIT_WORK(&ctrl->async_event_work, nvme_async_event_work);
	INIT_WORK(&ctrl->fw_act_work, nvme_fw_act_work);
	INIT_WORK(&ctrl->delete_work, nvme_delete_ctrl_work);
	init_waitqueue_head(&ctrl->state_wq);

	INIT_DELAYED_WORK(&ctrl->ka_work, nvme_keep_alive_work);
	INIT_DELAYED_WORK(&ctrl->failfast_work, nvme_failfast_work);
	memset(&ctrl->ka_cmd, 0, sizeof(ctrl->ka_cmd));
	ctrl->ka_cmd.common.opcode = nvme_admin_keep_alive;

	BUILD_BUG_ON(NVME_DSM_MAX_RANGES * sizeof(struct nvme_dsm_range) >
			PAGE_SIZE);
	ctrl->discard_page = alloc_page(GFP_KERNEL);
	if (!ctrl->discard_page) {
		ret = -ENOMEM;
		goto out;
	}

	ret = ida_alloc(&nvme_instance_ida, GFP_KERNEL);
	if (ret < 0)
		goto out;
	ctrl->instance = ret;

	device_initialize(&ctrl->ctrl_device);
	ctrl->device = &ctrl->ctrl_device;
	ctrl->device->devt = MKDEV(MAJOR(nvme_ctrl_base_chr_devt),
			ctrl->instance);
	ctrl->device->class = nvme_class;
	ctrl->device->parent = ctrl->dev;
	if (ops->dev_attr_groups)
		ctrl->device->groups = ops->dev_attr_groups;
	else
		ctrl->device->groups = nvme_dev_attr_groups;
	ctrl->device->release = nvme_free_ctrl;
	dev_set_drvdata(ctrl->device, ctrl);
	ret = dev_set_name(ctrl->device, "nvme%d", ctrl->instance);
	if (ret)
		goto out_release_instance;

	nvme_get_ctrl(ctrl);
	cdev_init(&ctrl->cdev, &nvme_dev_fops);
	ctrl->cdev.owner = ops->module;
	ret = cdev_device_add(&ctrl->cdev, ctrl->device);
	if (ret)
		goto out_free_name;

	/*
	 * Initialize latency tolerance controls.  The sysfs files won't
	 * be visible to userspace unless the device actually supports APST.
	 */
	ctrl->device->power.set_latency_tolerance = nvme_set_latency_tolerance;
	dev_pm_qos_update_user_latency_tolerance(ctrl->device,
		min(default_ps_max_latency_us, (unsigned long)S32_MAX));

	nvme_fault_inject_init(&ctrl->fault_inject, dev_name(ctrl->device));
	nvme_mpath_init_ctrl(ctrl);
	ret = nvme_auth_init_ctrl(ctrl);
	if (ret)
		goto out_free_cdev;

	return 0;
out_free_cdev:
	nvme_fault_inject_fini(&ctrl->fault_inject);
	dev_pm_qos_hide_latency_tolerance(ctrl->device);
	cdev_device_del(&ctrl->cdev, ctrl->device);
out_free_name:
	nvme_put_ctrl(ctrl);
	kfree_const(ctrl->device->kobj.name);
out_release_instance:
	ida_free(&nvme_instance_ida, ctrl->instance);
out:
	if (ctrl->discard_page)
		__free_page(ctrl->discard_page);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_init_ctrl);

/* let I/O to all namespaces fail in preparation for surprise removal */
void nvme_mark_namespaces_dead(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	down_read(&ctrl->namespaces_rwsem);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_mark_disk_dead(ns->disk);
	up_read(&ctrl->namespaces_rwsem);
}
EXPORT_SYMBOL_GPL(nvme_mark_namespaces_dead);

void nvme_unfreeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	down_read(&ctrl->namespaces_rwsem);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_mq_unfreeze_queue(ns->queue);
	up_read(&ctrl->namespaces_rwsem);
}
EXPORT_SYMBOL_GPL(nvme_unfreeze);

int nvme_wait_freeze_timeout(struct nvme_ctrl *ctrl, long timeout)
{
	struct nvme_ns *ns;

	down_read(&ctrl->namespaces_rwsem);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		timeout = blk_mq_freeze_queue_wait_timeout(ns->queue, timeout);
		if (timeout <= 0)
			break;
	}
	up_read(&ctrl->namespaces_rwsem);
	return timeout;
}
EXPORT_SYMBOL_GPL(nvme_wait_freeze_timeout);

void nvme_wait_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	down_read(&ctrl->namespaces_rwsem);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_mq_freeze_queue_wait(ns->queue);
	up_read(&ctrl->namespaces_rwsem);
}
EXPORT_SYMBOL_GPL(nvme_wait_freeze);

void nvme_start_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	down_read(&ctrl->namespaces_rwsem);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_freeze_queue_start(ns->queue);
	up_read(&ctrl->namespaces_rwsem);
}
EXPORT_SYMBOL_GPL(nvme_start_freeze);

void nvme_quiesce_io_queues(struct nvme_ctrl *ctrl)
{
	if (!ctrl->tagset)
		return;
	if (!test_and_set_bit(NVME_CTRL_STOPPED, &ctrl->flags))
		blk_mq_quiesce_tagset(ctrl->tagset);
	else
		blk_mq_wait_quiesce_done(ctrl->tagset);
}
EXPORT_SYMBOL_GPL(nvme_quiesce_io_queues);

void nvme_unquiesce_io_queues(struct nvme_ctrl *ctrl)
{
	if (!ctrl->tagset)
		return;
	if (test_and_clear_bit(NVME_CTRL_STOPPED, &ctrl->flags))
		blk_mq_unquiesce_tagset(ctrl->tagset);
}
EXPORT_SYMBOL_GPL(nvme_unquiesce_io_queues);

void nvme_quiesce_admin_queue(struct nvme_ctrl *ctrl)
{
	if (!test_and_set_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
		blk_mq_quiesce_queue(ctrl->admin_q);
	else
		blk_mq_wait_quiesce_done(ctrl->admin_q->tag_set);
}
EXPORT_SYMBOL_GPL(nvme_quiesce_admin_queue);

void nvme_unquiesce_admin_queue(struct nvme_ctrl *ctrl)
{
	if (test_and_clear_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
		blk_mq_unquiesce_queue(ctrl->admin_q);
}
EXPORT_SYMBOL_GPL(nvme_unquiesce_admin_queue);

void nvme_sync_io_queues(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	down_read(&ctrl->namespaces_rwsem);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		blk_sync_queue(ns->queue);
	up_read(&ctrl->namespaces_rwsem);
}
EXPORT_SYMBOL_GPL(nvme_sync_io_queues);

void nvme_sync_queues(struct nvme_ctrl *ctrl)
{
	nvme_sync_io_queues(ctrl);
	if (ctrl->admin_q)
		blk_sync_queue(ctrl->admin_q);
}
EXPORT_SYMBOL_GPL(nvme_sync_queues);

struct nvme_ctrl *nvme_ctrl_from_file(struct file *file)
{
	if (file->f_op != &nvme_dev_fops)
		return NULL;
	return file->private_data;
}
EXPORT_SYMBOL_NS_GPL(nvme_ctrl_from_file, NVME_TARGET_PASSTHRU);

/*
 * Check we didn't inadvertently grow the command structure sizes:
 */
static inline void _nvme_check_size(void)
{
	BUILD_BUG_ON(sizeof(struct nvme_common_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_rw_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_identify) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_features) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_download_firmware) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_format_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_dsm_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_write_zeroes_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_abort_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_get_log_page_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl) != NVME_IDENTIFY_DATA_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns) != NVME_IDENTIFY_DATA_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_cs_indep) !=
			NVME_IDENTIFY_DATA_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_zns) != NVME_IDENTIFY_DATA_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_nvm) != NVME_IDENTIFY_DATA_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl_zns) != NVME_IDENTIFY_DATA_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl_nvm) != NVME_IDENTIFY_DATA_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_lba_range_type) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_smart_log) != 512);
	BUILD_BUG_ON(sizeof(struct nvme_dbbuf) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_directive_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_feat_host_behavior) != 512);
}


static int __init nvme_core_init(void)
{
	int result = -ENOMEM;

	_nvme_check_size();

	nvme_wq = alloc_workqueue("nvme-wq",
			WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);
	if (!nvme_wq)
		goto out;

	nvme_reset_wq = alloc_workqueue("nvme-reset-wq",
			WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);
	if (!nvme_reset_wq)
		goto destroy_wq;

	nvme_delete_wq = alloc_workqueue("nvme-delete-wq",
			WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);
	if (!nvme_delete_wq)
		goto destroy_reset_wq;

	result = alloc_chrdev_region(&nvme_ctrl_base_chr_devt, 0,
			NVME_MINORS, "nvme");
	if (result < 0)
		goto destroy_delete_wq;

	nvme_class = class_create("nvme");
	if (IS_ERR(nvme_class)) {
		result = PTR_ERR(nvme_class);
		goto unregister_chrdev;
	}
	nvme_class->dev_uevent = nvme_class_uevent;

	nvme_subsys_class = class_create("nvme-subsystem");
	if (IS_ERR(nvme_subsys_class)) {
		result = PTR_ERR(nvme_subsys_class);
		goto destroy_class;
	}

	result = alloc_chrdev_region(&nvme_ns_chr_devt, 0, NVME_MINORS,
				     "nvme-generic");
	if (result < 0)
		goto destroy_subsys_class;

	nvme_ns_chr_class = class_create("nvme-generic");
	if (IS_ERR(nvme_ns_chr_class)) {
		result = PTR_ERR(nvme_ns_chr_class);
		goto unregister_generic_ns;
	}

	result = nvme_init_auth();
	if (result)
		goto destroy_ns_chr;
	return 0;

destroy_ns_chr:
	class_destroy(nvme_ns_chr_class);
unregister_generic_ns:
	unregister_chrdev_region(nvme_ns_chr_devt, NVME_MINORS);
destroy_subsys_class:
	class_destroy(nvme_subsys_class);
destroy_class:
	class_destroy(nvme_class);
unregister_chrdev:
	unregister_chrdev_region(nvme_ctrl_base_chr_devt, NVME_MINORS);
destroy_delete_wq:
	destroy_workqueue(nvme_delete_wq);
destroy_reset_wq:
	destroy_workqueue(nvme_reset_wq);
destroy_wq:
	destroy_workqueue(nvme_wq);
out:
	return result;
}

static void __exit nvme_core_exit(void)
{
	nvme_exit_auth();
	class_destroy(nvme_ns_chr_class);
	class_destroy(nvme_subsys_class);
	class_destroy(nvme_class);
	unregister_chrdev_region(nvme_ns_chr_devt, NVME_MINORS);
	unregister_chrdev_region(nvme_ctrl_base_chr_devt, NVME_MINORS);
	destroy_workqueue(nvme_delete_wq);
	destroy_workqueue(nvme_reset_wq);
	destroy_workqueue(nvme_wq);
	ida_destroy(&nvme_ns_chr_minor_ida);
	ida_destroy(&nvme_instance_ida);
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
module_init(nvme_core_init);
module_exit(nvme_core_exit);
