/*
 * linux/arch/arm/mach-omap1/board-osk.c
 *
 * Board specific init for OMAP5912 OSK
 *
 * Written by Dirk Behme <dirk.behme@de.bosch.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You should have received a copy of the  GNU General Public License along
 * with this program; if not, write  to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/smc91x.h>
#include <linux/omapfb.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/physmap.h>
#include <linux/mfd/tps65010.h>
#include <linux/platform_data/gpio-omap.h>
#include <linux/platform_data/omap1_bl.h>
#include <linux/soc/ti/omap1-io.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "tc.h"
#include "flash.h"
#include "mux.h"
#include "hardware.h"
#include "usb.h"
#include "common.h"

/* Name of the GPIO chip used by the OMAP for GPIOs 0..15 */
#define OMAP_GPIO_LABEL		"gpio-0-15"

/* At OMAP5912 OSK the Ethernet is directly connected to CS1 */
#define OMAP_OSK_ETHR_START		0x04800300

/* TPS65010 has four GPIOs.  nPG and LED2 can be treated like GPIOs with
 * alternate pin configurations for hardware-controlled blinking.
 */
#define OSK_TPS_GPIO_USB_PWR_EN	0
#define OSK_TPS_GPIO_LED_D3	1
#define OSK_TPS_GPIO_LAN_RESET	2
#define OSK_TPS_GPIO_DSP_PWR_EN	3
#define OSK_TPS_GPIO_LED_D9	4
#define OSK_TPS_GPIO_LED_D2	5

static struct mtd_partition osk_partitions[] = {
	/* bootloader (U-Boot, etc) in first sector */
	{
	      .name		= "bootloader",
	      .offset		= 0,
	      .size		= SZ_128K,
	      .mask_flags	= MTD_WRITEABLE, /* force read-only */
	},
	/* bootloader params in the next sector */
	{
	      .name		= "params",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= SZ_128K,
	      .mask_flags	= 0,
	}, {
	      .name		= "kernel",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= SZ_2M,
	      .mask_flags	= 0
	}, {
	      .name		= "filesystem",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= MTDPART_SIZ_FULL,
	      .mask_flags	= 0
	}
};

static struct physmap_flash_data osk_flash_data = {
	.width		= 2,
	.set_vpp	= omap1_set_vpp,
	.parts		= osk_partitions,
	.nr_parts	= ARRAY_SIZE(osk_partitions),
};

static struct resource osk_flash_resource = {
	/* this is on CS3, wherever it's mapped */
	.flags		= IORESOURCE_MEM,
};

static struct platform_device osk5912_flash_device = {
	.name		= "physmap-flash",
	.id		= 0,
	.dev		= {
		.platform_data	= &osk_flash_data,
	},
	.num_resources	= 1,
	.resource	= &osk_flash_resource,
};

static struct smc91x_platdata osk5912_smc91x_info = {
	.flags	= SMC91X_USE_16BIT | SMC91X_NOWAIT,
	.leda	= RPC_LED_100_10,
	.ledb	= RPC_LED_TX_RX,
};

static struct resource osk5912_smc91x_resources[] = {
	[0] = {
		.start	= OMAP_OSK_ETHR_START,		/* Physical */
		.end	= OMAP_OSK_ETHR_START + 0xf,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.flags	= IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHEDGE,
	},
};

static struct platform_device osk5912_smc91x_device = {
	.name		= "smc91x",
	.id		= -1,
	.dev	= {
		.platform_data	= &osk5912_smc91x_info,
	},
	.num_resources	= ARRAY_SIZE(osk5912_smc91x_resources),
	.resource	= osk5912_smc91x_resources,
};

static struct resource osk5912_cf_resources[] = {
	[0] = {
		.flags	= IORESOURCE_IRQ,
	},
	[1] = {
		.flags = IORESOURCE_MEM,
	},
};

static struct platform_device osk5912_cf_device = {
	.name		= "omap_cf",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(osk5912_cf_resources),
	.resource	= osk5912_cf_resources,
};

static struct platform_device *osk5912_devices[] __initdata = {
	&osk5912_flash_device,
	&osk5912_smc91x_device,
	&osk5912_cf_device,
};

static const struct gpio_led tps_leds[] = {
	/* NOTE:  D9 and D2 have hardware blink support.
	 * Also, D9 requires non-battery power.
	 */
	{ .name = "d9", .default_trigger = "disk-activity", },
	{ .name = "d2", },
	{ .name = "d3", .default_trigger = "heartbeat", },
};

static struct gpiod_lookup_table tps_leds_gpio_table = {
	.dev_id = "leds-gpio",
	.table = {
		/* Use local offsets on TPS65010 */
		GPIO_LOOKUP_IDX("tps65010", OSK_TPS_GPIO_LED_D9, NULL, 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps65010", OSK_TPS_GPIO_LED_D2, NULL, 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps65010", OSK_TPS_GPIO_LED_D3, NULL, 2, GPIO_ACTIVE_LOW),
		{ }
	},
};

static struct gpio_led_platform_data tps_leds_data = {
	.num_leds	= 3,
	.leds		= tps_leds,
};

static struct platform_device osk5912_tps_leds = {
	.name			= "leds-gpio",
	.id			= 0,
	.dev.platform_data	= &tps_leds_data,
};

/* The board just hold these GPIOs hogged from setup to teardown */
static struct gpio_desc *eth_reset;
static struct gpio_desc *vdd_dsp;

static int osk_tps_setup(struct i2c_client *client, struct gpio_chip *gc)
{
	struct gpio_desc *d;
	if (!IS_BUILTIN(CONFIG_TPS65010))
		return -ENOSYS;

	/* Set GPIO 1 HIGH to disable VBUS power supply;
	 * OHCI driver powers it up/down as needed.
	 */
	d = gpiochip_request_own_desc(gc, OSK_TPS_GPIO_USB_PWR_EN, "n_vbus_en",
				      GPIO_ACTIVE_HIGH, GPIOD_OUT_HIGH);
	/* Free the GPIO again as the driver will request it */
	gpiochip_free_own_desc(d);

	/* Set GPIO 2 high so LED D3 is off by default */
	tps65010_set_gpio_out_value(GPIO2, HIGH);

	/* Set GPIO 3 low to take ethernet out of reset */
	eth_reset = gpiochip_request_own_desc(gc, OSK_TPS_GPIO_LAN_RESET, "smc_reset",
					      GPIO_ACTIVE_HIGH, GPIOD_OUT_LOW);

	/* GPIO4 is VDD_DSP */
	vdd_dsp = gpiochip_request_own_desc(gc, OSK_TPS_GPIO_DSP_PWR_EN, "dsp_power",
					    GPIO_ACTIVE_HIGH, GPIOD_OUT_HIGH);
	/* REVISIT if DSP support isn't configured, power it off ... */

	/* Let LED1 (D9) blink; leds-gpio may override it */
	tps65010_set_led(LED1, BLINK);

	/* Set LED2 off by default */
	tps65010_set_led(LED2, OFF);

	/* Enable LOW_PWR handshake */
	tps65010_set_low_pwr(ON);

	/* Switch VLDO2 to 3.0V for AIC23 */
	tps65010_config_vregs1(TPS_LDO2_ENABLE | TPS_VLDO2_3_0V
			| TPS_LDO1_ENABLE);

	/* register these three LEDs */
	osk5912_tps_leds.dev.parent = &client->dev;
	gpiod_add_lookup_table(&tps_leds_gpio_table);
	platform_device_register(&osk5912_tps_leds);

	return 0;
}

static void osk_tps_teardown(struct i2c_client *client, struct gpio_chip *gc)
{
	gpiochip_free_own_desc(eth_reset);
	gpiochip_free_own_desc(vdd_dsp);
}

static struct tps65010_board tps_board = {
	.outmask	= 0x0f,
	.setup		= osk_tps_setup,
	.teardown	= osk_tps_teardown,
};

static struct i2c_board_info __initdata osk_i2c_board_info[] = {
	{
		/* This device will get the name "i2c-tps65010" */
		I2C_BOARD_INFO("tps65010", 0x48),
		.dev_name = "tps65010",
		.platform_data	= &tps_board,

	},
	{
		I2C_BOARD_INFO("tlv320aic23", 0x1B),
	},
	/* TODO when driver support is ready:
	 *  - optionally on Mistral, ov9640 camera sensor at 0x30
	 */
};

static void __init osk_init_smc91x(void)
{
	u32 l;

	/* Check EMIFS wait states to fix errors with SMC_GET_PKT_HDR */
	l = omap_readl(EMIFS_CCS(1));
	l |= 0x3;
	omap_writel(l, EMIFS_CCS(1));
}

static void __init osk_init_cf(int seg)
{
	struct resource *res = &osk5912_cf_resources[1];

	omap_cfg_reg(M7_1610_GPIO62);

	switch (seg) {
	/* NOTE: CS0 could be configured too ... */
	case 1:
		res->start = OMAP_CS1_PHYS;
		break;
	case 2:
		res->start = OMAP_CS2_PHYS;
		break;
	case 3:
		res->start = omap_cs3_phys();
		break;
	}

	res->end = res->start + SZ_8K - 1;
	osk5912_cf_device.dev.platform_data = (void *)(uintptr_t)seg;

	/* NOTE:  better EMIFS setup might support more cards; but the
	 * TRM only shows how to affect regular flash signals, not their
	 * CF/PCMCIA variants...
	 */
	pr_debug("%s: cs%d, previous ccs %08x acs %08x\n", __func__,
		seg, omap_readl(EMIFS_CCS(seg)), omap_readl(EMIFS_ACS(seg)));
	omap_writel(0x0004a1b3, EMIFS_CCS(seg));	/* synch mode 4 etc */
	omap_writel(0x00000000, EMIFS_ACS(seg));	/* OE hold/setup */
}

static struct gpiod_lookup_table osk_usb_gpio_table = {
	.dev_id = "ohci",
	.table = {
		/* Power GPIO on the I2C-attached TPS65010 */
		GPIO_LOOKUP("tps65010", OSK_TPS_GPIO_USB_PWR_EN, "power",
			    GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP(OMAP_GPIO_LABEL, 9, "overcurrent",
			    GPIO_ACTIVE_HIGH),
		{ }
	},
};

static struct omap_usb_config osk_usb_config __initdata = {
	/* has usb host connector (A) ... for development it can also
	 * be used, with a NONSTANDARD gender-bending cable/dongle, as
	 * a peripheral.
	 */
#if IS_ENABLED(CONFIG_USB_OMAP)
	.register_dev	= 1,
	.hmc_mode	= 0,
#else
	.register_host	= 1,
	.hmc_mode	= 16,
	.rwc		= 1,
#endif
	.pins[0]	= 2,
};

#define EMIFS_CS3_VAL	(0x88013141)

static struct gpiod_lookup_table osk_irq_gpio_table = {
	.dev_id = NULL,
	.table = {
		/* GPIO used for SMC91x IRQ */
		GPIO_LOOKUP(OMAP_GPIO_LABEL, 0, "smc_irq",
			    GPIO_ACTIVE_HIGH),
		/* GPIO used for CF IRQ */
		GPIO_LOOKUP("gpio-48-63", 14, "cf_irq",
			    GPIO_ACTIVE_HIGH),
		/* GPIO used by the TPS65010 chip */
		GPIO_LOOKUP("mpuio", 1, "tps65010",
			    GPIO_ACTIVE_HIGH),
		/* GPIOs used for serial wakeup IRQs */
		GPIO_LOOKUP_IDX("gpio-32-47", 5, "wakeup", 0,
			    GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpio-16-31", 2, "wakeup", 1,
			    GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpio-48-63", 1, "wakeup", 2,
			    GPIO_ACTIVE_HIGH),
		{ }
	},
};

static void __init osk_init(void)
{
	struct gpio_desc *d;
	u32 l;

	osk_init_smc91x();
	osk_init_cf(2); /* CS2 */

	/* Workaround for wrong CS3 (NOR flash) timing
	 * There are some U-Boot versions out there which configure
	 * wrong CS3 memory timings. This mainly leads to CRC
	 * or similar errors if you use NOR flash (e.g. with JFFS2)
	 */
	l = omap_readl(EMIFS_CCS(3));
	if (l != EMIFS_CS3_VAL)
		omap_writel(EMIFS_CS3_VAL, EMIFS_CCS(3));

	osk_flash_resource.end = osk_flash_resource.start = omap_cs3_phys();
	osk_flash_resource.end += SZ_32M - 1;

	/*
	 * Add the GPIOs to be used as IRQs and immediately look them up
	 * to be passed as an IRQ resource. This is ugly but should work
	 * until the day we convert to device tree.
	 */
	gpiod_add_lookup_table(&osk_irq_gpio_table);

	d = gpiod_get(NULL, "smc_irq", GPIOD_IN);
	if (IS_ERR(d)) {
		pr_err("Unable to get SMC IRQ GPIO descriptor\n");
	} else {
		irq_set_irq_type(gpiod_to_irq(d), IRQ_TYPE_EDGE_RISING);
		osk5912_smc91x_resources[1] = DEFINE_RES_IRQ(gpiod_to_irq(d));
	}

	d = gpiod_get(NULL, "cf_irq", GPIOD_IN);
	if (IS_ERR(d)) {
		pr_err("Unable to get CF IRQ GPIO descriptor\n");
	} else {
		/* the CF I/O IRQ is really active-low */
		irq_set_irq_type(gpiod_to_irq(d), IRQ_TYPE_EDGE_FALLING);
		osk5912_cf_resources[0] = DEFINE_RES_IRQ(gpiod_to_irq(d));
	}

	platform_add_devices(osk5912_devices, ARRAY_SIZE(osk5912_devices));

	l = omap_readl(USB_TRANSCEIVER_CTRL);
	l |= (3 << 1);
	omap_writel(l, USB_TRANSCEIVER_CTRL);

	gpiod_add_lookup_table(&osk_usb_gpio_table);
	omap1_usb_init(&osk_usb_config);

	omap_serial_init();

	/* irq for tps65010 chip */
	/* bootloader effectively does:  omap_cfg_reg(U19_1610_MPUIO1); */
	d = gpiod_get(NULL, "tps65010", GPIOD_IN);
	if (IS_ERR(d))
		pr_err("Unable to get TPS65010 IRQ GPIO descriptor\n");
	else
		osk_i2c_board_info[0].irq = gpiod_to_irq(d);
	omap_register_i2c_bus(1, 400, osk_i2c_board_info,
			      ARRAY_SIZE(osk_i2c_board_info));
}

MACHINE_START(OMAP_OSK, "TI-OSK")
	/* Maintainer: Dirk Behme <dirk.behme@de.bosch.com> */
	.atag_offset	= 0x100,
	.map_io		= omap1_map_io,
	.init_early	= omap1_init_early,
	.init_irq	= omap1_init_irq,
	.init_machine	= osk_init,
	.init_late	= omap1_init_late,
	.init_time	= omap1_timer_init,
	.restart	= omap1_restart,
MACHINE_END
