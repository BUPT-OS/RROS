/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_S390_STACKTRACE_H
#define _ASM_S390_STACKTRACE_H

#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <asm/switch_to.h>

enum stack_type {
	STACK_TYPE_UNKNOWN,
	STACK_TYPE_TASK,
	STACK_TYPE_IRQ,
	STACK_TYPE_NODAT,
	STACK_TYPE_RESTART,
	STACK_TYPE_MCCK,
};

struct stack_info {
	enum stack_type type;
	unsigned long begin, end;
};

const char *stack_type_name(enum stack_type type);
int get_stack_info(unsigned long sp, struct task_struct *task,
		   struct stack_info *info, unsigned long *visit_mask);

static inline bool on_stack(struct stack_info *info,
			    unsigned long addr, size_t len)
{
	if (info->type == STACK_TYPE_UNKNOWN)
		return false;
	if (addr + len < addr)
		return false;
	return addr >= info->begin && addr + len <= info->end;
}

/*
 * Stack layout of a C stack frame.
 * Kernel uses the packed stack layout (-mpacked-stack).
 */
struct stack_frame {
	union {
		unsigned long empty[9];
		struct {
			unsigned long sie_control_block;
			unsigned long sie_savearea;
			unsigned long sie_reason;
			unsigned long sie_flags;
			unsigned long sie_control_block_phys;
		};
	};
	unsigned long gprs[10];
	unsigned long back_chain;
};

/*
 * Unlike current_stack_pointer which simply contains the current value of %r15
 * current_frame_address() returns function stack frame address, which matches
 * %r15 upon function invocation. It may differ from %r15 later if function
 * allocates stack for local variables or new stack frame to call other
 * functions.
 */
#define current_frame_address()						\
	((unsigned long)__builtin_frame_address(0) -			\
	 offsetof(struct stack_frame, back_chain))

static __always_inline unsigned long get_stack_pointer(struct task_struct *task,
						       struct pt_regs *regs)
{
	if (regs)
		return (unsigned long)kernel_stack_pointer(regs);
	if (task == current)
		return current_frame_address();
	return (unsigned long)task->thread.ksp;
}

/*
 * To keep this simple mark register 2-6 as being changed (volatile)
 * by the called function, even though register 6 is saved/nonvolatile.
 */
#define CALL_FMT_0 "=&d" (r2)
#define CALL_FMT_1 "+&d" (r2)
#define CALL_FMT_2 CALL_FMT_1, "+&d" (r3)
#define CALL_FMT_3 CALL_FMT_2, "+&d" (r4)
#define CALL_FMT_4 CALL_FMT_3, "+&d" (r5)
#define CALL_FMT_5 CALL_FMT_4, "+&d" (r6)

#define CALL_CLOBBER_5 "0", "1", "14", "cc", "memory"
#define CALL_CLOBBER_4 CALL_CLOBBER_5
#define CALL_CLOBBER_3 CALL_CLOBBER_4, "5"
#define CALL_CLOBBER_2 CALL_CLOBBER_3, "4"
#define CALL_CLOBBER_1 CALL_CLOBBER_2, "3"
#define CALL_CLOBBER_0 CALL_CLOBBER_1

#define CALL_LARGS_0(...)						\
	long dummy = 0
#define CALL_LARGS_1(t1, a1)						\
	long arg1  = (long)(t1)(a1)
#define CALL_LARGS_2(t1, a1, t2, a2)					\
	CALL_LARGS_1(t1, a1);						\
	long arg2 = (long)(t2)(a2)
#define CALL_LARGS_3(t1, a1, t2, a2, t3, a3)				\
	CALL_LARGS_2(t1, a1, t2, a2);					\
	long arg3 = (long)(t3)(a3)
#define CALL_LARGS_4(t1, a1, t2, a2, t3, a3, t4, a4)			\
	CALL_LARGS_3(t1, a1, t2, a2, t3, a3);				\
	long arg4  = (long)(t4)(a4)
#define CALL_LARGS_5(t1, a1, t2, a2, t3, a3, t4, a4, t5, a5)		\
	CALL_LARGS_4(t1, a1, t2, a2, t3, a3, t4, a4);			\
	long arg5 = (long)(t5)(a5)

#define CALL_REGS_0							\
	register long r2 asm("2") = dummy
#define CALL_REGS_1							\
	register long r2 asm("2") = arg1
#define CALL_REGS_2							\
	CALL_REGS_1;							\
	register long r3 asm("3") = arg2
#define CALL_REGS_3							\
	CALL_REGS_2;							\
	register long r4 asm("4") = arg3
#define CALL_REGS_4							\
	CALL_REGS_3;							\
	register long r5 asm("5") = arg4
#define CALL_REGS_5							\
	CALL_REGS_4;							\
	register long r6 asm("6") = arg5

#define CALL_TYPECHECK_0(...)
#define CALL_TYPECHECK_1(t, a, ...)					\
	typecheck(t, a)
#define CALL_TYPECHECK_2(t, a, ...)					\
	CALL_TYPECHECK_1(__VA_ARGS__);					\
	typecheck(t, a)
#define CALL_TYPECHECK_3(t, a, ...)					\
	CALL_TYPECHECK_2(__VA_ARGS__);					\
	typecheck(t, a)
#define CALL_TYPECHECK_4(t, a, ...)					\
	CALL_TYPECHECK_3(__VA_ARGS__);					\
	typecheck(t, a)
#define CALL_TYPECHECK_5(t, a, ...)					\
	CALL_TYPECHECK_4(__VA_ARGS__);					\
	typecheck(t, a)

#define CALL_PARM_0(...) void
#define CALL_PARM_1(t, a, ...) t
#define CALL_PARM_2(t, a, ...) t, CALL_PARM_1(__VA_ARGS__)
#define CALL_PARM_3(t, a, ...) t, CALL_PARM_2(__VA_ARGS__)
#define CALL_PARM_4(t, a, ...) t, CALL_PARM_3(__VA_ARGS__)
#define CALL_PARM_5(t, a, ...) t, CALL_PARM_4(__VA_ARGS__)
#define CALL_PARM_6(t, a, ...) t, CALL_PARM_5(__VA_ARGS__)

/*
 * Use call_on_stack() to call a function switching to a specified
 * stack. Proper sign and zero extension of function arguments is
 * done. Usage:
 *
 * rc = call_on_stack(nr, stack, rettype, fn, t1, a1, t2, a2, ...)
 *
 * - nr specifies the number of function arguments of fn.
 * - stack specifies the stack to be used.
 * - fn is the function to be called.
 * - rettype is the return type of fn.
 * - t1, a1, ... are pairs, where t1 must match the type of the first
 *   argument of fn, t2 the second, etc. a1 is the corresponding
 *   first function argument (not name), etc.
 */
#define call_on_stack(nr, stack, rettype, fn, ...)			\
({									\
	rettype (*__fn)(CALL_PARM_##nr(__VA_ARGS__)) = fn;		\
	unsigned long frame = current_frame_address();			\
	unsigned long __stack = stack;					\
	unsigned long prev;						\
	CALL_LARGS_##nr(__VA_ARGS__);					\
	CALL_REGS_##nr;							\
									\
	CALL_TYPECHECK_##nr(__VA_ARGS__);				\
	asm volatile(							\
		"	lgr	%[_prev],15\n"				\
		"	lg	15,%[_stack]\n"				\
		"	stg	%[_frame],%[_bc](15)\n"			\
		"	brasl	14,%[_fn]\n"				\
		"	lgr	15,%[_prev]\n"				\
		: [_prev] "=&d" (prev), CALL_FMT_##nr			\
		: [_stack] "R" (__stack),				\
		  [_bc] "i" (offsetof(struct stack_frame, back_chain)),	\
		  [_frame] "d" (frame),					\
		  [_fn] "X" (__fn) : CALL_CLOBBER_##nr);		\
	(rettype)r2;							\
})

/*
 * Use call_nodat() to call a function with DAT disabled.
 * Proper sign and zero extension of function arguments is done.
 * Usage:
 *
 * rc = call_nodat(nr, rettype, fn, t1, a1, t2, a2, ...)
 *
 * - nr specifies the number of function arguments of fn.
 * - fn is the function to be called, where fn is a physical address.
 * - rettype is the return type of fn.
 * - t1, a1, ... are pairs, where t1 must match the type of the first
 *   argument of fn, t2 the second, etc. a1 is the corresponding
 *   first function argument (not name), etc.
 *
 * fn() is called with standard C function call ABI, with the exception
 * that no useful stackframe or stackpointer is passed via register 15.
 * Therefore the called function must not use r15 to access the stack.
 */
#define call_nodat(nr, rettype, fn, ...)				\
({									\
	rettype (*__fn)(CALL_PARM_##nr(__VA_ARGS__)) = (fn);		\
	/* aligned since psw_leave must not cross page boundary */	\
	psw_t __aligned(16) psw_leave;					\
	psw_t psw_enter;						\
	CALL_LARGS_##nr(__VA_ARGS__);					\
	CALL_REGS_##nr;							\
									\
	CALL_TYPECHECK_##nr(__VA_ARGS__);				\
	psw_enter.mask = PSW_KERNEL_BITS & ~PSW_MASK_DAT;		\
	psw_enter.addr = (unsigned long)__fn;				\
	asm volatile(							\
		"	epsw	0,1\n"					\
		"	risbg	1,0,0,31,32\n"				\
		"	larl	7,1f\n"					\
		"	stg	1,%[psw_leave]\n"			\
		"	stg	7,8+%[psw_leave]\n"			\
		"	la	7,%[psw_leave]\n"			\
		"	lra	7,0(7)\n"				\
		"	larl	1,0f\n"					\
		"	lra	14,0(1)\n"				\
		"	lpswe	%[psw_enter]\n"				\
		"0:	lpswe	0(7)\n"					\
		"1:\n"							\
		: CALL_FMT_##nr, [psw_leave] "=Q" (psw_leave)		\
		: [psw_enter] "Q" (psw_enter)				\
		: "7", CALL_CLOBBER_##nr);				\
	(rettype)r2;							\
})

#endif /* _ASM_S390_STACKTRACE_H */
