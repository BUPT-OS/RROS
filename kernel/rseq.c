// SPDX-License-Identifier: GPL-2.0+
/*
 * Restartable sequences system call
 *
 * Copyright (C) 2015, Google, Inc.,
 * Paul Turner <pjt@google.com> and Andrew Hunter <ahh@google.com>
 * Copyright (C) 2015-2018, EfficiOS Inc.,
 * Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/rseq.h>
#include <linux/types.h>
#include <asm/ptrace.h>

#define CREATE_TRACE_POINTS
#include <trace/events/rseq.h>

/* The original rseq structure size (including padding) is 32 bytes. */
#define ORIG_RSEQ_SIZE		32

#define RSEQ_CS_NO_RESTART_FLAGS (RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT | \
				  RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL | \
				  RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE)

/*
 *
 * Restartable sequences are a lightweight interface that allows
 * user-level code to be executed atomically relative to scheduler
 * preemption and signal delivery. Typically used for implementing
 * per-cpu operations.
 *
 * It allows user-space to perform update operations on per-cpu data
 * without requiring heavy-weight atomic operations.
 *
 * Detailed algorithm of rseq user-space assembly sequences:
 *
 *                     init(rseq_cs)
 *                     cpu = TLS->rseq::cpu_id_start
 *   [1]               TLS->rseq::rseq_cs = rseq_cs
 *   [start_ip]        ----------------------------
 *   [2]               if (cpu != TLS->rseq::cpu_id)
 *                             goto abort_ip;
 *   [3]               <last_instruction_in_cs>
 *   [post_commit_ip]  ----------------------------
 *
 *   The address of jump target abort_ip must be outside the critical
 *   region, i.e.:
 *
 *     [abort_ip] < [start_ip]  || [abort_ip] >= [post_commit_ip]
 *
 *   Steps [2]-[3] (inclusive) need to be a sequence of instructions in
 *   userspace that can handle being interrupted between any of those
 *   instructions, and then resumed to the abort_ip.
 *
 *   1.  Userspace stores the address of the struct rseq_cs assembly
 *       block descriptor into the rseq_cs field of the registered
 *       struct rseq TLS area. This update is performed through a single
 *       store within the inline assembly instruction sequence.
 *       [start_ip]
 *
 *   2.  Userspace tests to check whether the current cpu_id field match
 *       the cpu number loaded before start_ip, branching to abort_ip
 *       in case of a mismatch.
 *
 *       If the sequence is preempted or interrupted by a signal
 *       at or after start_ip and before post_commit_ip, then the kernel
 *       clears TLS->__rseq_abi::rseq_cs, and sets the user-space return
 *       ip to abort_ip before returning to user-space, so the preempted
 *       execution resumes at abort_ip.
 *
 *   3.  Userspace critical section final instruction before
 *       post_commit_ip is the commit. The critical section is
 *       self-terminating.
 *       [post_commit_ip]
 *
 *   4.  <success>
 *
 *   On failure at [2], or if interrupted by preempt or signal delivery
 *   between [1] and [3]:
 *
 *       [abort_ip]
 *   F1. <failure>
 */

static int rseq_update_cpu_node_id(struct task_struct *t)
{
	struct rseq __user *rseq = t->rseq;
	u32 cpu_id = raw_smp_processor_id();
	u32 node_id = cpu_to_node(cpu_id);
	u32 mm_cid = task_mm_cid(t);

	WARN_ON_ONCE((int) mm_cid < 0);
	if (!user_write_access_begin(rseq, t->rseq_len))
		goto efault;
	unsafe_put_user(cpu_id, &rseq->cpu_id_start, efault_end);
	unsafe_put_user(cpu_id, &rseq->cpu_id, efault_end);
	unsafe_put_user(node_id, &rseq->node_id, efault_end);
	unsafe_put_user(mm_cid, &rseq->mm_cid, efault_end);
	/*
	 * Additional feature fields added after ORIG_RSEQ_SIZE
	 * need to be conditionally updated only if
	 * t->rseq_len != ORIG_RSEQ_SIZE.
	 */
	user_write_access_end();
	trace_rseq_update(t);
	return 0;

efault_end:
	user_write_access_end();
efault:
	return -EFAULT;
}

static int rseq_reset_rseq_cpu_node_id(struct task_struct *t)
{
	u32 cpu_id_start = 0, cpu_id = RSEQ_CPU_ID_UNINITIALIZED, node_id = 0,
	    mm_cid = 0;

	/*
	 * Reset cpu_id_start to its initial state (0).
	 */
	if (put_user(cpu_id_start, &t->rseq->cpu_id_start))
		return -EFAULT;
	/*
	 * Reset cpu_id to RSEQ_CPU_ID_UNINITIALIZED, so any user coming
	 * in after unregistration can figure out that rseq needs to be
	 * registered again.
	 */
	if (put_user(cpu_id, &t->rseq->cpu_id))
		return -EFAULT;
	/*
	 * Reset node_id to its initial state (0).
	 */
	if (put_user(node_id, &t->rseq->node_id))
		return -EFAULT;
	/*
	 * Reset mm_cid to its initial state (0).
	 */
	if (put_user(mm_cid, &t->rseq->mm_cid))
		return -EFAULT;
	/*
	 * Additional feature fields added after ORIG_RSEQ_SIZE
	 * need to be conditionally reset only if
	 * t->rseq_len != ORIG_RSEQ_SIZE.
	 */
	return 0;
}

static int rseq_get_rseq_cs(struct task_struct *t, struct rseq_cs *rseq_cs)
{
	struct rseq_cs __user *urseq_cs;
	u64 ptr;
	u32 __user *usig;
	u32 sig;
	int ret;

#ifdef CONFIG_64BIT
	if (get_user(ptr, &t->rseq->rseq_cs))
		return -EFAULT;
#else
	if (copy_from_user(&ptr, &t->rseq->rseq_cs, sizeof(ptr)))
		return -EFAULT;
#endif
	if (!ptr) {
		memset(rseq_cs, 0, sizeof(*rseq_cs));
		return 0;
	}
	if (ptr >= TASK_SIZE)
		return -EINVAL;
	urseq_cs = (struct rseq_cs __user *)(unsigned long)ptr;
	if (copy_from_user(rseq_cs, urseq_cs, sizeof(*rseq_cs)))
		return -EFAULT;

	if (rseq_cs->start_ip >= TASK_SIZE ||
	    rseq_cs->start_ip + rseq_cs->post_commit_offset >= TASK_SIZE ||
	    rseq_cs->abort_ip >= TASK_SIZE ||
	    rseq_cs->version > 0)
		return -EINVAL;
	/* Check for overflow. */
	if (rseq_cs->start_ip + rseq_cs->post_commit_offset < rseq_cs->start_ip)
		return -EINVAL;
	/* Ensure that abort_ip is not in the critical section. */
	if (rseq_cs->abort_ip - rseq_cs->start_ip < rseq_cs->post_commit_offset)
		return -EINVAL;

	usig = (u32 __user *)(unsigned long)(rseq_cs->abort_ip - sizeof(u32));
	ret = get_user(sig, usig);
	if (ret)
		return ret;

	if (current->rseq_sig != sig) {
		printk_ratelimited(KERN_WARNING
			"Possible attack attempt. Unexpected rseq signature 0x%x, expecting 0x%x (pid=%d, addr=%p).\n",
			sig, current->rseq_sig, current->pid, usig);
		return -EINVAL;
	}
	return 0;
}

static bool rseq_warn_flags(const char *str, u32 flags)
{
	u32 test_flags;

	if (!flags)
		return false;
	test_flags = flags & RSEQ_CS_NO_RESTART_FLAGS;
	if (test_flags)
		pr_warn_once("Deprecated flags (%u) in %s ABI structure", test_flags, str);
	test_flags = flags & ~RSEQ_CS_NO_RESTART_FLAGS;
	if (test_flags)
		pr_warn_once("Unknown flags (%u) in %s ABI structure", test_flags, str);
	return true;
}

static int rseq_need_restart(struct task_struct *t, u32 cs_flags)
{
	u32 flags, event_mask;
	int ret;

	if (rseq_warn_flags("rseq_cs", cs_flags))
		return -EINVAL;

	/* Get thread flags. */
	ret = get_user(flags, &t->rseq->flags);
	if (ret)
		return ret;

	if (rseq_warn_flags("rseq", flags))
		return -EINVAL;

	/*
	 * Load and clear event mask atomically with respect to
	 * scheduler preemption.
	 */
	preempt_disable();
	event_mask = t->rseq_event_mask;
	t->rseq_event_mask = 0;
	preempt_enable();

	return !!event_mask;
}

static int clear_rseq_cs(struct task_struct *t)
{
	/*
	 * The rseq_cs field is set to NULL on preemption or signal
	 * delivery on top of rseq assembly block, as well as on top
	 * of code outside of the rseq assembly block. This performs
	 * a lazy clear of the rseq_cs field.
	 *
	 * Set rseq_cs to NULL.
	 */
#ifdef CONFIG_64BIT
	return put_user(0UL, &t->rseq->rseq_cs);
#else
	if (clear_user(&t->rseq->rseq_cs, sizeof(t->rseq->rseq_cs)))
		return -EFAULT;
	return 0;
#endif
}

/*
 * Unsigned comparison will be true when ip >= start_ip, and when
 * ip < start_ip + post_commit_offset.
 */
static bool in_rseq_cs(unsigned long ip, struct rseq_cs *rseq_cs)
{
	return ip - rseq_cs->start_ip < rseq_cs->post_commit_offset;
}

static int rseq_ip_fixup(struct pt_regs *regs)
{
	unsigned long ip = instruction_pointer(regs);
	struct task_struct *t = current;
	struct rseq_cs rseq_cs;
	int ret;

	ret = rseq_get_rseq_cs(t, &rseq_cs);
	if (ret)
		return ret;

	/*
	 * Handle potentially not being within a critical section.
	 * If not nested over a rseq critical section, restart is useless.
	 * Clear the rseq_cs pointer and return.
	 */
	if (!in_rseq_cs(ip, &rseq_cs))
		return clear_rseq_cs(t);
	ret = rseq_need_restart(t, rseq_cs.flags);
	if (ret <= 0)
		return ret;
	ret = clear_rseq_cs(t);
	if (ret)
		return ret;
	trace_rseq_ip_fixup(ip, rseq_cs.start_ip, rseq_cs.post_commit_offset,
			    rseq_cs.abort_ip);
	instruction_pointer_set(regs, (unsigned long)rseq_cs.abort_ip);
	return 0;
}

/*
 * This resume handler must always be executed between any of:
 * - preemption,
 * - signal delivery,
 * and return to user-space.
 *
 * This is how we can ensure that the entire rseq critical section
 * will issue the commit instruction only if executed atomically with
 * respect to other threads scheduled on the same CPU, and with respect
 * to signal handlers.
 */
void __rseq_handle_notify_resume(struct ksignal *ksig, struct pt_regs *regs)
{
	struct task_struct *t = current;
	int ret, sig;

	if (unlikely(t->flags & PF_EXITING))
		return;

	/*
	 * regs is NULL if and only if the caller is in a syscall path.  Skip
	 * fixup and leave rseq_cs as is so that rseq_sycall() will detect and
	 * kill a misbehaving userspace on debug kernels.
	 */
	if (regs) {
		ret = rseq_ip_fixup(regs);
		if (unlikely(ret < 0))
			goto error;
	}
	if (unlikely(rseq_update_cpu_node_id(t)))
		goto error;
	return;

error:
	sig = ksig ? ksig->sig : 0;
	force_sigsegv(sig);
}

#ifdef CONFIG_DEBUG_RSEQ

/*
 * Terminate the process if a syscall is issued within a restartable
 * sequence.
 */
void rseq_syscall(struct pt_regs *regs)
{
	unsigned long ip = instruction_pointer(regs);
	struct task_struct *t = current;
	struct rseq_cs rseq_cs;

	if (!t->rseq)
		return;
	if (rseq_get_rseq_cs(t, &rseq_cs) || in_rseq_cs(ip, &rseq_cs))
		force_sig(SIGSEGV);
}

#endif

/*
 * sys_rseq - setup restartable sequences for caller thread.
 */
SYSCALL_DEFINE4(rseq, struct rseq __user *, rseq, u32, rseq_len,
		int, flags, u32, sig)
{
	int ret;

	if (flags & RSEQ_FLAG_UNREGISTER) {
		if (flags & ~RSEQ_FLAG_UNREGISTER)
			return -EINVAL;
		/* Unregister rseq for current thread. */
		if (current->rseq != rseq || !current->rseq)
			return -EINVAL;
		if (rseq_len != current->rseq_len)
			return -EINVAL;
		if (current->rseq_sig != sig)
			return -EPERM;
		ret = rseq_reset_rseq_cpu_node_id(current);
		if (ret)
			return ret;
		current->rseq = NULL;
		current->rseq_sig = 0;
		current->rseq_len = 0;
		return 0;
	}

	if (unlikely(flags))
		return -EINVAL;

	if (current->rseq) {
		/*
		 * If rseq is already registered, check whether
		 * the provided address differs from the prior
		 * one.
		 */
		if (current->rseq != rseq || rseq_len != current->rseq_len)
			return -EINVAL;
		if (current->rseq_sig != sig)
			return -EPERM;
		/* Already registered. */
		return -EBUSY;
	}

	/*
	 * If there was no rseq previously registered, ensure the provided rseq
	 * is properly aligned, as communcated to user-space through the ELF
	 * auxiliary vector AT_RSEQ_ALIGN. If rseq_len is the original rseq
	 * size, the required alignment is the original struct rseq alignment.
	 *
	 * In order to be valid, rseq_len is either the original rseq size, or
	 * large enough to contain all supported fields, as communicated to
	 * user-space through the ELF auxiliary vector AT_RSEQ_FEATURE_SIZE.
	 */
	if (rseq_len < ORIG_RSEQ_SIZE ||
	    (rseq_len == ORIG_RSEQ_SIZE && !IS_ALIGNED((unsigned long)rseq, ORIG_RSEQ_SIZE)) ||
	    (rseq_len != ORIG_RSEQ_SIZE && (!IS_ALIGNED((unsigned long)rseq, __alignof__(*rseq)) ||
					    rseq_len < offsetof(struct rseq, end))))
		return -EINVAL;
	if (!access_ok(rseq, rseq_len))
		return -EFAULT;
	current->rseq = rseq;
	current->rseq_len = rseq_len;
	current->rseq_sig = sig;
	/*
	 * If rseq was previously inactive, and has just been
	 * registered, ensure the cpu_id_start and cpu_id fields
	 * are updated before returning to user-space.
	 */
	rseq_set_notify_resume(current);

	return 0;
}
