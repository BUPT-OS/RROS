// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 - Virtual Open Systems and Columbia University
 * Author: Christoffer Dall <c.dall@virtualopensystems.com>
 */

#include <linux/mman.h>
#include <linux/kvm_host.h>
#include <linux/io.h>
#include <linux/hugetlb.h>
#include <linux/sched/signal.h>
#include <trace/events/kvm.h>
#include <asm/pgalloc.h>
#include <asm/cacheflush.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/kvm_ras.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_emulate.h>
#include <asm/virt.h>

#include "trace.h"

static struct kvm_pgtable *hyp_pgtable;
static DEFINE_MUTEX(kvm_hyp_pgd_mutex);

static unsigned long __ro_after_init hyp_idmap_start;
static unsigned long __ro_after_init hyp_idmap_end;
static phys_addr_t __ro_after_init hyp_idmap_vector;

static unsigned long __ro_after_init io_map_base;

static phys_addr_t __stage2_range_addr_end(phys_addr_t addr, phys_addr_t end,
					   phys_addr_t size)
{
	phys_addr_t boundary = ALIGN_DOWN(addr + size, size);

	return (boundary - 1 < end - 1) ? boundary : end;
}

static phys_addr_t stage2_range_addr_end(phys_addr_t addr, phys_addr_t end)
{
	phys_addr_t size = kvm_granule_size(KVM_PGTABLE_MIN_BLOCK_LEVEL);

	return __stage2_range_addr_end(addr, end, size);
}

/*
 * Release kvm_mmu_lock periodically if the memory region is large. Otherwise,
 * we may see kernel panics with CONFIG_DETECT_HUNG_TASK,
 * CONFIG_LOCKUP_DETECTOR, CONFIG_LOCKDEP. Additionally, holding the lock too
 * long will also starve other vCPUs. We have to also make sure that the page
 * tables are not freed while we released the lock.
 */
static int stage2_apply_range(struct kvm_s2_mmu *mmu, phys_addr_t addr,
			      phys_addr_t end,
			      int (*fn)(struct kvm_pgtable *, u64, u64),
			      bool resched)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(mmu);
	int ret;
	u64 next;

	do {
		struct kvm_pgtable *pgt = mmu->pgt;
		if (!pgt)
			return -EINVAL;

		next = stage2_range_addr_end(addr, end);
		ret = fn(pgt, addr, next - addr);
		if (ret)
			break;

		if (resched && next != end)
			cond_resched_rwlock_write(&kvm->mmu_lock);
	} while (addr = next, addr != end);

	return ret;
}

#define stage2_apply_range_resched(mmu, addr, end, fn)			\
	stage2_apply_range(mmu, addr, end, fn, true)

/*
 * Get the maximum number of page-tables pages needed to split a range
 * of blocks into PAGE_SIZE PTEs. It assumes the range is already
 * mapped at level 2, or at level 1 if allowed.
 */
static int kvm_mmu_split_nr_page_tables(u64 range)
{
	int n = 0;

	if (KVM_PGTABLE_MIN_BLOCK_LEVEL < 2)
		n += DIV_ROUND_UP(range, PUD_SIZE);
	n += DIV_ROUND_UP(range, PMD_SIZE);
	return n;
}

static bool need_split_memcache_topup_or_resched(struct kvm *kvm)
{
	struct kvm_mmu_memory_cache *cache;
	u64 chunk_size, min;

	if (need_resched() || rwlock_needbreak(&kvm->mmu_lock))
		return true;

	chunk_size = kvm->arch.mmu.split_page_chunk_size;
	min = kvm_mmu_split_nr_page_tables(chunk_size);
	cache = &kvm->arch.mmu.split_page_cache;
	return kvm_mmu_memory_cache_nr_free_objects(cache) < min;
}

static int kvm_mmu_split_huge_pages(struct kvm *kvm, phys_addr_t addr,
				    phys_addr_t end)
{
	struct kvm_mmu_memory_cache *cache;
	struct kvm_pgtable *pgt;
	int ret, cache_capacity;
	u64 next, chunk_size;

	lockdep_assert_held_write(&kvm->mmu_lock);

	chunk_size = kvm->arch.mmu.split_page_chunk_size;
	cache_capacity = kvm_mmu_split_nr_page_tables(chunk_size);

	if (chunk_size == 0)
		return 0;

	cache = &kvm->arch.mmu.split_page_cache;

	do {
		if (need_split_memcache_topup_or_resched(kvm)) {
			write_unlock(&kvm->mmu_lock);
			cond_resched();
			/* Eager page splitting is best-effort. */
			ret = __kvm_mmu_topup_memory_cache(cache,
							   cache_capacity,
							   cache_capacity);
			write_lock(&kvm->mmu_lock);
			if (ret)
				break;
		}

		pgt = kvm->arch.mmu.pgt;
		if (!pgt)
			return -EINVAL;

		next = __stage2_range_addr_end(addr, end, chunk_size);
		ret = kvm_pgtable_stage2_split(pgt, addr, next - addr, cache);
		if (ret)
			break;
	} while (addr = next, addr != end);

	return ret;
}

static bool memslot_is_logging(struct kvm_memory_slot *memslot)
{
	return memslot->dirty_bitmap && !(memslot->flags & KVM_MEM_READONLY);
}

/**
 * kvm_arch_flush_remote_tlbs() - flush all VM TLB entries for v7/8
 * @kvm:	pointer to kvm structure.
 *
 * Interface to HYP function to flush all VM TLB entries
 */
int kvm_arch_flush_remote_tlbs(struct kvm *kvm)
{
	kvm_call_hyp(__kvm_tlb_flush_vmid, &kvm->arch.mmu);
	return 0;
}

int kvm_arch_flush_remote_tlbs_range(struct kvm *kvm,
				      gfn_t gfn, u64 nr_pages)
{
	kvm_tlb_flush_vmid_range(&kvm->arch.mmu,
				gfn << PAGE_SHIFT, nr_pages << PAGE_SHIFT);
	return 0;
}

static bool kvm_is_device_pfn(unsigned long pfn)
{
	return !pfn_is_map_memory(pfn);
}

static void *stage2_memcache_zalloc_page(void *arg)
{
	struct kvm_mmu_memory_cache *mc = arg;
	void *virt;

	/* Allocated with __GFP_ZERO, so no need to zero */
	virt = kvm_mmu_memory_cache_alloc(mc);
	if (virt)
		kvm_account_pgtable_pages(virt, 1);
	return virt;
}

static void *kvm_host_zalloc_pages_exact(size_t size)
{
	return alloc_pages_exact(size, GFP_KERNEL_ACCOUNT | __GFP_ZERO);
}

static void *kvm_s2_zalloc_pages_exact(size_t size)
{
	void *virt = kvm_host_zalloc_pages_exact(size);

	if (virt)
		kvm_account_pgtable_pages(virt, (size >> PAGE_SHIFT));
	return virt;
}

static void kvm_s2_free_pages_exact(void *virt, size_t size)
{
	kvm_account_pgtable_pages(virt, -(size >> PAGE_SHIFT));
	free_pages_exact(virt, size);
}

static struct kvm_pgtable_mm_ops kvm_s2_mm_ops;

static void stage2_free_unlinked_table_rcu_cb(struct rcu_head *head)
{
	struct page *page = container_of(head, struct page, rcu_head);
	void *pgtable = page_to_virt(page);
	u32 level = page_private(page);

	kvm_pgtable_stage2_free_unlinked(&kvm_s2_mm_ops, pgtable, level);
}

static void stage2_free_unlinked_table(void *addr, u32 level)
{
	struct page *page = virt_to_page(addr);

	set_page_private(page, (unsigned long)level);
	call_rcu(&page->rcu_head, stage2_free_unlinked_table_rcu_cb);
}

static void kvm_host_get_page(void *addr)
{
	get_page(virt_to_page(addr));
}

static void kvm_host_put_page(void *addr)
{
	put_page(virt_to_page(addr));
}

static void kvm_s2_put_page(void *addr)
{
	struct page *p = virt_to_page(addr);
	/* Dropping last refcount, the page will be freed */
	if (page_count(p) == 1)
		kvm_account_pgtable_pages(addr, -1);
	put_page(p);
}

static int kvm_host_page_count(void *addr)
{
	return page_count(virt_to_page(addr));
}

static phys_addr_t kvm_host_pa(void *addr)
{
	return __pa(addr);
}

static void *kvm_host_va(phys_addr_t phys)
{
	return __va(phys);
}

static void clean_dcache_guest_page(void *va, size_t size)
{
	__clean_dcache_guest_page(va, size);
}

static void invalidate_icache_guest_page(void *va, size_t size)
{
	__invalidate_icache_guest_page(va, size);
}

/*
 * Unmapping vs dcache management:
 *
 * If a guest maps certain memory pages as uncached, all writes will
 * bypass the data cache and go directly to RAM.  However, the CPUs
 * can still speculate reads (not writes) and fill cache lines with
 * data.
 *
 * Those cache lines will be *clean* cache lines though, so a
 * clean+invalidate operation is equivalent to an invalidate
 * operation, because no cache lines are marked dirty.
 *
 * Those clean cache lines could be filled prior to an uncached write
 * by the guest, and the cache coherent IO subsystem would therefore
 * end up writing old data to disk.
 *
 * This is why right after unmapping a page/section and invalidating
 * the corresponding TLBs, we flush to make sure the IO subsystem will
 * never hit in the cache.
 *
 * This is all avoided on systems that have ARM64_HAS_STAGE2_FWB, as
 * we then fully enforce cacheability of RAM, no matter what the guest
 * does.
 */
/**
 * unmap_stage2_range -- Clear stage2 page table entries to unmap a range
 * @mmu:   The KVM stage-2 MMU pointer
 * @start: The intermediate physical base address of the range to unmap
 * @size:  The size of the area to unmap
 * @may_block: Whether or not we are permitted to block
 *
 * Clear a range of stage-2 mappings, lowering the various ref-counts.  Must
 * be called while holding mmu_lock (unless for freeing the stage2 pgd before
 * destroying the VM), otherwise another faulting VCPU may come in and mess
 * with things behind our backs.
 */
static void __unmap_stage2_range(struct kvm_s2_mmu *mmu, phys_addr_t start, u64 size,
				 bool may_block)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(mmu);
	phys_addr_t end = start + size;

	lockdep_assert_held_write(&kvm->mmu_lock);
	WARN_ON(size & ~PAGE_MASK);
	WARN_ON(stage2_apply_range(mmu, start, end, kvm_pgtable_stage2_unmap,
				   may_block));
}

static void unmap_stage2_range(struct kvm_s2_mmu *mmu, phys_addr_t start, u64 size)
{
	__unmap_stage2_range(mmu, start, size, true);
}

static void stage2_flush_memslot(struct kvm *kvm,
				 struct kvm_memory_slot *memslot)
{
	phys_addr_t addr = memslot->base_gfn << PAGE_SHIFT;
	phys_addr_t end = addr + PAGE_SIZE * memslot->npages;

	stage2_apply_range_resched(&kvm->arch.mmu, addr, end, kvm_pgtable_stage2_flush);
}

/**
 * stage2_flush_vm - Invalidate cache for pages mapped in stage 2
 * @kvm: The struct kvm pointer
 *
 * Go through the stage 2 page tables and invalidate any cache lines
 * backing memory already mapped to the VM.
 */
static void stage2_flush_vm(struct kvm *kvm)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	int idx, bkt;

	idx = srcu_read_lock(&kvm->srcu);
	write_lock(&kvm->mmu_lock);

	slots = kvm_memslots(kvm);
	kvm_for_each_memslot(memslot, bkt, slots)
		stage2_flush_memslot(kvm, memslot);

	write_unlock(&kvm->mmu_lock);
	srcu_read_unlock(&kvm->srcu, idx);
}

/**
 * free_hyp_pgds - free Hyp-mode page tables
 */
void __init free_hyp_pgds(void)
{
	mutex_lock(&kvm_hyp_pgd_mutex);
	if (hyp_pgtable) {
		kvm_pgtable_hyp_destroy(hyp_pgtable);
		kfree(hyp_pgtable);
		hyp_pgtable = NULL;
	}
	mutex_unlock(&kvm_hyp_pgd_mutex);
}

static bool kvm_host_owns_hyp_mappings(void)
{
	if (is_kernel_in_hyp_mode())
		return false;

	if (static_branch_likely(&kvm_protected_mode_initialized))
		return false;

	/*
	 * This can happen at boot time when __create_hyp_mappings() is called
	 * after the hyp protection has been enabled, but the static key has
	 * not been flipped yet.
	 */
	if (!hyp_pgtable && is_protected_kvm_enabled())
		return false;

	WARN_ON(!hyp_pgtable);

	return true;
}

int __create_hyp_mappings(unsigned long start, unsigned long size,
			  unsigned long phys, enum kvm_pgtable_prot prot)
{
	int err;

	if (WARN_ON(!kvm_host_owns_hyp_mappings()))
		return -EINVAL;

	mutex_lock(&kvm_hyp_pgd_mutex);
	err = kvm_pgtable_hyp_map(hyp_pgtable, start, size, phys, prot);
	mutex_unlock(&kvm_hyp_pgd_mutex);

	return err;
}

static phys_addr_t kvm_kaddr_to_phys(void *kaddr)
{
	if (!is_vmalloc_addr(kaddr)) {
		BUG_ON(!virt_addr_valid(kaddr));
		return __pa(kaddr);
	} else {
		return page_to_phys(vmalloc_to_page(kaddr)) +
		       offset_in_page(kaddr);
	}
}

struct hyp_shared_pfn {
	u64 pfn;
	int count;
	struct rb_node node;
};

static DEFINE_MUTEX(hyp_shared_pfns_lock);
static struct rb_root hyp_shared_pfns = RB_ROOT;

static struct hyp_shared_pfn *find_shared_pfn(u64 pfn, struct rb_node ***node,
					      struct rb_node **parent)
{
	struct hyp_shared_pfn *this;

	*node = &hyp_shared_pfns.rb_node;
	*parent = NULL;
	while (**node) {
		this = container_of(**node, struct hyp_shared_pfn, node);
		*parent = **node;
		if (this->pfn < pfn)
			*node = &((**node)->rb_left);
		else if (this->pfn > pfn)
			*node = &((**node)->rb_right);
		else
			return this;
	}

	return NULL;
}

static int share_pfn_hyp(u64 pfn)
{
	struct rb_node **node, *parent;
	struct hyp_shared_pfn *this;
	int ret = 0;

	mutex_lock(&hyp_shared_pfns_lock);
	this = find_shared_pfn(pfn, &node, &parent);
	if (this) {
		this->count++;
		goto unlock;
	}

	this = kzalloc(sizeof(*this), GFP_KERNEL);
	if (!this) {
		ret = -ENOMEM;
		goto unlock;
	}

	this->pfn = pfn;
	this->count = 1;
	rb_link_node(&this->node, parent, node);
	rb_insert_color(&this->node, &hyp_shared_pfns);
	ret = kvm_call_hyp_nvhe(__pkvm_host_share_hyp, pfn, 1);
unlock:
	mutex_unlock(&hyp_shared_pfns_lock);

	return ret;
}

static int unshare_pfn_hyp(u64 pfn)
{
	struct rb_node **node, *parent;
	struct hyp_shared_pfn *this;
	int ret = 0;

	mutex_lock(&hyp_shared_pfns_lock);
	this = find_shared_pfn(pfn, &node, &parent);
	if (WARN_ON(!this)) {
		ret = -ENOENT;
		goto unlock;
	}

	this->count--;
	if (this->count)
		goto unlock;

	rb_erase(&this->node, &hyp_shared_pfns);
	kfree(this);
	ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp, pfn, 1);
unlock:
	mutex_unlock(&hyp_shared_pfns_lock);

	return ret;
}

int kvm_share_hyp(void *from, void *to)
{
	phys_addr_t start, end, cur;
	u64 pfn;
	int ret;

	if (is_kernel_in_hyp_mode())
		return 0;

	/*
	 * The share hcall maps things in the 'fixed-offset' region of the hyp
	 * VA space, so we can only share physically contiguous data-structures
	 * for now.
	 */
	if (is_vmalloc_or_module_addr(from) || is_vmalloc_or_module_addr(to))
		return -EINVAL;

	if (kvm_host_owns_hyp_mappings())
		return create_hyp_mappings(from, to, PAGE_HYP);

	start = ALIGN_DOWN(__pa(from), PAGE_SIZE);
	end = PAGE_ALIGN(__pa(to));
	for (cur = start; cur < end; cur += PAGE_SIZE) {
		pfn = __phys_to_pfn(cur);
		ret = share_pfn_hyp(pfn);
		if (ret)
			return ret;
	}

	return 0;
}

void kvm_unshare_hyp(void *from, void *to)
{
	phys_addr_t start, end, cur;
	u64 pfn;

	if (is_kernel_in_hyp_mode() || kvm_host_owns_hyp_mappings() || !from)
		return;

	start = ALIGN_DOWN(__pa(from), PAGE_SIZE);
	end = PAGE_ALIGN(__pa(to));
	for (cur = start; cur < end; cur += PAGE_SIZE) {
		pfn = __phys_to_pfn(cur);
		WARN_ON(unshare_pfn_hyp(pfn));
	}
}

/**
 * create_hyp_mappings - duplicate a kernel virtual address range in Hyp mode
 * @from:	The virtual kernel start address of the range
 * @to:		The virtual kernel end address of the range (exclusive)
 * @prot:	The protection to be applied to this range
 *
 * The same virtual address as the kernel virtual address is also used
 * in Hyp-mode mapping (modulo HYP_PAGE_OFFSET) to the same underlying
 * physical pages.
 */
int create_hyp_mappings(void *from, void *to, enum kvm_pgtable_prot prot)
{
	phys_addr_t phys_addr;
	unsigned long virt_addr;
	unsigned long start = kern_hyp_va((unsigned long)from);
	unsigned long end = kern_hyp_va((unsigned long)to);

	if (is_kernel_in_hyp_mode())
		return 0;

	if (!kvm_host_owns_hyp_mappings())
		return -EPERM;

	start = start & PAGE_MASK;
	end = PAGE_ALIGN(end);

	for (virt_addr = start; virt_addr < end; virt_addr += PAGE_SIZE) {
		int err;

		phys_addr = kvm_kaddr_to_phys(from + virt_addr - start);
		err = __create_hyp_mappings(virt_addr, PAGE_SIZE, phys_addr,
					    prot);
		if (err)
			return err;
	}

	return 0;
}

static int __hyp_alloc_private_va_range(unsigned long base)
{
	lockdep_assert_held(&kvm_hyp_pgd_mutex);

	if (!PAGE_ALIGNED(base))
		return -EINVAL;

	/*
	 * Verify that BIT(VA_BITS - 1) hasn't been flipped by
	 * allocating the new area, as it would indicate we've
	 * overflowed the idmap/IO address range.
	 */
	if ((base ^ io_map_base) & BIT(VA_BITS - 1))
		return -ENOMEM;

	io_map_base = base;

	return 0;
}

/**
 * hyp_alloc_private_va_range - Allocates a private VA range.
 * @size:	The size of the VA range to reserve.
 * @haddr:	The hypervisor virtual start address of the allocation.
 *
 * The private virtual address (VA) range is allocated below io_map_base
 * and aligned based on the order of @size.
 *
 * Return: 0 on success or negative error code on failure.
 */
int hyp_alloc_private_va_range(size_t size, unsigned long *haddr)
{
	unsigned long base;
	int ret = 0;

	mutex_lock(&kvm_hyp_pgd_mutex);

	/*
	 * This assumes that we have enough space below the idmap
	 * page to allocate our VAs. If not, the check in
	 * __hyp_alloc_private_va_range() will kick. A potential
	 * alternative would be to detect that overflow and switch
	 * to an allocation above the idmap.
	 *
	 * The allocated size is always a multiple of PAGE_SIZE.
	 */
	size = PAGE_ALIGN(size);
	base = io_map_base - size;
	ret = __hyp_alloc_private_va_range(base);

	mutex_unlock(&kvm_hyp_pgd_mutex);

	if (!ret)
		*haddr = base;

	return ret;
}

static int __create_hyp_private_mapping(phys_addr_t phys_addr, size_t size,
					unsigned long *haddr,
					enum kvm_pgtable_prot prot)
{
	unsigned long addr;
	int ret = 0;

	if (!kvm_host_owns_hyp_mappings()) {
		addr = kvm_call_hyp_nvhe(__pkvm_create_private_mapping,
					 phys_addr, size, prot);
		if (IS_ERR_VALUE(addr))
			return addr;
		*haddr = addr;

		return 0;
	}

	size = PAGE_ALIGN(size + offset_in_page(phys_addr));
	ret = hyp_alloc_private_va_range(size, &addr);
	if (ret)
		return ret;

	ret = __create_hyp_mappings(addr, size, phys_addr, prot);
	if (ret)
		return ret;

	*haddr = addr + offset_in_page(phys_addr);
	return ret;
}

int create_hyp_stack(phys_addr_t phys_addr, unsigned long *haddr)
{
	unsigned long base;
	size_t size;
	int ret;

	mutex_lock(&kvm_hyp_pgd_mutex);
	/*
	 * Efficient stack verification using the PAGE_SHIFT bit implies
	 * an alignment of our allocation on the order of the size.
	 */
	size = PAGE_SIZE * 2;
	base = ALIGN_DOWN(io_map_base - size, size);

	ret = __hyp_alloc_private_va_range(base);

	mutex_unlock(&kvm_hyp_pgd_mutex);

	if (ret) {
		kvm_err("Cannot allocate hyp stack guard page\n");
		return ret;
	}

	/*
	 * Since the stack grows downwards, map the stack to the page
	 * at the higher address and leave the lower guard page
	 * unbacked.
	 *
	 * Any valid stack address now has the PAGE_SHIFT bit as 1
	 * and addresses corresponding to the guard page have the
	 * PAGE_SHIFT bit as 0 - this is used for overflow detection.
	 */
	ret = __create_hyp_mappings(base + PAGE_SIZE, PAGE_SIZE, phys_addr,
				    PAGE_HYP);
	if (ret)
		kvm_err("Cannot map hyp stack\n");

	*haddr = base + size;

	return ret;
}

/**
 * create_hyp_io_mappings - Map IO into both kernel and HYP
 * @phys_addr:	The physical start address which gets mapped
 * @size:	Size of the region being mapped
 * @kaddr:	Kernel VA for this mapping
 * @haddr:	HYP VA for this mapping
 */
int create_hyp_io_mappings(phys_addr_t phys_addr, size_t size,
			   void __iomem **kaddr,
			   void __iomem **haddr)
{
	unsigned long addr;
	int ret;

	if (is_protected_kvm_enabled())
		return -EPERM;

	*kaddr = ioremap(phys_addr, size);
	if (!*kaddr)
		return -ENOMEM;

	if (is_kernel_in_hyp_mode()) {
		*haddr = *kaddr;
		return 0;
	}

	ret = __create_hyp_private_mapping(phys_addr, size,
					   &addr, PAGE_HYP_DEVICE);
	if (ret) {
		iounmap(*kaddr);
		*kaddr = NULL;
		*haddr = NULL;
		return ret;
	}

	*haddr = (void __iomem *)addr;
	return 0;
}

/**
 * create_hyp_exec_mappings - Map an executable range into HYP
 * @phys_addr:	The physical start address which gets mapped
 * @size:	Size of the region being mapped
 * @haddr:	HYP VA for this mapping
 */
int create_hyp_exec_mappings(phys_addr_t phys_addr, size_t size,
			     void **haddr)
{
	unsigned long addr;
	int ret;

	BUG_ON(is_kernel_in_hyp_mode());

	ret = __create_hyp_private_mapping(phys_addr, size,
					   &addr, PAGE_HYP_EXEC);
	if (ret) {
		*haddr = NULL;
		return ret;
	}

	*haddr = (void *)addr;
	return 0;
}

static struct kvm_pgtable_mm_ops kvm_user_mm_ops = {
	/* We shouldn't need any other callback to walk the PT */
	.phys_to_virt		= kvm_host_va,
};

static int get_user_mapping_size(struct kvm *kvm, u64 addr)
{
	struct kvm_pgtable pgt = {
		.pgd		= (kvm_pteref_t)kvm->mm->pgd,
		.ia_bits	= vabits_actual,
		.start_level	= (KVM_PGTABLE_MAX_LEVELS -
				   CONFIG_PGTABLE_LEVELS),
		.mm_ops		= &kvm_user_mm_ops,
	};
	unsigned long flags;
	kvm_pte_t pte = 0;	/* Keep GCC quiet... */
	u32 level = ~0;
	int ret;

	/*
	 * Disable IRQs so that we hazard against a concurrent
	 * teardown of the userspace page tables (which relies on
	 * IPI-ing threads).
	 */
	local_irq_save(flags);
	ret = kvm_pgtable_get_leaf(&pgt, addr, &pte, &level);
	local_irq_restore(flags);

	if (ret)
		return ret;

	/*
	 * Not seeing an error, but not updating level? Something went
	 * deeply wrong...
	 */
	if (WARN_ON(level >= KVM_PGTABLE_MAX_LEVELS))
		return -EFAULT;

	/* Oops, the userspace PTs are gone... Replay the fault */
	if (!kvm_pte_valid(pte))
		return -EAGAIN;

	return BIT(ARM64_HW_PGTABLE_LEVEL_SHIFT(level));
}

static struct kvm_pgtable_mm_ops kvm_s2_mm_ops = {
	.zalloc_page		= stage2_memcache_zalloc_page,
	.zalloc_pages_exact	= kvm_s2_zalloc_pages_exact,
	.free_pages_exact	= kvm_s2_free_pages_exact,
	.free_unlinked_table	= stage2_free_unlinked_table,
	.get_page		= kvm_host_get_page,
	.put_page		= kvm_s2_put_page,
	.page_count		= kvm_host_page_count,
	.phys_to_virt		= kvm_host_va,
	.virt_to_phys		= kvm_host_pa,
	.dcache_clean_inval_poc	= clean_dcache_guest_page,
	.icache_inval_pou	= invalidate_icache_guest_page,
};

/**
 * kvm_init_stage2_mmu - Initialise a S2 MMU structure
 * @kvm:	The pointer to the KVM structure
 * @mmu:	The pointer to the s2 MMU structure
 * @type:	The machine type of the virtual machine
 *
 * Allocates only the stage-2 HW PGD level table(s).
 * Note we don't need locking here as this is only called when the VM is
 * created, which can only be done once.
 */
int kvm_init_stage2_mmu(struct kvm *kvm, struct kvm_s2_mmu *mmu, unsigned long type)
{
	u32 kvm_ipa_limit = get_kvm_ipa_limit();
	int cpu, err;
	struct kvm_pgtable *pgt;
	u64 mmfr0, mmfr1;
	u32 phys_shift;

	if (type & ~KVM_VM_TYPE_ARM_IPA_SIZE_MASK)
		return -EINVAL;

	phys_shift = KVM_VM_TYPE_ARM_IPA_SIZE(type);
	if (is_protected_kvm_enabled()) {
		phys_shift = kvm_ipa_limit;
	} else if (phys_shift) {
		if (phys_shift > kvm_ipa_limit ||
		    phys_shift < ARM64_MIN_PARANGE_BITS)
			return -EINVAL;
	} else {
		phys_shift = KVM_PHYS_SHIFT;
		if (phys_shift > kvm_ipa_limit) {
			pr_warn_once("%s using unsupported default IPA limit, upgrade your VMM\n",
				     current->comm);
			return -EINVAL;
		}
	}

	mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
	mmfr1 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR1_EL1);
	kvm->arch.vtcr = kvm_get_vtcr(mmfr0, mmfr1, phys_shift);

	if (mmu->pgt != NULL) {
		kvm_err("kvm_arch already initialized?\n");
		return -EINVAL;
	}

	pgt = kzalloc(sizeof(*pgt), GFP_KERNEL_ACCOUNT);
	if (!pgt)
		return -ENOMEM;

	mmu->arch = &kvm->arch;
	err = kvm_pgtable_stage2_init(pgt, mmu, &kvm_s2_mm_ops);
	if (err)
		goto out_free_pgtable;

	mmu->last_vcpu_ran = alloc_percpu(typeof(*mmu->last_vcpu_ran));
	if (!mmu->last_vcpu_ran) {
		err = -ENOMEM;
		goto out_destroy_pgtable;
	}

	for_each_possible_cpu(cpu)
		*per_cpu_ptr(mmu->last_vcpu_ran, cpu) = -1;

	 /* The eager page splitting is disabled by default */
	mmu->split_page_chunk_size = KVM_ARM_EAGER_SPLIT_CHUNK_SIZE_DEFAULT;
	mmu->split_page_cache.gfp_zero = __GFP_ZERO;

	mmu->pgt = pgt;
	mmu->pgd_phys = __pa(pgt->pgd);
	return 0;

out_destroy_pgtable:
	kvm_pgtable_stage2_destroy(pgt);
out_free_pgtable:
	kfree(pgt);
	return err;
}

void kvm_uninit_stage2_mmu(struct kvm *kvm)
{
	kvm_free_stage2_pgd(&kvm->arch.mmu);
	kvm_mmu_free_memory_cache(&kvm->arch.mmu.split_page_cache);
}

static void stage2_unmap_memslot(struct kvm *kvm,
				 struct kvm_memory_slot *memslot)
{
	hva_t hva = memslot->userspace_addr;
	phys_addr_t addr = memslot->base_gfn << PAGE_SHIFT;
	phys_addr_t size = PAGE_SIZE * memslot->npages;
	hva_t reg_end = hva + size;

	/*
	 * A memory region could potentially cover multiple VMAs, and any holes
	 * between them, so iterate over all of them to find out if we should
	 * unmap any of them.
	 *
	 *     +--------------------------------------------+
	 * +---------------+----------------+   +----------------+
	 * |   : VMA 1     |      VMA 2     |   |    VMA 3  :    |
	 * +---------------+----------------+   +----------------+
	 *     |               memory region                |
	 *     +--------------------------------------------+
	 */
	do {
		struct vm_area_struct *vma;
		hva_t vm_start, vm_end;

		vma = find_vma_intersection(current->mm, hva, reg_end);
		if (!vma)
			break;

		/*
		 * Take the intersection of this VMA with the memory region
		 */
		vm_start = max(hva, vma->vm_start);
		vm_end = min(reg_end, vma->vm_end);

		if (!(vma->vm_flags & VM_PFNMAP)) {
			gpa_t gpa = addr + (vm_start - memslot->userspace_addr);
			unmap_stage2_range(&kvm->arch.mmu, gpa, vm_end - vm_start);
		}
		hva = vm_end;
	} while (hva < reg_end);
}

/**
 * stage2_unmap_vm - Unmap Stage-2 RAM mappings
 * @kvm: The struct kvm pointer
 *
 * Go through the memregions and unmap any regular RAM
 * backing memory already mapped to the VM.
 */
void stage2_unmap_vm(struct kvm *kvm)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	int idx, bkt;

	idx = srcu_read_lock(&kvm->srcu);
	mmap_read_lock(current->mm);
	write_lock(&kvm->mmu_lock);

	slots = kvm_memslots(kvm);
	kvm_for_each_memslot(memslot, bkt, slots)
		stage2_unmap_memslot(kvm, memslot);

	write_unlock(&kvm->mmu_lock);
	mmap_read_unlock(current->mm);
	srcu_read_unlock(&kvm->srcu, idx);
}

void kvm_free_stage2_pgd(struct kvm_s2_mmu *mmu)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(mmu);
	struct kvm_pgtable *pgt = NULL;

	write_lock(&kvm->mmu_lock);
	pgt = mmu->pgt;
	if (pgt) {
		mmu->pgd_phys = 0;
		mmu->pgt = NULL;
		free_percpu(mmu->last_vcpu_ran);
	}
	write_unlock(&kvm->mmu_lock);

	if (pgt) {
		kvm_pgtable_stage2_destroy(pgt);
		kfree(pgt);
	}
}

static void hyp_mc_free_fn(void *addr, void *unused)
{
	free_page((unsigned long)addr);
}

static void *hyp_mc_alloc_fn(void *unused)
{
	return (void *)__get_free_page(GFP_KERNEL_ACCOUNT);
}

void free_hyp_memcache(struct kvm_hyp_memcache *mc)
{
	if (is_protected_kvm_enabled())
		__free_hyp_memcache(mc, hyp_mc_free_fn,
				    kvm_host_va, NULL);
}

int topup_hyp_memcache(struct kvm_hyp_memcache *mc, unsigned long min_pages)
{
	if (!is_protected_kvm_enabled())
		return 0;

	return __topup_hyp_memcache(mc, min_pages, hyp_mc_alloc_fn,
				    kvm_host_pa, NULL);
}

/**
 * kvm_phys_addr_ioremap - map a device range to guest IPA
 *
 * @kvm:	The KVM pointer
 * @guest_ipa:	The IPA at which to insert the mapping
 * @pa:		The physical address of the device
 * @size:	The size of the mapping
 * @writable:   Whether or not to create a writable mapping
 */
int kvm_phys_addr_ioremap(struct kvm *kvm, phys_addr_t guest_ipa,
			  phys_addr_t pa, unsigned long size, bool writable)
{
	phys_addr_t addr;
	int ret = 0;
	struct kvm_mmu_memory_cache cache = { .gfp_zero = __GFP_ZERO };
	struct kvm_pgtable *pgt = kvm->arch.mmu.pgt;
	enum kvm_pgtable_prot prot = KVM_PGTABLE_PROT_DEVICE |
				     KVM_PGTABLE_PROT_R |
				     (writable ? KVM_PGTABLE_PROT_W : 0);

	if (is_protected_kvm_enabled())
		return -EPERM;

	size += offset_in_page(guest_ipa);
	guest_ipa &= PAGE_MASK;

	for (addr = guest_ipa; addr < guest_ipa + size; addr += PAGE_SIZE) {
		ret = kvm_mmu_topup_memory_cache(&cache,
						 kvm_mmu_cache_min_pages(kvm));
		if (ret)
			break;

		write_lock(&kvm->mmu_lock);
		ret = kvm_pgtable_stage2_map(pgt, addr, PAGE_SIZE, pa, prot,
					     &cache, 0);
		write_unlock(&kvm->mmu_lock);
		if (ret)
			break;

		pa += PAGE_SIZE;
	}

	kvm_mmu_free_memory_cache(&cache);
	return ret;
}

/**
 * stage2_wp_range() - write protect stage2 memory region range
 * @mmu:        The KVM stage-2 MMU pointer
 * @addr:	Start address of range
 * @end:	End address of range
 */
static void stage2_wp_range(struct kvm_s2_mmu *mmu, phys_addr_t addr, phys_addr_t end)
{
	stage2_apply_range_resched(mmu, addr, end, kvm_pgtable_stage2_wrprotect);
}

/**
 * kvm_mmu_wp_memory_region() - write protect stage 2 entries for memory slot
 * @kvm:	The KVM pointer
 * @slot:	The memory slot to write protect
 *
 * Called to start logging dirty pages after memory region
 * KVM_MEM_LOG_DIRTY_PAGES operation is called. After this function returns
 * all present PUD, PMD and PTEs are write protected in the memory region.
 * Afterwards read of dirty page log can be called.
 *
 * Acquires kvm_mmu_lock. Called with kvm->slots_lock mutex acquired,
 * serializing operations for VM memory regions.
 */
static void kvm_mmu_wp_memory_region(struct kvm *kvm, int slot)
{
	struct kvm_memslots *slots = kvm_memslots(kvm);
	struct kvm_memory_slot *memslot = id_to_memslot(slots, slot);
	phys_addr_t start, end;

	if (WARN_ON_ONCE(!memslot))
		return;

	start = memslot->base_gfn << PAGE_SHIFT;
	end = (memslot->base_gfn + memslot->npages) << PAGE_SHIFT;

	write_lock(&kvm->mmu_lock);
	stage2_wp_range(&kvm->arch.mmu, start, end);
	write_unlock(&kvm->mmu_lock);
	kvm_flush_remote_tlbs_memslot(kvm, memslot);
}

/**
 * kvm_mmu_split_memory_region() - split the stage 2 blocks into PAGE_SIZE
 *				   pages for memory slot
 * @kvm:	The KVM pointer
 * @slot:	The memory slot to split
 *
 * Acquires kvm->mmu_lock. Called with kvm->slots_lock mutex acquired,
 * serializing operations for VM memory regions.
 */
static void kvm_mmu_split_memory_region(struct kvm *kvm, int slot)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	phys_addr_t start, end;

	lockdep_assert_held(&kvm->slots_lock);

	slots = kvm_memslots(kvm);
	memslot = id_to_memslot(slots, slot);

	start = memslot->base_gfn << PAGE_SHIFT;
	end = (memslot->base_gfn + memslot->npages) << PAGE_SHIFT;

	write_lock(&kvm->mmu_lock);
	kvm_mmu_split_huge_pages(kvm, start, end);
	write_unlock(&kvm->mmu_lock);
}

/*
 * kvm_arch_mmu_enable_log_dirty_pt_masked() - enable dirty logging for selected pages.
 * @kvm:	The KVM pointer
 * @slot:	The memory slot associated with mask
 * @gfn_offset:	The gfn offset in memory slot
 * @mask:	The mask of pages at offset 'gfn_offset' in this memory
 *		slot to enable dirty logging on
 *
 * Writes protect selected pages to enable dirty logging, and then
 * splits them to PAGE_SIZE. Caller must acquire kvm->mmu_lock.
 */
void kvm_arch_mmu_enable_log_dirty_pt_masked(struct kvm *kvm,
		struct kvm_memory_slot *slot,
		gfn_t gfn_offset, unsigned long mask)
{
	phys_addr_t base_gfn = slot->base_gfn + gfn_offset;
	phys_addr_t start = (base_gfn +  __ffs(mask)) << PAGE_SHIFT;
	phys_addr_t end = (base_gfn + __fls(mask) + 1) << PAGE_SHIFT;

	lockdep_assert_held_write(&kvm->mmu_lock);

	stage2_wp_range(&kvm->arch.mmu, start, end);

	/*
	 * Eager-splitting is done when manual-protect is set.  We
	 * also check for initially-all-set because we can avoid
	 * eager-splitting if initially-all-set is false.
	 * Initially-all-set equal false implies that huge-pages were
	 * already split when enabling dirty logging: no need to do it
	 * again.
	 */
	if (kvm_dirty_log_manual_protect_and_init_set(kvm))
		kvm_mmu_split_huge_pages(kvm, start, end);
}

static void kvm_send_hwpoison_signal(unsigned long address, short lsb)
{
	send_sig_mceerr(BUS_MCEERR_AR, (void __user *)address, lsb, current);
}

static bool fault_supports_stage2_huge_mapping(struct kvm_memory_slot *memslot,
					       unsigned long hva,
					       unsigned long map_size)
{
	gpa_t gpa_start;
	hva_t uaddr_start, uaddr_end;
	size_t size;

	/* The memslot and the VMA are guaranteed to be aligned to PAGE_SIZE */
	if (map_size == PAGE_SIZE)
		return true;

	size = memslot->npages * PAGE_SIZE;

	gpa_start = memslot->base_gfn << PAGE_SHIFT;

	uaddr_start = memslot->userspace_addr;
	uaddr_end = uaddr_start + size;

	/*
	 * Pages belonging to memslots that don't have the same alignment
	 * within a PMD/PUD for userspace and IPA cannot be mapped with stage-2
	 * PMD/PUD entries, because we'll end up mapping the wrong pages.
	 *
	 * Consider a layout like the following:
	 *
	 *    memslot->userspace_addr:
	 *    +-----+--------------------+--------------------+---+
	 *    |abcde|fgh  Stage-1 block  |    Stage-1 block tv|xyz|
	 *    +-----+--------------------+--------------------+---+
	 *
	 *    memslot->base_gfn << PAGE_SHIFT:
	 *      +---+--------------------+--------------------+-----+
	 *      |abc|def  Stage-2 block  |    Stage-2 block   |tvxyz|
	 *      +---+--------------------+--------------------+-----+
	 *
	 * If we create those stage-2 blocks, we'll end up with this incorrect
	 * mapping:
	 *   d -> f
	 *   e -> g
	 *   f -> h
	 */
	if ((gpa_start & (map_size - 1)) != (uaddr_start & (map_size - 1)))
		return false;

	/*
	 * Next, let's make sure we're not trying to map anything not covered
	 * by the memslot. This means we have to prohibit block size mappings
	 * for the beginning and end of a non-block aligned and non-block sized
	 * memory slot (illustrated by the head and tail parts of the
	 * userspace view above containing pages 'abcde' and 'xyz',
	 * respectively).
	 *
	 * Note that it doesn't matter if we do the check using the
	 * userspace_addr or the base_gfn, as both are equally aligned (per
	 * the check above) and equally sized.
	 */
	return (hva & ~(map_size - 1)) >= uaddr_start &&
	       (hva & ~(map_size - 1)) + map_size <= uaddr_end;
}

/*
 * Check if the given hva is backed by a transparent huge page (THP) and
 * whether it can be mapped using block mapping in stage2. If so, adjust
 * the stage2 PFN and IPA accordingly. Only PMD_SIZE THPs are currently
 * supported. This will need to be updated to support other THP sizes.
 *
 * Returns the size of the mapping.
 */
static long
transparent_hugepage_adjust(struct kvm *kvm, struct kvm_memory_slot *memslot,
			    unsigned long hva, kvm_pfn_t *pfnp,
			    phys_addr_t *ipap)
{
	kvm_pfn_t pfn = *pfnp;

	/*
	 * Make sure the adjustment is done only for THP pages. Also make
	 * sure that the HVA and IPA are sufficiently aligned and that the
	 * block map is contained within the memslot.
	 */
	if (fault_supports_stage2_huge_mapping(memslot, hva, PMD_SIZE)) {
		int sz = get_user_mapping_size(kvm, hva);

		if (sz < 0)
			return sz;

		if (sz < PMD_SIZE)
			return PAGE_SIZE;

		/*
		 * The address we faulted on is backed by a transparent huge
		 * page.  However, because we map the compound huge page and
		 * not the individual tail page, we need to transfer the
		 * refcount to the head page.  We have to be careful that the
		 * THP doesn't start to split while we are adjusting the
		 * refcounts.
		 *
		 * We are sure this doesn't happen, because mmu_invalidate_retry
		 * was successful and we are holding the mmu_lock, so if this
		 * THP is trying to split, it will be blocked in the mmu
		 * notifier before touching any of the pages, specifically
		 * before being able to call __split_huge_page_refcount().
		 *
		 * We can therefore safely transfer the refcount from PG_tail
		 * to PG_head and switch the pfn from a tail page to the head
		 * page accordingly.
		 */
		*ipap &= PMD_MASK;
		kvm_release_pfn_clean(pfn);
		pfn &= ~(PTRS_PER_PMD - 1);
		get_page(pfn_to_page(pfn));
		*pfnp = pfn;

		return PMD_SIZE;
	}

	/* Use page mapping if we cannot use block mapping. */
	return PAGE_SIZE;
}

static int get_vma_page_shift(struct vm_area_struct *vma, unsigned long hva)
{
	unsigned long pa;

	if (is_vm_hugetlb_page(vma) && !(vma->vm_flags & VM_PFNMAP))
		return huge_page_shift(hstate_vma(vma));

	if (!(vma->vm_flags & VM_PFNMAP))
		return PAGE_SHIFT;

	VM_BUG_ON(is_vm_hugetlb_page(vma));

	pa = (vma->vm_pgoff << PAGE_SHIFT) + (hva - vma->vm_start);

#ifndef __PAGETABLE_PMD_FOLDED
	if ((hva & (PUD_SIZE - 1)) == (pa & (PUD_SIZE - 1)) &&
	    ALIGN_DOWN(hva, PUD_SIZE) >= vma->vm_start &&
	    ALIGN(hva, PUD_SIZE) <= vma->vm_end)
		return PUD_SHIFT;
#endif

	if ((hva & (PMD_SIZE - 1)) == (pa & (PMD_SIZE - 1)) &&
	    ALIGN_DOWN(hva, PMD_SIZE) >= vma->vm_start &&
	    ALIGN(hva, PMD_SIZE) <= vma->vm_end)
		return PMD_SHIFT;

	return PAGE_SHIFT;
}

/*
 * The page will be mapped in stage 2 as Normal Cacheable, so the VM will be
 * able to see the page's tags and therefore they must be initialised first. If
 * PG_mte_tagged is set, tags have already been initialised.
 *
 * The race in the test/set of the PG_mte_tagged flag is handled by:
 * - preventing VM_SHARED mappings in a memslot with MTE preventing two VMs
 *   racing to santise the same page
 * - mmap_lock protects between a VM faulting a page in and the VMM performing
 *   an mprotect() to add VM_MTE
 */
static void sanitise_mte_tags(struct kvm *kvm, kvm_pfn_t pfn,
			      unsigned long size)
{
	unsigned long i, nr_pages = size >> PAGE_SHIFT;
	struct page *page = pfn_to_page(pfn);

	if (!kvm_has_mte(kvm))
		return;

	for (i = 0; i < nr_pages; i++, page++) {
		if (try_page_mte_tagging(page)) {
			mte_clear_page_tags(page_address(page));
			set_page_mte_tagged(page);
		}
	}
}

static bool kvm_vma_mte_allowed(struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_MTE_ALLOWED;
}

static int user_mem_abort(struct kvm_vcpu *vcpu, phys_addr_t fault_ipa,
			  struct kvm_memory_slot *memslot, unsigned long hva,
			  unsigned long fault_status)
{
	int ret = 0;
	bool write_fault, writable, force_pte = false;
	bool exec_fault, mte_allowed;
	bool device = false;
	unsigned long mmu_seq;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_mmu_memory_cache *memcache = &vcpu->arch.mmu_page_cache;
	struct vm_area_struct *vma;
	short vma_shift;
	gfn_t gfn;
	kvm_pfn_t pfn;
	bool logging_active = memslot_is_logging(memslot);
	unsigned long fault_level = kvm_vcpu_trap_get_fault_level(vcpu);
	long vma_pagesize, fault_granule;
	enum kvm_pgtable_prot prot = KVM_PGTABLE_PROT_R;
	struct kvm_pgtable *pgt;

	fault_granule = 1UL << ARM64_HW_PGTABLE_LEVEL_SHIFT(fault_level);
	write_fault = kvm_is_write_fault(vcpu);
	exec_fault = kvm_vcpu_trap_is_exec_fault(vcpu);
	VM_BUG_ON(write_fault && exec_fault);

	if (fault_status == ESR_ELx_FSC_PERM && !write_fault && !exec_fault) {
		kvm_err("Unexpected L2 read permission error\n");
		return -EFAULT;
	}

	/*
	 * Permission faults just need to update the existing leaf entry,
	 * and so normally don't require allocations from the memcache. The
	 * only exception to this is when dirty logging is enabled at runtime
	 * and a write fault needs to collapse a block entry into a table.
	 */
	if (fault_status != ESR_ELx_FSC_PERM ||
	    (logging_active && write_fault)) {
		ret = kvm_mmu_topup_memory_cache(memcache,
						 kvm_mmu_cache_min_pages(kvm));
		if (ret)
			return ret;
	}

	/*
	 * Let's check if we will get back a huge page backed by hugetlbfs, or
	 * get block mapping for device MMIO region.
	 */
	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, hva);
	if (unlikely(!vma)) {
		kvm_err("Failed to find VMA for hva 0x%lx\n", hva);
		mmap_read_unlock(current->mm);
		return -EFAULT;
	}

	/*
	 * logging_active is guaranteed to never be true for VM_PFNMAP
	 * memslots.
	 */
	if (logging_active) {
		force_pte = true;
		vma_shift = PAGE_SHIFT;
	} else {
		vma_shift = get_vma_page_shift(vma, hva);
	}

	switch (vma_shift) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SHIFT:
		if (fault_supports_stage2_huge_mapping(memslot, hva, PUD_SIZE))
			break;
		fallthrough;
#endif
	case CONT_PMD_SHIFT:
		vma_shift = PMD_SHIFT;
		fallthrough;
	case PMD_SHIFT:
		if (fault_supports_stage2_huge_mapping(memslot, hva, PMD_SIZE))
			break;
		fallthrough;
	case CONT_PTE_SHIFT:
		vma_shift = PAGE_SHIFT;
		force_pte = true;
		fallthrough;
	case PAGE_SHIFT:
		break;
	default:
		WARN_ONCE(1, "Unknown vma_shift %d", vma_shift);
	}

	vma_pagesize = 1UL << vma_shift;
	if (vma_pagesize == PMD_SIZE || vma_pagesize == PUD_SIZE)
		fault_ipa &= ~(vma_pagesize - 1);

	gfn = fault_ipa >> PAGE_SHIFT;
	mte_allowed = kvm_vma_mte_allowed(vma);

	/* Don't use the VMA after the unlock -- it may have vanished */
	vma = NULL;

	/*
	 * Read mmu_invalidate_seq so that KVM can detect if the results of
	 * vma_lookup() or __gfn_to_pfn_memslot() become stale prior to
	 * acquiring kvm->mmu_lock.
	 *
	 * Rely on mmap_read_unlock() for an implicit smp_rmb(), which pairs
	 * with the smp_wmb() in kvm_mmu_invalidate_end().
	 */
	mmu_seq = vcpu->kvm->mmu_invalidate_seq;
	mmap_read_unlock(current->mm);

	pfn = __gfn_to_pfn_memslot(memslot, gfn, false, false, NULL,
				   write_fault, &writable, NULL);
	if (pfn == KVM_PFN_ERR_HWPOISON) {
		kvm_send_hwpoison_signal(hva, vma_shift);
		return 0;
	}
	if (is_error_noslot_pfn(pfn))
		return -EFAULT;

	if (kvm_is_device_pfn(pfn)) {
		/*
		 * If the page was identified as device early by looking at
		 * the VMA flags, vma_pagesize is already representing the
		 * largest quantity we can map.  If instead it was mapped
		 * via gfn_to_pfn_prot(), vma_pagesize is set to PAGE_SIZE
		 * and must not be upgraded.
		 *
		 * In both cases, we don't let transparent_hugepage_adjust()
		 * change things at the last minute.
		 */
		device = true;
	} else if (logging_active && !write_fault) {
		/*
		 * Only actually map the page as writable if this was a write
		 * fault.
		 */
		writable = false;
	}

	if (exec_fault && device)
		return -ENOEXEC;

	read_lock(&kvm->mmu_lock);
	pgt = vcpu->arch.hw_mmu->pgt;
	if (mmu_invalidate_retry(kvm, mmu_seq))
		goto out_unlock;

	/*
	 * If we are not forced to use page mapping, check if we are
	 * backed by a THP and thus use block mapping if possible.
	 */
	if (vma_pagesize == PAGE_SIZE && !(force_pte || device)) {
		if (fault_status ==  ESR_ELx_FSC_PERM &&
		    fault_granule > PAGE_SIZE)
			vma_pagesize = fault_granule;
		else
			vma_pagesize = transparent_hugepage_adjust(kvm, memslot,
								   hva, &pfn,
								   &fault_ipa);

		if (vma_pagesize < 0) {
			ret = vma_pagesize;
			goto out_unlock;
		}
	}

	if (fault_status != ESR_ELx_FSC_PERM && !device && kvm_has_mte(kvm)) {
		/* Check the VMM hasn't introduced a new disallowed VMA */
		if (mte_allowed) {
			sanitise_mte_tags(kvm, pfn, vma_pagesize);
		} else {
			ret = -EFAULT;
			goto out_unlock;
		}
	}

	if (writable)
		prot |= KVM_PGTABLE_PROT_W;

	if (exec_fault)
		prot |= KVM_PGTABLE_PROT_X;

	if (device)
		prot |= KVM_PGTABLE_PROT_DEVICE;
	else if (cpus_have_const_cap(ARM64_HAS_CACHE_DIC))
		prot |= KVM_PGTABLE_PROT_X;

	/*
	 * Under the premise of getting a FSC_PERM fault, we just need to relax
	 * permissions only if vma_pagesize equals fault_granule. Otherwise,
	 * kvm_pgtable_stage2_map() should be called to change block size.
	 */
	if (fault_status == ESR_ELx_FSC_PERM && vma_pagesize == fault_granule)
		ret = kvm_pgtable_stage2_relax_perms(pgt, fault_ipa, prot);
	else
		ret = kvm_pgtable_stage2_map(pgt, fault_ipa, vma_pagesize,
					     __pfn_to_phys(pfn), prot,
					     memcache,
					     KVM_PGTABLE_WALK_HANDLE_FAULT |
					     KVM_PGTABLE_WALK_SHARED);

	/* Mark the page dirty only if the fault is handled successfully */
	if (writable && !ret) {
		kvm_set_pfn_dirty(pfn);
		mark_page_dirty_in_slot(kvm, memslot, gfn);
	}

out_unlock:
	read_unlock(&kvm->mmu_lock);
	kvm_release_pfn_clean(pfn);
	return ret != -EAGAIN ? ret : 0;
}

/* Resolve the access fault by making the page young again. */
static void handle_access_fault(struct kvm_vcpu *vcpu, phys_addr_t fault_ipa)
{
	kvm_pte_t pte;
	struct kvm_s2_mmu *mmu;

	trace_kvm_access_fault(fault_ipa);

	read_lock(&vcpu->kvm->mmu_lock);
	mmu = vcpu->arch.hw_mmu;
	pte = kvm_pgtable_stage2_mkyoung(mmu->pgt, fault_ipa);
	read_unlock(&vcpu->kvm->mmu_lock);

	if (kvm_pte_valid(pte))
		kvm_set_pfn_accessed(kvm_pte_to_pfn(pte));
}

/**
 * kvm_handle_guest_abort - handles all 2nd stage aborts
 * @vcpu:	the VCPU pointer
 *
 * Any abort that gets to the host is almost guaranteed to be caused by a
 * missing second stage translation table entry, which can mean that either the
 * guest simply needs more memory and we must allocate an appropriate page or it
 * can mean that the guest tried to access I/O memory, which is emulated by user
 * space. The distinction is based on the IPA causing the fault and whether this
 * memory region has been registered as standard RAM by user space.
 */
int kvm_handle_guest_abort(struct kvm_vcpu *vcpu)
{
	unsigned long fault_status;
	phys_addr_t fault_ipa;
	struct kvm_memory_slot *memslot;
	unsigned long hva;
	bool is_iabt, write_fault, writable;
	gfn_t gfn;
	int ret, idx;

	fault_status = kvm_vcpu_trap_get_fault_type(vcpu);

	fault_ipa = kvm_vcpu_get_fault_ipa(vcpu);
	is_iabt = kvm_vcpu_trap_is_iabt(vcpu);

	if (fault_status == ESR_ELx_FSC_FAULT) {
		/* Beyond sanitised PARange (which is the IPA limit) */
		if (fault_ipa >= BIT_ULL(get_kvm_ipa_limit())) {
			kvm_inject_size_fault(vcpu);
			return 1;
		}

		/* Falls between the IPA range and the PARange? */
		if (fault_ipa >= BIT_ULL(vcpu->arch.hw_mmu->pgt->ia_bits)) {
			fault_ipa |= kvm_vcpu_get_hfar(vcpu) & GENMASK(11, 0);

			if (is_iabt)
				kvm_inject_pabt(vcpu, fault_ipa);
			else
				kvm_inject_dabt(vcpu, fault_ipa);
			return 1;
		}
	}

	/* Synchronous External Abort? */
	if (kvm_vcpu_abt_issea(vcpu)) {
		/*
		 * For RAS the host kernel may handle this abort.
		 * There is no need to pass the error into the guest.
		 */
		if (kvm_handle_guest_sea(fault_ipa, kvm_vcpu_get_esr(vcpu)))
			kvm_inject_vabt(vcpu);

		return 1;
	}

	trace_kvm_guest_fault(*vcpu_pc(vcpu), kvm_vcpu_get_esr(vcpu),
			      kvm_vcpu_get_hfar(vcpu), fault_ipa);

	/* Check the stage-2 fault is trans. fault or write fault */
	if (fault_status != ESR_ELx_FSC_FAULT &&
	    fault_status != ESR_ELx_FSC_PERM &&
	    fault_status != ESR_ELx_FSC_ACCESS) {
		kvm_err("Unsupported FSC: EC=%#x xFSC=%#lx ESR_EL2=%#lx\n",
			kvm_vcpu_trap_get_class(vcpu),
			(unsigned long)kvm_vcpu_trap_get_fault(vcpu),
			(unsigned long)kvm_vcpu_get_esr(vcpu));
		return -EFAULT;
	}

	idx = srcu_read_lock(&vcpu->kvm->srcu);

	gfn = fault_ipa >> PAGE_SHIFT;
	memslot = gfn_to_memslot(vcpu->kvm, gfn);
	hva = gfn_to_hva_memslot_prot(memslot, gfn, &writable);
	write_fault = kvm_is_write_fault(vcpu);
	if (kvm_is_error_hva(hva) || (write_fault && !writable)) {
		/*
		 * The guest has put either its instructions or its page-tables
		 * somewhere it shouldn't have. Userspace won't be able to do
		 * anything about this (there's no syndrome for a start), so
		 * re-inject the abort back into the guest.
		 */
		if (is_iabt) {
			ret = -ENOEXEC;
			goto out;
		}

		if (kvm_vcpu_abt_iss1tw(vcpu)) {
			kvm_inject_dabt(vcpu, kvm_vcpu_get_hfar(vcpu));
			ret = 1;
			goto out_unlock;
		}

		/*
		 * Check for a cache maintenance operation. Since we
		 * ended-up here, we know it is outside of any memory
		 * slot. But we can't find out if that is for a device,
		 * or if the guest is just being stupid. The only thing
		 * we know for sure is that this range cannot be cached.
		 *
		 * So let's assume that the guest is just being
		 * cautious, and skip the instruction.
		 */
		if (kvm_is_error_hva(hva) && kvm_vcpu_dabt_is_cm(vcpu)) {
			kvm_incr_pc(vcpu);
			ret = 1;
			goto out_unlock;
		}

		/*
		 * The IPA is reported as [MAX:12], so we need to
		 * complement it with the bottom 12 bits from the
		 * faulting VA. This is always 12 bits, irrespective
		 * of the page size.
		 */
		fault_ipa |= kvm_vcpu_get_hfar(vcpu) & ((1 << 12) - 1);
		ret = io_mem_abort(vcpu, fault_ipa);
		goto out_unlock;
	}

	/* Userspace should not be able to register out-of-bounds IPAs */
	VM_BUG_ON(fault_ipa >= kvm_phys_size(vcpu->kvm));

	if (fault_status == ESR_ELx_FSC_ACCESS) {
		handle_access_fault(vcpu, fault_ipa);
		ret = 1;
		goto out_unlock;
	}

	ret = user_mem_abort(vcpu, fault_ipa, memslot, hva, fault_status);
	if (ret == 0)
		ret = 1;
out:
	if (ret == -ENOEXEC) {
		kvm_inject_pabt(vcpu, kvm_vcpu_get_hfar(vcpu));
		ret = 1;
	}
out_unlock:
	srcu_read_unlock(&vcpu->kvm->srcu, idx);
	return ret;
}

bool kvm_unmap_gfn_range(struct kvm *kvm, struct kvm_gfn_range *range)
{
	if (!kvm->arch.mmu.pgt)
		return false;

	__unmap_stage2_range(&kvm->arch.mmu, range->start << PAGE_SHIFT,
			     (range->end - range->start) << PAGE_SHIFT,
			     range->may_block);

	return false;
}

bool kvm_set_spte_gfn(struct kvm *kvm, struct kvm_gfn_range *range)
{
	kvm_pfn_t pfn = pte_pfn(range->arg.pte);

	if (!kvm->arch.mmu.pgt)
		return false;

	WARN_ON(range->end - range->start != 1);

	/*
	 * If the page isn't tagged, defer to user_mem_abort() for sanitising
	 * the MTE tags. The S2 pte should have been unmapped by
	 * mmu_notifier_invalidate_range_end().
	 */
	if (kvm_has_mte(kvm) && !page_mte_tagged(pfn_to_page(pfn)))
		return false;

	/*
	 * We've moved a page around, probably through CoW, so let's treat
	 * it just like a translation fault and the map handler will clean
	 * the cache to the PoC.
	 *
	 * The MMU notifiers will have unmapped a huge PMD before calling
	 * ->change_pte() (which in turn calls kvm_set_spte_gfn()) and
	 * therefore we never need to clear out a huge PMD through this
	 * calling path and a memcache is not required.
	 */
	kvm_pgtable_stage2_map(kvm->arch.mmu.pgt, range->start << PAGE_SHIFT,
			       PAGE_SIZE, __pfn_to_phys(pfn),
			       KVM_PGTABLE_PROT_R, NULL, 0);

	return false;
}

bool kvm_age_gfn(struct kvm *kvm, struct kvm_gfn_range *range)
{
	u64 size = (range->end - range->start) << PAGE_SHIFT;

	if (!kvm->arch.mmu.pgt)
		return false;

	return kvm_pgtable_stage2_test_clear_young(kvm->arch.mmu.pgt,
						   range->start << PAGE_SHIFT,
						   size, true);
}

bool kvm_test_age_gfn(struct kvm *kvm, struct kvm_gfn_range *range)
{
	u64 size = (range->end - range->start) << PAGE_SHIFT;

	if (!kvm->arch.mmu.pgt)
		return false;

	return kvm_pgtable_stage2_test_clear_young(kvm->arch.mmu.pgt,
						   range->start << PAGE_SHIFT,
						   size, false);
}

phys_addr_t kvm_mmu_get_httbr(void)
{
	return __pa(hyp_pgtable->pgd);
}

phys_addr_t kvm_get_idmap_vector(void)
{
	return hyp_idmap_vector;
}

static int kvm_map_idmap_text(void)
{
	unsigned long size = hyp_idmap_end - hyp_idmap_start;
	int err = __create_hyp_mappings(hyp_idmap_start, size, hyp_idmap_start,
					PAGE_HYP_EXEC);
	if (err)
		kvm_err("Failed to idmap %lx-%lx\n",
			hyp_idmap_start, hyp_idmap_end);

	return err;
}

static void *kvm_hyp_zalloc_page(void *arg)
{
	return (void *)get_zeroed_page(GFP_KERNEL);
}

static struct kvm_pgtable_mm_ops kvm_hyp_mm_ops = {
	.zalloc_page		= kvm_hyp_zalloc_page,
	.get_page		= kvm_host_get_page,
	.put_page		= kvm_host_put_page,
	.phys_to_virt		= kvm_host_va,
	.virt_to_phys		= kvm_host_pa,
};

int __init kvm_mmu_init(u32 *hyp_va_bits)
{
	int err;
	u32 idmap_bits;
	u32 kernel_bits;

	hyp_idmap_start = __pa_symbol(__hyp_idmap_text_start);
	hyp_idmap_start = ALIGN_DOWN(hyp_idmap_start, PAGE_SIZE);
	hyp_idmap_end = __pa_symbol(__hyp_idmap_text_end);
	hyp_idmap_end = ALIGN(hyp_idmap_end, PAGE_SIZE);
	hyp_idmap_vector = __pa_symbol(__kvm_hyp_init);

	/*
	 * We rely on the linker script to ensure at build time that the HYP
	 * init code does not cross a page boundary.
	 */
	BUG_ON((hyp_idmap_start ^ (hyp_idmap_end - 1)) & PAGE_MASK);

	/*
	 * The ID map may be configured to use an extended virtual address
	 * range. This is only the case if system RAM is out of range for the
	 * currently configured page size and VA_BITS_MIN, in which case we will
	 * also need the extended virtual range for the HYP ID map, or we won't
	 * be able to enable the EL2 MMU.
	 *
	 * However, in some cases the ID map may be configured for fewer than
	 * the number of VA bits used by the regular kernel stage 1. This
	 * happens when VA_BITS=52 and the kernel image is placed in PA space
	 * below 48 bits.
	 *
	 * At EL2, there is only one TTBR register, and we can't switch between
	 * translation tables *and* update TCR_EL2.T0SZ at the same time. Bottom
	 * line: we need to use the extended range with *both* our translation
	 * tables.
	 *
	 * So use the maximum of the idmap VA bits and the regular kernel stage
	 * 1 VA bits to assure that the hypervisor can both ID map its code page
	 * and map any kernel memory.
	 */
	idmap_bits = 64 - ((idmap_t0sz & TCR_T0SZ_MASK) >> TCR_T0SZ_OFFSET);
	kernel_bits = vabits_actual;
	*hyp_va_bits = max(idmap_bits, kernel_bits);

	kvm_debug("Using %u-bit virtual addresses at EL2\n", *hyp_va_bits);
	kvm_debug("IDMAP page: %lx\n", hyp_idmap_start);
	kvm_debug("HYP VA range: %lx:%lx\n",
		  kern_hyp_va(PAGE_OFFSET),
		  kern_hyp_va((unsigned long)high_memory - 1));

	if (hyp_idmap_start >= kern_hyp_va(PAGE_OFFSET) &&
	    hyp_idmap_start <  kern_hyp_va((unsigned long)high_memory - 1) &&
	    hyp_idmap_start != (unsigned long)__hyp_idmap_text_start) {
		/*
		 * The idmap page is intersecting with the VA space,
		 * it is not safe to continue further.
		 */
		kvm_err("IDMAP intersecting with HYP VA, unable to continue\n");
		err = -EINVAL;
		goto out;
	}

	hyp_pgtable = kzalloc(sizeof(*hyp_pgtable), GFP_KERNEL);
	if (!hyp_pgtable) {
		kvm_err("Hyp mode page-table not allocated\n");
		err = -ENOMEM;
		goto out;
	}

	err = kvm_pgtable_hyp_init(hyp_pgtable, *hyp_va_bits, &kvm_hyp_mm_ops);
	if (err)
		goto out_free_pgtable;

	err = kvm_map_idmap_text();
	if (err)
		goto out_destroy_pgtable;

	io_map_base = hyp_idmap_start;
	return 0;

out_destroy_pgtable:
	kvm_pgtable_hyp_destroy(hyp_pgtable);
out_free_pgtable:
	kfree(hyp_pgtable);
	hyp_pgtable = NULL;
out:
	return err;
}

void kvm_arch_commit_memory_region(struct kvm *kvm,
				   struct kvm_memory_slot *old,
				   const struct kvm_memory_slot *new,
				   enum kvm_mr_change change)
{
	bool log_dirty_pages = new && new->flags & KVM_MEM_LOG_DIRTY_PAGES;

	/*
	 * At this point memslot has been committed and there is an
	 * allocated dirty_bitmap[], dirty pages will be tracked while the
	 * memory slot is write protected.
	 */
	if (log_dirty_pages) {

		if (change == KVM_MR_DELETE)
			return;

		/*
		 * Huge and normal pages are write-protected and split
		 * on either of these two cases:
		 *
		 * 1. with initial-all-set: gradually with CLEAR ioctls,
		 */
		if (kvm_dirty_log_manual_protect_and_init_set(kvm))
			return;
		/*
		 * or
		 * 2. without initial-all-set: all in one shot when
		 *    enabling dirty logging.
		 */
		kvm_mmu_wp_memory_region(kvm, new->id);
		kvm_mmu_split_memory_region(kvm, new->id);
	} else {
		/*
		 * Free any leftovers from the eager page splitting cache. Do
		 * this when deleting, moving, disabling dirty logging, or
		 * creating the memslot (a nop). Doing it for deletes makes
		 * sure we don't leak memory, and there's no need to keep the
		 * cache around for any of the other cases.
		 */
		kvm_mmu_free_memory_cache(&kvm->arch.mmu.split_page_cache);
	}
}

int kvm_arch_prepare_memory_region(struct kvm *kvm,
				   const struct kvm_memory_slot *old,
				   struct kvm_memory_slot *new,
				   enum kvm_mr_change change)
{
	hva_t hva, reg_end;
	int ret = 0;

	if (change != KVM_MR_CREATE && change != KVM_MR_MOVE &&
			change != KVM_MR_FLAGS_ONLY)
		return 0;

	/*
	 * Prevent userspace from creating a memory region outside of the IPA
	 * space addressable by the KVM guest IPA space.
	 */
	if ((new->base_gfn + new->npages) > (kvm_phys_size(kvm) >> PAGE_SHIFT))
		return -EFAULT;

	hva = new->userspace_addr;
	reg_end = hva + (new->npages << PAGE_SHIFT);

	mmap_read_lock(current->mm);
	/*
	 * A memory region could potentially cover multiple VMAs, and any holes
	 * between them, so iterate over all of them.
	 *
	 *     +--------------------------------------------+
	 * +---------------+----------------+   +----------------+
	 * |   : VMA 1     |      VMA 2     |   |    VMA 3  :    |
	 * +---------------+----------------+   +----------------+
	 *     |               memory region                |
	 *     +--------------------------------------------+
	 */
	do {
		struct vm_area_struct *vma;

		vma = find_vma_intersection(current->mm, hva, reg_end);
		if (!vma)
			break;

		if (kvm_has_mte(kvm) && !kvm_vma_mte_allowed(vma)) {
			ret = -EINVAL;
			break;
		}

		if (vma->vm_flags & VM_PFNMAP) {
			/* IO region dirty page logging not allowed */
			if (new->flags & KVM_MEM_LOG_DIRTY_PAGES) {
				ret = -EINVAL;
				break;
			}
		}
		hva = min(reg_end, vma->vm_end);
	} while (hva < reg_end);

	mmap_read_unlock(current->mm);
	return ret;
}

void kvm_arch_free_memslot(struct kvm *kvm, struct kvm_memory_slot *slot)
{
}

void kvm_arch_memslots_updated(struct kvm *kvm, u64 gen)
{
}

void kvm_arch_flush_shadow_all(struct kvm *kvm)
{
	kvm_uninit_stage2_mmu(kvm);
}

void kvm_arch_flush_shadow_memslot(struct kvm *kvm,
				   struct kvm_memory_slot *slot)
{
	gpa_t gpa = slot->base_gfn << PAGE_SHIFT;
	phys_addr_t size = slot->npages << PAGE_SHIFT;

	write_lock(&kvm->mmu_lock);
	unmap_stage2_range(&kvm->arch.mmu, gpa, size);
	write_unlock(&kvm->mmu_lock);
}

/*
 * See note at ARMv7 ARM B1.14.4 (TL;DR: S/W ops are not easily virtualized).
 *
 * Main problems:
 * - S/W ops are local to a CPU (not broadcast)
 * - We have line migration behind our back (speculation)
 * - System caches don't support S/W at all (damn!)
 *
 * In the face of the above, the best we can do is to try and convert
 * S/W ops to VA ops. Because the guest is not allowed to infer the
 * S/W to PA mapping, it can only use S/W to nuke the whole cache,
 * which is a rather good thing for us.
 *
 * Also, it is only used when turning caches on/off ("The expected
 * usage of the cache maintenance instructions that operate by set/way
 * is associated with the cache maintenance instructions associated
 * with the powerdown and powerup of caches, if this is required by
 * the implementation.").
 *
 * We use the following policy:
 *
 * - If we trap a S/W operation, we enable VM trapping to detect
 *   caches being turned on/off, and do a full clean.
 *
 * - We flush the caches on both caches being turned on and off.
 *
 * - Once the caches are enabled, we stop trapping VM ops.
 */
void kvm_set_way_flush(struct kvm_vcpu *vcpu)
{
	unsigned long hcr = *vcpu_hcr(vcpu);

	/*
	 * If this is the first time we do a S/W operation
	 * (i.e. HCR_TVM not set) flush the whole memory, and set the
	 * VM trapping.
	 *
	 * Otherwise, rely on the VM trapping to wait for the MMU +
	 * Caches to be turned off. At that point, we'll be able to
	 * clean the caches again.
	 */
	if (!(hcr & HCR_TVM)) {
		trace_kvm_set_way_flush(*vcpu_pc(vcpu),
					vcpu_has_cache_enabled(vcpu));
		stage2_flush_vm(vcpu->kvm);
		*vcpu_hcr(vcpu) = hcr | HCR_TVM;
	}
}

void kvm_toggle_cache(struct kvm_vcpu *vcpu, bool was_enabled)
{
	bool now_enabled = vcpu_has_cache_enabled(vcpu);

	/*
	 * If switching the MMU+caches on, need to invalidate the caches.
	 * If switching it off, need to clean the caches.
	 * Clean + invalidate does the trick always.
	 */
	if (now_enabled != was_enabled)
		stage2_flush_vm(vcpu->kvm);

	/* Caches are now on, stop trapping VM ops (until a S/W op) */
	if (now_enabled)
		*vcpu_hcr(vcpu) &= ~HCR_TVM;

	trace_kvm_toggle_cache(*vcpu_pc(vcpu), was_enabled, now_enabled);
}
