/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UACCESS_H
#define _ASM_X86_UACCESS_H
/*
 * User space memory access functions
 */
#include <linux/compiler.h>
#include <linux/instrumented.h>
#include <linux/kasan-checks.h>
#include <linux/mm_types.h>
#include <linux/string.h>
#include <linux/mmap_lock.h>
#include <asm/asm.h>
#include <asm/page.h>
#include <asm/smap.h>
#include <asm/extable.h>
#include <asm/tlbflush.h>

#ifdef CONFIG_X86_32
# include <asm/uaccess_32.h>
#else
# include <asm/uaccess_64.h>
#endif

#include <asm-generic/access_ok.h>

extern int __get_user_1(void);
extern int __get_user_2(void);
extern int __get_user_4(void);
extern int __get_user_8(void);
extern int __get_user_nocheck_1(void);
extern int __get_user_nocheck_2(void);
extern int __get_user_nocheck_4(void);
extern int __get_user_nocheck_8(void);
extern int __get_user_bad(void);

#define __uaccess_begin() stac()
#define __uaccess_end()   clac()
#define __uaccess_begin_nospec()	\
({					\
	stac();				\
	barrier_nospec();		\
})

/*
 * This is the smallest unsigned integer type that can fit a value
 * (up to 'long long')
 */
#define __inttype(x) __typeof__(		\
	__typefits(x,char,			\
	  __typefits(x,short,			\
	    __typefits(x,int,			\
	      __typefits(x,long,0ULL)))))

#define __typefits(x,type,not) \
	__builtin_choose_expr(sizeof(x)<=sizeof(type),(unsigned type)0,not)

/*
 * This is used for both get_user() and __get_user() to expand to
 * the proper special function call that has odd calling conventions
 * due to returning both a value and an error, and that depends on
 * the size of the pointer passed in.
 *
 * Careful: we have to cast the result to the type of the pointer
 * for sign reasons.
 *
 * The use of _ASM_DX as the register specifier is a bit of a
 * simplification, as gcc only cares about it as the starting point
 * and not size: for a 64-bit value it will use %ecx:%edx on 32 bits
 * (%ecx being the next register in gcc's x86 register sequence), and
 * %rdx on 64 bits.
 *
 * Clang/LLVM cares about the size of the register, but still wants
 * the base register for something that ends up being a pair.
 */
#define do_get_user_call(fn,x,ptr)					\
({									\
	int __ret_gu;							\
	register __inttype(*(ptr)) __val_gu asm("%"_ASM_DX);		\
	__chk_user_ptr(ptr);						\
	asm volatile("call __" #fn "_%P4"				\
		     : "=a" (__ret_gu), "=r" (__val_gu),		\
			ASM_CALL_CONSTRAINT				\
		     : "0" (ptr), "i" (sizeof(*(ptr))));		\
	instrument_get_user(__val_gu);					\
	(x) = (__force __typeof__(*(ptr))) __val_gu;			\
	__builtin_expect(__ret_gu, 0);					\
})

/**
 * get_user - Get a simple variable from user space.
 * @x:   Variable to store result.
 * @ptr: Source address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * This macro copies a single simple variable from user space to kernel
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and the result of
 * dereferencing @ptr must be assignable to @x without a cast.
 *
 * Return: zero on success, or -EFAULT on error.
 * On error, the variable @x is set to zero.
 */
#define get_user(x,ptr) ({ might_fault(); do_get_user_call(get_user,x,ptr); })

/**
 * __get_user - Get a simple variable from user space, with less checking.
 * @x:   Variable to store result.
 * @ptr: Source address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
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
 * Return: zero on success, or -EFAULT on error.
 * On error, the variable @x is set to zero.
 */
#define __get_user(x,ptr) do_get_user_call(get_user_nocheck,x,ptr)


#ifdef CONFIG_X86_32
#define __put_user_goto_u64(x, addr, label)			\
	asm_volatile_goto("\n"					\
		     "1:	movl %%eax,0(%1)\n"		\
		     "2:	movl %%edx,4(%1)\n"		\
		     _ASM_EXTABLE_UA(1b, %l2)			\
		     _ASM_EXTABLE_UA(2b, %l2)			\
		     : : "A" (x), "r" (addr)			\
		     : : label)

#else
#define __put_user_goto_u64(x, ptr, label) \
	__put_user_goto(x, ptr, "q", "er", label)
#endif

extern void __put_user_bad(void);

/*
 * Strange magic calling convention: pointer in %ecx,
 * value in %eax(:%edx), return value in %ecx. clobbers %rbx
 */
extern void __put_user_1(void);
extern void __put_user_2(void);
extern void __put_user_4(void);
extern void __put_user_8(void);
extern void __put_user_nocheck_1(void);
extern void __put_user_nocheck_2(void);
extern void __put_user_nocheck_4(void);
extern void __put_user_nocheck_8(void);

/*
 * ptr must be evaluated and assigned to the temporary __ptr_pu before
 * the assignment of x to __val_pu, to avoid any function calls
 * involved in the ptr expression (possibly implicitly generated due
 * to KASAN) from clobbering %ax.
 */
#define do_put_user_call(fn,x,ptr)					\
({									\
	int __ret_pu;							\
	void __user *__ptr_pu;						\
	register __typeof__(*(ptr)) __val_pu asm("%"_ASM_AX);		\
	__typeof__(*(ptr)) __x = (x); /* eval x once */			\
	__typeof__(ptr) __ptr = (ptr); /* eval ptr once */		\
	__chk_user_ptr(__ptr);						\
	__ptr_pu = __ptr;						\
	__val_pu = __x;							\
	asm volatile("call __" #fn "_%P[size]"				\
		     : "=c" (__ret_pu),					\
			ASM_CALL_CONSTRAINT				\
		     : "0" (__ptr_pu),					\
		       "r" (__val_pu),					\
		       [size] "i" (sizeof(*(ptr)))			\
		     :"ebx");						\
	instrument_put_user(__x, __ptr, sizeof(*(ptr)));		\
	__builtin_expect(__ret_pu, 0);					\
})

/**
 * put_user - Write a simple value into user space.
 * @x:   Value to copy to user space.
 * @ptr: Destination address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * This macro copies a single simple value from kernel space to user
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and @x must be assignable
 * to the result of dereferencing @ptr.
 *
 * Return: zero on success, or -EFAULT on error.
 */
#define put_user(x, ptr) ({ might_fault(); do_put_user_call(put_user,x,ptr); })

/**
 * __put_user - Write a simple value into user space, with less checking.
 * @x:   Value to copy to user space.
 * @ptr: Destination address, in user space.
 *
 * Context: User context only. This function may sleep if pagefaults are
 *          enabled.
 *
 * This macro copies a single simple value from kernel space to user
 * space.  It supports simple types like char and int, but not larger
 * data types like structures or arrays.
 *
 * @ptr must have pointer-to-simple-variable type, and @x must be assignable
 * to the result of dereferencing @ptr.
 *
 * Caller must check the pointer with access_ok() before calling this
 * function.
 *
 * Return: zero on success, or -EFAULT on error.
 */
#define __put_user(x, ptr) do_put_user_call(put_user_nocheck,x,ptr)

#define __put_user_size(x, ptr, size, label)				\
do {									\
	__typeof__(*(ptr)) __x = (x); /* eval x once */			\
	__typeof__(ptr) __ptr = (ptr); /* eval ptr once */		\
	__chk_user_ptr(__ptr);						\
	switch (size) {							\
	case 1:								\
		__put_user_goto(__x, __ptr, "b", "iq", label);		\
		break;							\
	case 2:								\
		__put_user_goto(__x, __ptr, "w", "ir", label);		\
		break;							\
	case 4:								\
		__put_user_goto(__x, __ptr, "l", "ir", label);		\
		break;							\
	case 8:								\
		__put_user_goto_u64(__x, __ptr, label);			\
		break;							\
	default:							\
		__put_user_bad();					\
	}								\
	instrument_put_user(__x, __ptr, size);				\
} while (0)

#ifdef CONFIG_CC_HAS_ASM_GOTO_OUTPUT

#ifdef CONFIG_X86_32
#define __get_user_asm_u64(x, ptr, label) do {				\
	unsigned int __gu_low, __gu_high;				\
	const unsigned int __user *__gu_ptr;				\
	__gu_ptr = (const void __user *)(ptr);				\
	__get_user_asm(__gu_low, __gu_ptr, "l", "=r", label);		\
	__get_user_asm(__gu_high, __gu_ptr+1, "l", "=r", label);	\
	(x) = ((unsigned long long)__gu_high << 32) | __gu_low;		\
} while (0)
#else
#define __get_user_asm_u64(x, ptr, label)				\
	__get_user_asm(x, ptr, "q", "=r", label)
#endif

#define __get_user_size(x, ptr, size, label)				\
do {									\
	__chk_user_ptr(ptr);						\
	switch (size) {							\
	case 1:	{							\
		unsigned char x_u8__;					\
		__get_user_asm(x_u8__, ptr, "b", "=q", label);		\
		(x) = x_u8__;						\
		break;							\
	}								\
	case 2:								\
		__get_user_asm(x, ptr, "w", "=r", label);		\
		break;							\
	case 4:								\
		__get_user_asm(x, ptr, "l", "=r", label);		\
		break;							\
	case 8:								\
		__get_user_asm_u64(x, ptr, label);			\
		break;							\
	default:							\
		(x) = __get_user_bad();					\
	}								\
	instrument_get_user(x);						\
} while (0)

#define __get_user_asm(x, addr, itype, ltype, label)			\
	asm_volatile_goto("\n"						\
		     "1:	mov"itype" %[umem],%[output]\n"		\
		     _ASM_EXTABLE_UA(1b, %l2)				\
		     : [output] ltype(x)				\
		     : [umem] "m" (__m(addr))				\
		     : : label)

#else // !CONFIG_CC_HAS_ASM_GOTO_OUTPUT

#ifdef CONFIG_X86_32
#define __get_user_asm_u64(x, ptr, retval)				\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	asm volatile("\n"						\
		     "1:	movl %[lowbits],%%eax\n"		\
		     "2:	movl %[highbits],%%edx\n"		\
		     "3:\n"						\
		     _ASM_EXTABLE_TYPE_REG(1b, 3b, EX_TYPE_EFAULT_REG |	\
					   EX_FLAG_CLEAR_AX_DX,		\
					   %[errout])			\
		     _ASM_EXTABLE_TYPE_REG(2b, 3b, EX_TYPE_EFAULT_REG |	\
					   EX_FLAG_CLEAR_AX_DX,		\
					   %[errout])			\
		     : [errout] "=r" (retval),				\
		       [output] "=&A"(x)				\
		     : [lowbits] "m" (__m(__ptr)),			\
		       [highbits] "m" __m(((u32 __user *)(__ptr)) + 1),	\
		       "0" (retval));					\
})

#else
#define __get_user_asm_u64(x, ptr, retval) \
	 __get_user_asm(x, ptr, retval, "q")
#endif

#define __get_user_size(x, ptr, size, retval)				\
do {									\
	unsigned char x_u8__;						\
									\
	retval = 0;							\
	__chk_user_ptr(ptr);						\
	switch (size) {							\
	case 1:								\
		__get_user_asm(x_u8__, ptr, retval, "b");		\
		(x) = x_u8__;						\
		break;							\
	case 2:								\
		__get_user_asm(x, ptr, retval, "w");			\
		break;							\
	case 4:								\
		__get_user_asm(x, ptr, retval, "l");			\
		break;							\
	case 8:								\
		__get_user_asm_u64(x, ptr, retval);			\
		break;							\
	default:							\
		(x) = __get_user_bad();					\
	}								\
} while (0)

#define __get_user_asm(x, addr, err, itype)				\
	asm volatile("\n"						\
		     "1:	mov"itype" %[umem],%[output]\n"		\
		     "2:\n"						\
		     _ASM_EXTABLE_TYPE_REG(1b, 2b, EX_TYPE_EFAULT_REG | \
					   EX_FLAG_CLEAR_AX,		\
					   %[errout])			\
		     : [errout] "=r" (err),				\
		       [output] "=a" (x)				\
		     : [umem] "m" (__m(addr)),				\
		       "0" (err))

#endif // CONFIG_CC_HAS_ASM_GOTO_OUTPUT

#ifdef CONFIG_CC_HAS_ASM_GOTO_TIED_OUTPUT
#define __try_cmpxchg_user_asm(itype, ltype, _ptr, _pold, _new, label)	({ \
	bool success;							\
	__typeof__(_ptr) _old = (__typeof__(_ptr))(_pold);		\
	__typeof__(*(_ptr)) __old = *_old;				\
	__typeof__(*(_ptr)) __new = (_new);				\
	asm_volatile_goto("\n"						\
		     "1: " LOCK_PREFIX "cmpxchg"itype" %[new], %[ptr]\n"\
		     _ASM_EXTABLE_UA(1b, %l[label])			\
		     : CC_OUT(z) (success),				\
		       [ptr] "+m" (*_ptr),				\
		       [old] "+a" (__old)				\
		     : [new] ltype (__new)				\
		     : "memory"						\
		     : label);						\
	if (unlikely(!success))						\
		*_old = __old;						\
	likely(success);					})

#ifdef CONFIG_X86_32
#define __try_cmpxchg64_user_asm(_ptr, _pold, _new, label)	({	\
	bool success;							\
	__typeof__(_ptr) _old = (__typeof__(_ptr))(_pold);		\
	__typeof__(*(_ptr)) __old = *_old;				\
	__typeof__(*(_ptr)) __new = (_new);				\
	asm_volatile_goto("\n"						\
		     "1: " LOCK_PREFIX "cmpxchg8b %[ptr]\n"		\
		     _ASM_EXTABLE_UA(1b, %l[label])			\
		     : CC_OUT(z) (success),				\
		       "+A" (__old),					\
		       [ptr] "+m" (*_ptr)				\
		     : "b" ((u32)__new),				\
		       "c" ((u32)((u64)__new >> 32))			\
		     : "memory"						\
		     : label);						\
	if (unlikely(!success))						\
		*_old = __old;						\
	likely(success);					})
#endif // CONFIG_X86_32
#else  // !CONFIG_CC_HAS_ASM_GOTO_TIED_OUTPUT
#define __try_cmpxchg_user_asm(itype, ltype, _ptr, _pold, _new, label)	({ \
	int __err = 0;							\
	bool success;							\
	__typeof__(_ptr) _old = (__typeof__(_ptr))(_pold);		\
	__typeof__(*(_ptr)) __old = *_old;				\
	__typeof__(*(_ptr)) __new = (_new);				\
	asm volatile("\n"						\
		     "1: " LOCK_PREFIX "cmpxchg"itype" %[new], %[ptr]\n"\
		     CC_SET(z)						\
		     "2:\n"						\
		     _ASM_EXTABLE_TYPE_REG(1b, 2b, EX_TYPE_EFAULT_REG,	\
					   %[errout])			\
		     : CC_OUT(z) (success),				\
		       [errout] "+r" (__err),				\
		       [ptr] "+m" (*_ptr),				\
		       [old] "+a" (__old)				\
		     : [new] ltype (__new)				\
		     : "memory");					\
	if (unlikely(__err))						\
		goto label;						\
	if (unlikely(!success))						\
		*_old = __old;						\
	likely(success);					})

#ifdef CONFIG_X86_32
/*
 * Unlike the normal CMPXCHG, use output GPR for both success/fail and error.
 * There are only six GPRs available and four (EAX, EBX, ECX, and EDX) are
 * hardcoded by CMPXCHG8B, leaving only ESI and EDI.  If the compiler uses
 * both ESI and EDI for the memory operand, compilation will fail if the error
 * is an input+output as there will be no register available for input.
 */
#define __try_cmpxchg64_user_asm(_ptr, _pold, _new, label)	({	\
	int __result;							\
	__typeof__(_ptr) _old = (__typeof__(_ptr))(_pold);		\
	__typeof__(*(_ptr)) __old = *_old;				\
	__typeof__(*(_ptr)) __new = (_new);				\
	asm volatile("\n"						\
		     "1: " LOCK_PREFIX "cmpxchg8b %[ptr]\n"		\
		     "mov $0, %[result]\n\t"				\
		     "setz %b[result]\n"				\
		     "2:\n"						\
		     _ASM_EXTABLE_TYPE_REG(1b, 2b, EX_TYPE_EFAULT_REG,	\
					   %[result])			\
		     : [result] "=q" (__result),			\
		       "+A" (__old),					\
		       [ptr] "+m" (*_ptr)				\
		     : "b" ((u32)__new),				\
		       "c" ((u32)((u64)__new >> 32))			\
		     : "memory", "cc");					\
	if (unlikely(__result < 0))					\
		goto label;						\
	if (unlikely(!__result))					\
		*_old = __old;						\
	likely(__result);					})
#endif // CONFIG_X86_32
#endif // CONFIG_CC_HAS_ASM_GOTO_TIED_OUTPUT

/* FIXME: this hack is definitely wrong -AK */
struct __large_struct { unsigned long buf[100]; };
#define __m(x) (*(struct __large_struct __user *)(x))

/*
 * Tell gcc we read from memory instead of writing: this is because
 * we do not write to any memory gcc knows about, so there are no
 * aliasing issues.
 */
#define __put_user_goto(x, addr, itype, ltype, label)			\
	asm_volatile_goto("\n"						\
		"1:	mov"itype" %0,%1\n"				\
		_ASM_EXTABLE_UA(1b, %l2)				\
		: : ltype(x), "m" (__m(addr))				\
		: : label)

extern unsigned long
copy_from_user_nmi(void *to, const void __user *from, unsigned long n);
extern __must_check long
strncpy_from_user(char *dst, const char __user *src, long count);

extern __must_check long strnlen_user(const char __user *str, long n);

#ifdef CONFIG_ARCH_HAS_COPY_MC
unsigned long __must_check
copy_mc_to_kernel(void *to, const void *from, unsigned len);
#define copy_mc_to_kernel copy_mc_to_kernel

unsigned long __must_check
copy_mc_to_user(void *to, const void *from, unsigned len);
#endif

/*
 * movsl can be slow when source and dest are not both 8-byte aligned
 */
#ifdef CONFIG_X86_INTEL_USERCOPY
extern struct movsl_mask {
	int mask;
} ____cacheline_aligned_in_smp movsl_mask;
#endif

#define ARCH_HAS_NOCACHE_UACCESS 1

/*
 * The "unsafe" user accesses aren't really "unsafe", but the naming
 * is a big fat warning: you have to not only do the access_ok()
 * checking before using them, but you have to surround them with the
 * user_access_begin/end() pair.
 */
static __must_check __always_inline bool user_access_begin(const void __user *ptr, size_t len)
{
	if (unlikely(!access_ok(ptr,len)))
		return 0;
	__uaccess_begin_nospec();
	return 1;
}
#define user_access_begin(a,b)	user_access_begin(a,b)
#define user_access_end()	__uaccess_end()

#define user_access_save()	smap_save()
#define user_access_restore(x)	smap_restore(x)

#define unsafe_put_user(x, ptr, label)	\
	__put_user_size((__typeof__(*(ptr)))(x), (ptr), sizeof(*(ptr)), label)

#ifdef CONFIG_CC_HAS_ASM_GOTO_OUTPUT
#define unsafe_get_user(x, ptr, err_label)					\
do {										\
	__inttype(*(ptr)) __gu_val;						\
	__get_user_size(__gu_val, (ptr), sizeof(*(ptr)), err_label);		\
	(x) = (__force __typeof__(*(ptr)))__gu_val;				\
} while (0)
#else // !CONFIG_CC_HAS_ASM_GOTO_OUTPUT
#define unsafe_get_user(x, ptr, err_label)					\
do {										\
	int __gu_err;								\
	__inttype(*(ptr)) __gu_val;						\
	__get_user_size(__gu_val, (ptr), sizeof(*(ptr)), __gu_err);		\
	(x) = (__force __typeof__(*(ptr)))__gu_val;				\
	if (unlikely(__gu_err)) goto err_label;					\
} while (0)
#endif // CONFIG_CC_HAS_ASM_GOTO_OUTPUT

extern void __try_cmpxchg_user_wrong_size(void);

#ifndef CONFIG_X86_32
#define __try_cmpxchg64_user_asm(_ptr, _oldp, _nval, _label)		\
	__try_cmpxchg_user_asm("q", "r", (_ptr), (_oldp), (_nval), _label)
#endif

/*
 * Force the pointer to u<size> to match the size expected by the asm helper.
 * clang/LLVM compiles all cases and only discards the unused paths after
 * processing errors, which breaks i386 if the pointer is an 8-byte value.
 */
#define unsafe_try_cmpxchg_user(_ptr, _oldp, _nval, _label) ({			\
	bool __ret;								\
	__chk_user_ptr(_ptr);							\
	switch (sizeof(*(_ptr))) {						\
	case 1:	__ret = __try_cmpxchg_user_asm("b", "q",			\
					       (__force u8 *)(_ptr), (_oldp),	\
					       (_nval), _label);		\
		break;								\
	case 2:	__ret = __try_cmpxchg_user_asm("w", "r",			\
					       (__force u16 *)(_ptr), (_oldp),	\
					       (_nval), _label);		\
		break;								\
	case 4:	__ret = __try_cmpxchg_user_asm("l", "r",			\
					       (__force u32 *)(_ptr), (_oldp),	\
					       (_nval), _label);		\
		break;								\
	case 8:	__ret = __try_cmpxchg64_user_asm((__force u64 *)(_ptr), (_oldp),\
						 (_nval), _label);		\
		break;								\
	default: __try_cmpxchg_user_wrong_size();				\
	}									\
	__ret;						})

/* "Returns" 0 on success, 1 on failure, -EFAULT if the access faults. */
#define __try_cmpxchg_user(_ptr, _oldp, _nval, _label)	({		\
	int __ret = -EFAULT;						\
	__uaccess_begin_nospec();					\
	__ret = !unsafe_try_cmpxchg_user(_ptr, _oldp, _nval, _label);	\
_label:									\
	__uaccess_end();						\
	__ret;								\
							})

/*
 * We want the unsafe accessors to always be inlined and use
 * the error labels - thus the macro games.
 */
#define unsafe_copy_loop(dst, src, len, type, label)				\
	while (len >= sizeof(type)) {						\
		unsafe_put_user(*(type *)(src),(type __user *)(dst),label);	\
		dst += sizeof(type);						\
		src += sizeof(type);						\
		len -= sizeof(type);						\
	}

#define unsafe_copy_to_user(_dst,_src,_len,label)			\
do {									\
	char __user *__ucu_dst = (_dst);				\
	const char *__ucu_src = (_src);					\
	size_t __ucu_len = (_len);					\
	unsafe_copy_loop(__ucu_dst, __ucu_src, __ucu_len, u64, label);	\
	unsafe_copy_loop(__ucu_dst, __ucu_src, __ucu_len, u32, label);	\
	unsafe_copy_loop(__ucu_dst, __ucu_src, __ucu_len, u16, label);	\
	unsafe_copy_loop(__ucu_dst, __ucu_src, __ucu_len, u8, label);	\
} while (0)

#ifdef CONFIG_CC_HAS_ASM_GOTO_OUTPUT
#define __get_kernel_nofault(dst, src, type, err_label)			\
	__get_user_size(*((type *)(dst)), (__force type __user *)(src),	\
			sizeof(type), err_label)
#else // !CONFIG_CC_HAS_ASM_GOTO_OUTPUT
#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	int __kr_err;							\
									\
	__get_user_size(*((type *)(dst)), (__force type __user *)(src),	\
			sizeof(type), __kr_err);			\
	if (unlikely(__kr_err))						\
		goto err_label;						\
} while (0)
#endif // CONFIG_CC_HAS_ASM_GOTO_OUTPUT

#define __put_kernel_nofault(dst, src, type, err_label)			\
	__put_user_size(*((type *)(src)), (__force type __user *)(dst),	\
			sizeof(type), err_label)

#endif /* _ASM_X86_UACCESS_H */

