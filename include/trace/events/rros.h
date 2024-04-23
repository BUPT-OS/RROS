#undef TRACE_SYSTEM
#define TRACE_SYSTEM rros

#if !defined(_TRACE_RROS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RROS_H

#include <linux/tracepoint.h>
#include <linux/trace_seq.h>

DECLARE_EVENT_CLASS(thread_event,
	TP_PROTO(pid_t pid,u32 state,u32 info),
	TP_ARGS(pid,state,info),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(u32, state)
		__field(u32, info)
	),

	TP_fast_assign(
		__entry->pid = pid;
		__entry->state = state;
		__entry->info = info;
	),

	TP_printk("pid=%d state=%#x info=%#x",
		  __entry->pid, __entry->state, __entry->info)
);

DECLARE_EVENT_CLASS(curr_thread_event,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info),

	TP_STRUCT__entry(
		__field(u32, state)
		__field(u32, info)
	),

	TP_fast_assign(
		__entry->state = state;
		__entry->info = info;
	),

	TP_printk("state=%#x info=%#x",
		  __entry->state, __entry->info)
);

DECLARE_EVENT_CLASS(wq_event,
	TP_PROTO(const char *name),
	TP_ARGS(name),

	TP_STRUCT__entry(
		__string(name, name)
	),

	TP_fast_assign(
        __assign_str(name, name);
    ),

	TP_printk("wq=%s", __get_str(name))
);

DECLARE_EVENT_CLASS(timer_event,
	TP_PROTO(const char *name),
	TP_ARGS(name),

	TP_STRUCT__entry(
		__string(name, name)
	),

	TP_fast_assign(
        __assign_str(name, name);
    ),

	TP_printk("timer=%s", __get_str(name))
);

#define rros_print_syscall(__nr)			\
	__print_symbolic(__nr,			\
			 { 0, "oob_read"  },	\
			 { 1, "oob_write" },	\
			 { 2, "oob_ioctl" })

DECLARE_EVENT_CLASS(rros_syscall_entry,
	TP_PROTO(unsigned int nr),
	TP_ARGS(nr),

	TP_STRUCT__entry(
		__field(unsigned int, nr)
	),

	TP_fast_assign(
		__entry->nr = nr;
	),

	TP_printk("syscall=%s", rros_print_syscall(__entry->nr))
);

DECLARE_EVENT_CLASS(rros_syscall_exit,
	TP_PROTO(long result),
	TP_ARGS(result),

	TP_STRUCT__entry(
		__field(long, result)
	),

	TP_fast_assign(
		__entry->result = result;
	),

	TP_printk("result=%ld", __entry->result)
);

// #define rros_print_sched_policy(__policy)		\
// 	__print_symbolic(__policy,			\
// 			 {0, "normal"},	\
// 			 {1, "fifo"},		\
// 			 {2, "rr"},		\
// 			 {44, "quota"},	\ //FIXME: export symbol
// 			 {43, "weak"})

// const char *rros_trace_sched_attrs(struct trace_seq *seq,
// 				  struct rros_sched_attrs *attrs);

// DECLARE_EVENT_CLASS(rros_sched_attrs,
// 	TP_PROTO(void *thread,
// 		 const struct rros_sched_attrs *attrs),
// 	TP_ARGS(thread, attrs),

// 	TP_STRUCT__entry(
// 		__field(void *, thread)
// 		__field(int, policy)
// 		__dynamic_array(char, attrs, sizeof(struct rros_sched_attrs))
// 	),

// 	TP_fast_assign(
// 		__entry->thread = thread;
// 		__entry->policy = attrs->sched_policy;
// 		memcpy(__get_dynamic_array(attrs), attrs, sizeof(*attrs));
// 	),

// 	TP_printk("thread=%s policy=%s param={ %s }",
// 		  rros_element_name(&__entry->thread->element)?
// 		  rros_element_name(&__entry->thread->element):"{ }",
// 		  rros_print_sched_policy(__entry->policy),
// 		  rros_trace_sched_attrs(p,
// 					(struct rros_sched_attrs *)
// 					__get_dynamic_array(attrs))
// 	)
// );

DECLARE_EVENT_CLASS(rros_clock_timespec,
	TP_PROTO(const char *name, const struct timespec64 *val),
	TP_ARGS(name, val),

	TP_STRUCT__entry(
		__field(time64_t, tv_sec_val)	
		__field(long long, tv_nsec_val)
		__string(name, name)
	),

	TP_fast_assign(
		__entry->tv_sec_val = val->tv_sec;
		__entry->tv_nsec_val = val->tv_nsec;
		__assign_str(name, name);
	),

	TP_printk("clock=%s timeval=(%lld.%09lld)",
		  __get_str(name),
		  __entry->tv_sec_val,__entry->tv_nsec_val
	)
);

DECLARE_EVENT_CLASS(rros_clock_ident,
	TP_PROTO(const char *name),
	TP_ARGS(name),
	TP_STRUCT__entry(
		__string(name, name)
	),
	TP_fast_assign(
		__assign_str(name, name);
	),
	TP_printk("name=%s", __get_str(name))
);

DECLARE_EVENT_CLASS(rros_schedule_event,
	TP_PROTO(unsigned long flags,unsigned long local_flags),
	TP_ARGS(flags,local_flags),

	TP_STRUCT__entry(
		__field(unsigned long, flags)
		__field(unsigned long, local_flags)
	),

	TP_fast_assign(
		__entry->flags = flags;
		__entry->local_flags = local_flags;
	),

	TP_printk("flags=%#lx, local_flags=%#lx",
		  __entry->flags, __entry->local_flags)
);

DEFINE_EVENT(rros_schedule_event, rros_schedule,
	TP_PROTO(unsigned long flags,unsigned long local_flags),
	TP_ARGS(flags,local_flags)
);

DEFINE_EVENT(rros_schedule_event, rros_reschedule_ipi,
	TP_PROTO(unsigned long flags,unsigned long local_flags),
	TP_ARGS(flags,local_flags)
);

TRACE_EVENT(rros_pick_thread,
	TP_PROTO(const char* name,pid_t next_pid),
	TP_ARGS(name,next_pid),

	TP_STRUCT__entry(
		__string(name, name)
		__field(pid_t, next_pid)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->next_pid = next_pid;
	),

	TP_printk("{ next=%s[%d] }",
		__get_str(name), __entry->next_pid)
);

TRACE_EVENT(rros_switch_context,
	TP_PROTO(const char *prev_name,const char *next_name,pid_t prev_pid,int prev_prio,u32 prev_state,pid_t next_pid,int next_prio),
	TP_ARGS(prev_name, next_name,prev_pid,prev_prio,prev_state,next_pid,next_prio),

	TP_STRUCT__entry(
		__string(prev_name, prev_name)
		__string(next_name, next_name)
		__field(pid_t, prev_pid)
		__field(int, prev_prio)
		__field(u32, prev_state)
		__field(pid_t, next_pid)
		__field(int, next_prio)
	),

	TP_fast_assign(
		__entry->prev_pid = prev_pid;
		__entry->prev_prio = prev_prio;
		__entry->prev_state = prev_state;
		__entry->next_pid = next_pid;
		__entry->next_prio = next_prio;
		__assign_str(prev_name, prev_name);
		__assign_str(next_name, next_name);
	),

	TP_printk("{ %s[%d] prio=%d, state=%#x } => { %s[%d] prio=%d }",
		  __get_str(prev_name), __entry->prev_pid,
		  __entry->prev_prio, __entry->prev_state,
		  __get_str(next_name), __entry->next_pid, __entry->next_prio)
);

TRACE_EVENT(rros_switch_tail,
	TP_PROTO(const char* curr_name,pid_t curr_pid),
	TP_ARGS(curr_name,curr_pid),

	TP_STRUCT__entry(
		__string(curr_name, curr_name)
		__field(pid_t, curr_pid)
	),

	TP_fast_assign(
		__assign_str(curr_name, curr_name);
		__entry->curr_pid = curr_pid;
	),

	TP_printk("{ current=%s[%d] }",
		__get_str(curr_name), __entry->curr_pid)
);

TRACE_EVENT(rros_init_thread,
	TP_PROTO(void* thread,const char* thread_name,const char *class_name,
		unsigned long flags, int cprio,int status),
	TP_ARGS(thread,thread_name,class_name, flags,cprio,status),

	TP_STRUCT__entry(
		__field(void *, thread)
		__string(thread_name, thread_name)
		__string(class_name, class_name)
		__field(unsigned long, flags)
		__field(int, cprio)
		__field(int, status)
	),

	TP_fast_assign(
		__entry->thread = thread;
		__assign_str(thread_name, thread_name);
		// __entry->flags = iattr->flags | (iattr->observable ? T_OBSERV : 0); //TODO:
		__entry->flags = flags;
		__assign_str(class_name, class_name);
		__entry->cprio = cprio;
		__entry->status = status;
	),

	TP_printk("thread=%p name=%s flags=%#lx class=%s prio=%d status=%#x",
		   __entry->thread, __get_str(thread_name), __entry->flags,
		  __get_str(class_name), __entry->cprio, __entry->status)
);

TRACE_EVENT(rros_sleep_on,
	TP_PROTO(pid_t pid,ktime_t timeout,
		 int timeout_mode, void *wchan,const char* clock_name,
		 const char* wchan_name),
	TP_ARGS(pid,timeout, timeout_mode, wchan,clock_name,wchan_name),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(ktime_t, timeout)
		__field(int, timeout_mode)
		__field(void *, wchan)
		__string(wchan_name, wchan_name)
		__string(clock_name, clock_name)
	),

	TP_fast_assign(
		__entry->pid = pid;
		__entry->timeout = timeout;
		__entry->timeout_mode = timeout_mode;
		__entry->wchan = wchan;
		__assign_str(clock_name, clock_name);
		__assign_str(wchan_name, wchan_name);
	),

	TP_printk("pid=%d timeout=%Lu timeout_mode=%d clock=%s wchan=%s(%p)",
		  __entry->pid,
		  ktime_to_ns(__entry->timeout), __entry->timeout_mode,
		  __get_str(clock_name),
		  __get_str(wchan_name),
		  __entry->wchan)
);

TRACE_EVENT(rros_wakeup_thread,
	TP_PROTO(const char *thread_name,pid_t pid, int mask, int info),
	TP_ARGS(thread_name,pid, mask, info),

	TP_STRUCT__entry(
		__string(name, thread_name)
		__field(pid_t, pid)
		__field(int, mask)
		__field(int, info)
	),

	TP_fast_assign(
		__assign_str(name, thread_name);
		__entry->pid = pid;
		__entry->mask = mask;
		__entry->info = info;
	),

	TP_printk("name=%s pid=%d mask=%#x info=%#x",
		__get_str(name), __entry->pid,
		__entry->mask, __entry->info)
);

TRACE_EVENT(rros_hold_thread,
	TP_PROTO(const char *thread_name,pid_t pid, unsigned long mask),
	TP_ARGS(thread_name, pid, mask),

	TP_STRUCT__entry(
		__string(thread_name, thread_name)
		__field(pid_t, pid)
		__field(unsigned long, mask)
	),

	TP_fast_assign(
		__assign_str(thread_name, thread_name);
		__entry->pid = pid;
		__entry->mask = mask;
	),

	TP_printk("name=%s pid=%d mask=%#lx",
		  __get_str(thread_name), __entry->pid, __entry->mask)
);

TRACE_EVENT(rros_release_thread,
	TP_PROTO(const char *thread_name,pid_t pid, int mask, int info),
	TP_ARGS(thread_name, pid, mask, info),

	TP_STRUCT__entry(
		__string(thread_name, thread_name)
		__field(pid_t, pid)
		__field(int, mask)
		__field(int, info)
	),

	TP_fast_assign(
		__assign_str(thread_name, thread_name);
		__entry->pid = pid;
		__entry->mask = mask;
		__entry->info = info;
	),

	TP_printk("name=%s pid=%d mask=%#x info=%#x",
		__get_str(thread_name), __entry->pid,
		__entry->mask, __entry->info)
);

// TRACE_EVENT(rros_thread_fault,
// 	TP_PROTO(int trapnr, struct pt_regs *regs),
// 	TP_ARGS(trapnr, regs),

// 	TP_STRUCT__entry(
// 		__field(long,	ip)
// 		__field(unsigned int, trapnr)
// 	),

// 	TP_fast_assign(
// 		__entry->ip = instruction_pointer(regs);
// 		__entry->trapnr = trapnr;
// 	),

// 	TP_printk("ip=%#lx trapnr=%#x",
// 		  __entry->ip, __entry->trapnr)
// );

TRACE_EVENT(rros_thread_set_current_prio,
	TP_PROTO(void *thread,pid_t pid, int cprio),
	TP_ARGS(thread,pid,cprio),

	TP_STRUCT__entry(
		__field(void *, thread)
		__field(pid_t, pid)
		__field(int, cprio)
	),

	TP_fast_assign(
		__entry->thread = thread;
		__entry->pid = pid;
		__entry->cprio = cprio;
	),

	TP_printk("thread=%p pid=%d prio=%d",
		  __entry->thread, __entry->pid, __entry->cprio)
);

DEFINE_EVENT(thread_event, rros_thread_cancel,
	TP_PROTO(pid_t pid,u32 state,u32 info),
	TP_ARGS(pid,state,info)
);

DEFINE_EVENT(thread_event, rros_thread_join,
	TP_PROTO(int pid,u32 state,u32 info),
	TP_ARGS(pid,state,info)
);

DEFINE_EVENT(thread_event, rros_unblock_thread,
	TP_PROTO(int pid,u32 state,u32 info),
	TP_ARGS(pid,state,info)
);

DEFINE_EVENT(curr_thread_event, rros_thread_wait_period,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

DEFINE_EVENT(curr_thread_event, rros_thread_missed_period,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

TRACE_EVENT(rros_thread_migrate,
	TP_PROTO(void *thread,pid_t pid,unsigned int cpu),
	TP_ARGS(thread,pid,cpu),

	TP_STRUCT__entry(
		__field(void *, thread)
		__field(pid_t, pid)
		__field(unsigned int, cpu)
	),

	TP_fast_assign(
		__entry->thread = thread;
		__entry->pid = pid;
		__entry->cpu = cpu;
	),

	TP_printk("thread=%p pid=%d cpu=%u",
		  __entry->thread, __entry->pid, __entry->cpu)
);

DEFINE_EVENT(curr_thread_event, rros_watchdog_signal,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

DEFINE_EVENT(curr_thread_event, rros_switch_oob,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

DEFINE_EVENT(curr_thread_event, rros_switched_oob,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

#define rros_print_switch_cause(cause)						\
	__print_symbolic(cause,							\
			{ -1,		"breakpoint trap" },	\
			{ 0,		"undefined" },		\
			{ 1,		"in-band signal" }, 	\
			{ 2,		"in-band syscall" },	\
			{ 3,		"processor exception" },\
			{ 4,		"watchdog" },		\
			{ 5,		"lock dependency" },	\
			{ 6,	"lock imbalance" },	\
			{ 7,		"sleep holding lock" },	\
			{ 8,		"stage exclusion" } )

TRACE_EVENT(rros_switch_inband,
	TP_PROTO(int cause),
	TP_ARGS(cause),

	TP_STRUCT__entry(
		__field(int, cause)
	),

	TP_fast_assign(
		__entry->cause = cause;
	),

	TP_printk("cause=%s", rros_print_switch_cause(__entry->cause))
);

DEFINE_EVENT(curr_thread_event, rros_switched_inband,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

DEFINE_EVENT(curr_thread_event, rros_kthread_entry,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

TRACE_EVENT(rros_thread_map,
	TP_PROTO(void* thread,pid_t pid,int prio),
	TP_ARGS(thread,pid,prio),

	TP_STRUCT__entry(
		__field(void *, thread)
		__field(pid_t, pid)
		__field(int, prio)
	),

	TP_fast_assign(
		__entry->thread = thread;
		__entry->pid = pid;
		__entry->prio = prio;
	),

	TP_printk("thread=%p pid=%d prio=%d",
		  __entry->thread, __entry->pid, __entry->prio)
);

DEFINE_EVENT(curr_thread_event, rros_thread_unmap,
	TP_PROTO(u32 state,u32 info),
	TP_ARGS(state,info)
);

TRACE_EVENT(rros_inband_wakeup,
	TP_PROTO(pid_t pid,char* comm),
	TP_ARGS(pid,comm),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
	),

	TP_fast_assign(
		__entry->pid = pid;
		memcpy(__entry->comm, comm, TASK_COMM_LEN);
	),

	TP_printk("pid=%d comm=%s",
		  __entry->pid, __entry->comm)
);

TRACE_EVENT(rros_inband_signal,
	TP_PROTO(const char *element_name,pid_t pid, int sig, int sigval),
	TP_ARGS(element_name,pid, sig, sigval),

	TP_STRUCT__entry(
		__string(element_name, element_name)
		__field(pid_t, pid)
		__field(int, sig)
		__field(int, sigval)
	),

	TP_fast_assign(
		__assign_str(element_name, element_name);
		__entry->pid = pid;
		__entry->sig = sig;
		__entry->sigval = sigval;
	),

	/* Caller holds a reference on @thread, memory cannot be stale. */
	TP_printk("thread=%s pid=%d sig=%d sigval=%d",
		__get_str(element_name),
		__entry->pid,
		__entry->sig, __entry->sigval)
);

DEFINE_EVENT(timer_event, rros_timer_stop,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);

DEFINE_EVENT(timer_event, rros_timer_expire,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);
#define rros_print_timer_mode(mode)		\
	__print_symbolic(mode,			\
			 { 0, "rel" },	\
			 { 1, "abs" })

TRACE_EVENT(rros_timer_start,
	TP_PROTO(const char *timer_name, ktime_t value, ktime_t interval),
	TP_ARGS(timer_name, value, interval),

	TP_STRUCT__entry(
		__string(timer_name, timer_name)
		__field(ktime_t, value)
		__field(ktime_t, interval)
	),

	TP_fast_assign(
		__assign_str(timer_name, timer_name);
		__entry->value = value;
		__entry->interval = interval;
	),

	TP_printk("timer=%s value=%Lu interval=%Lu",
		__get_str(timer_name),
		ktime_to_ns(__entry->value),
		ktime_to_ns(__entry->interval))
);

TRACE_EVENT(rros_timer_move,
	TP_PROTO(const char* timer_name,
		 const char* clock_name,
		 unsigned int cpu),
	TP_ARGS(timer_name, clock_name, cpu),

	TP_STRUCT__entry(
		__field(unsigned int, cpu)
		__string(timer_name, timer_name)
		__string(clock_name, clock_name)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
		__assign_str(timer_name, timer_name);
		__assign_str(clock_name, clock_name);
	),

	TP_printk("timer=%s clock=%s cpu=%u",
		  __get_str(timer_name),
		  __get_str(clock_name),
		  __entry->cpu)
);

TRACE_EVENT(rros_timer_shot,
	TP_PROTO(const char* timer_name, s64 delta, u64 cycles),
	TP_ARGS(timer_name, delta, cycles),

	TP_STRUCT__entry(
		__field(u64, secs)
		__field(u32, nsecs)
		__field(s64, delta)
		__field(u64, cycles)
		__string(name, timer_name)
	),

	TP_fast_assign(
		__entry->cycles = cycles;
		__entry->delta = delta;
		__entry->secs = div_u64_rem(trace_clock_local() + delta,
					    NSEC_PER_SEC, &__entry->nsecs);
		__assign_str(name, timer_name);
	),

	TP_printk("%s at %Lu.%06u (delay: %Ld us, %Lu cycles)",
		  __get_str(name),
		  (unsigned long long)__entry->secs,
		  __entry->nsecs / 1000, div_s64(__entry->delta, 1000),
		  __entry->cycles)
);

DEFINE_EVENT(wq_event, rros_wait,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);

DEFINE_EVENT(wq_event, rros_wake_up,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);

DEFINE_EVENT(wq_event, rros_flush_wait,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);

DEFINE_EVENT(wq_event, rros_finish_wait,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);

DEFINE_EVENT(rros_syscall_entry, rros_oob_sysentry,
	TP_PROTO(unsigned int nr),
	TP_ARGS(nr)
);

DEFINE_EVENT(rros_syscall_exit, rros_oob_sysexit,
	TP_PROTO(long result),
	TP_ARGS(result)
);

DEFINE_EVENT(rros_syscall_entry, rros_inband_sysentry,
	TP_PROTO(unsigned int nr),
	TP_ARGS(nr)
);

DEFINE_EVENT(rros_syscall_exit, rros_inband_sysexit,
	TP_PROTO(long result),
	TP_ARGS(result)
);

// DEFINE_EVENT(rros_sched_attrs, rros_thread_setsched,
// 	TP_PROTO(struct rros_thread *thread,
// 		 const struct rros_sched_attrs *attrs),
// 	TP_ARGS(thread, attrs)
// );

// DEFINE_EVENT(rros_sched_attrs, rros_thread_getsched,
// 	TP_PROTO(struct rros_thread *thread,
// 		 const struct rros_sched_attrs *attrs),
// 	TP_ARGS(thread, attrs)
// );

#define rros_print_thread_mode(__mode)	\
	__print_flags(__mode, "|",	\
		{0x00200000, "hmobs"},	\
		{0x00100000, "hmsig"},	\
		{0x00020000, "wosx"},	\
		{0x00008000, "woss"},	\
		{0x00010000, "woli"})

TRACE_EVENT(rros_thread_update_mode,
	TP_PROTO(const char *element_name, int mode, bool set),
	TP_ARGS(element_name, mode, set),
	TP_STRUCT__entry(
		__string(element_name, element_name)
		__field(int, mode)
		__field(bool, set)
	),
	TP_fast_assign(
		__assign_str(element_name,element_name);
		__entry->mode = mode;
		__entry->set = set;
	),
	TP_printk("thread=%s %s %#x(%s)",
		  __get_str(element_name),
		  __entry->set ? "set" : "clear",
		  __entry->mode, rros_print_thread_mode(__entry->mode))
);

DEFINE_EVENT(rros_clock_timespec, rros_clock_getres,
	TP_PROTO(const char *clock_name, const struct timespec64 *val),
	TP_ARGS(clock_name, val)
);

DEFINE_EVENT(rros_clock_timespec, rros_clock_gettime,
	TP_PROTO(const char *clock_name, const struct timespec64 *val),
	TP_ARGS(clock_name, val)
);

DEFINE_EVENT(rros_clock_timespec, rros_clock_settime,
	TP_PROTO(const char *clock_name, const struct timespec64 *val),
	TP_ARGS(clock_name, val)
);

TRACE_EVENT(rros_clock_adjtime,
	TP_PROTO(const char *clock_name, struct __kernel_timex *tx),
	TP_ARGS(clock_name, tx),

	TP_STRUCT__entry(
		__field(struct __kernel_timex *, tx)
		__string(clock_name, clock_name)
	),

	TP_fast_assign(
		__entry->tx = tx;
		__assign_str(clock_name, clock_name);
	),

	TP_printk("clock=%s timex=%p",
		  __get_str(clock_name),
		  __entry->tx
	)
);

DEFINE_EVENT(rros_clock_ident, rros_register_clock,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);

DEFINE_EVENT(rros_clock_ident, rros_unregister_clock,
	TP_PROTO(const char *name),
	TP_ARGS(name)
);

TRACE_EVENT(rros_trace,
	TP_PROTO(const char *msg),
	TP_ARGS(msg),
	TP_STRUCT__entry(
		__string(msg, msg)
	),
	TP_fast_assign(
		__assign_str(msg, msg);
	),
	TP_printk("%s", __get_str(msg))
);

TRACE_EVENT(rros_latspot,
	TP_PROTO(int latmax_ns),
	TP_ARGS(latmax_ns),
	TP_STRUCT__entry(
		 __field(int, latmax_ns)
	),
	TP_fast_assign(
		__entry->latmax_ns = latmax_ns;
	),
	TP_printk("** latency peak: %d.%.3d us **",
		  __entry->latmax_ns / 1000,
		  __entry->latmax_ns % 1000)
);

TRACE_EVENT(rros_fpu_corrupt,
	TP_PROTO(unsigned int fp_val),
	TP_ARGS(fp_val),
	TP_STRUCT__entry(
		 __field(unsigned int, fp_val)
	),
	TP_fast_assign(
		__entry->fp_val = fp_val;
	),
	TP_printk("** bad FPU context: fp_val = %u **",
		__entry->fp_val)
);

/* Basically evl_trace() + trigger point */
TRACE_EVENT(rros_trigger,
	TP_PROTO(const char *issuer),
	TP_ARGS(issuer),
	TP_STRUCT__entry(
		__string(issuer, issuer)
	),
	TP_fast_assign(
		__assign_str(issuer, issuer);
	),
	TP_printk("%s", __get_str(issuer))
);

#endif /* _TRACE_RROS_H */

/* This part must be outside protection */
#include <trace/define_trace.h>