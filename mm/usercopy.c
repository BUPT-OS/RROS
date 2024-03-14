// SPDX-License-Identifier: GPL-2.0-only
/*
 * This implements the various checks for CONFIG_HARDENED_USERCOPY*,
 * which are designed to protect kernel memory from needless exposure
 * and overwrite under many unintended conditions. This code is based
 * on PAX_USERCOPY, which is:
 *
 * Copyright (C) 2001-2016 PaX Team, Bradley Spengler, Open Source
 * Security Inc.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/kstrtox.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/thread_info.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>
#include <linux/jump_label.h>
#include <asm/sections.h>
#include "slab.h"

/*
 * Checks if a given pointer and length is contained by the current
 * stack frame (if possible).
 *
 * Returns:
 *	NOT_STACK: not at all on the stack
 *	GOOD_FRAME: fully within a valid stack frame
 *	GOOD_STACK: within the current stack (when can't frame-check exactly)
 *	BAD_STACK: error condition (invalid stack position or bad stack frame)
 */
static noinline int check_stack_object(const void *obj, unsigned long len)
{
	const void * const stack = task_stack_page(current);
	const void * const stackend = stack + THREAD_SIZE;
	int ret;

	/* Object is not on the stack at all. */
	if (obj + len <= stack || stackend <= obj)
		return NOT_STACK;

	/*
	 * Reject: object partially overlaps the stack (passing the
	 * check above means at least one end is within the stack,
	 * so if this check fails, the other end is outside the stack).
	 */
	if (obj < stack || stackend < obj + len)
		return BAD_STACK;

	/* Check if object is safely within a valid frame. */
	ret = arch_within_stack_frames(stack, stackend, obj, len);
	if (ret)
		return ret;

	/* Finally, check stack depth if possible. */
#ifdef CONFIG_ARCH_HAS_CURRENT_STACK_POINTER
	if (IS_ENABLED(CONFIG_STACK_GROWSUP)) {
		if ((void *)current_stack_pointer < obj + len)
			return BAD_STACK;
	} else {
		if (obj < (void *)current_stack_pointer)
			return BAD_STACK;
	}
#endif

	return GOOD_STACK;
}

/*
 * If these functions are reached, then CONFIG_HARDENED_USERCOPY has found
 * an unexpected state during a copy_from_user() or copy_to_user() call.
 * There are several checks being performed on the buffer by the
 * __check_object_size() function. Normal stack buffer usage should never
 * trip the checks, and kernel text addressing will always trip the check.
 * For cache objects, it is checking that only the whitelisted range of
 * bytes for a given cache is being accessed (via the cache's usersize and
 * useroffset fields). To adjust a cache whitelist, use the usercopy-aware
 * kmem_cache_create_usercopy() function to create the cache (and
 * carefully audit the whitelist range).
 */
void __noreturn usercopy_abort(const char *name, const char *detail,
			       bool to_user, unsigned long offset,
			       unsigned long len)
{
	pr_emerg("Kernel memory %s attempt detected %s %s%s%s%s (offset %lu, size %lu)!\n",
		 to_user ? "exposure" : "overwrite",
		 to_user ? "from" : "to",
		 name ? : "unknown?!",
		 detail ? " '" : "", detail ? : "", detail ? "'" : "",
		 offset, len);

	/*
	 * For greater effect, it would be nice to do do_group_exit(),
	 * but BUG() actually hooks all the lock-breaking and per-arch
	 * Oops code, so that is used here instead.
	 */
	BUG();
}

/* Returns true if any portion of [ptr,ptr+n) over laps with [low,high). */
static bool overlaps(const unsigned long ptr, unsigned long n,
		     unsigned long low, unsigned long high)
{
	const unsigned long check_low = ptr;
	unsigned long check_high = check_low + n;

	/* Does not overlap if entirely above or entirely below. */
	if (check_low >= high || check_high <= low)
		return false;

	return true;
}

/* Is this address range in the kernel text area? */
static inline void check_kernel_text_object(const unsigned long ptr,
					    unsigned long n, bool to_user)
{
	unsigned long textlow = (unsigned long)_stext;
	unsigned long texthigh = (unsigned long)_etext;
	unsigned long textlow_linear, texthigh_linear;

	if (overlaps(ptr, n, textlow, texthigh))
		usercopy_abort("kernel text", NULL, to_user, ptr - textlow, n);

	/*
	 * Some architectures have virtual memory mappings with a secondary
	 * mapping of the kernel text, i.e. there is more than one virtual
	 * kernel address that points to the kernel image. It is usually
	 * when there is a separate linear physical memory mapping, in that
	 * __pa() is not just the reverse of __va(). This can be detected
	 * and checked:
	 */
	textlow_linear = (unsigned long)lm_alias(textlow);
	/* No different mapping: we're done. */
	if (textlow_linear == textlow)
		return;

	/* Check the secondary mapping... */
	texthigh_linear = (unsigned long)lm_alias(texthigh);
	if (overlaps(ptr, n, textlow_linear, texthigh_linear))
		usercopy_abort("linear kernel text", NULL, to_user,
			       ptr - textlow_linear, n);
}

static inline void check_bogus_address(const unsigned long ptr, unsigned long n,
				       bool to_user)
{
	/* Reject if object wraps past end of memory. */
	if (ptr + (n - 1) < ptr)
		usercopy_abort("wrapped address", NULL, to_user, 0, ptr + n);

	/* Reject if NULL or ZERO-allocation. */
	if (ZERO_OR_NULL_PTR(ptr))
		usercopy_abort("null address", NULL, to_user, ptr, n);
}

static inline void check_heap_object(const void *ptr, unsigned long n,
				     bool to_user)
{
	unsigned long addr = (unsigned long)ptr;
	unsigned long offset;
	struct folio *folio;

	if (is_kmap_addr(ptr)) {
		offset = offset_in_page(ptr);
		if (n > PAGE_SIZE - offset)
			usercopy_abort("kmap", NULL, to_user, offset, n);
		return;
	}

	if (is_vmalloc_addr(ptr) && !pagefault_disabled()) {
		struct vmap_area *area = find_vmap_area(addr);

		if (!area)
			usercopy_abort("vmalloc", "no area", to_user, 0, n);

		if (n > area->va_end - addr) {
			offset = addr - area->va_start;
			usercopy_abort("vmalloc", NULL, to_user, offset, n);
		}
		return;
	}

	if (!virt_addr_valid(ptr))
		return;

	folio = virt_to_folio(ptr);

	if (folio_test_slab(folio)) {
		/* Check slab allocator for flags and size. */
		__check_heap_object(ptr, n, folio_slab(folio), to_user);
	} else if (folio_test_large(folio)) {
		offset = ptr - folio_address(folio);
		if (n > folio_size(folio) - offset)
			usercopy_abort("page alloc", NULL, to_user, offset, n);
	}
}

static DEFINE_STATIC_KEY_FALSE_RO(bypass_usercopy_checks);

/*
 * Validates that the given object is:
 * - not bogus address
 * - fully contained by stack (or stack frame, when available)
 * - fully within SLAB object (or object whitelist area, when available)
 * - not in kernel text
 */
void __check_object_size(const void *ptr, unsigned long n, bool to_user)
{
	if (static_branch_unlikely(&bypass_usercopy_checks))
		return;

	/* Skip all tests if size is zero. */
	if (!n)
		return;

	/* Check for invalid addresses. */
	check_bogus_address((const unsigned long)ptr, n, to_user);

	/* Check for bad stack object. */
	switch (check_stack_object(ptr, n)) {
	case NOT_STACK:
		/* Object is not touching the current process stack. */
		break;
	case GOOD_FRAME:
	case GOOD_STACK:
		/*
		 * Object is either in the correct frame (when it
		 * is possible to check) or just generally on the
		 * process stack (when frame checking not available).
		 */
		return;
	default:
		usercopy_abort("process stack", NULL, to_user,
#ifdef CONFIG_ARCH_HAS_CURRENT_STACK_POINTER
			IS_ENABLED(CONFIG_STACK_GROWSUP) ?
				ptr - (void *)current_stack_pointer :
				(void *)current_stack_pointer - ptr,
#else
			0,
#endif
			n);
	}

	/* Check for bad heap object. */
	check_heap_object(ptr, n, to_user);

	/* Check for object in kernel to avoid text exposure. */
	check_kernel_text_object((const unsigned long)ptr, n, to_user);
}
EXPORT_SYMBOL(__check_object_size);

static bool enable_checks __initdata = true;

static int __init parse_hardened_usercopy(char *str)
{
	if (kstrtobool(str, &enable_checks))
		pr_warn("Invalid option string for hardened_usercopy: '%s'\n",
			str);
	return 1;
}

__setup("hardened_usercopy=", parse_hardened_usercopy);

static int __init set_hardened_usercopy(void)
{
	if (enable_checks == false)
		static_branch_enable(&bypass_usercopy_checks);
	return 1;
}

late_initcall(set_hardened_usercopy);
