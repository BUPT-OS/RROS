// SPDX-License-Identifier: GPL-2.0-only

#include <linux/efi.h>
#include <linux/memblock.h>
#include <linux/spinlock.h>
#include <asm/unaccepted_memory.h>

/* Protects unaccepted memory bitmap */
static DEFINE_SPINLOCK(unaccepted_memory_lock);

/*
 * accept_memory() -- Consult bitmap and accept the memory if needed.
 *
 * Only memory that is explicitly marked as unaccepted in the bitmap requires
 * an action. All the remaining memory is implicitly accepted and doesn't need
 * acceptance.
 *
 * No need to accept:
 *  - anything if the system has no unaccepted table;
 *  - memory that is below phys_base;
 *  - memory that is above the memory that addressable by the bitmap;
 */
void accept_memory(phys_addr_t start, phys_addr_t end)
{
	struct efi_unaccepted_memory *unaccepted;
	unsigned long range_start, range_end;
	unsigned long flags;
	u64 unit_size;

	unaccepted = efi_get_unaccepted_table();
	if (!unaccepted)
		return;

	unit_size = unaccepted->unit_size;

	/*
	 * Only care for the part of the range that is represented
	 * in the bitmap.
	 */
	if (start < unaccepted->phys_base)
		start = unaccepted->phys_base;
	if (end < unaccepted->phys_base)
		return;

	/* Translate to offsets from the beginning of the bitmap */
	start -= unaccepted->phys_base;
	end -= unaccepted->phys_base;

	/*
	 * load_unaligned_zeropad() can lead to unwanted loads across page
	 * boundaries. The unwanted loads are typically harmless. But, they
	 * might be made to totally unrelated or even unmapped memory.
	 * load_unaligned_zeropad() relies on exception fixup (#PF, #GP and now
	 * #VE) to recover from these unwanted loads.
	 *
	 * But, this approach does not work for unaccepted memory. For TDX, a
	 * load from unaccepted memory will not lead to a recoverable exception
	 * within the guest. The guest will exit to the VMM where the only
	 * recourse is to terminate the guest.
	 *
	 * There are two parts to fix this issue and comprehensively avoid
	 * access to unaccepted memory. Together these ensure that an extra
	 * "guard" page is accepted in addition to the memory that needs to be
	 * used:
	 *
	 * 1. Implicitly extend the range_contains_unaccepted_memory(start, end)
	 *    checks up to end+unit_size if 'end' is aligned on a unit_size
	 *    boundary.
	 *
	 * 2. Implicitly extend accept_memory(start, end) to end+unit_size if
	 *    'end' is aligned on a unit_size boundary. (immediately following
	 *    this comment)
	 */
	if (!(end % unit_size))
		end += unit_size;

	/* Make sure not to overrun the bitmap */
	if (end > unaccepted->size * unit_size * BITS_PER_BYTE)
		end = unaccepted->size * unit_size * BITS_PER_BYTE;

	range_start = start / unit_size;

	spin_lock_irqsave(&unaccepted_memory_lock, flags);
	for_each_set_bitrange_from(range_start, range_end, unaccepted->bitmap,
				   DIV_ROUND_UP(end, unit_size)) {
		unsigned long phys_start, phys_end;
		unsigned long len = range_end - range_start;

		phys_start = range_start * unit_size + unaccepted->phys_base;
		phys_end = range_end * unit_size + unaccepted->phys_base;

		arch_accept_memory(phys_start, phys_end);
		bitmap_clear(unaccepted->bitmap, range_start, len);
	}
	spin_unlock_irqrestore(&unaccepted_memory_lock, flags);
}

bool range_contains_unaccepted_memory(phys_addr_t start, phys_addr_t end)
{
	struct efi_unaccepted_memory *unaccepted;
	unsigned long flags;
	bool ret = false;
	u64 unit_size;

	unaccepted = efi_get_unaccepted_table();
	if (!unaccepted)
		return false;

	unit_size = unaccepted->unit_size;

	/*
	 * Only care for the part of the range that is represented
	 * in the bitmap.
	 */
	if (start < unaccepted->phys_base)
		start = unaccepted->phys_base;
	if (end < unaccepted->phys_base)
		return false;

	/* Translate to offsets from the beginning of the bitmap */
	start -= unaccepted->phys_base;
	end -= unaccepted->phys_base;

	/*
	 * Also consider the unaccepted state of the *next* page. See fix #1 in
	 * the comment on load_unaligned_zeropad() in accept_memory().
	 */
	if (!(end % unit_size))
		end += unit_size;

	/* Make sure not to overrun the bitmap */
	if (end > unaccepted->size * unit_size * BITS_PER_BYTE)
		end = unaccepted->size * unit_size * BITS_PER_BYTE;

	spin_lock_irqsave(&unaccepted_memory_lock, flags);
	while (start < end) {
		if (test_bit(start / unit_size, unaccepted->bitmap)) {
			ret = true;
			break;
		}

		start += unit_size;
	}
	spin_unlock_irqrestore(&unaccepted_memory_lock, flags);

	return ret;
}
