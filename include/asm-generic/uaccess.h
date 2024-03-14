/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_UACCESS_H
#define __ASM_GENERIC_UACCESS_H

/*
 * User space memory access functions, these should work
 * on any machine that has kernel and user data in the same
 * address space, e.g. all NOMMU machines.
 */
#include <linux/string.h>
#include <asm-generic/access_ok.h>

#ifdef CONFIG_UACCESS_MEMCPY
#include <asm/unaligned.h>

static __always_inline int
__get_user_fn(size_t size, const void __user *from, void *to)
{
	BUILD_BUG_ON(!__builtin_constant_p(size));

	switch (size) {
	case 1:
		*(u8 *)to = *((u8 __force *)from);
		return 0;
	case 2:
		*(u16 *)to = get_unaligned((u16 __force *)from);
		return 0;
	case 4:
		*(u32 *)to = get_unaligned((u32 __force *)from);
		return 0;
	case 8:
		*(u64 *)to = get_unaligned((u64 __force *)from);
		return 0;
	default:
		BUILD_BUG();
		return 0;
	}

}
#define __get_user_fn(sz, u, k)	__get_user_fn(sz, u, k)

static __always_inline int
__put_user_fn(size_t size, void __user *to, void *from)
{
	BUILD_BUG_ON(!__builtin_constant_p(size));

	switch (size) {
	case 1:
		*(u8 __force *)to = *(u8 *)from;
		return 0;
	case 2:
		put_unaligned(*(u16 *)from, (u16 __force *)to);
		return 0;
	case 4:
		put_unaligned(*(u32 *)from, (u32 __force *)to);
		return 0;
	case 8:
		put_unaligned(*(u64 *)from, (u64 __force *)to);
		return 0;
	default:
		BUILD_BUG();
		return 0;
	}
}
#define __put_user_fn(sz, u, k)	__put_user_fn(sz, u, k)

#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	*((type *)dst) = get_unaligned((type *)(src));			\
	if (0) /* make sure the label looks used to the compiler */	\
		goto err_label;						\
} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)			\
do {									\
	put_unaligned(*((type *)src), (type *)(dst));			\
	if (0) /* make sure the label looks used to the compiler */	\
		goto err_label;						\
} while (0)

static inline __must_check unsigned long
raw_copy_from_user(void *to, const void __user * from, unsigned long n)
{
	memcpy(to, (const void __force *)from, n);
	return 0;
}

static inline __must_check unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	memcpy((void __force *)to, from, n);
	return 0;
}
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER
#endif /* CONFIG_UACCESS_MEMCPY */

/*
 * These are the main single-value transfer routines.  They automatically
 * use the right size if we just have the right pointer type.
 * This version just falls back to copy_{from,to}_user, which should
 * provide a fast-path for small values.
 */
#define __put_user(x, ptr) \
({								\
	__typeof__(*(ptr)) __x = (x);				\
	int __pu_err = -EFAULT;					\
        __chk_user_ptr(ptr);                                    \
	switch (sizeof (*(ptr))) {				\
	case 1:							\
	case 2:							\
	case 4:							\
	case 8:							\
		__pu_err = __put_user_fn(sizeof (*(ptr)),	\
					 ptr, &__x);		\
		break;						\
	default:						\
		__put_user_bad();				\
		break;						\
	 }							\
	__pu_err;						\
})

#define put_user(x, ptr)					\
({								\
	void __user *__p = (ptr);				\
	might_fault();						\
	access_ok(__p, sizeof(*ptr)) ?		\
		__put_user((x), ((__typeof__(*(ptr)) __user *)__p)) :	\
		-EFAULT;					\
})

#ifndef __put_user_fn

static inline int __put_user_fn(size_t size, void __user *ptr, void *x)
{
	return unlikely(raw_copy_to_user(ptr, x, size)) ? -EFAULT : 0;
}

#define __put_user_fn(sz, u, k)	__put_user_fn(sz, u, k)

#endif

extern int __put_user_bad(void) __attribute__((noreturn));

#define __get_user(x, ptr)					\
({								\
	int __gu_err = -EFAULT;					\
	__chk_user_ptr(ptr);					\
	switch (sizeof(*(ptr))) {				\
	case 1: {						\
		unsigned char __x = 0;				\
		__gu_err = __get_user_fn(sizeof (*(ptr)),	\
					 ptr, &__x);		\
		(x) = *(__force __typeof__(*(ptr)) *) &__x;	\
		break;						\
	};							\
	case 2: {						\
		unsigned short __x = 0;				\
		__gu_err = __get_user_fn(sizeof (*(ptr)),	\
					 ptr, &__x);		\
		(x) = *(__force __typeof__(*(ptr)) *) &__x;	\
		break;						\
	};							\
	case 4: {						\
		unsigned int __x = 0;				\
		__gu_err = __get_user_fn(sizeof (*(ptr)),	\
					 ptr, &__x);		\
		(x) = *(__force __typeof__(*(ptr)) *) &__x;	\
		break;						\
	};							\
	case 8: {						\
		unsigned long long __x = 0;			\
		__gu_err = __get_user_fn(sizeof (*(ptr)),	\
					 ptr, &__x);		\
		(x) = *(__force __typeof__(*(ptr)) *) &__x;	\
		break;						\
	};							\
	default:						\
		__get_user_bad();				\
		break;						\
	}							\
	__gu_err;						\
})

#define get_user(x, ptr)					\
({								\
	const void __user *__p = (ptr);				\
	might_fault();						\
	access_ok(__p, sizeof(*ptr)) ?		\
		__get_user((x), (__typeof__(*(ptr)) __user *)__p) :\
		((x) = (__typeof__(*(ptr)))0,-EFAULT);		\
})

#ifndef __get_user_fn
static inline int __get_user_fn(size_t size, const void __user *ptr, void *x)
{
	return unlikely(raw_copy_from_user(x, ptr, size)) ? -EFAULT : 0;
}

#define __get_user_fn(sz, u, k)	__get_user_fn(sz, u, k)

#endif

extern int __get_user_bad(void) __attribute__((noreturn));

/*
 * Zero Userspace
 */
#ifndef __clear_user
static inline __must_check unsigned long
__clear_user(void __user *to, unsigned long n)
{
	memset((void __force *)to, 0, n);
	return 0;
}
#endif

static inline __must_check unsigned long
clear_user(void __user *to, unsigned long n)
{
	might_fault();
	if (!access_ok(to, n))
		return n;

	return __clear_user(to, n);
}

#include <asm/extable.h>

__must_check long strncpy_from_user(char *dst, const char __user *src,
				    long count);
__must_check long strnlen_user(const char __user *src, long n);

#endif /* __ASM_GENERIC_UACCESS_H */
