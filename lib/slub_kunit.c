// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include "../mm/slab.h"

static struct kunit_resource resource;
static int slab_errors;

/*
 * Wrapper function for kmem_cache_create(), which reduces 2 parameters:
 * 'align' and 'ctor', and sets SLAB_SKIP_KFENCE flag to avoid getting an
 * object from kfence pool, where the operation could be caught by both
 * our test and kfence sanity check.
 */
static struct kmem_cache *test_kmem_cache_create(const char *name,
				unsigned int size, slab_flags_t flags)
{
	struct kmem_cache *s = kmem_cache_create(name, size, 0,
					(flags | SLAB_NO_USER_FLAGS), NULL);
	s->flags |= SLAB_SKIP_KFENCE;
	return s;
}

static void test_clobber_zone(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_RZ_alloc", 64,
							SLAB_RED_ZONE);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kasan_disable_current();
	p[64] = 0x12;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kasan_enable_current();
	kmem_cache_free(s, p);
	kmem_cache_destroy(s);
}

#ifndef CONFIG_KASAN
static void test_next_pointer(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_next_ptr_free",
							64, SLAB_POISON);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);
	unsigned long tmp;
	unsigned long *ptr_addr;

	kmem_cache_free(s, p);

	ptr_addr = (unsigned long *)(p + s->offset);
	tmp = *ptr_addr;
	p[s->offset] = 0x12;

	/*
	 * Expecting three errors.
	 * One for the corrupted freechain and the other one for the wrong
	 * count of objects in use. The third error is fixing broken cache.
	 */
	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 3, slab_errors);

	/*
	 * Try to repair corrupted freepointer.
	 * Still expecting two errors. The first for the wrong count
	 * of objects in use.
	 * The second error is for fixing broken cache.
	 */
	*ptr_addr = tmp;
	slab_errors = 0;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	/*
	 * Previous validation repaired the count of objects in use.
	 * Now expecting no error.
	 */
	slab_errors = 0;
	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 0, slab_errors);

	kmem_cache_destroy(s);
}

static void test_first_word(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_1th_word_free",
							64, SLAB_POISON);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	*p = 0x78;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kmem_cache_destroy(s);
}

static void test_clobber_50th_byte(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_50th_word_free",
							64, SLAB_POISON);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	p[50] = 0x9a;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kmem_cache_destroy(s);
}
#endif

static void test_clobber_redzone_free(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_RZ_free", 64,
							SLAB_RED_ZONE);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kasan_disable_current();
	kmem_cache_free(s, p);
	p[64] = 0xab;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kasan_enable_current();
	kmem_cache_destroy(s);
}

static void test_kmalloc_redzone_access(struct kunit *test)
{
	struct kmem_cache *s = test_kmem_cache_create("TestSlub_RZ_kmalloc", 32,
				SLAB_KMALLOC|SLAB_STORE_USER|SLAB_RED_ZONE);
	u8 *p = kmalloc_trace(s, GFP_KERNEL, 18);

	kasan_disable_current();

	/* Suppress the -Warray-bounds warning */
	OPTIMIZER_HIDE_VAR(p);
	p[18] = 0xab;
	p[19] = 0xab;

	validate_slab_cache(s);
	KUNIT_EXPECT_EQ(test, 2, slab_errors);

	kasan_enable_current();
	kmem_cache_free(s, p);
	kmem_cache_destroy(s);
}

static int test_init(struct kunit *test)
{
	slab_errors = 0;

	kunit_add_named_resource(test, NULL, NULL, &resource,
					"slab_errors", &slab_errors);
	return 0;
}

static struct kunit_case test_cases[] = {
	KUNIT_CASE(test_clobber_zone),

#ifndef CONFIG_KASAN
	KUNIT_CASE(test_next_pointer),
	KUNIT_CASE(test_first_word),
	KUNIT_CASE(test_clobber_50th_byte),
#endif

	KUNIT_CASE(test_clobber_redzone_free),
	KUNIT_CASE(test_kmalloc_redzone_access),
	{}
};

static struct kunit_suite test_suite = {
	.name = "slub_test",
	.init = test_init,
	.test_cases = test_cases,
};
kunit_test_suite(test_suite);

MODULE_LICENSE("GPL");
