/*
 * User space memory access functions for Nios II
 *
 * Copyright (C) 2010-2011, Tobias Klauser <tklauser@distanz.ch>
 * Copyright (C) 2009, Wind River Systems Inc
 *   Implemented by fredrik.markstrom@gmail.com and ivarholmqvist@gmail.com
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#ifndef _ASM_NIOS2_UACCESS_H
#define _ASM_NIOS2_UACCESS_H

#include <linux/string.h>

#include <asm/page.h>

#include <asm/extable.h>
#include <asm-generic/access_ok.h>

# define __EX_TABLE_SECTION	".section __ex_table,\"a\"\n"

/*
 * Zero Userspace
 */

static inline unsigned long __must_check __clear_user(void __user *to,
						      unsigned long n)
{
	__asm__ __volatile__ (
		"1:     stb     zero, 0(%1)\n"
		"       addi    %0, %0, -1\n"
		"       addi    %1, %1, 1\n"
		"       bne     %0, zero, 1b\n"
		"2:\n"
		__EX_TABLE_SECTION
		".word  1b, 2b\n"
		".previous\n"
		: "=r" (n), "=r" (to)
		: "0" (n), "1" (to)
	);

	return n;
}

static inline unsigned long __must_check clear_user(void __user *to,
						    unsigned long n)
{
	if (!access_ok(to, n))
		return n;
	return __clear_user(to, n);
}

extern unsigned long
raw_copy_from_user(void *to, const void __user *from, unsigned long n);
extern unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n);
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

extern long strncpy_from_user(char *__to, const char __user *__from,
			      long __len);
extern __must_check long strnlen_user(const char __user *s, long n);

/* Optimized macros */
#define __get_user_asm(val, insn, addr, err)				\
{									\
	unsigned long __gu_val;						\
	__asm__ __volatile__(						\
	"       movi    %0, %3\n"					\
	"1:   " insn " %1, 0(%2)\n"					\
	"       movi     %0, 0\n"					\
	"2:\n"								\
	"       .section __ex_table,\"a\"\n"				\
	"       .word 1b, 2b\n"						\
	"       .previous"						\
	: "=&r" (err), "=r" (__gu_val)					\
	: "r" (addr), "i" (-EFAULT));					\
	val = (__force __typeof__(*(addr)))__gu_val;			\
}

extern void __get_user_unknown(void);

#define __get_user_8(val, ptr, err) do {				\
	u64 __val = 0;							\
	err = 0;							\
	if (raw_copy_from_user(&(__val), ptr, sizeof(val))) {		\
		err = -EFAULT;						\
	} else {							\
		val = (typeof(val))(typeof((val) - (val)))__val;	\
	}								\
	} while (0)

#define __get_user_common(val, size, ptr, err)				\
do {									\
	switch (size) {							\
	case 1:								\
		__get_user_asm(val, "ldbu", ptr, err);			\
		break;							\
	case 2:								\
		__get_user_asm(val, "ldhu", ptr, err);			\
		break;							\
	case 4:								\
		__get_user_asm(val, "ldw", ptr, err);			\
		break;							\
	case 8:								\
		__get_user_8(val, ptr, err);				\
		break;							\
	default:							\
		__get_user_unknown();					\
		break;							\
	}								\
} while (0)

#define __get_user(x, ptr)						\
	({								\
	long __gu_err = -EFAULT;					\
	const __typeof__(*(ptr)) __user *__gu_ptr = (ptr);		\
	__get_user_common(x, sizeof(*(ptr)), __gu_ptr, __gu_err);	\
	__gu_err;							\
	})

#define get_user(x, ptr)						\
({									\
	long __gu_err = -EFAULT;					\
	const __typeof__(*(ptr)) __user *__gu_ptr = (ptr);		\
	if (access_ok( __gu_ptr, sizeof(*__gu_ptr)))	\
		__get_user_common(x, sizeof(*__gu_ptr),			\
			__gu_ptr, __gu_err);				\
	__gu_err;							\
})

#define __put_user_asm(val, insn, ptr, err)				\
{									\
	__asm__ __volatile__(						\
	"       movi    %0, %3\n"					\
	"1:   " insn " %1, 0(%2)\n"					\
	"       movi     %0, 0\n"					\
	"2:\n"								\
	"       .section __ex_table,\"a\"\n"				\
	"       .word 1b, 2b\n"						\
	"       .previous\n"						\
	: "=&r" (err)							\
	: "r" (val), "r" (ptr), "i" (-EFAULT));				\
}

#define __put_user_common(__pu_val, __pu_ptr)				\
({									\
	long __pu_err = -EFAULT;					\
	switch (sizeof(*__pu_ptr)) {					\
	case 1:								\
		__put_user_asm(__pu_val, "stb", __pu_ptr, __pu_err);	\
		break;							\
	case 2:								\
		__put_user_asm(__pu_val, "sth", __pu_ptr, __pu_err);	\
		break;							\
	case 4:								\
		__put_user_asm(__pu_val, "stw", __pu_ptr, __pu_err);	\
		break;							\
	default:							\
		/* XXX: This looks wrong... */				\
		__pu_err = 0;						\
		if (__copy_to_user(__pu_ptr, &(__pu_val),		\
			sizeof(*__pu_ptr)))				\
			__pu_err = -EFAULT;				\
		break;							\
	}								\
	__pu_err;							\
})

#define __put_user(x, ptr)						\
({									\
	__auto_type __pu_ptr = (ptr);					\
	typeof(*__pu_ptr) __pu_val = (typeof(*__pu_ptr))(x);		\
	__put_user_common(__pu_val, __pu_ptr);				\
})

#define put_user(x, ptr)						\
({									\
	__auto_type __pu_ptr = (ptr);					\
	typeof(*__pu_ptr) __pu_val = (typeof(*__pu_ptr))(x);		\
	access_ok(__pu_ptr, sizeof(*__pu_ptr)) ?			\
		__put_user_common(__pu_val, __pu_ptr) :			\
		-EFAULT;						\
})

#endif /* _ASM_NIOS2_UACCESS_H */
