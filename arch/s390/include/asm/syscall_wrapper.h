/* SPDX-License-Identifier: GPL-2.0 */
/*
 * syscall_wrapper.h - s390 specific wrappers to syscall definitions
 *
 */

#ifndef _ASM_S390_SYSCALL_WRAPPER_H
#define _ASM_S390_SYSCALL_WRAPPER_H

/* Mapping of registers to parameters for syscalls */
#define SC_S390_REGS_TO_ARGS(x, ...)					\
	__MAP(x, __SC_ARGS						\
	      ,, regs->orig_gpr2,, regs->gprs[3],, regs->gprs[4]	\
	      ,, regs->gprs[5],, regs->gprs[6],, regs->gprs[7])

#ifdef CONFIG_COMPAT

#define __SC_COMPAT_CAST(t, a)						\
({									\
	long __ReS = a;							\
									\
	BUILD_BUG_ON((sizeof(t) > 4) && !__TYPE_IS_L(t) &&		\
		     !__TYPE_IS_UL(t) && !__TYPE_IS_PTR(t) &&		\
		     !__TYPE_IS_LL(t));					\
	if (__TYPE_IS_L(t))						\
		__ReS = (s32)a;						\
	if (__TYPE_IS_UL(t))						\
		__ReS = (u32)a;						\
	if (__TYPE_IS_PTR(t))						\
		__ReS = a & 0x7fffffff;					\
	if (__TYPE_IS_LL(t))						\
		return -ENOSYS;						\
	(t)__ReS;							\
})

/*
 * To keep the naming coherent, re-define SYSCALL_DEFINE0 to create an alias
 * named __s390x_sys_*()
 */
#define COMPAT_SYSCALL_DEFINE0(sname)					\
	long __s390_compat_sys_##sname(void);				\
	ALLOW_ERROR_INJECTION(__s390_compat_sys_##sname, ERRNO);	\
	long __s390_compat_sys_##sname(void)

#define SYSCALL_DEFINE0(sname)						\
	SYSCALL_METADATA(_##sname, 0);					\
	long __s390_sys_##sname(void);					\
	ALLOW_ERROR_INJECTION(__s390_sys_##sname, ERRNO);		\
	long __s390x_sys_##sname(void);					\
	ALLOW_ERROR_INJECTION(__s390x_sys_##sname, ERRNO);		\
	static inline long __do_sys_##sname(void);			\
	long __s390_sys_##sname(void)					\
	{								\
		return __do_sys_##sname();				\
	}								\
	long __s390x_sys_##sname(void)					\
	{								\
		return __do_sys_##sname();				\
	}								\
	static inline long __do_sys_##sname(void)

#define COND_SYSCALL(name)						\
	cond_syscall(__s390x_sys_##name);				\
	cond_syscall(__s390_sys_##name)

#define SYS_NI(name)							\
	SYSCALL_ALIAS(__s390x_sys_##name, sys_ni_posix_timers);		\
	SYSCALL_ALIAS(__s390_sys_##name, sys_ni_posix_timers)

#define COMPAT_SYSCALL_DEFINEx(x, name, ...)						\
	long __s390_compat_sys##name(struct pt_regs *regs);				\
	ALLOW_ERROR_INJECTION(__s390_compat_sys##name, ERRNO);				\
	static inline long __se_compat_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__));	\
	static inline long __do_compat_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__));	\
	long __s390_compat_sys##name(struct pt_regs *regs)				\
	{										\
		return __se_compat_sys##name(SC_S390_REGS_TO_ARGS(x, __VA_ARGS__));	\
	}										\
	static inline long __se_compat_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__))	\
	{										\
		__MAP(x, __SC_TEST, __VA_ARGS__);					\
		return __do_compat_sys##name(__MAP(x, __SC_DELOUSE, __VA_ARGS__));	\
	}										\
	static inline long __do_compat_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__))

/*
 * As some compat syscalls may not be implemented, we need to expand
 * COND_SYSCALL_COMPAT in kernel/sys_ni.c and COMPAT_SYS_NI in
 * kernel/time/posix-stubs.c to cover this case as well.
 */
#define COND_SYSCALL_COMPAT(name)					\
	cond_syscall(__s390_compat_sys_##name)

#define COMPAT_SYS_NI(name)						\
	SYSCALL_ALIAS(__s390_compat_sys_##name, sys_ni_posix_timers)

#define __S390_SYS_STUBx(x, name, ...)						\
	long __s390_sys##name(struct pt_regs *regs);				\
	ALLOW_ERROR_INJECTION(__s390_sys##name, ERRNO);				\
	static inline long ___se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__));	\
	long __s390_sys##name(struct pt_regs *regs)				\
	{									\
		return ___se_sys##name(SC_S390_REGS_TO_ARGS(x, __VA_ARGS__));	\
	}									\
	static inline long ___se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__))	\
	{									\
		__MAP(x, __SC_TEST, __VA_ARGS__);				\
		return __do_sys##name(__MAP(x, __SC_COMPAT_CAST, __VA_ARGS__));	\
	}

#else /* CONFIG_COMPAT */

#define SYSCALL_DEFINE0(sname)						\
	SYSCALL_METADATA(_##sname, 0);					\
	long __s390x_sys_##sname(void);					\
	ALLOW_ERROR_INJECTION(__s390x_sys_##sname, ERRNO);		\
	static inline long __do_sys_##sname(void);			\
	long __s390x_sys_##sname(void)					\
	{								\
		return __do_sys_##sname();				\
	}								\
	static inline long __do_sys_##sname(void)

#define COND_SYSCALL(name)						\
	cond_syscall(__s390x_sys_##name)

#define SYS_NI(name)							\
	SYSCALL_ALIAS(__s390x_sys_##name, sys_ni_posix_timers)

#define __S390_SYS_STUBx(x, fullname, name, ...)

#endif /* CONFIG_COMPAT */

#define __SYSCALL_DEFINEx(x, name, ...)						\
	long __s390x_sys##name(struct pt_regs *regs);				\
	ALLOW_ERROR_INJECTION(__s390x_sys##name, ERRNO);			\
	static inline long __se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__));	\
	static inline long __do_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__));	\
	__S390_SYS_STUBx(x, name, __VA_ARGS__);					\
	long __s390x_sys##name(struct pt_regs *regs)				\
	{									\
		return __se_sys##name(SC_S390_REGS_TO_ARGS(x, __VA_ARGS__));	\
	}									\
	static inline long __se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__))	\
	{									\
		__MAP(x, __SC_TEST, __VA_ARGS__);				\
		return __do_sys##name(__MAP(x, __SC_CAST, __VA_ARGS__));	\
	}									\
	static inline long __do_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__))

#endif /* _ASM_S390_SYSCALL_WRAPPER_H */
