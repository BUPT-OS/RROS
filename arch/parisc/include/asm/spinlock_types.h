/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SPINLOCK_TYPES_H
#define __ASM_SPINLOCK_TYPES_H

#define __ARCH_SPIN_LOCK_UNLOCKED_VAL	0x1a46

#define SPINLOCK_BREAK_INSN	0x0000c006	/* break 6,6 */

#ifndef __ASSEMBLY__

typedef struct {
#ifdef CONFIG_PA20
	volatile unsigned int slock;
# define __ARCH_SPIN_LOCK_UNLOCKED { __ARCH_SPIN_LOCK_UNLOCKED_VAL }
#else
	volatile unsigned int lock[4];
# define __ARCH_SPIN_LOCK_UNLOCKED	\
	{ { __ARCH_SPIN_LOCK_UNLOCKED_VAL, __ARCH_SPIN_LOCK_UNLOCKED_VAL, \
	    __ARCH_SPIN_LOCK_UNLOCKED_VAL, __ARCH_SPIN_LOCK_UNLOCKED_VAL } }
#endif
} arch_spinlock_t;


/* counter:
 * Unlocked     : 0x0100_0000
 * Read lock(s) : 0x00FF_FFFF to 0x01  (Multiple Readers decrement it)
 * Write lock   : 0x0, but only if prior value is "unlocked" 0x0100_0000
 */
typedef struct {
	arch_spinlock_t		lock_mutex;
	volatile unsigned int	counter;
} arch_rwlock_t;

#endif /* __ASSEMBLY__ */

#define __ARCH_RW_LOCK_UNLOCKED__       0x01000000
#define __ARCH_RW_LOCK_UNLOCKED         { .lock_mutex = __ARCH_SPIN_LOCK_UNLOCKED, \
					.counter = __ARCH_RW_LOCK_UNLOCKED__ }

#endif
