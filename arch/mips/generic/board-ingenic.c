// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Support for Ingenic SoCs
 *
 * Copyright (C) 2009-2010, Lars-Peter Clausen <lars@metafoo.de>
 * Copyright (C) 2011, Maarten ter Huurne <maarten@treewalker.org>
 * Copyright (C) 2020 Paul Cercueil <paul@crapouillou.net>
 */

#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/pm.h>
#include <linux/sizes.h>
#include <linux/suspend.h>
#include <linux/types.h>

#include <asm/bootinfo.h>
#include <asm/io.h>
#include <asm/machine.h>
#include <asm/reboot.h>

static __init char *ingenic_get_system_type(unsigned long machtype)
{
	switch (machtype) {
	case MACH_INGENIC_X2100:
		return "X2100";
	case MACH_INGENIC_X2000H:
		return "X2000H";
	case MACH_INGENIC_X2000E:
		return "X2000E";
	case MACH_INGENIC_X2000:
		return "X2000";
	case MACH_INGENIC_X1830:
		return "X1830";
	case MACH_INGENIC_X1000E:
		return "X1000E";
	case MACH_INGENIC_X1000:
		return "X1000";
	case MACH_INGENIC_JZ4780:
		return "JZ4780";
	case MACH_INGENIC_JZ4775:
		return "JZ4775";
	case MACH_INGENIC_JZ4770:
		return "JZ4770";
	case MACH_INGENIC_JZ4760B:
		return "JZ4760B";
	case MACH_INGENIC_JZ4760:
		return "JZ4760";
	case MACH_INGENIC_JZ4755:
		return "JZ4755";
	case MACH_INGENIC_JZ4750:
		return "JZ4750";
	case MACH_INGENIC_JZ4725B:
		return "JZ4725B";
	case MACH_INGENIC_JZ4730:
		return "JZ4730";
	default:
		return "JZ4740";
	}
}

#define INGENIC_CGU_BASE	0x10000000
#define JZ4750_CGU_CPCCR_ECS	BIT(30)
#define JZ4760_CGU_CPCCR_ECS	BIT(31)

static __init void ingenic_force_12M_ext(const void *fdt, unsigned int mask)
{
	const __be32 *prop;
	unsigned int cpccr;
	void __iomem *cgu;
	bool use_div;
	int offset;

	offset = fdt_path_offset(fdt, "/ext");
	if (offset < 0)
		return;

	prop = fdt_getprop(fdt, offset, "clock-frequency", NULL);
	if (!prop)
		return;

	/*
	 * If the external oscillator is 24 MHz, enable the /2 divider to
	 * drive it down to 12 MHz, since this is what the hardware can work
	 * with.
	 * The 16 MHz cutoff value is arbitrary; setting it to 12 MHz would not
	 * work as the crystal frequency (as reported in the Device Tree) might
	 * be slightly above this value.
	 */
	use_div = be32_to_cpup(prop) >= 16000000;

	cgu = ioremap(INGENIC_CGU_BASE, 0x4);
	if (!cgu)
		return;

	cpccr = ioread32(cgu);
	if (use_div)
		cpccr |= mask;
	else
		cpccr &= ~mask;
	iowrite32(cpccr, cgu);

	iounmap(cgu);
}

static __init const void *ingenic_fixup_fdt(const void *fdt, const void *match_data)
{
	/*
	 * Old devicetree files for the qi,lb60 board did not have a /memory
	 * node. Hardcode the memory info here.
	 */
	if (!fdt_node_check_compatible(fdt, 0, "qi,lb60") &&
	    fdt_path_offset(fdt, "/memory") < 0)
		early_init_dt_add_memory_arch(0, SZ_32M);

	mips_machtype = (unsigned long)match_data;
	system_type = ingenic_get_system_type(mips_machtype);

	switch (mips_machtype) {
	case MACH_INGENIC_JZ4750:
	case MACH_INGENIC_JZ4755:
		ingenic_force_12M_ext(fdt, JZ4750_CGU_CPCCR_ECS);
		break;
	case MACH_INGENIC_JZ4760:
		ingenic_force_12M_ext(fdt, JZ4760_CGU_CPCCR_ECS);
		break;
	default:
		break;
	}

	return fdt;
}

static const struct of_device_id ingenic_of_match[] __initconst = {
	{ .compatible = "ingenic,jz4730", .data = (void *)MACH_INGENIC_JZ4730 },
	{ .compatible = "ingenic,jz4740", .data = (void *)MACH_INGENIC_JZ4740 },
	{ .compatible = "ingenic,jz4725b", .data = (void *)MACH_INGENIC_JZ4725B },
	{ .compatible = "ingenic,jz4750", .data = (void *)MACH_INGENIC_JZ4750 },
	{ .compatible = "ingenic,jz4755", .data = (void *)MACH_INGENIC_JZ4755 },
	{ .compatible = "ingenic,jz4760", .data = (void *)MACH_INGENIC_JZ4760 },
	{ .compatible = "ingenic,jz4760b", .data = (void *)MACH_INGENIC_JZ4760B },
	{ .compatible = "ingenic,jz4770", .data = (void *)MACH_INGENIC_JZ4770 },
	{ .compatible = "ingenic,jz4775", .data = (void *)MACH_INGENIC_JZ4775 },
	{ .compatible = "ingenic,jz4780", .data = (void *)MACH_INGENIC_JZ4780 },
	{ .compatible = "ingenic,x1000", .data = (void *)MACH_INGENIC_X1000 },
	{ .compatible = "ingenic,x1000e", .data = (void *)MACH_INGENIC_X1000E },
	{ .compatible = "ingenic,x1830", .data = (void *)MACH_INGENIC_X1830 },
	{ .compatible = "ingenic,x2000", .data = (void *)MACH_INGENIC_X2000 },
	{ .compatible = "ingenic,x2000e", .data = (void *)MACH_INGENIC_X2000E },
	{ .compatible = "ingenic,x2000h", .data = (void *)MACH_INGENIC_X2000H },
	{ .compatible = "ingenic,x2100", .data = (void *)MACH_INGENIC_X2100 },
	{}
};

MIPS_MACHINE(ingenic) = {
	.matches = ingenic_of_match,
	.fixup_fdt = ingenic_fixup_fdt,
};

static void ingenic_wait_instr(void)
{
	__asm__(".set push;\n"
		".set mips3;\n"
		"wait;\n"
		".set pop;\n"
	);
}

static void ingenic_halt(void)
{
	for (;;)
		ingenic_wait_instr();
}

static int ingenic_pm_enter(suspend_state_t state)
{
	ingenic_wait_instr();

	return 0;
}

static const struct platform_suspend_ops ingenic_pm_ops = {
	.valid = suspend_valid_only_mem,
	.enter = ingenic_pm_enter,
};

static int __init ingenic_pm_init(void)
{
	if (boot_cpu_type() == CPU_XBURST) {
		if (IS_ENABLED(CONFIG_PM_SLEEP))
			suspend_set_ops(&ingenic_pm_ops);
		_machine_halt = ingenic_halt;
	}

	return 0;

}
late_initcall(ingenic_pm_init);
