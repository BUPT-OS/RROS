/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stack depot - a stack trace storage that avoids duplication.
 *
 * Stack depot is intended to be used by subsystems that need to store and
 * later retrieve many potentially duplicated stack traces without wasting
 * memory.
 *
 * For example, KASAN needs to save allocation and free stack traces for each
 * object. Storing two stack traces per object requires a lot of memory (e.g.
 * SLUB_DEBUG needs 256 bytes per object for that). Since allocation and free
 * stack traces often repeat, using stack depot allows to save about 100x space.
 *
 * Stack traces are never removed from the stack depot.
 *
 * Author: Alexander Potapenko <glider@google.com>
 * Copyright (C) 2016 Google, Inc.
 *
 * Based on the code by Dmitry Chernenkov.
 */

#ifndef _LINUX_STACKDEPOT_H
#define _LINUX_STACKDEPOT_H

#include <linux/gfp.h>

typedef u32 depot_stack_handle_t;

/*
 * Number of bits in the handle that stack depot doesn't use. Users may store
 * information in them via stack_depot_set/get_extra_bits.
 */
#define STACK_DEPOT_EXTRA_BITS 5

/*
 * Using stack depot requires its initialization, which can be done in 3 ways:
 *
 * 1. Selecting CONFIG_STACKDEPOT_ALWAYS_INIT. This option is suitable in
 *    scenarios where it's known at compile time that stack depot will be used.
 *    Enabling this config makes the kernel initialize stack depot in mm_init().
 *
 * 2. Calling stack_depot_request_early_init() during early boot, before
 *    stack_depot_early_init() in mm_init() completes. For example, this can
 *    be done when evaluating kernel boot parameters.
 *
 * 3. Calling stack_depot_init(). Possible after boot is complete. This option
 *    is recommended for modules initialized later in the boot process, after
 *    mm_init() completes.
 *
 * stack_depot_init() and stack_depot_request_early_init() can be called
 * regardless of whether CONFIG_STACKDEPOT is enabled and are no-op when this
 * config is disabled. The save/fetch/print stack depot functions can only be
 * called from the code that makes sure CONFIG_STACKDEPOT is enabled _and_
 * initializes stack depot via one of the ways listed above.
 */
#ifdef CONFIG_STACKDEPOT
int stack_depot_init(void);

void __init stack_depot_request_early_init(void);

/* Must be only called from mm_init(). */
int __init stack_depot_early_init(void);
#else
static inline int stack_depot_init(void) { return 0; }

static inline void stack_depot_request_early_init(void) { }

static inline int stack_depot_early_init(void)	{ return 0; }
#endif

/**
 * __stack_depot_save - Save a stack trace to stack depot
 *
 * @entries:		Pointer to the stack trace
 * @nr_entries:		Number of frames in the stack
 * @alloc_flags:	Allocation GFP flags
 * @can_alloc:		Allocate stack pools (increased chance of failure if false)
 *
 * Saves a stack trace from @entries array of size @nr_entries. If @can_alloc is
 * %true, stack depot can replenish the stack pools in case no space is left
 * (allocates using GFP flags of @alloc_flags). If @can_alloc is %false, avoids
 * any allocations and fails if no space is left to store the stack trace.
 *
 * If the provided stack trace comes from the interrupt context, only the part
 * up to the interrupt entry is saved.
 *
 * Context: Any context, but setting @can_alloc to %false is required if
 *          alloc_pages() cannot be used from the current context. Currently
 *          this is the case for contexts where neither %GFP_ATOMIC nor
 *          %GFP_NOWAIT can be used (NMI, raw_spin_lock).
 *
 * Return: Handle of the stack struct stored in depot, 0 on failure
 */
depot_stack_handle_t __stack_depot_save(unsigned long *entries,
					unsigned int nr_entries,
					gfp_t gfp_flags, bool can_alloc);

/**
 * stack_depot_save - Save a stack trace to stack depot
 *
 * @entries:		Pointer to the stack trace
 * @nr_entries:		Number of frames in the stack
 * @alloc_flags:	Allocation GFP flags
 *
 * Context: Contexts where allocations via alloc_pages() are allowed.
 *          See __stack_depot_save() for more details.
 *
 * Return: Handle of the stack trace stored in depot, 0 on failure
 */
depot_stack_handle_t stack_depot_save(unsigned long *entries,
				      unsigned int nr_entries, gfp_t gfp_flags);

/**
 * stack_depot_fetch - Fetch a stack trace from stack depot
 *
 * @handle:	Stack depot handle returned from stack_depot_save()
 * @entries:	Pointer to store the address of the stack trace
 *
 * Return: Number of frames for the fetched stack
 */
unsigned int stack_depot_fetch(depot_stack_handle_t handle,
			       unsigned long **entries);

/**
 * stack_depot_print - Print a stack trace from stack depot
 *
 * @stack:	Stack depot handle returned from stack_depot_save()
 */
void stack_depot_print(depot_stack_handle_t stack);

/**
 * stack_depot_snprint - Print a stack trace from stack depot into a buffer
 *
 * @handle:	Stack depot handle returned from stack_depot_save()
 * @buf:	Pointer to the print buffer
 * @size:	Size of the print buffer
 * @spaces:	Number of leading spaces to print
 *
 * Return:	Number of bytes printed
 */
int stack_depot_snprint(depot_stack_handle_t handle, char *buf, size_t size,
		       int spaces);

/**
 * stack_depot_set_extra_bits - Set extra bits in a stack depot handle
 *
 * @handle:	Stack depot handle returned from stack_depot_save()
 * @extra_bits:	Value to set the extra bits
 *
 * Return: Stack depot handle with extra bits set
 *
 * Stack depot handles have a few unused bits, which can be used for storing
 * user-specific information. These bits are transparent to the stack depot.
 */
depot_stack_handle_t __must_check stack_depot_set_extra_bits(
			depot_stack_handle_t handle, unsigned int extra_bits);

/**
 * stack_depot_get_extra_bits - Retrieve extra bits from a stack depot handle
 *
 * @handle:	Stack depot handle with extra bits saved
 *
 * Return: Extra bits retrieved from the stack depot handle
 */
unsigned int stack_depot_get_extra_bits(depot_stack_handle_t handle);

#endif
