/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_RANDOMIZE_KSTACK_H
#define _LINUX_RANDOMIZE_KSTACK_H

#ifdef CONFIG_RANDOMIZE_KSTACK_OFFSET
#include <linux/kernel.h>
#include <linux/jump_label.h>
#include <linux/percpu-defs.h>

DECLARE_STATIC_KEY_MAYBE(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,
			 randomize_kstack_offset);
DECLARE_PER_CPU(u32, kstack_offset);

/*
 * Do not use this anywhere else in the kernel. This is used here because
 * it provides an arch-agnostic way to grow the stack with correct
 * alignment. Also, since this use is being explicitly masked to a max of
 * 10 bits, stack-clash style attacks are unlikely. For more details see
 * "VLAs" in Documentation/process/deprecated.rst
 *
 * The normal __builtin_alloca() is initialized with INIT_STACK_ALL (currently
 * only with Clang and not GCC). Initializing the unused area on each syscall
 * entry is expensive, and generating an implicit call to memset() may also be
 * problematic (such as in noinstr functions). Therefore, if the compiler
 * supports it (which it should if it initializes allocas), always use the
 * "uninitialized" variant of the builtin.
 */
#if __has_builtin(__builtin_alloca_uninitialized)
#define __kstack_alloca __builtin_alloca_uninitialized
#else
#define __kstack_alloca __builtin_alloca
#endif

/*
 * Use, at most, 10 bits of entropy. We explicitly cap this to keep the
 * "VLA" from being unbounded (see above). 10 bits leaves enough room for
 * per-arch offset masks to reduce entropy (by removing higher bits, since
 * high entropy may overly constrain usable stack space), and for
 * compiler/arch-specific stack alignment to remove the lower bits.
 */
#define KSTACK_OFFSET_MAX(x)	((x) & 0x3FF)

/**
 * add_random_kstack_offset - Increase stack utilization by previously
 *			      chosen random offset
 *
 * This should be used in the syscall entry path when interrupts and
 * preempt are disabled, and after user registers have been stored to
 * the stack. For testing the resulting entropy, please see:
 * tools/testing/selftests/lkdtm/stack-entropy.sh
 */
#define add_random_kstack_offset() do {					\
	if (static_branch_maybe(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,	\
				&randomize_kstack_offset)) {		\
		u32 offset = raw_cpu_read(kstack_offset);		\
		u8 *ptr = __kstack_alloca(KSTACK_OFFSET_MAX(offset));	\
		/* Keep allocation even after "ptr" loses scope. */	\
		asm volatile("" :: "r"(ptr) : "memory");		\
	}								\
} while (0)

/**
 * choose_random_kstack_offset - Choose the random offset for the next
 *				 add_random_kstack_offset()
 *
 * This should only be used during syscall exit when interrupts and
 * preempt are disabled. This position in the syscall flow is done to
 * frustrate attacks from userspace attempting to learn the next offset:
 * - Maximize the timing uncertainty visible from userspace: if the
 *   offset is chosen at syscall entry, userspace has much more control
 *   over the timing between choosing offsets. "How long will we be in
 *   kernel mode?" tends to be more difficult to predict than "how long
 *   will we be in user mode?"
 * - Reduce the lifetime of the new offset sitting in memory during
 *   kernel mode execution. Exposure of "thread-local" memory content
 *   (e.g. current, percpu, etc) tends to be easier than arbitrary
 *   location memory exposure.
 */
#define choose_random_kstack_offset(rand) do {				\
	if (static_branch_maybe(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,	\
				&randomize_kstack_offset)) {		\
		u32 offset = raw_cpu_read(kstack_offset);		\
		offset ^= (rand);					\
		raw_cpu_write(kstack_offset, offset);			\
	}								\
} while (0)
#else /* CONFIG_RANDOMIZE_KSTACK_OFFSET */
#define add_random_kstack_offset()		do { } while (0)
#define choose_random_kstack_offset(rand)	do { } while (0)
#endif /* CONFIG_RANDOMIZE_KSTACK_OFFSET */

#endif
