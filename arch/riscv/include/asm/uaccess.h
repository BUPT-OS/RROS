/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 *
 * This file was copied from include/asm-generic/uaccess.h
 */

#ifndef _ASM_RISCV_UACCESS_H
#define _ASM_RISCV_UACCESS_H

#include <asm/asm-extable.h>
#include <asm/pgtable.h>		/* for TASK_SIZE */

/*
 * User space memory access functions
 */
#ifdef CONFIG_MMU
#include <linux/errno.h>
#include <linux/compiler.h>
#include <linux/thread_info.h>
#include <asm/byteorder.h>
#include <asm/extable.h>
#include <asm/asm.h>
#include <asm-generic/access_ok.h>

#define __enable_user_access()							\
	__asm__ __volatile__ ("csrs sstatus, %0" : : "r" (SR_SUM) : "memory")
#define __disable_user_access()							\
	__asm__ __volatile__ ("csrc sstatus, %0" : : "r" (SR_SUM) : "memory")

/*
 * The exception table consists of pairs of addresses: the first is the
 * address of an instruction that is allowed to fault, and the second is
 * the address at which the program should continue.  No registers are
 * modified, so it is entirely up to the continuation code to figure out
 * what to do.
 *
 * All the routines below use bits of fixup code that are out of line
 * with the main instruction path.  This means when everything is well,
 * we don't even have to jump over them.  Further, they do not intrude
 * on our cache or tlb entries.
 */

#define __LSW	0
#define __MSW	1

/*
 * The "__xxx" versions of the user access functions do not verify the address
 * space - it must have been done previously with a separate "access_ok()"
 * call.
 */

#define __get_user_asm(insn, x, ptr, err)			\
do {								\
	__typeof__(x) __x;					\
	__asm__ __volatile__ (					\
		"1:\n"						\
		"	" insn " %1, %2\n"			\
		"2:\n"						\
		_ASM_EXTABLE_UACCESS_ERR_ZERO(1b, 2b, %0, %1)	\
		: "+r" (err), "=&r" (__x)			\
		: "m" (*(ptr)));				\
	(x) = __x;						\
} while (0)

#ifdef CONFIG_64BIT
#define __get_user_8(x, ptr, err) \
	__get_user_asm("ld", x, ptr, err)
#else /* !CONFIG_64BIT */
#define __get_user_8(x, ptr, err)				\
do {								\
	u32 __user *__ptr = (u32 __user *)(ptr);		\
	u32 __lo, __hi;						\
	__asm__ __volatile__ (					\
		"1:\n"						\
		"	lw %1, %3\n"				\
		"2:\n"						\
		"	lw %2, %4\n"				\
		"3:\n"						\
		_ASM_EXTABLE_UACCESS_ERR_ZERO(1b, 3b, %0, %1)	\
		_ASM_EXTABLE_UACCESS_ERR_ZERO(2b, 3b, %0, %1)	\
		: "+r" (err), "=&r" (__lo), "=r" (__hi)		\
		: "m" (__ptr[__LSW]), "m" (__ptr[__MSW]));	\
	if (err)						\
		__hi = 0;					\
	(x) = (__typeof__(x))((__typeof__((x)-(x)))(		\
		(((u64)__hi << 32) | __lo)));			\
} while (0)
#endif /* CONFIG_64BIT */

#define __get_user_nocheck(x, __gu_ptr, __gu_err)		\
do {								\
	switch (sizeof(*__gu_ptr)) {				\
	case 1:							\
		__get_user_asm("lb", (x), __gu_ptr, __gu_err);	\
		break;						\
	case 2:							\
		__get_user_asm("lh", (x), __gu_ptr, __gu_err);	\
		break;						\
	case 4:							\
		__get_user_asm("lw", (x), __gu_ptr, __gu_err);	\
		break;						\
	case 8:							\
		__get_user_8((x), __gu_ptr, __gu_err);	\
		break;						\
	default:						\
		BUILD_BUG();					\
	}							\
} while (0)

/**
 * __get_user: - Get a simple variable from user space, with less checking.
 * @x:   Variable to store result.
 * @ptr: Source address, in user space.
 *
 * Context: User context only.  This function may sleep.
 *
 * This macro copies a single simple variable from user space to kernel
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and the result of
 * dereferencing @ptr must be assignable to @x without a cast.
 *
 * Caller must check the pointer with access_ok() before calling this
 * function.
 *
 * Returns zero on success, or -EFAULT on error.
 * On error, the variable @x is set to zero.
 */
#define __get_user(x, ptr)					\
({								\
	const __typeof__(*(ptr)) __user *__gu_ptr = (ptr);	\
	long __gu_err = 0;					\
								\
	__chk_user_ptr(__gu_ptr);				\
								\
	__enable_user_access();					\
	__get_user_nocheck(x, __gu_ptr, __gu_err);		\
	__disable_user_access();				\
								\
	__gu_err;						\
})

/**
 * get_user: - Get a simple variable from user space.
 * @x:   Variable to store result.
 * @ptr: Source address, in user space.
 *
 * Context: User context only.  This function may sleep.
 *
 * This macro copies a single simple variable from user space to kernel
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and the result of
 * dereferencing @ptr must be assignable to @x without a cast.
 *
 * Returns zero on success, or -EFAULT on error.
 * On error, the variable @x is set to zero.
 */
#define get_user(x, ptr)					\
({								\
	const __typeof__(*(ptr)) __user *__p = (ptr);		\
	might_fault();						\
	access_ok(__p, sizeof(*__p)) ?		\
		__get_user((x), __p) :				\
		((x) = (__force __typeof__(x))0, -EFAULT);	\
})

#define __put_user_asm(insn, x, ptr, err)			\
do {								\
	__typeof__(*(ptr)) __x = x;				\
	__asm__ __volatile__ (					\
		"1:\n"						\
		"	" insn " %z2, %1\n"			\
		"2:\n"						\
		_ASM_EXTABLE_UACCESS_ERR(1b, 2b, %0)		\
		: "+r" (err), "=m" (*(ptr))			\
		: "rJ" (__x));					\
} while (0)

#ifdef CONFIG_64BIT
#define __put_user_8(x, ptr, err) \
	__put_user_asm("sd", x, ptr, err)
#else /* !CONFIG_64BIT */
#define __put_user_8(x, ptr, err)				\
do {								\
	u32 __user *__ptr = (u32 __user *)(ptr);		\
	u64 __x = (__typeof__((x)-(x)))(x);			\
	__asm__ __volatile__ (					\
		"1:\n"						\
		"	sw %z3, %1\n"				\
		"2:\n"						\
		"	sw %z4, %2\n"				\
		"3:\n"						\
		_ASM_EXTABLE_UACCESS_ERR(1b, 3b, %0)		\
		_ASM_EXTABLE_UACCESS_ERR(2b, 3b, %0)		\
		: "+r" (err),					\
			"=m" (__ptr[__LSW]),			\
			"=m" (__ptr[__MSW])			\
		: "rJ" (__x), "rJ" (__x >> 32));		\
} while (0)
#endif /* CONFIG_64BIT */

#define __put_user_nocheck(x, __gu_ptr, __pu_err)					\
do {								\
	switch (sizeof(*__gu_ptr)) {				\
	case 1:							\
		__put_user_asm("sb", (x), __gu_ptr, __pu_err);	\
		break;						\
	case 2:							\
		__put_user_asm("sh", (x), __gu_ptr, __pu_err);	\
		break;						\
	case 4:							\
		__put_user_asm("sw", (x), __gu_ptr, __pu_err);	\
		break;						\
	case 8:							\
		__put_user_8((x), __gu_ptr, __pu_err);	\
		break;						\
	default:						\
		BUILD_BUG();					\
	}							\
} while (0)

/**
 * __put_user: - Write a simple value into user space, with less checking.
 * @x:   Value to copy to user space.
 * @ptr: Destination address, in user space.
 *
 * Context: User context only.  This function may sleep.
 *
 * This macro copies a single simple value from kernel space to user
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and @x must be assignable
 * to the result of dereferencing @ptr. The value of @x is copied to avoid
 * re-ordering where @x is evaluated inside the block that enables user-space
 * access (thus bypassing user space protection if @x is a function).
 *
 * Caller must check the pointer with access_ok() before calling this
 * function.
 *
 * Returns zero on success, or -EFAULT on error.
 */
#define __put_user(x, ptr)					\
({								\
	__typeof__(*(ptr)) __user *__gu_ptr = (ptr);		\
	__typeof__(*__gu_ptr) __val = (x);			\
	long __pu_err = 0;					\
								\
	__chk_user_ptr(__gu_ptr);				\
								\
	__enable_user_access();					\
	__put_user_nocheck(__val, __gu_ptr, __pu_err);		\
	__disable_user_access();				\
								\
	__pu_err;						\
})

/**
 * put_user: - Write a simple value into user space.
 * @x:   Value to copy to user space.
 * @ptr: Destination address, in user space.
 *
 * Context: User context only.  This function may sleep.
 *
 * This macro copies a single simple value from kernel space to user
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and @x must be assignable
 * to the result of dereferencing @ptr.
 *
 * Returns zero on success, or -EFAULT on error.
 */
#define put_user(x, ptr)					\
({								\
	__typeof__(*(ptr)) __user *__p = (ptr);			\
	might_fault();						\
	access_ok(__p, sizeof(*__p)) ?		\
		__put_user((x), __p) :				\
		-EFAULT;					\
})


unsigned long __must_check __asm_copy_to_user(void __user *to,
	const void *from, unsigned long n);
unsigned long __must_check __asm_copy_from_user(void *to,
	const void __user *from, unsigned long n);

static inline unsigned long
raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	return __asm_copy_from_user(to, from, n);
}

static inline unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	return __asm_copy_to_user(to, from, n);
}

extern long strncpy_from_user(char *dest, const char __user *src, long count);

extern long __must_check strnlen_user(const char __user *str, long n);

extern
unsigned long __must_check __clear_user(void __user *addr, unsigned long n);

static inline
unsigned long __must_check clear_user(void __user *to, unsigned long n)
{
	might_fault();
	return access_ok(to, n) ?
		__clear_user(to, n) : n;
}

#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	long __kr_err;							\
									\
	__get_user_nocheck(*((type *)(dst)), (type *)(src), __kr_err);	\
	if (unlikely(__kr_err))						\
		goto err_label;						\
} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)			\
do {									\
	long __kr_err;							\
									\
	__put_user_nocheck(*((type *)(src)), (type *)(dst), __kr_err);	\
	if (unlikely(__kr_err))						\
		goto err_label;						\
} while (0)

#else /* CONFIG_MMU */
#include <asm-generic/uaccess.h>
#endif /* CONFIG_MMU */
#endif /* _ASM_RISCV_UACCESS_H */
