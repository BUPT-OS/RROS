// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>

#include <linux/sched.h> /* for wake_up_process() */
#include <linux/ftrace.h>
#ifndef CONFIG_ARM64
#include <asm/asm-offsets.h>
#endif

extern void my_direct_func(struct task_struct *p);

void my_direct_func(struct task_struct *p)
{
	trace_printk("waking up %s-%d\n", p->comm, p->pid);
}

extern void my_tramp(void *);

#ifdef CONFIG_X86_64

#include <asm/ibt.h>
#include <asm/nospec-branch.h>

asm (
"	.pushsection    .text, \"ax\", @progbits\n"
"	.type		my_tramp, @function\n"
"	.globl		my_tramp\n"
"   my_tramp:"
	ASM_ENDBR
"	pushq %rbp\n"
"	movq %rsp, %rbp\n"
	CALL_DEPTH_ACCOUNT
"	pushq %rdi\n"
"	call my_direct_func\n"
"	popq %rdi\n"
"	leave\n"
	ASM_RET
"	.size		my_tramp, .-my_tramp\n"
"	.popsection\n"
);

#endif /* CONFIG_X86_64 */

#ifdef CONFIG_S390

asm (
"	.pushsection	.text, \"ax\", @progbits\n"
"	.type		my_tramp, @function\n"
"	.globl		my_tramp\n"
"   my_tramp:"
"	lgr		%r1,%r15\n"
"	stmg		%r0,%r5,"__stringify(__SF_GPRS)"(%r15)\n"
"	stg		%r14,"__stringify(__SF_GPRS+8*8)"(%r15)\n"
"	aghi		%r15,"__stringify(-STACK_FRAME_OVERHEAD)"\n"
"	stg		%r1,"__stringify(__SF_BACKCHAIN)"(%r15)\n"
"	brasl		%r14,my_direct_func\n"
"	aghi		%r15,"__stringify(STACK_FRAME_OVERHEAD)"\n"
"	lmg		%r0,%r5,"__stringify(__SF_GPRS)"(%r15)\n"
"	lg		%r14,"__stringify(__SF_GPRS+8*8)"(%r15)\n"
"	lgr		%r1,%r0\n"
"	br		%r1\n"
"	.size		my_tramp, .-my_tramp\n"
"	.popsection\n"
);

#endif /* CONFIG_S390 */

#ifdef CONFIG_ARM64

asm (
"	.pushsection	.text, \"ax\", @progbits\n"
"	.type		my_tramp, @function\n"
"	.globl		my_tramp\n"
"   my_tramp:"
"	hint	34\n" // bti	c
"	sub	sp, sp, #32\n"
"	stp	x9, x30, [sp]\n"
"	str	x0, [sp, #16]\n"
"	bl	my_direct_func\n"
"	ldp	x30, x9, [sp]\n"
"	ldr	x0, [sp, #16]\n"
"	add	sp, sp, #32\n"
"	ret	x9\n"
"	.size		my_tramp, .-my_tramp\n"
"	.popsection\n"
);

#endif /* CONFIG_ARM64 */

#ifdef CONFIG_LOONGARCH

asm (
"	.pushsection	.text, \"ax\", @progbits\n"
"	.type		my_tramp, @function\n"
"	.globl		my_tramp\n"
"   my_tramp:\n"
"	addi.d	$sp, $sp, -32\n"
"	st.d	$a0, $sp, 0\n"
"	st.d	$t0, $sp, 8\n"
"	st.d	$ra, $sp, 16\n"
"	bl	my_direct_func\n"
"	ld.d	$a0, $sp, 0\n"
"	ld.d	$t0, $sp, 8\n"
"	ld.d	$ra, $sp, 16\n"
"	addi.d	$sp, $sp, 32\n"
"	jr	$t0\n"
"	.size		my_tramp, .-my_tramp\n"
"	.popsection\n"
);

#endif /* CONFIG_LOONGARCH */

static struct ftrace_ops direct;

static int __init ftrace_direct_init(void)
{
	ftrace_set_filter_ip(&direct, (unsigned long) wake_up_process, 0, 0);

	return register_ftrace_direct(&direct, (unsigned long) my_tramp);
}

static void __exit ftrace_direct_exit(void)
{
	unregister_ftrace_direct(&direct, (unsigned long)my_tramp, true);
}

module_init(ftrace_direct_init);
module_exit(ftrace_direct_exit);

MODULE_AUTHOR("Steven Rostedt");
MODULE_DESCRIPTION("Example use case of using register_ftrace_direct()");
MODULE_LICENSE("GPL");
