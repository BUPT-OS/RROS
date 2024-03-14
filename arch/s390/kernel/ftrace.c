// SPDX-License-Identifier: GPL-2.0
/*
 * Dynamic function tracer architecture backend.
 *
 * Copyright IBM Corp. 2009,2014
 *
 *   Author(s): Martin Schwidefsky <schwidefsky@de.ibm.com>
 */

#include <linux/moduleloader.h>
#include <linux/hardirq.h>
#include <linux/uaccess.h>
#include <linux/ftrace.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kprobes.h>
#include <trace/syscall.h>
#include <asm/asm-offsets.h>
#include <asm/text-patching.h>
#include <asm/cacheflush.h>
#include <asm/ftrace.lds.h>
#include <asm/nospec-branch.h>
#include <asm/set_memory.h>
#include "entry.h"
#include "ftrace.h"

/*
 * To generate function prologue either gcc's hotpatch feature (since gcc 4.8)
 * or a combination of -pg -mrecord-mcount -mnop-mcount -mfentry flags
 * (since gcc 9 / clang 10) is used.
 * In both cases the original and also the disabled function prologue contains
 * only a single six byte instruction and looks like this:
 * >	brcl	0,0			# offset 0
 * To enable ftrace the code gets patched like above and afterwards looks
 * like this:
 * >	brasl	%r0,ftrace_caller	# offset 0
 *
 * The instruction will be patched by ftrace_make_call / ftrace_make_nop.
 * The ftrace function gets called with a non-standard C function call ABI
 * where r0 contains the return address. It is also expected that the called
 * function only clobbers r0 and r1, but restores r2-r15.
 * For module code we can't directly jump to ftrace caller, but need a
 * trampoline (ftrace_plt), which clobbers also r1.
 */

void *ftrace_func __read_mostly = ftrace_stub;
struct ftrace_insn {
	u16 opc;
	s32 disp;
} __packed;

#ifdef CONFIG_MODULES
static char *ftrace_plt;
#endif /* CONFIG_MODULES */

static const char *ftrace_shared_hotpatch_trampoline(const char **end)
{
	const char *tstart, *tend;

	tstart = ftrace_shared_hotpatch_trampoline_br;
	tend = ftrace_shared_hotpatch_trampoline_br_end;
#ifdef CONFIG_EXPOLINE
	if (!nospec_disable) {
		tstart = ftrace_shared_hotpatch_trampoline_exrl;
		tend = ftrace_shared_hotpatch_trampoline_exrl_end;
	}
#endif /* CONFIG_EXPOLINE */
	if (end)
		*end = tend;
	return tstart;
}

bool ftrace_need_init_nop(void)
{
	return true;
}

int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec)
{
	static struct ftrace_hotpatch_trampoline *next_vmlinux_trampoline =
		__ftrace_hotpatch_trampolines_start;
	static const char orig[6] = { 0xc0, 0x04, 0x00, 0x00, 0x00, 0x00 };
	static struct ftrace_hotpatch_trampoline *trampoline;
	struct ftrace_hotpatch_trampoline **next_trampoline;
	struct ftrace_hotpatch_trampoline *trampolines_end;
	struct ftrace_hotpatch_trampoline tmp;
	struct ftrace_insn *insn;
	const char *shared;
	s32 disp;

	BUILD_BUG_ON(sizeof(struct ftrace_hotpatch_trampoline) !=
		     SIZEOF_FTRACE_HOTPATCH_TRAMPOLINE);

	next_trampoline = &next_vmlinux_trampoline;
	trampolines_end = __ftrace_hotpatch_trampolines_end;
	shared = ftrace_shared_hotpatch_trampoline(NULL);
#ifdef CONFIG_MODULES
	if (mod) {
		next_trampoline = &mod->arch.next_trampoline;
		trampolines_end = mod->arch.trampolines_end;
		shared = ftrace_plt;
	}
#endif

	if (WARN_ON_ONCE(*next_trampoline >= trampolines_end))
		return -ENOMEM;
	trampoline = (*next_trampoline)++;

	/* Check for the compiler-generated fentry nop (brcl 0, .). */
	if (WARN_ON_ONCE(memcmp((const void *)rec->ip, &orig, sizeof(orig))))
		return -EINVAL;

	/* Generate the trampoline. */
	tmp.brasl_opc = 0xc015; /* brasl %r1, shared */
	tmp.brasl_disp = (shared - (const char *)&trampoline->brasl_opc) / 2;
	tmp.interceptor = FTRACE_ADDR;
	tmp.rest_of_intercepted_function = rec->ip + sizeof(struct ftrace_insn);
	s390_kernel_write(trampoline, &tmp, sizeof(tmp));

	/* Generate a jump to the trampoline. */
	disp = ((char *)trampoline - (char *)rec->ip) / 2;
	insn = (struct ftrace_insn *)rec->ip;
	s390_kernel_write(&insn->disp, &disp, sizeof(disp));

	return 0;
}

static struct ftrace_hotpatch_trampoline *ftrace_get_trampoline(struct dyn_ftrace *rec)
{
	struct ftrace_hotpatch_trampoline *trampoline;
	struct ftrace_insn insn;
	s64 disp;
	u16 opc;

	if (copy_from_kernel_nofault(&insn, (void *)rec->ip, sizeof(insn)))
		return ERR_PTR(-EFAULT);
	disp = (s64)insn.disp * 2;
	trampoline = (void *)(rec->ip + disp);
	if (get_kernel_nofault(opc, &trampoline->brasl_opc))
		return ERR_PTR(-EFAULT);
	if (opc != 0xc015)
		return ERR_PTR(-EINVAL);
	return trampoline;
}

int ftrace_modify_call(struct dyn_ftrace *rec, unsigned long old_addr,
		       unsigned long addr)
{
	struct ftrace_hotpatch_trampoline *trampoline;
	u64 old;

	trampoline = ftrace_get_trampoline(rec);
	if (IS_ERR(trampoline))
		return PTR_ERR(trampoline);
	if (get_kernel_nofault(old, &trampoline->interceptor))
		return -EFAULT;
	if (old != old_addr)
		return -EINVAL;
	s390_kernel_write(&trampoline->interceptor, &addr, sizeof(addr));
	return 0;
}

static int ftrace_patch_branch_mask(void *addr, u16 expected, bool enable)
{
	u16 old;
	u8 op;

	if (get_kernel_nofault(old, addr))
		return -EFAULT;
	if (old != expected)
		return -EINVAL;
	/* set mask field to all ones or zeroes */
	op = enable ? 0xf4 : 0x04;
	s390_kernel_write((char *)addr + 1, &op, sizeof(op));
	return 0;
}

int ftrace_make_nop(struct module *mod, struct dyn_ftrace *rec,
		    unsigned long addr)
{
	/* Expect brcl 0xf,... */
	return ftrace_patch_branch_mask((void *)rec->ip, 0xc0f4, false);
}

int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	struct ftrace_hotpatch_trampoline *trampoline;

	trampoline = ftrace_get_trampoline(rec);
	if (IS_ERR(trampoline))
		return PTR_ERR(trampoline);
	s390_kernel_write(&trampoline->interceptor, &addr, sizeof(addr));
	/* Expect brcl 0x0,... */
	return ftrace_patch_branch_mask((void *)rec->ip, 0xc004, true);
}

int ftrace_update_ftrace_func(ftrace_func_t func)
{
	ftrace_func = func;
	return 0;
}

void arch_ftrace_update_code(int command)
{
	ftrace_modify_all_code(command);
}

void ftrace_arch_code_modify_post_process(void)
{
	/*
	 * Flush any pre-fetched instructions on all
	 * CPUs to make the new code visible.
	 */
	text_poke_sync_lock();
}

#ifdef CONFIG_MODULES

static int __init ftrace_plt_init(void)
{
	const char *start, *end;

	ftrace_plt = module_alloc(PAGE_SIZE);
	if (!ftrace_plt)
		panic("cannot allocate ftrace plt\n");

	start = ftrace_shared_hotpatch_trampoline(&end);
	memcpy(ftrace_plt, start, end - start);
	set_memory_rox((unsigned long)ftrace_plt, 1);
	return 0;
}
device_initcall(ftrace_plt_init);

#endif /* CONFIG_MODULES */

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
/*
 * Hook the return address and push it in the stack of return addresses
 * in current thread info.
 */
unsigned long prepare_ftrace_return(unsigned long ra, unsigned long sp,
				    unsigned long ip)
{
	if (unlikely(ftrace_graph_is_dead()))
		goto out;
	if (unlikely(atomic_read(&current->tracing_graph_pause)))
		goto out;
	ip -= MCOUNT_INSN_SIZE;
	if (!function_graph_enter(ra, ip, 0, (void *) sp))
		ra = (unsigned long) return_to_handler;
out:
	return ra;
}
NOKPROBE_SYMBOL(prepare_ftrace_return);

/*
 * Patch the kernel code at ftrace_graph_caller location. The instruction
 * there is branch relative on condition. To enable the ftrace graph code
 * block, we simply patch the mask field of the instruction to zero and
 * turn the instruction into a nop.
 * To disable the ftrace graph code the mask field will be patched to
 * all ones, which turns the instruction into an unconditional branch.
 */
int ftrace_enable_ftrace_graph_caller(void)
{
	int rc;

	/* Expect brc 0xf,... */
	rc = ftrace_patch_branch_mask(ftrace_graph_caller, 0xa7f4, false);
	if (rc)
		return rc;
	text_poke_sync_lock();
	return 0;
}

int ftrace_disable_ftrace_graph_caller(void)
{
	int rc;

	/* Expect brc 0x0,... */
	rc = ftrace_patch_branch_mask(ftrace_graph_caller, 0xa704, true);
	if (rc)
		return rc;
	text_poke_sync_lock();
	return 0;
}

#endif /* CONFIG_FUNCTION_GRAPH_TRACER */

#ifdef CONFIG_KPROBES_ON_FTRACE
void kprobe_ftrace_handler(unsigned long ip, unsigned long parent_ip,
		struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct kprobe_ctlblk *kcb;
	struct pt_regs *regs;
	struct kprobe *p;
	int bit;

	bit = ftrace_test_recursion_trylock(ip, parent_ip);
	if (bit < 0)
		return;

	regs = ftrace_get_regs(fregs);
	p = get_kprobe((kprobe_opcode_t *)ip);
	if (!regs || unlikely(!p) || kprobe_disabled(p))
		goto out;

	if (kprobe_running()) {
		kprobes_inc_nmissed_count(p);
		goto out;
	}

	__this_cpu_write(current_kprobe, p);

	kcb = get_kprobe_ctlblk();
	kcb->kprobe_status = KPROBE_HIT_ACTIVE;

	instruction_pointer_set(regs, ip);

	if (!p->pre_handler || !p->pre_handler(p, regs)) {

		instruction_pointer_set(regs, ip + MCOUNT_INSN_SIZE);

		if (unlikely(p->post_handler)) {
			kcb->kprobe_status = KPROBE_HIT_SSDONE;
			p->post_handler(p, regs, 0);
		}
	}
	__this_cpu_write(current_kprobe, NULL);
out:
	ftrace_test_recursion_unlock(bit);
}
NOKPROBE_SYMBOL(kprobe_ftrace_handler);

int arch_prepare_kprobe_ftrace(struct kprobe *p)
{
	p->ainsn.insn = NULL;
	return 0;
}
#endif
