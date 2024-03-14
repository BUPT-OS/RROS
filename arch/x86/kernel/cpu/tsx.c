// SPDX-License-Identifier: GPL-2.0
/*
 * Intel Transactional Synchronization Extensions (TSX) control.
 *
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * Author:
 *	Pawan Gupta <pawan.kumar.gupta@linux.intel.com>
 */

#include <linux/cpufeature.h>

#include <asm/cmdline.h>
#include <asm/cpu.h>

#include "cpu.h"

#undef pr_fmt
#define pr_fmt(fmt) "tsx: " fmt

enum tsx_ctrl_states tsx_ctrl_state __ro_after_init = TSX_CTRL_NOT_SUPPORTED;

static void tsx_disable(void)
{
	u64 tsx;

	rdmsrl(MSR_IA32_TSX_CTRL, tsx);

	/* Force all transactions to immediately abort */
	tsx |= TSX_CTRL_RTM_DISABLE;

	/*
	 * Ensure TSX support is not enumerated in CPUID.
	 * This is visible to userspace and will ensure they
	 * do not waste resources trying TSX transactions that
	 * will always abort.
	 */
	tsx |= TSX_CTRL_CPUID_CLEAR;

	wrmsrl(MSR_IA32_TSX_CTRL, tsx);
}

static void tsx_enable(void)
{
	u64 tsx;

	rdmsrl(MSR_IA32_TSX_CTRL, tsx);

	/* Enable the RTM feature in the cpu */
	tsx &= ~TSX_CTRL_RTM_DISABLE;

	/*
	 * Ensure TSX support is enumerated in CPUID.
	 * This is visible to userspace and will ensure they
	 * can enumerate and use the TSX feature.
	 */
	tsx &= ~TSX_CTRL_CPUID_CLEAR;

	wrmsrl(MSR_IA32_TSX_CTRL, tsx);
}

static enum tsx_ctrl_states x86_get_tsx_auto_mode(void)
{
	if (boot_cpu_has_bug(X86_BUG_TAA))
		return TSX_CTRL_DISABLE;

	return TSX_CTRL_ENABLE;
}

/*
 * Disabling TSX is not a trivial business.
 *
 * First of all, there's a CPUID bit: X86_FEATURE_RTM_ALWAYS_ABORT
 * which says that TSX is practically disabled (all transactions are
 * aborted by default). When that bit is set, the kernel unconditionally
 * disables TSX.
 *
 * In order to do that, however, it needs to dance a bit:
 *
 * 1. The first method to disable it is through MSR_TSX_FORCE_ABORT and
 * the MSR is present only when *two* CPUID bits are set:
 *
 * - X86_FEATURE_RTM_ALWAYS_ABORT
 * - X86_FEATURE_TSX_FORCE_ABORT
 *
 * 2. The second method is for CPUs which do not have the above-mentioned
 * MSR: those use a different MSR - MSR_IA32_TSX_CTRL and disable TSX
 * through that one. Those CPUs can also have the initially mentioned
 * CPUID bit X86_FEATURE_RTM_ALWAYS_ABORT set and for those the same strategy
 * applies: TSX gets disabled unconditionally.
 *
 * When either of the two methods are present, the kernel disables TSX and
 * clears the respective RTM and HLE feature flags.
 *
 * An additional twist in the whole thing presents late microcode loading
 * which, when done, may cause for the X86_FEATURE_RTM_ALWAYS_ABORT CPUID
 * bit to be set after the update.
 *
 * A subsequent hotplug operation on any logical CPU except the BSP will
 * cause for the supported CPUID feature bits to get re-detected and, if
 * RTM and HLE get cleared all of a sudden, but, userspace did consult
 * them before the update, then funny explosions will happen. Long story
 * short: the kernel doesn't modify CPUID feature bits after booting.
 *
 * That's why, this function's call in init_intel() doesn't clear the
 * feature flags.
 */
static void tsx_clear_cpuid(void)
{
	u64 msr;

	/*
	 * MSR_TFA_TSX_CPUID_CLEAR bit is only present when both CPUID
	 * bits RTM_ALWAYS_ABORT and TSX_FORCE_ABORT are present.
	 */
	if (boot_cpu_has(X86_FEATURE_RTM_ALWAYS_ABORT) &&
	    boot_cpu_has(X86_FEATURE_TSX_FORCE_ABORT)) {
		rdmsrl(MSR_TSX_FORCE_ABORT, msr);
		msr |= MSR_TFA_TSX_CPUID_CLEAR;
		wrmsrl(MSR_TSX_FORCE_ABORT, msr);
	} else if (cpu_feature_enabled(X86_FEATURE_MSR_TSX_CTRL)) {
		rdmsrl(MSR_IA32_TSX_CTRL, msr);
		msr |= TSX_CTRL_CPUID_CLEAR;
		wrmsrl(MSR_IA32_TSX_CTRL, msr);
	}
}

/*
 * Disable TSX development mode
 *
 * When the microcode released in Feb 2022 is applied, TSX will be disabled by
 * default on some processors. MSR 0x122 (TSX_CTRL) and MSR 0x123
 * (IA32_MCU_OPT_CTRL) can be used to re-enable TSX for development, doing so is
 * not recommended for production deployments. In particular, applying MD_CLEAR
 * flows for mitigation of the Intel TSX Asynchronous Abort (TAA) transient
 * execution attack may not be effective on these processors when Intel TSX is
 * enabled with updated microcode.
 */
static void tsx_dev_mode_disable(void)
{
	u64 mcu_opt_ctrl;

	/* Check if RTM_ALLOW exists */
	if (!boot_cpu_has_bug(X86_BUG_TAA) ||
	    !cpu_feature_enabled(X86_FEATURE_MSR_TSX_CTRL) ||
	    !cpu_feature_enabled(X86_FEATURE_SRBDS_CTRL))
		return;

	rdmsrl(MSR_IA32_MCU_OPT_CTRL, mcu_opt_ctrl);

	if (mcu_opt_ctrl & RTM_ALLOW) {
		mcu_opt_ctrl &= ~RTM_ALLOW;
		wrmsrl(MSR_IA32_MCU_OPT_CTRL, mcu_opt_ctrl);
		setup_force_cpu_cap(X86_FEATURE_RTM_ALWAYS_ABORT);
	}
}

void __init tsx_init(void)
{
	char arg[5] = {};
	int ret;

	tsx_dev_mode_disable();

	/*
	 * Hardware will always abort a TSX transaction when the CPUID bit
	 * RTM_ALWAYS_ABORT is set. In this case, it is better not to enumerate
	 * CPUID.RTM and CPUID.HLE bits. Clear them here.
	 */
	if (boot_cpu_has(X86_FEATURE_RTM_ALWAYS_ABORT)) {
		tsx_ctrl_state = TSX_CTRL_RTM_ALWAYS_ABORT;
		tsx_clear_cpuid();
		setup_clear_cpu_cap(X86_FEATURE_RTM);
		setup_clear_cpu_cap(X86_FEATURE_HLE);
		return;
	}

	/*
	 * TSX is controlled via MSR_IA32_TSX_CTRL.  However, support for this
	 * MSR is enumerated by ARCH_CAP_TSX_MSR bit in MSR_IA32_ARCH_CAPABILITIES.
	 *
	 * TSX control (aka MSR_IA32_TSX_CTRL) is only available after a
	 * microcode update on CPUs that have their MSR_IA32_ARCH_CAPABILITIES
	 * bit MDS_NO=1. CPUs with MDS_NO=0 are not planned to get
	 * MSR_IA32_TSX_CTRL support even after a microcode update. Thus,
	 * tsx= cmdline requests will do nothing on CPUs without
	 * MSR_IA32_TSX_CTRL support.
	 */
	if (x86_read_arch_cap_msr() & ARCH_CAP_TSX_CTRL_MSR) {
		setup_force_cpu_cap(X86_FEATURE_MSR_TSX_CTRL);
	} else {
		tsx_ctrl_state = TSX_CTRL_NOT_SUPPORTED;
		return;
	}

	ret = cmdline_find_option(boot_command_line, "tsx", arg, sizeof(arg));
	if (ret >= 0) {
		if (!strcmp(arg, "on")) {
			tsx_ctrl_state = TSX_CTRL_ENABLE;
		} else if (!strcmp(arg, "off")) {
			tsx_ctrl_state = TSX_CTRL_DISABLE;
		} else if (!strcmp(arg, "auto")) {
			tsx_ctrl_state = x86_get_tsx_auto_mode();
		} else {
			tsx_ctrl_state = TSX_CTRL_DISABLE;
			pr_err("invalid option, defaulting to off\n");
		}
	} else {
		/* tsx= not provided */
		if (IS_ENABLED(CONFIG_X86_INTEL_TSX_MODE_AUTO))
			tsx_ctrl_state = x86_get_tsx_auto_mode();
		else if (IS_ENABLED(CONFIG_X86_INTEL_TSX_MODE_OFF))
			tsx_ctrl_state = TSX_CTRL_DISABLE;
		else
			tsx_ctrl_state = TSX_CTRL_ENABLE;
	}

	if (tsx_ctrl_state == TSX_CTRL_DISABLE) {
		tsx_disable();

		/*
		 * tsx_disable() will change the state of the RTM and HLE CPUID
		 * bits. Clear them here since they are now expected to be not
		 * set.
		 */
		setup_clear_cpu_cap(X86_FEATURE_RTM);
		setup_clear_cpu_cap(X86_FEATURE_HLE);
	} else if (tsx_ctrl_state == TSX_CTRL_ENABLE) {

		/*
		 * HW defaults TSX to be enabled at bootup.
		 * We may still need the TSX enable support
		 * during init for special cases like
		 * kexec after TSX is disabled.
		 */
		tsx_enable();

		/*
		 * tsx_enable() will change the state of the RTM and HLE CPUID
		 * bits. Force them here since they are now expected to be set.
		 */
		setup_force_cpu_cap(X86_FEATURE_RTM);
		setup_force_cpu_cap(X86_FEATURE_HLE);
	}
}

void tsx_ap_init(void)
{
	tsx_dev_mode_disable();

	if (tsx_ctrl_state == TSX_CTRL_ENABLE)
		tsx_enable();
	else if (tsx_ctrl_state == TSX_CTRL_DISABLE)
		tsx_disable();
	else if (tsx_ctrl_state == TSX_CTRL_RTM_ALWAYS_ABORT)
		/* See comment over that function for more details. */
		tsx_clear_cpuid();
}
