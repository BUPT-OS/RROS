#define CREATE_TRACE_POINTS
#include <trace/events/rros.h>
#include <uapi/linux/uio.h>

#define copy_on_stack(dst,src) \
char dst[src.iov_len+1]; \
strncpy(dst,src.iov_base,src.iov_len); \
dst[src.iov_len] = 0;

void rust_helper_trace_rros_schedule(unsigned long flags,unsigned long local_flags)
{
        trace_rros_schedule(flags,local_flags);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_schedule);

void rust_helper_trace_rros_reschedule_ipi(unsigned long flags,unsigned long local_flags)
{
        trace_rros_reschedule_ipi(flags,local_flags);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_reschedule_ipi);

void rust_helper_trace_rros_pick_thread(struct iovec  name_struct,pid_t next_pid)
{
        copy_on_stack(name,name_struct);
        trace_rros_pick_thread(name,next_pid);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_pick_thread);

void rust_helper_trace_rros_switch_context(struct iovec prev_name_struct,struct iovec next_name_struct,pid_t prev_pid,int prev_prio,u32 prev_state,pid_t next_pid,int next_prio)
{
        copy_on_stack(prev_name,prev_name_struct);
        copy_on_stack(next_name,next_name_struct);
        trace_rros_switch_context(prev_name,next_name,prev_pid,prev_prio,prev_state,next_pid,next_prio);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_switch_context);

void rust_helper_trace_rros_switch_tail(struct iovec  curr_name_struct,pid_t curr_pid)
{
        copy_on_stack(curr_name,curr_name_struct);
        trace_rros_switch_tail(curr_name,curr_pid);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_switch_tail);

void rust_helper_trace_rros_init_thread(void*  thread,struct iovec  thread_name_struct,struct iovec class_name_struct,  unsigned long flags, int cprio,int status)
{
        copy_on_stack(thread_name,thread_name_struct);
        copy_on_stack(class_name,class_name_struct);
        trace_rros_init_thread(thread,thread_name,class_name,flags,cprio,status);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_init_thread);

void rust_helper_trace_rros_sleep_on(pid_t pid,ktime_t timeout,   int timeout_mode, void * wchan,struct iovec  clock_name_struct,   struct iovec  wchan_name_struct)
{
        copy_on_stack(clock_name,clock_name_struct);
        copy_on_stack(wchan_name,wchan_name_struct);
        trace_rros_sleep_on(pid,timeout,timeout_mode,wchan,clock_name,wchan_name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_sleep_on);

void rust_helper_trace_rros_wakeup_thread(struct iovec thread_name_struct,pid_t pid, int mask, int info)
{
        copy_on_stack(thread_name,thread_name_struct);
        trace_rros_wakeup_thread(thread_name,pid,mask,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_wakeup_thread);

void rust_helper_trace_rros_hold_thread(struct iovec thread_name_struct,pid_t pid, unsigned long mask)
{
        copy_on_stack(thread_name,thread_name_struct);
        trace_rros_hold_thread(thread_name,pid,mask);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_hold_thread);

void rust_helper_trace_rros_release_thread(struct iovec thread_name_struct,pid_t pid, int mask, int info)
{
        copy_on_stack(thread_name,thread_name_struct);
        trace_rros_release_thread(thread_name,pid,mask,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_release_thread);

void rust_helper_trace_rros_thread_set_current_prio(void * thread,pid_t pid, int cprio)
{
        trace_rros_thread_set_current_prio(thread,pid,cprio);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_set_current_prio);

void rust_helper_trace_rros_thread_cancel(pid_t pid,u32 state,u32 info)
{
        trace_rros_thread_cancel(pid,state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_cancel);

void rust_helper_trace_rros_thread_join(int pid,u32 state,u32 info)
{
        trace_rros_thread_join(pid,state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_join);

void rust_helper_trace_rros_unblock_thread(int pid,u32 state,u32 info)
{
        trace_rros_unblock_thread(pid,state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_unblock_thread);

void rust_helper_trace_rros_thread_wait_period(u32 state,u32 info)
{
        trace_rros_thread_wait_period(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_wait_period);

void rust_helper_trace_rros_thread_missed_period(u32 state,u32 info)
{
        trace_rros_thread_missed_period(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_missed_period);

void rust_helper_trace_rros_thread_migrate(void * thread,pid_t pid,unsigned int cpu)
{
        trace_rros_thread_migrate(thread,pid,cpu);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_migrate);

void rust_helper_trace_rros_watchdog_signal(u32 state,u32 info)
{
        trace_rros_watchdog_signal(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_watchdog_signal);

void rust_helper_trace_rros_switch_oob(u32 state,u32 info)
{
        trace_rros_switch_oob(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_switch_oob);

void rust_helper_trace_rros_switched_oob(u32 state,u32 info)
{
        trace_rros_switched_oob(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_switched_oob);

void rust_helper_trace_rros_switch_inband(int cause)
{
        trace_rros_switch_inband(cause);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_switch_inband);

void rust_helper_trace_rros_switched_inband(u32 state,u32 info)
{
        trace_rros_switched_inband(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_switched_inband);

void rust_helper_trace_rros_kthread_entry(u32 state,u32 info)
{
        trace_rros_kthread_entry(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_kthread_entry);

void rust_helper_trace_rros_thread_map(void*  thread,pid_t pid,int prio)
{
        trace_rros_thread_map(thread,pid,prio);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_map);

void rust_helper_trace_rros_thread_unmap(u32 state,u32 info)
{
        trace_rros_thread_unmap(state,info);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_unmap);

void rust_helper_trace_rros_inband_wakeup(pid_t pid,char*  comm)
{
        trace_rros_inband_wakeup(pid,comm);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_inband_wakeup);

void rust_helper_trace_rros_inband_signal(struct iovec element_name_struct,pid_t pid, int sig, int sigval)
{
        copy_on_stack(element_name,element_name_struct);
        trace_rros_inband_signal(element_name,pid,sig,sigval);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_inband_signal);

void rust_helper_trace_rros_timer_stop(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_timer_stop(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_timer_stop);

void rust_helper_trace_rros_timer_expire(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_timer_expire(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_timer_expire);

void rust_helper_trace_rros_timer_start(struct iovec timer_name_struct, ktime_t value, ktime_t interval)
{
        copy_on_stack(timer_name,timer_name_struct);
        trace_rros_timer_start(timer_name,value,interval);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_timer_start);

void rust_helper_trace_rros_timer_move(struct iovec  timer_name_struct,   struct iovec  clock_name_struct,   unsigned int cpu)
{
        copy_on_stack(timer_name,timer_name_struct);
        copy_on_stack(clock_name,clock_name_struct);
        trace_rros_timer_move(timer_name,clock_name,cpu);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_timer_move);

void rust_helper_trace_rros_timer_shot(struct iovec  timer_name_struct, s64 delta, u64 cycles)
{
        copy_on_stack(timer_name,timer_name_struct);
        trace_rros_timer_shot(timer_name,delta,cycles);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_timer_shot);

void rust_helper_trace_rros_wait(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_wait(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_wait);

void rust_helper_trace_rros_wake_up(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_wake_up(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_wake_up);

void rust_helper_trace_rros_flush_wait(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_flush_wait(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_flush_wait);

void rust_helper_trace_rros_finish_wait(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_finish_wait(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_finish_wait);

void rust_helper_trace_rros_oob_sysentry(unsigned int nr)
{
        trace_rros_oob_sysentry(nr);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_oob_sysentry);

void rust_helper_trace_rros_oob_sysexit(long result)
{
        trace_rros_oob_sysexit(result);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_oob_sysexit);

void rust_helper_trace_rros_inband_sysentry(unsigned int nr)
{
        trace_rros_inband_sysentry(nr);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_inband_sysentry);

void rust_helper_trace_rros_inband_sysexit(long result)
{
        trace_rros_inband_sysexit(result);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_inband_sysexit);

void rust_helper_trace_rros_thread_update_mode(struct iovec element_name_struct, int mode, bool set)
{
        copy_on_stack(element_name,element_name_struct);
        trace_rros_thread_update_mode(element_name,mode,set);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_thread_update_mode);

void rust_helper_trace_rros_clock_getres(struct iovec clock_name_struct, const struct timespec64 * val)
{
        copy_on_stack(clock_name,clock_name_struct);
        trace_rros_clock_getres(clock_name,val);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_clock_getres);

void rust_helper_trace_rros_clock_gettime(struct iovec clock_name_struct, const struct timespec64 * val)
{
        copy_on_stack(clock_name,clock_name_struct);
        trace_rros_clock_gettime(clock_name,val);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_clock_gettime);

void rust_helper_trace_rros_clock_settime(struct iovec clock_name_struct, const struct timespec64 * val)
{
        copy_on_stack(clock_name,clock_name_struct);
        trace_rros_clock_settime(clock_name,val);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_clock_settime);

void rust_helper_trace_rros_clock_adjtime(struct iovec clock_name_struct, struct __kernel_timex * tx)
{
        copy_on_stack(clock_name,clock_name_struct);
        trace_rros_clock_adjtime(clock_name,tx);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_clock_adjtime);

void rust_helper_trace_rros_register_clock(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_register_clock(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_register_clock);

void rust_helper_trace_rros_unregister_clock(struct iovec name_struct)
{
        copy_on_stack(name,name_struct);
        trace_rros_unregister_clock(name);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_unregister_clock);

void rust_helper_trace_rros_trace(struct iovec msg_struct)
{
        copy_on_stack(msg,msg_struct);
        trace_rros_trace(msg);
}
EXPORT_SYMBOL_GPL(rust_helper_trace_rros_trace);