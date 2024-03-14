/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tools/testing/selftests/kvm/include/kvm_util_base.h
 *
 * Copyright (C) 2018, Google LLC.
 */
#ifndef SELFTEST_KVM_UTIL_BASE_H
#define SELFTEST_KVM_UTIL_BASE_H

#include "test_util.h"

#include <linux/compiler.h>
#include "linux/hashtable.h"
#include "linux/list.h"
#include <linux/kernel.h>
#include <linux/kvm.h>
#include "linux/rbtree.h"
#include <linux/types.h>

#include <asm/atomic.h>

#include <sys/ioctl.h>

#include "sparsebit.h"

/*
 * Provide a version of static_assert() that is guaranteed to have an optional
 * message param.  If _ISOC11_SOURCE is defined, glibc (/usr/include/assert.h)
 * #undefs and #defines static_assert() as a direct alias to _Static_assert(),
 * i.e. effectively makes the message mandatory.  Many KVM selftests #define
 * _GNU_SOURCE for various reasons, and _GNU_SOURCE implies _ISOC11_SOURCE.  As
 * a result, static_assert() behavior is non-deterministic and may or may not
 * require a message depending on #include order.
 */
#define __kvm_static_assert(expr, msg, ...) _Static_assert(expr, msg)
#define kvm_static_assert(expr, ...) __kvm_static_assert(expr, ##__VA_ARGS__, #expr)

#define KVM_DEV_PATH "/dev/kvm"
#define KVM_MAX_VCPUS 512

#define NSEC_PER_SEC 1000000000L

typedef uint64_t vm_paddr_t; /* Virtual Machine (Guest) physical address */
typedef uint64_t vm_vaddr_t; /* Virtual Machine (Guest) virtual address */

struct userspace_mem_region {
	struct kvm_userspace_memory_region region;
	struct sparsebit *unused_phy_pages;
	int fd;
	off_t offset;
	enum vm_mem_backing_src_type backing_src_type;
	void *host_mem;
	void *host_alias;
	void *mmap_start;
	void *mmap_alias;
	size_t mmap_size;
	struct rb_node gpa_node;
	struct rb_node hva_node;
	struct hlist_node slot_node;
};

struct kvm_vcpu {
	struct list_head list;
	uint32_t id;
	int fd;
	struct kvm_vm *vm;
	struct kvm_run *run;
#ifdef __x86_64__
	struct kvm_cpuid2 *cpuid;
#endif
	struct kvm_dirty_gfn *dirty_gfns;
	uint32_t fetch_index;
	uint32_t dirty_gfns_count;
};

struct userspace_mem_regions {
	struct rb_root gpa_tree;
	struct rb_root hva_tree;
	DECLARE_HASHTABLE(slot_hash, 9);
};

enum kvm_mem_region_type {
	MEM_REGION_CODE,
	MEM_REGION_DATA,
	MEM_REGION_PT,
	MEM_REGION_TEST_DATA,
	NR_MEM_REGIONS,
};

struct kvm_vm {
	int mode;
	unsigned long type;
	int kvm_fd;
	int fd;
	unsigned int pgtable_levels;
	unsigned int page_size;
	unsigned int page_shift;
	unsigned int pa_bits;
	unsigned int va_bits;
	uint64_t max_gfn;
	struct list_head vcpus;
	struct userspace_mem_regions regions;
	struct sparsebit *vpages_valid;
	struct sparsebit *vpages_mapped;
	bool has_irqchip;
	bool pgd_created;
	vm_paddr_t ucall_mmio_addr;
	vm_paddr_t pgd;
	vm_vaddr_t gdt;
	vm_vaddr_t tss;
	vm_vaddr_t idt;
	vm_vaddr_t handlers;
	uint32_t dirty_ring_size;

	/* Cache of information for binary stats interface */
	int stats_fd;
	struct kvm_stats_header stats_header;
	struct kvm_stats_desc *stats_desc;

	/*
	 * KVM region slots. These are the default memslots used by page
	 * allocators, e.g., lib/elf uses the memslots[MEM_REGION_CODE]
	 * memslot.
	 */
	uint32_t memslots[NR_MEM_REGIONS];
};

struct vcpu_reg_sublist {
	const char *name;
	long capability;
	int feature;
	bool finalize;
	__u64 *regs;
	__u64 regs_n;
	__u64 *rejects_set;
	__u64 rejects_set_n;
	__u64 *skips_set;
	__u64 skips_set_n;
};

struct vcpu_reg_list {
	char *name;
	struct vcpu_reg_sublist sublists[];
};

#define for_each_sublist(c, s)		\
	for ((s) = &(c)->sublists[0]; (s)->regs; ++(s))

#define kvm_for_each_vcpu(vm, i, vcpu)			\
	for ((i) = 0; (i) <= (vm)->last_vcpu_id; (i)++)	\
		if (!((vcpu) = vm->vcpus[i]))		\
			continue;			\
		else

struct userspace_mem_region *
memslot2region(struct kvm_vm *vm, uint32_t memslot);

static inline struct userspace_mem_region *vm_get_mem_region(struct kvm_vm *vm,
							     enum kvm_mem_region_type type)
{
	assert(type < NR_MEM_REGIONS);
	return memslot2region(vm, vm->memslots[type]);
}

/* Minimum allocated guest virtual and physical addresses */
#define KVM_UTIL_MIN_VADDR		0x2000
#define KVM_GUEST_PAGE_TABLE_MIN_PADDR	0x180000

#define DEFAULT_GUEST_STACK_VADDR_MIN	0xab6000
#define DEFAULT_STACK_PGS		5

enum vm_guest_mode {
	VM_MODE_P52V48_4K,
	VM_MODE_P52V48_64K,
	VM_MODE_P48V48_4K,
	VM_MODE_P48V48_16K,
	VM_MODE_P48V48_64K,
	VM_MODE_P40V48_4K,
	VM_MODE_P40V48_16K,
	VM_MODE_P40V48_64K,
	VM_MODE_PXXV48_4K,	/* For 48bits VA but ANY bits PA */
	VM_MODE_P47V64_4K,
	VM_MODE_P44V64_4K,
	VM_MODE_P36V48_4K,
	VM_MODE_P36V48_16K,
	VM_MODE_P36V48_64K,
	VM_MODE_P36V47_16K,
	NUM_VM_MODES,
};

#if defined(__aarch64__)

extern enum vm_guest_mode vm_mode_default;

#define VM_MODE_DEFAULT			vm_mode_default
#define MIN_PAGE_SHIFT			12U
#define ptes_per_page(page_size)	((page_size) / 8)

#elif defined(__x86_64__)

#define VM_MODE_DEFAULT			VM_MODE_PXXV48_4K
#define MIN_PAGE_SHIFT			12U
#define ptes_per_page(page_size)	((page_size) / 8)

#elif defined(__s390x__)

#define VM_MODE_DEFAULT			VM_MODE_P44V64_4K
#define MIN_PAGE_SHIFT			12U
#define ptes_per_page(page_size)	((page_size) / 16)

#elif defined(__riscv)

#if __riscv_xlen == 32
#error "RISC-V 32-bit kvm selftests not supported"
#endif

#define VM_MODE_DEFAULT			VM_MODE_P40V48_4K
#define MIN_PAGE_SHIFT			12U
#define ptes_per_page(page_size)	((page_size) / 8)

#endif

#define MIN_PAGE_SIZE		(1U << MIN_PAGE_SHIFT)
#define PTES_PER_MIN_PAGE	ptes_per_page(MIN_PAGE_SIZE)

struct vm_guest_mode_params {
	unsigned int pa_bits;
	unsigned int va_bits;
	unsigned int page_size;
	unsigned int page_shift;
};
extern const struct vm_guest_mode_params vm_guest_mode_params[];

int open_path_or_exit(const char *path, int flags);
int open_kvm_dev_path_or_exit(void);

bool get_kvm_param_bool(const char *param);
bool get_kvm_intel_param_bool(const char *param);
bool get_kvm_amd_param_bool(const char *param);

unsigned int kvm_check_cap(long cap);

static inline bool kvm_has_cap(long cap)
{
	return kvm_check_cap(cap);
}

#define __KVM_SYSCALL_ERROR(_name, _ret) \
	"%s failed, rc: %i errno: %i (%s)", (_name), (_ret), errno, strerror(errno)

#define __KVM_IOCTL_ERROR(_name, _ret)	__KVM_SYSCALL_ERROR(_name, _ret)
#define KVM_IOCTL_ERROR(_ioctl, _ret) __KVM_IOCTL_ERROR(#_ioctl, _ret)

#define kvm_do_ioctl(fd, cmd, arg)						\
({										\
	kvm_static_assert(!_IOC_SIZE(cmd) || sizeof(*arg) == _IOC_SIZE(cmd));	\
	ioctl(fd, cmd, arg);							\
})

#define __kvm_ioctl(kvm_fd, cmd, arg)				\
	kvm_do_ioctl(kvm_fd, cmd, arg)


#define _kvm_ioctl(kvm_fd, cmd, name, arg)			\
({								\
	int ret = __kvm_ioctl(kvm_fd, cmd, arg);		\
								\
	TEST_ASSERT(!ret, __KVM_IOCTL_ERROR(name, ret));	\
})

#define kvm_ioctl(kvm_fd, cmd, arg) \
	_kvm_ioctl(kvm_fd, cmd, #cmd, arg)

static __always_inline void static_assert_is_vm(struct kvm_vm *vm) { }

#define __vm_ioctl(vm, cmd, arg)				\
({								\
	static_assert_is_vm(vm);				\
	kvm_do_ioctl((vm)->fd, cmd, arg);			\
})

#define _vm_ioctl(vm, cmd, name, arg)				\
({								\
	int ret = __vm_ioctl(vm, cmd, arg);			\
								\
	TEST_ASSERT(!ret, __KVM_IOCTL_ERROR(name, ret));	\
})

#define vm_ioctl(vm, cmd, arg)					\
	_vm_ioctl(vm, cmd, #cmd, arg)


static __always_inline void static_assert_is_vcpu(struct kvm_vcpu *vcpu) { }

#define __vcpu_ioctl(vcpu, cmd, arg)				\
({								\
	static_assert_is_vcpu(vcpu);				\
	kvm_do_ioctl((vcpu)->fd, cmd, arg);			\
})

#define _vcpu_ioctl(vcpu, cmd, name, arg)			\
({								\
	int ret = __vcpu_ioctl(vcpu, cmd, arg);			\
								\
	TEST_ASSERT(!ret, __KVM_IOCTL_ERROR(name, ret));	\
})

#define vcpu_ioctl(vcpu, cmd, arg)				\
	_vcpu_ioctl(vcpu, cmd, #cmd, arg)

/*
 * Looks up and returns the value corresponding to the capability
 * (KVM_CAP_*) given by cap.
 */
static inline int vm_check_cap(struct kvm_vm *vm, long cap)
{
	int ret =  __vm_ioctl(vm, KVM_CHECK_EXTENSION, (void *)cap);

	TEST_ASSERT(ret >= 0, KVM_IOCTL_ERROR(KVM_CHECK_EXTENSION, ret));
	return ret;
}

static inline int __vm_enable_cap(struct kvm_vm *vm, uint32_t cap, uint64_t arg0)
{
	struct kvm_enable_cap enable_cap = { .cap = cap, .args = { arg0 } };

	return __vm_ioctl(vm, KVM_ENABLE_CAP, &enable_cap);
}
static inline void vm_enable_cap(struct kvm_vm *vm, uint32_t cap, uint64_t arg0)
{
	struct kvm_enable_cap enable_cap = { .cap = cap, .args = { arg0 } };

	vm_ioctl(vm, KVM_ENABLE_CAP, &enable_cap);
}

void vm_enable_dirty_ring(struct kvm_vm *vm, uint32_t ring_size);
const char *vm_guest_mode_string(uint32_t i);

void kvm_vm_free(struct kvm_vm *vmp);
void kvm_vm_restart(struct kvm_vm *vmp);
void kvm_vm_release(struct kvm_vm *vmp);
int kvm_memcmp_hva_gva(void *hva, struct kvm_vm *vm, const vm_vaddr_t gva,
		       size_t len);
void kvm_vm_elf_load(struct kvm_vm *vm, const char *filename);
int kvm_memfd_alloc(size_t size, bool hugepages);

void vm_dump(FILE *stream, struct kvm_vm *vm, uint8_t indent);

static inline void kvm_vm_get_dirty_log(struct kvm_vm *vm, int slot, void *log)
{
	struct kvm_dirty_log args = { .dirty_bitmap = log, .slot = slot };

	vm_ioctl(vm, KVM_GET_DIRTY_LOG, &args);
}

static inline void kvm_vm_clear_dirty_log(struct kvm_vm *vm, int slot, void *log,
					  uint64_t first_page, uint32_t num_pages)
{
	struct kvm_clear_dirty_log args = {
		.dirty_bitmap = log,
		.slot = slot,
		.first_page = first_page,
		.num_pages = num_pages
	};

	vm_ioctl(vm, KVM_CLEAR_DIRTY_LOG, &args);
}

static inline uint32_t kvm_vm_reset_dirty_ring(struct kvm_vm *vm)
{
	return __vm_ioctl(vm, KVM_RESET_DIRTY_RINGS, NULL);
}

static inline int vm_get_stats_fd(struct kvm_vm *vm)
{
	int fd = __vm_ioctl(vm, KVM_GET_STATS_FD, NULL);

	TEST_ASSERT(fd >= 0, KVM_IOCTL_ERROR(KVM_GET_STATS_FD, fd));
	return fd;
}

static inline void read_stats_header(int stats_fd, struct kvm_stats_header *header)
{
	ssize_t ret;

	ret = pread(stats_fd, header, sizeof(*header), 0);
	TEST_ASSERT(ret == sizeof(*header),
		    "Failed to read '%lu' header bytes, ret = '%ld'",
		    sizeof(*header), ret);
}

struct kvm_stats_desc *read_stats_descriptors(int stats_fd,
					      struct kvm_stats_header *header);

static inline ssize_t get_stats_descriptor_size(struct kvm_stats_header *header)
{
	 /*
	  * The base size of the descriptor is defined by KVM's ABI, but the
	  * size of the name field is variable, as far as KVM's ABI is
	  * concerned. For a given instance of KVM, the name field is the same
	  * size for all stats and is provided in the overall stats header.
	  */
	return sizeof(struct kvm_stats_desc) + header->name_size;
}

static inline struct kvm_stats_desc *get_stats_descriptor(struct kvm_stats_desc *stats,
							  int index,
							  struct kvm_stats_header *header)
{
	/*
	 * Note, size_desc includes the size of the name field, which is
	 * variable. i.e. this is NOT equivalent to &stats_desc[i].
	 */
	return (void *)stats + index * get_stats_descriptor_size(header);
}

void read_stat_data(int stats_fd, struct kvm_stats_header *header,
		    struct kvm_stats_desc *desc, uint64_t *data,
		    size_t max_elements);

void __vm_get_stat(struct kvm_vm *vm, const char *stat_name, uint64_t *data,
		   size_t max_elements);

static inline uint64_t vm_get_stat(struct kvm_vm *vm, const char *stat_name)
{
	uint64_t data;

	__vm_get_stat(vm, stat_name, &data, 1);
	return data;
}

void vm_create_irqchip(struct kvm_vm *vm);

void vm_set_user_memory_region(struct kvm_vm *vm, uint32_t slot, uint32_t flags,
			       uint64_t gpa, uint64_t size, void *hva);
int __vm_set_user_memory_region(struct kvm_vm *vm, uint32_t slot, uint32_t flags,
				uint64_t gpa, uint64_t size, void *hva);
void vm_userspace_mem_region_add(struct kvm_vm *vm,
	enum vm_mem_backing_src_type src_type,
	uint64_t guest_paddr, uint32_t slot, uint64_t npages,
	uint32_t flags);

void vm_mem_region_set_flags(struct kvm_vm *vm, uint32_t slot, uint32_t flags);
void vm_mem_region_move(struct kvm_vm *vm, uint32_t slot, uint64_t new_gpa);
void vm_mem_region_delete(struct kvm_vm *vm, uint32_t slot);
struct kvm_vcpu *__vm_vcpu_add(struct kvm_vm *vm, uint32_t vcpu_id);
void vm_populate_vaddr_bitmap(struct kvm_vm *vm);
vm_vaddr_t vm_vaddr_unused_gap(struct kvm_vm *vm, size_t sz, vm_vaddr_t vaddr_min);
vm_vaddr_t vm_vaddr_alloc(struct kvm_vm *vm, size_t sz, vm_vaddr_t vaddr_min);
vm_vaddr_t __vm_vaddr_alloc(struct kvm_vm *vm, size_t sz, vm_vaddr_t vaddr_min,
			    enum kvm_mem_region_type type);
vm_vaddr_t vm_vaddr_alloc_pages(struct kvm_vm *vm, int nr_pages);
vm_vaddr_t __vm_vaddr_alloc_page(struct kvm_vm *vm,
				 enum kvm_mem_region_type type);
vm_vaddr_t vm_vaddr_alloc_page(struct kvm_vm *vm);

void virt_map(struct kvm_vm *vm, uint64_t vaddr, uint64_t paddr,
	      unsigned int npages);
void *addr_gpa2hva(struct kvm_vm *vm, vm_paddr_t gpa);
void *addr_gva2hva(struct kvm_vm *vm, vm_vaddr_t gva);
vm_paddr_t addr_hva2gpa(struct kvm_vm *vm, void *hva);
void *addr_gpa2alias(struct kvm_vm *vm, vm_paddr_t gpa);

void vcpu_run(struct kvm_vcpu *vcpu);
int _vcpu_run(struct kvm_vcpu *vcpu);

static inline int __vcpu_run(struct kvm_vcpu *vcpu)
{
	return __vcpu_ioctl(vcpu, KVM_RUN, NULL);
}

void vcpu_run_complete_io(struct kvm_vcpu *vcpu);
struct kvm_reg_list *vcpu_get_reg_list(struct kvm_vcpu *vcpu);

static inline void vcpu_enable_cap(struct kvm_vcpu *vcpu, uint32_t cap,
				   uint64_t arg0)
{
	struct kvm_enable_cap enable_cap = { .cap = cap, .args = { arg0 } };

	vcpu_ioctl(vcpu, KVM_ENABLE_CAP, &enable_cap);
}

static inline void vcpu_guest_debug_set(struct kvm_vcpu *vcpu,
					struct kvm_guest_debug *debug)
{
	vcpu_ioctl(vcpu, KVM_SET_GUEST_DEBUG, debug);
}

static inline void vcpu_mp_state_get(struct kvm_vcpu *vcpu,
				     struct kvm_mp_state *mp_state)
{
	vcpu_ioctl(vcpu, KVM_GET_MP_STATE, mp_state);
}
static inline void vcpu_mp_state_set(struct kvm_vcpu *vcpu,
				     struct kvm_mp_state *mp_state)
{
	vcpu_ioctl(vcpu, KVM_SET_MP_STATE, mp_state);
}

static inline void vcpu_regs_get(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	vcpu_ioctl(vcpu, KVM_GET_REGS, regs);
}

static inline void vcpu_regs_set(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	vcpu_ioctl(vcpu, KVM_SET_REGS, regs);
}
static inline void vcpu_sregs_get(struct kvm_vcpu *vcpu, struct kvm_sregs *sregs)
{
	vcpu_ioctl(vcpu, KVM_GET_SREGS, sregs);

}
static inline void vcpu_sregs_set(struct kvm_vcpu *vcpu, struct kvm_sregs *sregs)
{
	vcpu_ioctl(vcpu, KVM_SET_SREGS, sregs);
}
static inline int _vcpu_sregs_set(struct kvm_vcpu *vcpu, struct kvm_sregs *sregs)
{
	return __vcpu_ioctl(vcpu, KVM_SET_SREGS, sregs);
}
static inline void vcpu_fpu_get(struct kvm_vcpu *vcpu, struct kvm_fpu *fpu)
{
	vcpu_ioctl(vcpu, KVM_GET_FPU, fpu);
}
static inline void vcpu_fpu_set(struct kvm_vcpu *vcpu, struct kvm_fpu *fpu)
{
	vcpu_ioctl(vcpu, KVM_SET_FPU, fpu);
}

static inline int __vcpu_get_reg(struct kvm_vcpu *vcpu, uint64_t id, void *addr)
{
	struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)addr };

	return __vcpu_ioctl(vcpu, KVM_GET_ONE_REG, &reg);
}
static inline int __vcpu_set_reg(struct kvm_vcpu *vcpu, uint64_t id, uint64_t val)
{
	struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)&val };

	return __vcpu_ioctl(vcpu, KVM_SET_ONE_REG, &reg);
}
static inline void vcpu_get_reg(struct kvm_vcpu *vcpu, uint64_t id, void *addr)
{
	struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)addr };

	vcpu_ioctl(vcpu, KVM_GET_ONE_REG, &reg);
}
static inline void vcpu_set_reg(struct kvm_vcpu *vcpu, uint64_t id, uint64_t val)
{
	struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)&val };

	vcpu_ioctl(vcpu, KVM_SET_ONE_REG, &reg);
}

#ifdef __KVM_HAVE_VCPU_EVENTS
static inline void vcpu_events_get(struct kvm_vcpu *vcpu,
				   struct kvm_vcpu_events *events)
{
	vcpu_ioctl(vcpu, KVM_GET_VCPU_EVENTS, events);
}
static inline void vcpu_events_set(struct kvm_vcpu *vcpu,
				   struct kvm_vcpu_events *events)
{
	vcpu_ioctl(vcpu, KVM_SET_VCPU_EVENTS, events);
}
#endif
#ifdef __x86_64__
static inline void vcpu_nested_state_get(struct kvm_vcpu *vcpu,
					 struct kvm_nested_state *state)
{
	vcpu_ioctl(vcpu, KVM_GET_NESTED_STATE, state);
}
static inline int __vcpu_nested_state_set(struct kvm_vcpu *vcpu,
					  struct kvm_nested_state *state)
{
	return __vcpu_ioctl(vcpu, KVM_SET_NESTED_STATE, state);
}

static inline void vcpu_nested_state_set(struct kvm_vcpu *vcpu,
					 struct kvm_nested_state *state)
{
	vcpu_ioctl(vcpu, KVM_SET_NESTED_STATE, state);
}
#endif
static inline int vcpu_get_stats_fd(struct kvm_vcpu *vcpu)
{
	int fd = __vcpu_ioctl(vcpu, KVM_GET_STATS_FD, NULL);

	TEST_ASSERT(fd >= 0, KVM_IOCTL_ERROR(KVM_GET_STATS_FD, fd));
	return fd;
}

int __kvm_has_device_attr(int dev_fd, uint32_t group, uint64_t attr);

static inline void kvm_has_device_attr(int dev_fd, uint32_t group, uint64_t attr)
{
	int ret = __kvm_has_device_attr(dev_fd, group, attr);

	TEST_ASSERT(!ret, "KVM_HAS_DEVICE_ATTR failed, rc: %i errno: %i", ret, errno);
}

int __kvm_device_attr_get(int dev_fd, uint32_t group, uint64_t attr, void *val);

static inline void kvm_device_attr_get(int dev_fd, uint32_t group,
				       uint64_t attr, void *val)
{
	int ret = __kvm_device_attr_get(dev_fd, group, attr, val);

	TEST_ASSERT(!ret, KVM_IOCTL_ERROR(KVM_GET_DEVICE_ATTR, ret));
}

int __kvm_device_attr_set(int dev_fd, uint32_t group, uint64_t attr, void *val);

static inline void kvm_device_attr_set(int dev_fd, uint32_t group,
				       uint64_t attr, void *val)
{
	int ret = __kvm_device_attr_set(dev_fd, group, attr, val);

	TEST_ASSERT(!ret, KVM_IOCTL_ERROR(KVM_SET_DEVICE_ATTR, ret));
}

static inline int __vcpu_has_device_attr(struct kvm_vcpu *vcpu, uint32_t group,
					 uint64_t attr)
{
	return __kvm_has_device_attr(vcpu->fd, group, attr);
}

static inline void vcpu_has_device_attr(struct kvm_vcpu *vcpu, uint32_t group,
					uint64_t attr)
{
	kvm_has_device_attr(vcpu->fd, group, attr);
}

static inline int __vcpu_device_attr_get(struct kvm_vcpu *vcpu, uint32_t group,
					 uint64_t attr, void *val)
{
	return __kvm_device_attr_get(vcpu->fd, group, attr, val);
}

static inline void vcpu_device_attr_get(struct kvm_vcpu *vcpu, uint32_t group,
					uint64_t attr, void *val)
{
	kvm_device_attr_get(vcpu->fd, group, attr, val);
}

static inline int __vcpu_device_attr_set(struct kvm_vcpu *vcpu, uint32_t group,
					 uint64_t attr, void *val)
{
	return __kvm_device_attr_set(vcpu->fd, group, attr, val);
}

static inline void vcpu_device_attr_set(struct kvm_vcpu *vcpu, uint32_t group,
					uint64_t attr, void *val)
{
	kvm_device_attr_set(vcpu->fd, group, attr, val);
}

int __kvm_test_create_device(struct kvm_vm *vm, uint64_t type);
int __kvm_create_device(struct kvm_vm *vm, uint64_t type);

static inline int kvm_create_device(struct kvm_vm *vm, uint64_t type)
{
	int fd = __kvm_create_device(vm, type);

	TEST_ASSERT(fd >= 0, KVM_IOCTL_ERROR(KVM_CREATE_DEVICE, fd));
	return fd;
}

void *vcpu_map_dirty_ring(struct kvm_vcpu *vcpu);

/*
 * VM VCPU Args Set
 *
 * Input Args:
 *   vm - Virtual Machine
 *   num - number of arguments
 *   ... - arguments, each of type uint64_t
 *
 * Output Args: None
 *
 * Return: None
 *
 * Sets the first @num input parameters for the function at @vcpu's entry point,
 * per the C calling convention of the architecture, to the values given as
 * variable args. Each of the variable args is expected to be of type uint64_t.
 * The maximum @num can be is specific to the architecture.
 */
void vcpu_args_set(struct kvm_vcpu *vcpu, unsigned int num, ...);

void kvm_irq_line(struct kvm_vm *vm, uint32_t irq, int level);
int _kvm_irq_line(struct kvm_vm *vm, uint32_t irq, int level);

#define KVM_MAX_IRQ_ROUTES		4096

struct kvm_irq_routing *kvm_gsi_routing_create(void);
void kvm_gsi_routing_irqchip_add(struct kvm_irq_routing *routing,
		uint32_t gsi, uint32_t pin);
int _kvm_gsi_routing_write(struct kvm_vm *vm, struct kvm_irq_routing *routing);
void kvm_gsi_routing_write(struct kvm_vm *vm, struct kvm_irq_routing *routing);

const char *exit_reason_str(unsigned int exit_reason);

vm_paddr_t vm_phy_page_alloc(struct kvm_vm *vm, vm_paddr_t paddr_min,
			     uint32_t memslot);
vm_paddr_t vm_phy_pages_alloc(struct kvm_vm *vm, size_t num,
			      vm_paddr_t paddr_min, uint32_t memslot);
vm_paddr_t vm_alloc_page_table(struct kvm_vm *vm);

/*
 * ____vm_create() does KVM_CREATE_VM and little else.  __vm_create() also
 * loads the test binary into guest memory and creates an IRQ chip (x86 only).
 * __vm_create() does NOT create vCPUs, @nr_runnable_vcpus is used purely to
 * calculate the amount of memory needed for per-vCPU data, e.g. stacks.
 */
struct kvm_vm *____vm_create(enum vm_guest_mode mode);
struct kvm_vm *__vm_create(enum vm_guest_mode mode, uint32_t nr_runnable_vcpus,
			   uint64_t nr_extra_pages);

static inline struct kvm_vm *vm_create_barebones(void)
{
	return ____vm_create(VM_MODE_DEFAULT);
}

static inline struct kvm_vm *vm_create(uint32_t nr_runnable_vcpus)
{
	return __vm_create(VM_MODE_DEFAULT, nr_runnable_vcpus, 0);
}

struct kvm_vm *__vm_create_with_vcpus(enum vm_guest_mode mode, uint32_t nr_vcpus,
				      uint64_t extra_mem_pages,
				      void *guest_code, struct kvm_vcpu *vcpus[]);

static inline struct kvm_vm *vm_create_with_vcpus(uint32_t nr_vcpus,
						  void *guest_code,
						  struct kvm_vcpu *vcpus[])
{
	return __vm_create_with_vcpus(VM_MODE_DEFAULT, nr_vcpus, 0,
				      guest_code, vcpus);
}

/*
 * Create a VM with a single vCPU with reasonable defaults and @extra_mem_pages
 * additional pages of guest memory.  Returns the VM and vCPU (via out param).
 */
struct kvm_vm *__vm_create_with_one_vcpu(struct kvm_vcpu **vcpu,
					 uint64_t extra_mem_pages,
					 void *guest_code);

static inline struct kvm_vm *vm_create_with_one_vcpu(struct kvm_vcpu **vcpu,
						     void *guest_code)
{
	return __vm_create_with_one_vcpu(vcpu, 0, guest_code);
}

struct kvm_vcpu *vm_recreate_with_one_vcpu(struct kvm_vm *vm);

void kvm_pin_this_task_to_pcpu(uint32_t pcpu);
void kvm_print_vcpu_pinning_help(void);
void kvm_parse_vcpu_pinning(const char *pcpus_string, uint32_t vcpu_to_pcpu[],
			    int nr_vcpus);

unsigned long vm_compute_max_gfn(struct kvm_vm *vm);
unsigned int vm_calc_num_guest_pages(enum vm_guest_mode mode, size_t size);
unsigned int vm_num_host_pages(enum vm_guest_mode mode, unsigned int num_guest_pages);
unsigned int vm_num_guest_pages(enum vm_guest_mode mode, unsigned int num_host_pages);
static inline unsigned int
vm_adjust_num_guest_pages(enum vm_guest_mode mode, unsigned int num_guest_pages)
{
	unsigned int n;
	n = vm_num_guest_pages(mode, vm_num_host_pages(mode, num_guest_pages));
#ifdef __s390x__
	/* s390 requires 1M aligned guest sizes */
	n = (n + 255) & ~255;
#endif
	return n;
}

struct kvm_userspace_memory_region *
kvm_userspace_memory_region_find(struct kvm_vm *vm, uint64_t start,
				 uint64_t end);

#define sync_global_to_guest(vm, g) ({				\
	typeof(g) *_p = addr_gva2hva(vm, (vm_vaddr_t)&(g));	\
	memcpy(_p, &(g), sizeof(g));				\
})

#define sync_global_from_guest(vm, g) ({			\
	typeof(g) *_p = addr_gva2hva(vm, (vm_vaddr_t)&(g));	\
	memcpy(&(g), _p, sizeof(g));				\
})

/*
 * Write a global value, but only in the VM's (guest's) domain.  Primarily used
 * for "globals" that hold per-VM values (VMs always duplicate code and global
 * data into their own region of physical memory), but can be used anytime it's
 * undesirable to change the host's copy of the global.
 */
#define write_guest_global(vm, g, val) ({			\
	typeof(g) *_p = addr_gva2hva(vm, (vm_vaddr_t)&(g));	\
	typeof(g) _val = val;					\
								\
	memcpy(_p, &(_val), sizeof(g));				\
})

void assert_on_unhandled_exception(struct kvm_vcpu *vcpu);

void vcpu_arch_dump(FILE *stream, struct kvm_vcpu *vcpu,
		    uint8_t indent);

static inline void vcpu_dump(FILE *stream, struct kvm_vcpu *vcpu,
			     uint8_t indent)
{
	vcpu_arch_dump(stream, vcpu, indent);
}

/*
 * Adds a vCPU with reasonable defaults (e.g. a stack)
 *
 * Input Args:
 *   vm - Virtual Machine
 *   vcpu_id - The id of the VCPU to add to the VM.
 *   guest_code - The vCPU's entry point
 */
struct kvm_vcpu *vm_arch_vcpu_add(struct kvm_vm *vm, uint32_t vcpu_id,
				  void *guest_code);

static inline struct kvm_vcpu *vm_vcpu_add(struct kvm_vm *vm, uint32_t vcpu_id,
					   void *guest_code)
{
	return vm_arch_vcpu_add(vm, vcpu_id, guest_code);
}

/* Re-create a vCPU after restarting a VM, e.g. for state save/restore tests. */
struct kvm_vcpu *vm_arch_vcpu_recreate(struct kvm_vm *vm, uint32_t vcpu_id);

static inline struct kvm_vcpu *vm_vcpu_recreate(struct kvm_vm *vm,
						uint32_t vcpu_id)
{
	return vm_arch_vcpu_recreate(vm, vcpu_id);
}

void vcpu_arch_free(struct kvm_vcpu *vcpu);

void virt_arch_pgd_alloc(struct kvm_vm *vm);

static inline void virt_pgd_alloc(struct kvm_vm *vm)
{
	virt_arch_pgd_alloc(vm);
}

/*
 * VM Virtual Page Map
 *
 * Input Args:
 *   vm - Virtual Machine
 *   vaddr - VM Virtual Address
 *   paddr - VM Physical Address
 *   memslot - Memory region slot for new virtual translation tables
 *
 * Output Args: None
 *
 * Return: None
 *
 * Within @vm, creates a virtual translation for the page starting
 * at @vaddr to the page starting at @paddr.
 */
void virt_arch_pg_map(struct kvm_vm *vm, uint64_t vaddr, uint64_t paddr);

static inline void virt_pg_map(struct kvm_vm *vm, uint64_t vaddr, uint64_t paddr)
{
	virt_arch_pg_map(vm, vaddr, paddr);
}


/*
 * Address Guest Virtual to Guest Physical
 *
 * Input Args:
 *   vm - Virtual Machine
 *   gva - VM virtual address
 *
 * Output Args: None
 *
 * Return:
 *   Equivalent VM physical address
 *
 * Returns the VM physical address of the translated VM virtual
 * address given by @gva.
 */
vm_paddr_t addr_arch_gva2gpa(struct kvm_vm *vm, vm_vaddr_t gva);

static inline vm_paddr_t addr_gva2gpa(struct kvm_vm *vm, vm_vaddr_t gva)
{
	return addr_arch_gva2gpa(vm, gva);
}

/*
 * Virtual Translation Tables Dump
 *
 * Input Args:
 *   stream - Output FILE stream
 *   vm     - Virtual Machine
 *   indent - Left margin indent amount
 *
 * Output Args: None
 *
 * Return: None
 *
 * Dumps to the FILE stream given by @stream, the contents of all the
 * virtual translation tables for the VM given by @vm.
 */
void virt_arch_dump(FILE *stream, struct kvm_vm *vm, uint8_t indent);

static inline void virt_dump(FILE *stream, struct kvm_vm *vm, uint8_t indent)
{
	virt_arch_dump(stream, vm, indent);
}


static inline int __vm_disable_nx_huge_pages(struct kvm_vm *vm)
{
	return __vm_enable_cap(vm, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES, 0);
}

/*
 * Arch hook that is invoked via a constructor, i.e. before exeucting main(),
 * to allow for arch-specific setup that is common to all tests, e.g. computing
 * the default guest "mode".
 */
void kvm_selftest_arch_init(void);

void kvm_arch_vm_post_create(struct kvm_vm *vm);

#endif /* SELFTEST_KVM_UTIL_BASE_H */
