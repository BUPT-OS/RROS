/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PID_NS_H
#define _LINUX_PID_NS_H

#include <linux/sched.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/threads.h>
#include <linux/nsproxy.h>
#include <linux/ns_common.h>
#include <linux/idr.h>

/* MAX_PID_NS_LEVEL is needed for limiting size of 'struct pid' */
#define MAX_PID_NS_LEVEL 32

struct fs_pin;

#if defined(CONFIG_SYSCTL) && defined(CONFIG_MEMFD_CREATE)
/* modes for vm.memfd_noexec sysctl */
#define MEMFD_NOEXEC_SCOPE_EXEC			0 /* MFD_EXEC implied if unset */
#define MEMFD_NOEXEC_SCOPE_NOEXEC_SEAL		1 /* MFD_NOEXEC_SEAL implied if unset */
#define MEMFD_NOEXEC_SCOPE_NOEXEC_ENFORCED	2 /* same as 1, except MFD_EXEC rejected */
#endif

struct pid_namespace {
	struct idr idr;
	struct rcu_head rcu;
	unsigned int pid_allocated;
	struct task_struct *child_reaper;
	struct kmem_cache *pid_cachep;
	unsigned int level;
	struct pid_namespace *parent;
#ifdef CONFIG_BSD_PROCESS_ACCT
	struct fs_pin *bacct;
#endif
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	int reboot;	/* group exit code if this pidns was rebooted */
	struct ns_common ns;
#if defined(CONFIG_SYSCTL) && defined(CONFIG_MEMFD_CREATE)
	int memfd_noexec_scope;
#endif
} __randomize_layout;

extern struct pid_namespace init_pid_ns;

#define PIDNS_ADDING (1U << 31)

#ifdef CONFIG_PID_NS
static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	if (ns != &init_pid_ns)
		refcount_inc(&ns->ns.count);
	return ns;
}

#if defined(CONFIG_SYSCTL) && defined(CONFIG_MEMFD_CREATE)
static inline int pidns_memfd_noexec_scope(struct pid_namespace *ns)
{
	int scope = MEMFD_NOEXEC_SCOPE_EXEC;

	for (; ns; ns = ns->parent)
		scope = max(scope, READ_ONCE(ns->memfd_noexec_scope));

	return scope;
}
#else
static inline int pidns_memfd_noexec_scope(struct pid_namespace *ns)
{
	return 0;
}
#endif

extern struct pid_namespace *copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *ns);
extern void zap_pid_ns_processes(struct pid_namespace *pid_ns);
extern int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd);
extern void put_pid_ns(struct pid_namespace *ns);

#else /* !CONFIG_PID_NS */
#include <linux/err.h>

static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	return ns;
}

static inline int pidns_memfd_noexec_scope(struct pid_namespace *ns)
{
	return 0;
}

static inline struct pid_namespace *copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *ns)
{
	if (flags & CLONE_NEWPID)
		ns = ERR_PTR(-EINVAL);
	return ns;
}

static inline void put_pid_ns(struct pid_namespace *ns)
{
}

static inline void zap_pid_ns_processes(struct pid_namespace *ns)
{
	BUG();
}

static inline int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd)
{
	return 0;
}
#endif /* CONFIG_PID_NS */

extern struct pid_namespace *task_active_pid_ns(struct task_struct *tsk);
void pidhash_init(void);
void pid_idr_init(void);

static inline bool task_is_in_init_pid_ns(struct task_struct *tsk)
{
	return task_active_pid_ns(tsk) == &init_pid_ns;
}

#endif /* _LINUX_PID_NS_H */
