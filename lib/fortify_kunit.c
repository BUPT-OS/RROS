// SPDX-License-Identifier: GPL-2.0
/*
 * Runtime test cases for CONFIG_FORTIFY_SOURCE that aren't expected to
 * Oops the kernel on success. (For those, see drivers/misc/lkdtm/fortify.c)
 *
 * For corner cases with UBSAN, try testing with:
 *
 * ./tools/testing/kunit/kunit.py run --arch=x86_64 \
 *	--kconfig_add CONFIG_FORTIFY_SOURCE=y \
 *	--kconfig_add CONFIG_UBSAN=y \
 *	--kconfig_add CONFIG_UBSAN_TRAP=y \
 *	--kconfig_add CONFIG_UBSAN_BOUNDS=y \
 *	--kconfig_add CONFIG_UBSAN_LOCAL_BOUNDS=y \
 *	--make_options LLVM=1 fortify
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <kunit/test.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

static const char array_of_10[] = "this is 10";
static const char *ptr_of_11 = "this is 11!";
static char array_unknown[] = "compiler thinks I might change";

static void known_sizes_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, __compiletime_strlen("88888888"), 8);
	KUNIT_EXPECT_EQ(test, __compiletime_strlen(array_of_10), 10);
	KUNIT_EXPECT_EQ(test, __compiletime_strlen(ptr_of_11), 11);

	KUNIT_EXPECT_EQ(test, __compiletime_strlen(array_unknown), SIZE_MAX);
	/* Externally defined and dynamically sized string pointer: */
	KUNIT_EXPECT_EQ(test, __compiletime_strlen(test->name), SIZE_MAX);
}

/* This is volatile so the optimizer can't perform DCE below. */
static volatile int pick;

/* Not inline to keep optimizer from figuring out which string we want. */
static noinline size_t want_minus_one(int pick)
{
	const char *str;

	switch (pick) {
	case 1:
		str = "4444";
		break;
	case 2:
		str = "333";
		break;
	default:
		str = "1";
		break;
	}
	return __compiletime_strlen(str);
}

static void control_flow_split_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, want_minus_one(pick), SIZE_MAX);
}

#define KUNIT_EXPECT_BOS(test, p, expected, name)			\
	KUNIT_EXPECT_EQ_MSG(test, __builtin_object_size(p, 1),		\
		expected,						\
		"__alloc_size() not working with __bos on " name "\n")

#if !__has_builtin(__builtin_dynamic_object_size)
#define KUNIT_EXPECT_BDOS(test, p, expected, name)			\
	/* Silence "unused variable 'expected'" warning. */		\
	KUNIT_EXPECT_EQ(test, expected, expected)
#else
#define KUNIT_EXPECT_BDOS(test, p, expected, name)			\
	KUNIT_EXPECT_EQ_MSG(test, __builtin_dynamic_object_size(p, 1),	\
		expected,						\
		"__alloc_size() not working with __bdos on " name "\n")
#endif

/* If the execpted size is a constant value, __bos can see it. */
#define check_const(_expected, alloc, free)		do {		\
	size_t expected = (_expected);					\
	void *p = alloc;						\
	KUNIT_EXPECT_TRUE_MSG(test, p != NULL, #alloc " failed?!\n");	\
	KUNIT_EXPECT_BOS(test, p, expected, #alloc);			\
	KUNIT_EXPECT_BDOS(test, p, expected, #alloc);			\
	free;								\
} while (0)

/* If the execpted size is NOT a constant value, __bos CANNOT see it. */
#define check_dynamic(_expected, alloc, free)		do {		\
	size_t expected = (_expected);					\
	void *p = alloc;						\
	KUNIT_EXPECT_TRUE_MSG(test, p != NULL, #alloc " failed?!\n");	\
	KUNIT_EXPECT_BOS(test, p, SIZE_MAX, #alloc);			\
	KUNIT_EXPECT_BDOS(test, p, expected, #alloc);			\
	free;								\
} while (0)

/* Assortment of constant-value kinda-edge cases. */
#define CONST_TEST_BODY(TEST_alloc)	do {				\
	/* Special-case vmalloc()-family to skip 0-sized allocs. */	\
	if (strcmp(#TEST_alloc, "TEST_vmalloc") != 0)			\
		TEST_alloc(check_const, 0, 0);				\
	TEST_alloc(check_const, 1, 1);					\
	TEST_alloc(check_const, 128, 128);				\
	TEST_alloc(check_const, 1023, 1023);				\
	TEST_alloc(check_const, 1025, 1025);				\
	TEST_alloc(check_const, 4096, 4096);				\
	TEST_alloc(check_const, 4097, 4097);				\
} while (0)

static volatile size_t zero_size;
static volatile size_t unknown_size = 50;

#if !__has_builtin(__builtin_dynamic_object_size)
#define DYNAMIC_TEST_BODY(TEST_alloc)					\
	kunit_skip(test, "Compiler is missing __builtin_dynamic_object_size() support\n")
#else
#define DYNAMIC_TEST_BODY(TEST_alloc)	do {				\
	size_t size = unknown_size;					\
									\
	/*								\
	 * Expected size is "size" in each test, before it is then	\
	 * internally incremented in each test.	Requires we disable	\
	 * -Wunsequenced.						\
	 */								\
	TEST_alloc(check_dynamic, size, size++);			\
	/* Make sure incrementing actually happened. */			\
	KUNIT_EXPECT_NE(test, size, unknown_size);			\
} while (0)
#endif

#define DEFINE_ALLOC_SIZE_TEST_PAIR(allocator)				\
static void alloc_size_##allocator##_const_test(struct kunit *test)	\
{									\
	CONST_TEST_BODY(TEST_##allocator);				\
}									\
static void alloc_size_##allocator##_dynamic_test(struct kunit *test)	\
{									\
	DYNAMIC_TEST_BODY(TEST_##allocator);				\
}

#define TEST_kmalloc(checker, expected_size, alloc_size)	do {	\
	gfp_t gfp = GFP_KERNEL | __GFP_NOWARN;				\
	void *orig;							\
	size_t len;							\
									\
	checker(expected_size, kmalloc(alloc_size, gfp),		\
		kfree(p));						\
	checker(expected_size,						\
		kmalloc_node(alloc_size, gfp, NUMA_NO_NODE),		\
		kfree(p));						\
	checker(expected_size, kzalloc(alloc_size, gfp),		\
		kfree(p));						\
	checker(expected_size,						\
		kzalloc_node(alloc_size, gfp, NUMA_NO_NODE),		\
		kfree(p));						\
	checker(expected_size, kcalloc(1, alloc_size, gfp),		\
		kfree(p));						\
	checker(expected_size, kcalloc(alloc_size, 1, gfp),		\
		kfree(p));						\
	checker(expected_size,						\
		kcalloc_node(1, alloc_size, gfp, NUMA_NO_NODE),		\
		kfree(p));						\
	checker(expected_size,						\
		kcalloc_node(alloc_size, 1, gfp, NUMA_NO_NODE),		\
		kfree(p));						\
	checker(expected_size, kmalloc_array(1, alloc_size, gfp),	\
		kfree(p));						\
	checker(expected_size, kmalloc_array(alloc_size, 1, gfp),	\
		kfree(p));						\
	checker(expected_size,						\
		kmalloc_array_node(1, alloc_size, gfp, NUMA_NO_NODE),	\
		kfree(p));						\
	checker(expected_size,						\
		kmalloc_array_node(alloc_size, 1, gfp, NUMA_NO_NODE),	\
		kfree(p));						\
	checker(expected_size, __kmalloc(alloc_size, gfp),		\
		kfree(p));						\
	checker(expected_size,						\
		__kmalloc_node(alloc_size, gfp, NUMA_NO_NODE),		\
		kfree(p));						\
									\
	orig = kmalloc(alloc_size, gfp);				\
	KUNIT_EXPECT_TRUE(test, orig != NULL);				\
	checker((expected_size) * 2,					\
		krealloc(orig, (alloc_size) * 2, gfp),			\
		kfree(p));						\
	orig = kmalloc(alloc_size, gfp);				\
	KUNIT_EXPECT_TRUE(test, orig != NULL);				\
	checker((expected_size) * 2,					\
		krealloc_array(orig, 1, (alloc_size) * 2, gfp),		\
		kfree(p));						\
	orig = kmalloc(alloc_size, gfp);				\
	KUNIT_EXPECT_TRUE(test, orig != NULL);				\
	checker((expected_size) * 2,					\
		krealloc_array(orig, (alloc_size) * 2, 1, gfp),		\
		kfree(p));						\
									\
	len = 11;							\
	/* Using memdup() with fixed size, so force unknown length. */	\
	if (!__builtin_constant_p(expected_size))			\
		len += zero_size;					\
	checker(len, kmemdup("hello there", len, gfp), kfree(p));	\
} while (0)
DEFINE_ALLOC_SIZE_TEST_PAIR(kmalloc)

/* Sizes are in pages, not bytes. */
#define TEST_vmalloc(checker, expected_pages, alloc_pages)	do {	\
	gfp_t gfp = GFP_KERNEL | __GFP_NOWARN;				\
	checker((expected_pages) * PAGE_SIZE,				\
		vmalloc((alloc_pages) * PAGE_SIZE),	   vfree(p));	\
	checker((expected_pages) * PAGE_SIZE,				\
		vzalloc((alloc_pages) * PAGE_SIZE),	   vfree(p));	\
	checker((expected_pages) * PAGE_SIZE,				\
		__vmalloc((alloc_pages) * PAGE_SIZE, gfp), vfree(p));	\
} while (0)
DEFINE_ALLOC_SIZE_TEST_PAIR(vmalloc)

/* Sizes are in pages (and open-coded for side-effects), not bytes. */
#define TEST_kvmalloc(checker, expected_pages, alloc_pages)	do {	\
	gfp_t gfp = GFP_KERNEL | __GFP_NOWARN;				\
	size_t prev_size;						\
	void *orig;							\
									\
	checker((expected_pages) * PAGE_SIZE,				\
		kvmalloc((alloc_pages) * PAGE_SIZE, gfp),		\
		vfree(p));						\
	checker((expected_pages) * PAGE_SIZE,				\
		kvmalloc_node((alloc_pages) * PAGE_SIZE, gfp, NUMA_NO_NODE), \
		vfree(p));						\
	checker((expected_pages) * PAGE_SIZE,				\
		kvzalloc((alloc_pages) * PAGE_SIZE, gfp),		\
		vfree(p));						\
	checker((expected_pages) * PAGE_SIZE,				\
		kvzalloc_node((alloc_pages) * PAGE_SIZE, gfp, NUMA_NO_NODE), \
		vfree(p));						\
	checker((expected_pages) * PAGE_SIZE,				\
		kvcalloc(1, (alloc_pages) * PAGE_SIZE, gfp),		\
		vfree(p));						\
	checker((expected_pages) * PAGE_SIZE,				\
		kvcalloc((alloc_pages) * PAGE_SIZE, 1, gfp),		\
		vfree(p));						\
	checker((expected_pages) * PAGE_SIZE,				\
		kvmalloc_array(1, (alloc_pages) * PAGE_SIZE, gfp),	\
		vfree(p));						\
	checker((expected_pages) * PAGE_SIZE,				\
		kvmalloc_array((alloc_pages) * PAGE_SIZE, 1, gfp),	\
		vfree(p));						\
									\
	prev_size = (expected_pages) * PAGE_SIZE;			\
	orig = kvmalloc(prev_size, gfp);				\
	KUNIT_EXPECT_TRUE(test, orig != NULL);				\
	checker(((expected_pages) * PAGE_SIZE) * 2,			\
		kvrealloc(orig, prev_size,				\
			  ((alloc_pages) * PAGE_SIZE) * 2, gfp),	\
		kvfree(p));						\
} while (0)
DEFINE_ALLOC_SIZE_TEST_PAIR(kvmalloc)

#define TEST_devm_kmalloc(checker, expected_size, alloc_size)	do {	\
	gfp_t gfp = GFP_KERNEL | __GFP_NOWARN;				\
	const char dev_name[] = "fortify-test";				\
	struct device *dev;						\
	void *orig;							\
	size_t len;							\
									\
	/* Create dummy device for devm_kmalloc()-family tests. */	\
	dev = root_device_register(dev_name);				\
	KUNIT_ASSERT_FALSE_MSG(test, IS_ERR(dev),			\
			       "Cannot register test device\n");	\
									\
	checker(expected_size, devm_kmalloc(dev, alloc_size, gfp),	\
		devm_kfree(dev, p));					\
	checker(expected_size, devm_kzalloc(dev, alloc_size, gfp),	\
		devm_kfree(dev, p));					\
	checker(expected_size,						\
		devm_kmalloc_array(dev, 1, alloc_size, gfp),		\
		devm_kfree(dev, p));					\
	checker(expected_size,						\
		devm_kmalloc_array(dev, alloc_size, 1, gfp),		\
		devm_kfree(dev, p));					\
	checker(expected_size,						\
		devm_kcalloc(dev, 1, alloc_size, gfp),			\
		devm_kfree(dev, p));					\
	checker(expected_size,						\
		devm_kcalloc(dev, alloc_size, 1, gfp),			\
		devm_kfree(dev, p));					\
									\
	orig = devm_kmalloc(dev, alloc_size, gfp);			\
	KUNIT_EXPECT_TRUE(test, orig != NULL);				\
	checker((expected_size) * 2,					\
		devm_krealloc(dev, orig, (alloc_size) * 2, gfp),	\
		devm_kfree(dev, p));					\
									\
	len = 4;							\
	/* Using memdup() with fixed size, so force unknown length. */	\
	if (!__builtin_constant_p(expected_size))			\
		len += zero_size;					\
	checker(len, devm_kmemdup(dev, "Ohai", len, gfp),		\
		devm_kfree(dev, p));					\
									\
	device_unregister(dev);						\
} while (0)
DEFINE_ALLOC_SIZE_TEST_PAIR(devm_kmalloc)

static struct kunit_case fortify_test_cases[] = {
	KUNIT_CASE(known_sizes_test),
	KUNIT_CASE(control_flow_split_test),
	KUNIT_CASE(alloc_size_kmalloc_const_test),
	KUNIT_CASE(alloc_size_kmalloc_dynamic_test),
	KUNIT_CASE(alloc_size_vmalloc_const_test),
	KUNIT_CASE(alloc_size_vmalloc_dynamic_test),
	KUNIT_CASE(alloc_size_kvmalloc_const_test),
	KUNIT_CASE(alloc_size_kvmalloc_dynamic_test),
	KUNIT_CASE(alloc_size_devm_kmalloc_const_test),
	KUNIT_CASE(alloc_size_devm_kmalloc_dynamic_test),
	{}
};

static struct kunit_suite fortify_test_suite = {
	.name = "fortify",
	.test_cases = fortify_test_cases,
};

kunit_test_suite(fortify_test_suite);

MODULE_LICENSE("GPL");
