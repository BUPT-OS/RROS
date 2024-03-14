#ifndef _LINUX_MMAP_LOCK_H
#define _LINUX_MMAP_LOCK_H

#include <linux/lockdep.h>
#include <linux/mm_types.h>
#include <linux/mmdebug.h>
#include <linux/rwsem.h>
#include <linux/tracepoint-defs.h>
#include <linux/types.h>

#define MMAP_LOCK_INITIALIZER(name) \
	.mmap_lock = __RWSEM_INITIALIZER((name).mmap_lock),

DECLARE_TRACEPOINT(mmap_lock_start_locking);
DECLARE_TRACEPOINT(mmap_lock_acquire_returned);
DECLARE_TRACEPOINT(mmap_lock_released);

#ifdef CONFIG_TRACING

void __mmap_lock_do_trace_start_locking(struct mm_struct *mm, bool write);
void __mmap_lock_do_trace_acquire_returned(struct mm_struct *mm, bool write,
					   bool success);
void __mmap_lock_do_trace_released(struct mm_struct *mm, bool write);

static inline void __mmap_lock_trace_start_locking(struct mm_struct *mm,
						   bool write)
{
	if (tracepoint_enabled(mmap_lock_start_locking))
		__mmap_lock_do_trace_start_locking(mm, write);
}

static inline void __mmap_lock_trace_acquire_returned(struct mm_struct *mm,
						      bool write, bool success)
{
	if (tracepoint_enabled(mmap_lock_acquire_returned))
		__mmap_lock_do_trace_acquire_returned(mm, write, success);
}

static inline void __mmap_lock_trace_released(struct mm_struct *mm, bool write)
{
	if (tracepoint_enabled(mmap_lock_released))
		__mmap_lock_do_trace_released(mm, write);
}

#else /* !CONFIG_TRACING */

static inline void __mmap_lock_trace_start_locking(struct mm_struct *mm,
						   bool write)
{
}

static inline void __mmap_lock_trace_acquire_returned(struct mm_struct *mm,
						      bool write, bool success)
{
}

static inline void __mmap_lock_trace_released(struct mm_struct *mm, bool write)
{
}

#endif /* CONFIG_TRACING */

static inline void mmap_assert_locked(struct mm_struct *mm)
{
	lockdep_assert_held(&mm->mmap_lock);
	VM_BUG_ON_MM(!rwsem_is_locked(&mm->mmap_lock), mm);
}

static inline void mmap_assert_write_locked(struct mm_struct *mm)
{
	lockdep_assert_held_write(&mm->mmap_lock);
	VM_BUG_ON_MM(!rwsem_is_locked(&mm->mmap_lock), mm);
}

#ifdef CONFIG_PER_VMA_LOCK
/*
 * Drop all currently-held per-VMA locks.
 * This is called from the mmap_lock implementation directly before releasing
 * a write-locked mmap_lock (or downgrading it to read-locked).
 * This should normally NOT be called manually from other places.
 * If you want to call this manually anyway, keep in mind that this will release
 * *all* VMA write locks, including ones from further up the stack.
 */
static inline void vma_end_write_all(struct mm_struct *mm)
{
	mmap_assert_write_locked(mm);
	/*
	 * Nobody can concurrently modify mm->mm_lock_seq due to exclusive
	 * mmap_lock being held.
	 * We need RELEASE semantics here to ensure that preceding stores into
	 * the VMA take effect before we unlock it with this store.
	 * Pairs with ACQUIRE semantics in vma_start_read().
	 */
	smp_store_release(&mm->mm_lock_seq, mm->mm_lock_seq + 1);
}
#else
static inline void vma_end_write_all(struct mm_struct *mm) {}
#endif

static inline void mmap_init_lock(struct mm_struct *mm)
{
	init_rwsem(&mm->mmap_lock);
}

static inline void mmap_write_lock(struct mm_struct *mm)
{
	__mmap_lock_trace_start_locking(mm, true);
	down_write(&mm->mmap_lock);
	__mmap_lock_trace_acquire_returned(mm, true, true);
}

static inline void mmap_write_lock_nested(struct mm_struct *mm, int subclass)
{
	__mmap_lock_trace_start_locking(mm, true);
	down_write_nested(&mm->mmap_lock, subclass);
	__mmap_lock_trace_acquire_returned(mm, true, true);
}

static inline int mmap_write_lock_killable(struct mm_struct *mm)
{
	int ret;

	__mmap_lock_trace_start_locking(mm, true);
	ret = down_write_killable(&mm->mmap_lock);
	__mmap_lock_trace_acquire_returned(mm, true, ret == 0);
	return ret;
}

static inline void mmap_write_unlock(struct mm_struct *mm)
{
	__mmap_lock_trace_released(mm, true);
	vma_end_write_all(mm);
	up_write(&mm->mmap_lock);
}

static inline void mmap_write_downgrade(struct mm_struct *mm)
{
	__mmap_lock_trace_acquire_returned(mm, false, true);
	vma_end_write_all(mm);
	downgrade_write(&mm->mmap_lock);
}

static inline void mmap_read_lock(struct mm_struct *mm)
{
	__mmap_lock_trace_start_locking(mm, false);
	down_read(&mm->mmap_lock);
	__mmap_lock_trace_acquire_returned(mm, false, true);
}

static inline int mmap_read_lock_killable(struct mm_struct *mm)
{
	int ret;

	__mmap_lock_trace_start_locking(mm, false);
	ret = down_read_killable(&mm->mmap_lock);
	__mmap_lock_trace_acquire_returned(mm, false, ret == 0);
	return ret;
}

static inline bool mmap_read_trylock(struct mm_struct *mm)
{
	bool ret;

	__mmap_lock_trace_start_locking(mm, false);
	ret = down_read_trylock(&mm->mmap_lock) != 0;
	__mmap_lock_trace_acquire_returned(mm, false, ret);
	return ret;
}

static inline void mmap_read_unlock(struct mm_struct *mm)
{
	__mmap_lock_trace_released(mm, false);
	up_read(&mm->mmap_lock);
}

static inline void mmap_read_unlock_non_owner(struct mm_struct *mm)
{
	__mmap_lock_trace_released(mm, false);
	up_read_non_owner(&mm->mmap_lock);
}

static inline int mmap_lock_is_contended(struct mm_struct *mm)
{
	return rwsem_is_contended(&mm->mmap_lock);
}

#endif /* _LINUX_MMAP_LOCK_H */
