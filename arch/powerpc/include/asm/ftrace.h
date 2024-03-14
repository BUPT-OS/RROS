/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_FTRACE
#define _ASM_POWERPC_FTRACE

#include <asm/types.h>

#ifdef CONFIG_FUNCTION_TRACER
#define MCOUNT_ADDR		((unsigned long)(_mcount))
#define MCOUNT_INSN_SIZE	4 /* sizeof mcount call */

#define HAVE_FUNCTION_GRAPH_RET_ADDR_PTR

/* Ignore unused weak functions which will have larger offsets */
#if defined(CONFIG_MPROFILE_KERNEL) || defined(CONFIG_ARCH_USING_PATCHABLE_FUNCTION_ENTRY)
#define FTRACE_MCOUNT_MAX_OFFSET	16
#elif defined(CONFIG_PPC32)
#define FTRACE_MCOUNT_MAX_OFFSET	8
#endif

#ifndef __ASSEMBLY__
extern void _mcount(void);

static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	if (IS_ENABLED(CONFIG_ARCH_USING_PATCHABLE_FUNCTION_ENTRY))
		addr += MCOUNT_INSN_SIZE;

       return addr;
}

unsigned long prepare_ftrace_return(unsigned long parent, unsigned long ip,
				    unsigned long sp);

struct module;
struct dyn_ftrace;
struct dyn_arch_ftrace {
	struct module *mod;
};

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
#define ftrace_need_init_nop()	(true)
int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec);
#define ftrace_init_nop ftrace_init_nop

struct ftrace_regs {
	struct pt_regs regs;
};

static __always_inline struct pt_regs *arch_ftrace_get_regs(struct ftrace_regs *fregs)
{
	/* We clear regs.msr in ftrace_call */
	return fregs->regs.msr ? &fregs->regs : NULL;
}

static __always_inline void
ftrace_regs_set_instruction_pointer(struct ftrace_regs *fregs,
				    unsigned long ip)
{
	regs_set_return_ip(&fregs->regs, ip);
}

static __always_inline unsigned long
ftrace_regs_get_instruction_pointer(struct ftrace_regs *fregs)
{
	return instruction_pointer(&fregs->regs);
}

#define ftrace_regs_get_argument(fregs, n) \
	regs_get_kernel_argument(&(fregs)->regs, n)
#define ftrace_regs_get_stack_pointer(fregs) \
	kernel_stack_pointer(&(fregs)->regs)
#define ftrace_regs_return_value(fregs) \
	regs_return_value(&(fregs)->regs)
#define ftrace_regs_set_return_value(fregs, ret) \
	regs_set_return_value(&(fregs)->regs, ret)
#define ftrace_override_function_with_return(fregs) \
	override_function_with_return(&(fregs)->regs)
#define ftrace_regs_query_register_offset(name) \
	regs_query_register_offset(name)

struct ftrace_ops;

#define ftrace_graph_func ftrace_graph_func
void ftrace_graph_func(unsigned long ip, unsigned long parent_ip,
		       struct ftrace_ops *op, struct ftrace_regs *fregs);
#endif
#endif /* __ASSEMBLY__ */

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_REGS
#define ARCH_SUPPORTS_FTRACE_OPS 1
#endif
#endif /* CONFIG_FUNCTION_TRACER */

#ifndef __ASSEMBLY__
#ifdef CONFIG_FTRACE_SYSCALLS
/*
 * Some syscall entry functions on powerpc start with "ppc_" (fork and clone,
 * for instance) or ppc32_/ppc64_. We should also match the sys_ variant with
 * those.
 */
#define ARCH_HAS_SYSCALL_MATCH_SYM_NAME
static inline bool arch_syscall_match_sym_name(const char *sym, const char *name)
{
	return !strcmp(sym, name) ||
		(!strncmp(sym, "__se_sys", 8) && !strcmp(sym + 5, name)) ||
		(!strncmp(sym, "ppc_", 4) && !strcmp(sym + 4, name + 4)) ||
		(!strncmp(sym, "ppc32_", 6) && !strcmp(sym + 6, name + 4)) ||
		(!strncmp(sym, "ppc64_", 6) && !strcmp(sym + 6, name + 4));
}
#endif /* CONFIG_FTRACE_SYSCALLS */

#if defined(CONFIG_PPC64) && defined(CONFIG_FUNCTION_TRACER)
#include <asm/paca.h>

static inline void this_cpu_disable_ftrace(void)
{
	get_paca()->ftrace_enabled = 0;
}

static inline void this_cpu_enable_ftrace(void)
{
	get_paca()->ftrace_enabled = 1;
}

/* Disable ftrace on this CPU if possible (may not be implemented) */
static inline void this_cpu_set_ftrace_enabled(u8 ftrace_enabled)
{
	get_paca()->ftrace_enabled = ftrace_enabled;
}

static inline u8 this_cpu_get_ftrace_enabled(void)
{
	return get_paca()->ftrace_enabled;
}
#else /* CONFIG_PPC64 */
static inline void this_cpu_disable_ftrace(void) { }
static inline void this_cpu_enable_ftrace(void) { }
static inline void this_cpu_set_ftrace_enabled(u8 ftrace_enabled) { }
static inline u8 this_cpu_get_ftrace_enabled(void) { return 1; }
#endif /* CONFIG_PPC64 */

#ifdef CONFIG_FUNCTION_TRACER
extern unsigned int ftrace_tramp_text[], ftrace_tramp_init[];
void ftrace_free_init_tramp(void);
#else
static inline void ftrace_free_init_tramp(void) { }
#endif
#endif /* !__ASSEMBLY__ */

#endif /* _ASM_POWERPC_FTRACE */
