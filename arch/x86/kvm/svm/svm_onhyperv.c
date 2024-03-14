// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM L1 hypervisor optimizations on Hyper-V for SVM.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kvm_host.h>

#include <asm/mshyperv.h>

#include "svm.h"
#include "svm_ops.h"

#include "hyperv.h"
#include "kvm_onhyperv.h"
#include "svm_onhyperv.h"

int svm_hv_enable_l2_tlb_flush(struct kvm_vcpu *vcpu)
{
	struct hv_vmcb_enlightenments *hve;
	struct hv_partition_assist_pg **p_hv_pa_pg =
			&to_kvm_hv(vcpu->kvm)->hv_pa_pg;

	if (!*p_hv_pa_pg)
		*p_hv_pa_pg = kzalloc(PAGE_SIZE, GFP_KERNEL);

	if (!*p_hv_pa_pg)
		return -ENOMEM;

	hve = &to_svm(vcpu)->vmcb->control.hv_enlightenments;

	hve->partition_assist_page = __pa(*p_hv_pa_pg);
	hve->hv_vm_id = (unsigned long)vcpu->kvm;
	if (!hve->hv_enlightenments_control.nested_flush_hypercall) {
		hve->hv_enlightenments_control.nested_flush_hypercall = 1;
		vmcb_mark_dirty(to_svm(vcpu)->vmcb, HV_VMCB_NESTED_ENLIGHTENMENTS);
	}

	return 0;
}

