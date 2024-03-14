// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests
 *
 * Copyright (C) 2020, Intel Corporation
 * Author: Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <kunit/test.h>
#include <linux/idr.h>

#include "tb.h"
#include "tunnel.h"

static int __ida_init(struct kunit_resource *res, void *context)
{
	struct ida *ida = context;

	ida_init(ida);
	res->data = ida;
	return 0;
}

static void __ida_destroy(struct kunit_resource *res)
{
	struct ida *ida = res->data;

	ida_destroy(ida);
}

static void kunit_ida_init(struct kunit *test, struct ida *ida)
{
	kunit_alloc_resource(test, __ida_init, __ida_destroy, GFP_KERNEL, ida);
}

static struct tb_switch *alloc_switch(struct kunit *test, u64 route,
				      u8 upstream_port, u8 max_port_number)
{
	struct tb_switch *sw;
	size_t size;
	int i;

	sw = kunit_kzalloc(test, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return NULL;

	sw->config.upstream_port_number = upstream_port;
	sw->config.depth = tb_route_length(route);
	sw->config.route_hi = upper_32_bits(route);
	sw->config.route_lo = lower_32_bits(route);
	sw->config.enabled = 0;
	sw->config.max_port_number = max_port_number;

	size = (sw->config.max_port_number + 1) * sizeof(*sw->ports);
	sw->ports = kunit_kzalloc(test, size, GFP_KERNEL);
	if (!sw->ports)
		return NULL;

	for (i = 0; i <= sw->config.max_port_number; i++) {
		sw->ports[i].sw = sw;
		sw->ports[i].port = i;
		sw->ports[i].config.port_number = i;
		if (i) {
			kunit_ida_init(test, &sw->ports[i].in_hopids);
			kunit_ida_init(test, &sw->ports[i].out_hopids);
		}
	}

	return sw;
}

static struct tb_switch *alloc_host(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_switch(test, 0, 7, 13);
	if (!sw)
		return NULL;

	sw->config.vendor_id = 0x8086;
	sw->config.device_id = 0x9a1b;

	sw->ports[0].config.type = TB_TYPE_PORT;
	sw->ports[0].config.max_in_hop_id = 7;
	sw->ports[0].config.max_out_hop_id = 7;

	sw->ports[1].config.type = TB_TYPE_PORT;
	sw->ports[1].config.max_in_hop_id = 19;
	sw->ports[1].config.max_out_hop_id = 19;
	sw->ports[1].total_credits = 60;
	sw->ports[1].ctl_credits = 2;
	sw->ports[1].dual_link_port = &sw->ports[2];

	sw->ports[2].config.type = TB_TYPE_PORT;
	sw->ports[2].config.max_in_hop_id = 19;
	sw->ports[2].config.max_out_hop_id = 19;
	sw->ports[2].total_credits = 60;
	sw->ports[2].ctl_credits = 2;
	sw->ports[2].dual_link_port = &sw->ports[1];
	sw->ports[2].link_nr = 1;

	sw->ports[3].config.type = TB_TYPE_PORT;
	sw->ports[3].config.max_in_hop_id = 19;
	sw->ports[3].config.max_out_hop_id = 19;
	sw->ports[3].total_credits = 60;
	sw->ports[3].ctl_credits = 2;
	sw->ports[3].dual_link_port = &sw->ports[4];

	sw->ports[4].config.type = TB_TYPE_PORT;
	sw->ports[4].config.max_in_hop_id = 19;
	sw->ports[4].config.max_out_hop_id = 19;
	sw->ports[4].total_credits = 60;
	sw->ports[4].ctl_credits = 2;
	sw->ports[4].dual_link_port = &sw->ports[3];
	sw->ports[4].link_nr = 1;

	sw->ports[5].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[5].config.max_in_hop_id = 9;
	sw->ports[5].config.max_out_hop_id = 9;
	sw->ports[5].cap_adap = -1;

	sw->ports[6].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[6].config.max_in_hop_id = 9;
	sw->ports[6].config.max_out_hop_id = 9;
	sw->ports[6].cap_adap = -1;

	sw->ports[7].config.type = TB_TYPE_NHI;
	sw->ports[7].config.max_in_hop_id = 11;
	sw->ports[7].config.max_out_hop_id = 11;
	sw->ports[7].config.nfc_credits = 0x41800000;

	sw->ports[8].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[8].config.max_in_hop_id = 8;
	sw->ports[8].config.max_out_hop_id = 8;

	sw->ports[9].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[9].config.max_in_hop_id = 8;
	sw->ports[9].config.max_out_hop_id = 8;

	sw->ports[10].disabled = true;
	sw->ports[11].disabled = true;

	sw->ports[12].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[12].config.max_in_hop_id = 8;
	sw->ports[12].config.max_out_hop_id = 8;

	sw->ports[13].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[13].config.max_in_hop_id = 8;
	sw->ports[13].config.max_out_hop_id = 8;

	return sw;
}

static struct tb_switch *alloc_host_usb4(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_host(test);
	if (!sw)
		return NULL;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 32;
	sw->min_dp_aux_credits = 1;
	sw->min_dp_main_credits = 0;
	sw->max_pcie_credits = 64;
	sw->max_dma_credits = 14;

	return sw;
}

static struct tb_switch *alloc_host_br(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_host_usb4(test);
	if (!sw)
		return NULL;

	sw->ports[10].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[10].config.max_in_hop_id = 9;
	sw->ports[10].config.max_out_hop_id = 9;
	sw->ports[10].cap_adap = -1;
	sw->ports[10].disabled = false;

	return sw;
}

static struct tb_switch *alloc_dev_default(struct kunit *test,
					   struct tb_switch *parent,
					   u64 route, bool bonded)
{
	struct tb_port *port, *upstream_port;
	struct tb_switch *sw;

	sw = alloc_switch(test, route, 1, 19);
	if (!sw)
		return NULL;

	sw->config.vendor_id = 0x8086;
	sw->config.device_id = 0x15ef;

	sw->ports[0].config.type = TB_TYPE_PORT;
	sw->ports[0].config.max_in_hop_id = 8;
	sw->ports[0].config.max_out_hop_id = 8;

	sw->ports[1].config.type = TB_TYPE_PORT;
	sw->ports[1].config.max_in_hop_id = 19;
	sw->ports[1].config.max_out_hop_id = 19;
	sw->ports[1].total_credits = 60;
	sw->ports[1].ctl_credits = 2;
	sw->ports[1].dual_link_port = &sw->ports[2];

	sw->ports[2].config.type = TB_TYPE_PORT;
	sw->ports[2].config.max_in_hop_id = 19;
	sw->ports[2].config.max_out_hop_id = 19;
	sw->ports[2].total_credits = 60;
	sw->ports[2].ctl_credits = 2;
	sw->ports[2].dual_link_port = &sw->ports[1];
	sw->ports[2].link_nr = 1;

	sw->ports[3].config.type = TB_TYPE_PORT;
	sw->ports[3].config.max_in_hop_id = 19;
	sw->ports[3].config.max_out_hop_id = 19;
	sw->ports[3].total_credits = 60;
	sw->ports[3].ctl_credits = 2;
	sw->ports[3].dual_link_port = &sw->ports[4];

	sw->ports[4].config.type = TB_TYPE_PORT;
	sw->ports[4].config.max_in_hop_id = 19;
	sw->ports[4].config.max_out_hop_id = 19;
	sw->ports[4].total_credits = 60;
	sw->ports[4].ctl_credits = 2;
	sw->ports[4].dual_link_port = &sw->ports[3];
	sw->ports[4].link_nr = 1;

	sw->ports[5].config.type = TB_TYPE_PORT;
	sw->ports[5].config.max_in_hop_id = 19;
	sw->ports[5].config.max_out_hop_id = 19;
	sw->ports[5].total_credits = 60;
	sw->ports[5].ctl_credits = 2;
	sw->ports[5].dual_link_port = &sw->ports[6];

	sw->ports[6].config.type = TB_TYPE_PORT;
	sw->ports[6].config.max_in_hop_id = 19;
	sw->ports[6].config.max_out_hop_id = 19;
	sw->ports[6].total_credits = 60;
	sw->ports[6].ctl_credits = 2;
	sw->ports[6].dual_link_port = &sw->ports[5];
	sw->ports[6].link_nr = 1;

	sw->ports[7].config.type = TB_TYPE_PORT;
	sw->ports[7].config.max_in_hop_id = 19;
	sw->ports[7].config.max_out_hop_id = 19;
	sw->ports[7].total_credits = 60;
	sw->ports[7].ctl_credits = 2;
	sw->ports[7].dual_link_port = &sw->ports[8];

	sw->ports[8].config.type = TB_TYPE_PORT;
	sw->ports[8].config.max_in_hop_id = 19;
	sw->ports[8].config.max_out_hop_id = 19;
	sw->ports[8].total_credits = 60;
	sw->ports[8].ctl_credits = 2;
	sw->ports[8].dual_link_port = &sw->ports[7];
	sw->ports[8].link_nr = 1;

	sw->ports[9].config.type = TB_TYPE_PCIE_UP;
	sw->ports[9].config.max_in_hop_id = 8;
	sw->ports[9].config.max_out_hop_id = 8;

	sw->ports[10].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[10].config.max_in_hop_id = 8;
	sw->ports[10].config.max_out_hop_id = 8;

	sw->ports[11].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[11].config.max_in_hop_id = 8;
	sw->ports[11].config.max_out_hop_id = 8;

	sw->ports[12].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[12].config.max_in_hop_id = 8;
	sw->ports[12].config.max_out_hop_id = 8;

	sw->ports[13].config.type = TB_TYPE_DP_HDMI_OUT;
	sw->ports[13].config.max_in_hop_id = 9;
	sw->ports[13].config.max_out_hop_id = 9;
	sw->ports[13].cap_adap = -1;

	sw->ports[14].config.type = TB_TYPE_DP_HDMI_OUT;
	sw->ports[14].config.max_in_hop_id = 9;
	sw->ports[14].config.max_out_hop_id = 9;
	sw->ports[14].cap_adap = -1;

	sw->ports[15].disabled = true;

	sw->ports[16].config.type = TB_TYPE_USB3_UP;
	sw->ports[16].config.max_in_hop_id = 8;
	sw->ports[16].config.max_out_hop_id = 8;

	sw->ports[17].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[17].config.max_in_hop_id = 8;
	sw->ports[17].config.max_out_hop_id = 8;

	sw->ports[18].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[18].config.max_in_hop_id = 8;
	sw->ports[18].config.max_out_hop_id = 8;

	sw->ports[19].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[19].config.max_in_hop_id = 8;
	sw->ports[19].config.max_out_hop_id = 8;

	if (!parent)
		return sw;

	/* Link them */
	upstream_port = tb_upstream_port(sw);
	port = tb_port_at(route, parent);
	port->remote = upstream_port;
	upstream_port->remote = port;
	if (port->dual_link_port && upstream_port->dual_link_port) {
		port->dual_link_port->remote = upstream_port->dual_link_port;
		upstream_port->dual_link_port->remote = port->dual_link_port;

		if (bonded) {
			/* Bonding is used */
			port->bonded = true;
			port->total_credits *= 2;
			port->dual_link_port->bonded = true;
			port->dual_link_port->total_credits = 0;
			upstream_port->bonded = true;
			upstream_port->total_credits *= 2;
			upstream_port->dual_link_port->bonded = true;
			upstream_port->dual_link_port->total_credits = 0;
		}
	}

	return sw;
}

static struct tb_switch *alloc_dev_with_dpin(struct kunit *test,
					     struct tb_switch *parent,
					     u64 route, bool bonded)
{
	struct tb_switch *sw;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;

	sw->ports[13].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[13].config.max_in_hop_id = 9;
	sw->ports[13].config.max_out_hop_id = 9;

	sw->ports[14].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[14].config.max_in_hop_id = 9;
	sw->ports[14].config.max_out_hop_id = 9;

	return sw;
}

static struct tb_switch *alloc_dev_without_dp(struct kunit *test,
					      struct tb_switch *parent,
					      u64 route, bool bonded)
{
	struct tb_switch *sw;
	int i;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;
	/*
	 * Device with:
	 * 2x USB4 Adapters (adapters 1,2 and 3,4),
	 * 1x PCIe Upstream (adapter 9),
	 * 1x PCIe Downstream (adapter 10),
	 * 1x USB3 Upstream (adapter 16),
	 * 1x USB3 Downstream (adapter 17)
	 */
	for (i = 5; i <= 8; i++)
		sw->ports[i].disabled = true;

	for (i = 11; i <= 14; i++)
		sw->ports[i].disabled = true;

	sw->ports[13].cap_adap = 0;
	sw->ports[14].cap_adap = 0;

	for (i = 18; i <= 19; i++)
		sw->ports[i].disabled = true;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 109;
	sw->min_dp_aux_credits = 0;
	sw->min_dp_main_credits = 0;
	sw->max_pcie_credits = 30;
	sw->max_dma_credits = 1;

	return sw;
}

static struct tb_switch *alloc_dev_usb4(struct kunit *test,
					struct tb_switch *parent,
					u64 route, bool bonded)
{
	struct tb_switch *sw;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 14;
	sw->min_dp_aux_credits = 1;
	sw->min_dp_main_credits = 18;
	sw->max_pcie_credits = 32;
	sw->max_dma_credits = 14;

	return sw;
}

static void tb_test_path_basic(struct kunit *test)
{
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host;

	host = alloc_host(test);

	src_port = &host->ports[5];
	dst_port = src_port;

	p = tb_next_port_on_path(src_port, dst_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, dst_port);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_TRUE(test, !p);
}

static void tb_test_path_not_connected_walk(struct kunit *test)
{
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev;

	host = alloc_host(test);
	/* No connection between host and dev */
	dev = alloc_dev_default(test, NULL, 3, true);

	src_port = &host->ports[12];
	dst_port = &dev->ports[16];

	p = tb_next_port_on_path(src_port, dst_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, src_port);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_PTR_EQ(test, p, &host->ports[3]);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_TRUE(test, !p);

	/* Other direction */

	p = tb_next_port_on_path(dst_port, src_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, dst_port);

	p = tb_next_port_on_path(dst_port, src_port, p);
	KUNIT_EXPECT_PTR_EQ(test, p, &dev->ports[1]);

	p = tb_next_port_on_path(dst_port, src_port, p);
	KUNIT_EXPECT_TRUE(test, !p);
}

struct port_expectation {
	u64 route;
	u8 port;
	enum tb_port_type type;
};

static void tb_test_path_single_hop_walk(struct kunit *test)
{
	/*
	 * Walks from Host PCIe downstream port to Device #1 PCIe
	 * upstream port.
	 *
	 *   [Host]
	 *   1 |
	 *   1 |
	 *  [Device]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 8, .type = TB_TYPE_PCIE_DOWN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 9, .type = TB_TYPE_PCIE_UP },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 1, true);

	src_port = &host->ports[8];
	dst_port = &dev->ports[9];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_daisy_chain_walk(struct kunit *test)
{
	/*
	 * Walks from Host DP IN to Device #2 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /
	 *      1 /
	 * [Device #2]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 5, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev1, *dev2;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);

	src_port = &host->ports[5];
	dst_port = &dev2->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_simple_tree_walk(struct kunit *test)
{
	/*
	 * Walks from Host DP IN to Device #3 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 5, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x501, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x501, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev1, *dev3;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev1, 0x501, true);
	alloc_dev_default(test, dev1, 0x701, true);

	src_port = &host->ports[5];
	dst_port = &dev3->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_complex_tree_walk(struct kunit *test)
{
	/*
	 * Walks from Device #3 DP IN to Device #9 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #5]
	 *    5 |      | 1         \ 7
	 *    1 |  [Device #4]      \ 1
	 * [Device #3]             [Device #6]
	 *                       3 /
	 *                      1 /
	 *                    [Device #7]
	 *                  3 /      | 5
	 *                 1 /       |
	 *               [Device #8] | 1
	 *                       [Device #9]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x50301, .port = 13, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x50301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 7, .type = TB_TYPE_PORT },
		{ .route = 0x701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x701, .port = 7, .type = TB_TYPE_PORT },
		{ .route = 0x70701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x70701, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3070701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3070701, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x503070701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x503070701, .port = 14, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev5, *dev6, *dev7, *dev9;
	struct tb_port *src_port, *dst_port, *p;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_with_dpin(test, dev2, 0x50301, true);
	alloc_dev_default(test, dev1, 0x501, true);
	dev5 = alloc_dev_default(test, dev1, 0x701, true);
	dev6 = alloc_dev_default(test, dev5, 0x70701, true);
	dev7 = alloc_dev_default(test, dev6, 0x3070701, true);
	alloc_dev_default(test, dev7, 0x303070701, true);
	dev9 = alloc_dev_default(test, dev7, 0x503070701, true);

	src_port = &dev3->ports[13];
	dst_port = &dev9->ports[14];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_max_length_walk(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5, *dev6;
	struct tb_switch *dev7, *dev8, *dev9, *dev10, *dev11, *dev12;
	struct tb_port *src_port, *dst_port, *p;
	int i;

	/*
	 * Walks from Device #6 DP IN to Device #12 DP OUT.
	 *
	 *          [Host]
	 *         1 /  \ 3
	 *        1 /    \ 1
	 * [Device #1]   [Device #7]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #2]   [Device #8]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #3]   [Device #9]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #4]   [Device #10]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #5]   [Device #11]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #6]   [Device #12]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x30303030301, .port = 13, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x30303030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303030301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3030301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x0, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3030303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303030303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30303030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30303030303, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev2, 0x30301, true);
	dev4 = alloc_dev_default(test, dev3, 0x3030301, true);
	dev5 = alloc_dev_default(test, dev4, 0x303030301, true);
	dev6 = alloc_dev_with_dpin(test, dev5, 0x30303030301, true);
	dev7 = alloc_dev_default(test, host, 0x3, true);
	dev8 = alloc_dev_default(test, dev7, 0x303, true);
	dev9 = alloc_dev_default(test, dev8, 0x30303, true);
	dev10 = alloc_dev_default(test, dev9, 0x3030303, true);
	dev11 = alloc_dev_default(test, dev10, 0x303030303, true);
	dev12 = alloc_dev_default(test, dev11, 0x30303030303, true);

	src_port = &dev6->ports[13];
	dst_port = &dev12->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_not_connected(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_port *down, *up;
	struct tb_path *path;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, false);
	/* Not connected to anything */
	dev2 = alloc_dev_default(test, NULL, 0x303, false);

	down = &dev1->ports[10];
	up = &dev2->ports[9];

	path = tb_path_alloc(NULL, down, 8, up, 8, 0, "PCIe Down");
	KUNIT_ASSERT_NULL(test, path);
	path = tb_path_alloc(NULL, down, 8, up, 8, 1, "PCIe Down");
	KUNIT_ASSERT_NULL(test, path);
}

struct hop_expectation {
	u64 route;
	u8 in_port;
	enum tb_port_type in_type;
	u8 out_port;
	enum tb_port_type out_type;
};

static void tb_test_path_not_bonded_lane0(struct kunit *test)
{
	/*
	 * PCIe path from host to device using lane 0.
	 *
	 *   [Host]
	 *   3 |: 4
	 *   1 |: 2
	 *  [Device]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 9,
			.in_type = TB_TYPE_PCIE_DOWN,
			.out_port = 3,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x3,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 9,
			.out_type = TB_TYPE_PCIE_UP,
		},
	};
	struct tb_switch *host, *dev;
	struct tb_port *down, *up;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x3, false);

	down = &host->ports[9];
	up = &dev->ports[9];

	path = tb_path_alloc(NULL, down, 8, up, 8, 0, "PCIe Down");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1(struct kunit *test)
{
	/*
	 * DP Video path from host to device using lane 1. Paths like
	 * these are only used with Thunderbolt 1 devices where lane
	 * bonding is not possible. USB4 specifically does not allow
	 * paths like this (you either use lane 0 where lane 1 is
	 * disabled or both lanes are bonded).
	 *
	 *   [Host]
	 *   1 :| 2
	 *   1 :| 2
	 *  [Device]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, false);

	in = &host->ports[5];
	out = &dev->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1_chain(struct kunit *test)
{
	/*
	 * DP Video path from host to device 3 using lane 1.
	 *
	 *    [Host]
	 *    1 :| 2
	 *    1 :| 2
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 8,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 6,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, false);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);

	in = &host->ports[5];
	out = &dev3->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1_chain_reverse(struct kunit *test)
{
	/*
	 * DP Video path from device 3 to host using lane 1.
	 *
	 *    [Host]
	 *    1 :| 2
	 *    1 :| 2
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x50701,
			.in_port = 13,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 6,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 8,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x0,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 5,
			.out_type = TB_TYPE_DP_HDMI_IN,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, false);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_with_dpin(test, dev2, 0x50701, false);

	in = &dev3->ports[13];
	out = &host->ports[5];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_mixed_chain(struct kunit *test)
{
	/*
	 * DP Video path from host to device 4 where first and last link
	 * is bonded.
	 *
	 *    [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 *    3 |
	 *    1 |
	 *  [Device #4]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 8,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 6,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 3,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x3050701,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);
	dev4 = alloc_dev_default(test, dev3, 0x3050701, true);

	in = &host->ports[5];
	out = &dev4->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_mixed_chain_reverse(struct kunit *test)
{
	/*
	 * DP Video path from device 4 to host where first and last link
	 * is bonded.
	 *
	 *    [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 *    3 |
	 *    1 |
	 *  [Device #4]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x3050701,
			.in_port = 13,
			.in_type = TB_TYPE_DP_HDMI_OUT,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 3,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 6,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 8,
			.in_type = TB_TYPE_PORT,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x0,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 5,
			.out_type = TB_TYPE_DP_HDMI_IN,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);
	dev4 = alloc_dev_default(test, dev3, 0x3050701, true);

	in = &dev4->ports[13];
	out = &host->ports[5];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_tunnel_pcie(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_tunnel *tunnel1, *tunnel2;
	struct tb_port *down, *up;

	/*
	 * Create PCIe tunnel between host and two devices.
	 *
	 *   [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    5 |
	 *    1 |
	 *  [Device #2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x501, true);

	down = &host->ports[8];
	up = &dev1->ports[9];
	tunnel1 = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_PCI);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[1].out_port, down);

	down = &dev1->ports[10];
	up = &dev2->ports[9];
	tunnel2 = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_PCI);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[1].out_port, down);

	tb_tunnel_free(tunnel2);
	tb_tunnel_free(tunnel1);
}

static void tb_test_tunnel_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel between Host and Device
	 *
	 *   [Host]
	 *   1 |
	 *   1 |
	 *  [Device]
	 */
	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x3, true);

	in = &host->ports[5];
	out = &dev->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[1].out_port, in);
	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_dp_chain(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev4;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel from Host DP IN to Device #4 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	alloc_dev_default(test, dev1, 0x301, true);
	alloc_dev_default(test, dev1, 0x501, true);
	dev4 = alloc_dev_default(test, dev1, 0x701, true);

	in = &host->ports[5];
	out = &dev4->ports[14];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[2].out_port, in);
	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_dp_tree(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev5;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel from Device #2 DP IN to Device #5 DP OUT.
	 *
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_with_dpin(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in = &dev2->ports[13];
	out = &dev5->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[3].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[3].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[3].out_port, in);
	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_dp_max_length(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5, *dev6;
	struct tb_switch *dev7, *dev8, *dev9, *dev10, *dev11, *dev12;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Creates DP tunnel from Device #6 to Device #12.
	 *
	 *          [Host]
	 *         1 /  \ 3
	 *        1 /    \ 1
	 * [Device #1]   [Device #7]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #2]   [Device #8]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #3]   [Device #9]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #4]   [Device #10]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #5]   [Device #11]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #6]   [Device #12]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev2, 0x30301, true);
	dev4 = alloc_dev_default(test, dev3, 0x3030301, true);
	dev5 = alloc_dev_default(test, dev4, 0x303030301, true);
	dev6 = alloc_dev_with_dpin(test, dev5, 0x30303030301, true);
	dev7 = alloc_dev_default(test, host, 0x3, true);
	dev8 = alloc_dev_default(test, dev7, 0x303, true);
	dev9 = alloc_dev_default(test, dev8, 0x30303, true);
	dev10 = alloc_dev_default(test, dev9, 0x3030303, true);
	dev11 = alloc_dev_default(test, dev10, 0x303030303, true);
	dev12 = alloc_dev_default(test, dev11, 0x30303030303, true);

	in = &dev6->ports[13];
	out = &dev12->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 13);
	/* First hop */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	/* Middle */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[6].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[6].out_port,
			    &host->ports[3]);
	/* Last */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[12].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 13);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[6].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[6].out_port,
			    &host->ports[3]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[12].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 13);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[6].in_port,
			    &host->ports[3]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[6].out_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[12].out_port, in);
	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_3dp(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5;
	struct tb_port *in1, *in2, *in3, *out1, *out2, *out3;
	struct tb_tunnel *tunnel1, *tunnel2, *tunnel3;

	/*
	 * Create 3 DP tunnels from Host to Devices #2, #5 and #4.
	 *
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host_br(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_default(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	dev4 = alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in1 = &host->ports[5];
	in2 = &host->ports[6];
	in3 = &host->ports[10];

	out1 = &dev2->ports[13];
	out2 = &dev5->ports[13];
	out3 = &dev4->ports[14];

	tunnel1 = tb_tunnel_alloc_dp(NULL, in1, out1, 1, 0, 0);
	KUNIT_ASSERT_TRUE(test, tunnel1 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, in1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, out1);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 3);

	tunnel2 = tb_tunnel_alloc_dp(NULL, in2, out2, 1, 0, 0);
	KUNIT_ASSERT_TRUE(test, tunnel2 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, in2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, out2);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 4);

	tunnel3 = tb_tunnel_alloc_dp(NULL, in3, out3, 1, 0, 0);
	KUNIT_ASSERT_TRUE(test, tunnel3 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel3->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel3->src_port, in3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel3->dst_port, out3);
	KUNIT_ASSERT_EQ(test, tunnel3->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel3->paths[0]->path_length, 3);

	tb_tunnel_free(tunnel2);
	tb_tunnel_free(tunnel1);
}

static void tb_test_tunnel_usb3(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_tunnel *tunnel1, *tunnel2;
	struct tb_port *down, *up;

	/*
	 * Create USB3 tunnel between host and two devices.
	 *
	 *   [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *          \ 7
	 *           \ 1
	 *         [Device #2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, true);

	down = &host->ports[12];
	up = &dev1->ports[16];
	tunnel1 = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_USB3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[1].out_port, down);

	down = &dev1->ports[17];
	up = &dev2->ports[16];
	tunnel2 = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_USB3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[1].out_port, down);

	tb_tunnel_free(tunnel2);
	tb_tunnel_free(tunnel1);
}

static void tb_test_tunnel_port_on_path(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5;
	struct tb_port *in, *out, *port;
	struct tb_tunnel *dp_tunnel;

	/*
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_with_dpin(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	dev4 = alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in = &dev2->ports[13];
	out = &dev5->ports[13];

	dp_tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel);

	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, in));
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, out));

	port = &host->ports[8];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &host->ports[3];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[1];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[3];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[5];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[7];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev3->ports[1];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev5->ports[1];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev4->ports[1];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	tb_tunnel_free(dp_tunnel);
}

static void tb_test_tunnel_dma(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA tunnel from NHI to port 1 and back.
	 *
	 *   [Host 1]
	 *    1 ^ In HopID 1 -> Out HopID 8
	 *      |
	 *      v In HopID 8 -> Out HopID 1
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 2);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 8);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].in_hop_index, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].next_hop_index, 8);

	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_dma_rx(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA RX tunnel from port 1 to NHI.
	 *
	 *   [Host 1]
	 *    1 ^
	 *      |
	 *      | In HopID 15 -> Out HopID 2
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, -1, -1, 15, 2);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 1);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 15);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 2);

	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_dma_tx(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA TX tunnel from NHI to port 1.
	 *
	 *   [Host 1]
	 *    1 | In HopID 2 -> Out HopID 15
	 *      |
	 *      v
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 2, -1, -1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 15);

	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_dma_chain(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;

	/*
	 * Create DMA tunnel from NHI to Device #2 port 3 and back.
	 *
	 *   [Host 1]
	 *    1 ^ In HopID 1 -> Out HopID x
	 *      |
	 *    1 | In HopID x -> Out HopID 1
	 *  [Device #1]
	 *         7 \
	 *          1 \
	 *         [Device #2]
	 *           3 | In HopID x -> Out HopID 8
	 *             |
	 *             v In HopID 8 -> Out HopID x
	 * ............ Domain border
	 *             |
	 *          [Host 2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, true);

	nhi = &host->ports[7];
	port = &dev2->ports[3];
	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 2);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 8);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port,
			    &dev2->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].in_port,
			    &dev1->ports[7]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].out_port,
			    &dev1->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[2].next_hop_index, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].in_hop_index, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].in_port,
			    &dev1->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].out_port,
			    &dev1->ports[7]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].in_port,
			    &dev2->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[2].next_hop_index, 8);

	tb_tunnel_free(tunnel);
}

static void tb_test_tunnel_dma_match(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 1, 15, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);

	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 8, 1, 15, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, -1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 8, -1, 8, -1));

	tb_tunnel_free(tunnel);

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 1, -1, -1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, -1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 11, -1, -1));

	tb_tunnel_free(tunnel);

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, -1, -1, 15, 11);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 11));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, 11));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 10, 11));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 11, -1, -1));

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_legacy_not_bonded(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, false);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 16U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 16U);

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_legacy_bonded(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, true);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_pcie(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_without_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_without_dp(test, host, 0x1, true);

	/*
	 * The device has no DP therefore baMinDPmain = baMinDPaux = 0
	 *
	 * Create PCIe path with buffers less than baMaxPCIe.
	 *
	 * For a device with buffers configurations:
	 * baMaxUSB3 = 109
	 * baMinDPaux = 0
	 * baMinDPmain = 0
	 * baMaxPCIe = 30
	 * baMaxHI = 1
	 * Remaining Buffers = Total - (CP + DP) = 120 - (2 + 0) = 118
	 * PCIe Credits = Max(6, Min(baMaxPCIe, Remaining Buffers - baMaxUSB3)
	 *		= Max(6, Min(30, 9) = 9
	 */
	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_TRUE(test, tunnel != NULL);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	/* PCIe downstream path */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 9U);

	/* PCIe upstream path */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	in = &host->ports[5];
	out = &dev->ports[14];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)3);

	/* Video (main) path */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	/* AUX TX */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	/* AUX RX */
	path = tunnel->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_usb3(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	down = &host->ports[12];
	up = &dev->ports[16];
	tunnel = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_dma(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	nhi = &host->ports[7];
	port = &dev->ports[3];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	/* DMA RX */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	/* DMA TX */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tb_tunnel_free(tunnel);
}

static void tb_test_credit_alloc_dma_multiple(struct kunit *test)
{
	struct tb_tunnel *tunnel1, *tunnel2, *tunnel3;
	struct tb_switch *host, *dev;
	struct tb_port *nhi, *port;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	nhi = &host->ports[7];
	port = &dev->ports[3];

	/*
	 * Create three DMA tunnels through the same ports. With the
	 * default buffers we should be able to create two and the last
	 * one fails.
	 *
	 * For default host we have following buffers for DMA:
	 *
	 *   120 - (2 + 2 * (1 + 0) + 32 + 64 + spare) = 20
	 *
	 * For device we have following:
	 *
	 *  120 - (2 + 2 * (1 + 18) + 14 + 32 + spare) = 34
	 *
	 * spare = 14 + 1 = 15
	 *
	 * So on host the first tunnel gets 14 and the second gets the
	 * remaining 1 and then we run out of buffers.
	 */
	tunnel1 = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, (size_t)2);

	path = tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tunnel2 = tb_tunnel_alloc_dma(NULL, nhi, port, 9, 2, 9, 2);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, (size_t)2);

	path = tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	tunnel3 = tb_tunnel_alloc_dma(NULL, nhi, port, 10, 3, 10, 3);
	KUNIT_ASSERT_NULL(test, tunnel3);

	/*
	 * Release the first DMA tunnel. That should make 14 buffers
	 * available for the next tunnel.
	 */
	tb_tunnel_free(tunnel1);

	tunnel3 = tb_tunnel_alloc_dma(NULL, nhi, port, 10, 3, 10, 3);
	KUNIT_ASSERT_NOT_NULL(test, tunnel3);

	path = tunnel3->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel3->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tb_tunnel_free(tunnel3);
	tb_tunnel_free(tunnel2);
}

static struct tb_tunnel *TB_TEST_PCIE_TUNNEL(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *up, *down;
	struct tb_tunnel *pcie_tunnel;
	struct tb_path *path;

	down = &host->ports[8];
	up = &dev->ports[9];
	pcie_tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, pcie_tunnel);
	KUNIT_ASSERT_EQ(test, pcie_tunnel->npaths, (size_t)2);

	path = pcie_tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = pcie_tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	return pcie_tunnel;
}

static struct tb_tunnel *TB_TEST_DP_TUNNEL1(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *in, *out;
	struct tb_tunnel *dp_tunnel1;
	struct tb_path *path;

	in = &host->ports[5];
	out = &dev->ports[13];
	dp_tunnel1 = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel1);
	KUNIT_ASSERT_EQ(test, dp_tunnel1->npaths, (size_t)3);

	path = dp_tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	path = dp_tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dp_tunnel1->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dp_tunnel1;
}

static struct tb_tunnel *TB_TEST_DP_TUNNEL2(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *in, *out;
	struct tb_tunnel *dp_tunnel2;
	struct tb_path *path;

	in = &host->ports[6];
	out = &dev->ports[14];
	dp_tunnel2 = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel2);
	KUNIT_ASSERT_EQ(test, dp_tunnel2->npaths, (size_t)3);

	path = dp_tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	path = dp_tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dp_tunnel2->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dp_tunnel2;
}

static struct tb_tunnel *TB_TEST_USB3_TUNNEL(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *up, *down;
	struct tb_tunnel *usb3_tunnel;
	struct tb_path *path;

	down = &host->ports[12];
	up = &dev->ports[16];
	usb3_tunnel = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, usb3_tunnel);
	KUNIT_ASSERT_EQ(test, usb3_tunnel->npaths, (size_t)2);

	path = usb3_tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = usb3_tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	return usb3_tunnel;
}

static struct tb_tunnel *TB_TEST_DMA_TUNNEL1(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *dma_tunnel1;
	struct tb_path *path;

	nhi = &host->ports[7];
	port = &dev->ports[3];
	dma_tunnel1 = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma_tunnel1);
	KUNIT_ASSERT_EQ(test, dma_tunnel1->npaths, (size_t)2);

	path = dma_tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = dma_tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	return dma_tunnel1;
}

static struct tb_tunnel *TB_TEST_DMA_TUNNEL2(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *dma_tunnel2;
	struct tb_path *path;

	nhi = &host->ports[7];
	port = &dev->ports[3];
	dma_tunnel2 = tb_tunnel_alloc_dma(NULL, nhi, port, 9, 2, 9, 2);
	KUNIT_ASSERT_NOT_NULL(test, dma_tunnel2);
	KUNIT_ASSERT_EQ(test, dma_tunnel2->npaths, (size_t)2);

	path = dma_tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dma_tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dma_tunnel2;
}

static void tb_test_credit_alloc_all(struct kunit *test)
{
	struct tb_tunnel *pcie_tunnel, *dp_tunnel1, *dp_tunnel2, *usb3_tunnel;
	struct tb_tunnel *dma_tunnel1, *dma_tunnel2;
	struct tb_switch *host, *dev;

	/*
	 * Create PCIe, 2 x DP, USB 3.x and two DMA tunnels from host to
	 * device. Expectation is that all these can be established with
	 * the default credit allocation found in Intel hardware.
	 */

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	pcie_tunnel = TB_TEST_PCIE_TUNNEL(test, host, dev);
	dp_tunnel1 = TB_TEST_DP_TUNNEL1(test, host, dev);
	dp_tunnel2 = TB_TEST_DP_TUNNEL2(test, host, dev);
	usb3_tunnel = TB_TEST_USB3_TUNNEL(test, host, dev);
	dma_tunnel1 = TB_TEST_DMA_TUNNEL1(test, host, dev);
	dma_tunnel2 = TB_TEST_DMA_TUNNEL2(test, host, dev);

	tb_tunnel_free(dma_tunnel2);
	tb_tunnel_free(dma_tunnel1);
	tb_tunnel_free(usb3_tunnel);
	tb_tunnel_free(dp_tunnel2);
	tb_tunnel_free(dp_tunnel1);
	tb_tunnel_free(pcie_tunnel);
}

static const u32 root_directory[] = {
	0x55584401,	/* "UXD" v1 */
	0x00000018,	/* Root directory length */
	0x76656e64,	/* "vend" */
	0x6f726964,	/* "orid" */
	0x76000001,	/* "v" R 1 */
	0x00000a27,	/* Immediate value, ! Vendor ID */
	0x76656e64,	/* "vend" */
	0x6f726964,	/* "orid" */
	0x74000003,	/* "t" R 3 */
	0x0000001a,	/* Text leaf offset, (“Apple Inc.”) */
	0x64657669,	/* "devi" */
	0x63656964,	/* "ceid" */
	0x76000001,	/* "v" R 1 */
	0x0000000a,	/* Immediate value, ! Device ID */
	0x64657669,	/* "devi" */
	0x63656964,	/* "ceid" */
	0x74000003,	/* "t" R 3 */
	0x0000001d,	/* Text leaf offset, (“Macintosh”) */
	0x64657669,	/* "devi" */
	0x63657276,	/* "cerv" */
	0x76000001,	/* "v" R 1 */
	0x80000100,	/* Immediate value, Device Revision */
	0x6e657477,	/* "netw" */
	0x6f726b00,	/* "ork" */
	0x44000014,	/* "D" R 20 */
	0x00000021,	/* Directory data offset, (Network Directory) */
	0x4170706c,	/* "Appl" */
	0x6520496e,	/* "e In" */
	0x632e0000,	/* "c." ! */
	0x4d616369,	/* "Maci" */
	0x6e746f73,	/* "ntos" */
	0x68000000,	/* "h" */
	0x00000000,	/* padding */
	0xca8961c6,	/* Directory UUID, Network Directory */
	0x9541ce1c,	/* Directory UUID, Network Directory */
	0x5949b8bd,	/* Directory UUID, Network Directory */
	0x4f5a5f2e,	/* Directory UUID, Network Directory */
	0x70727463,	/* "prtc" */
	0x69640000,	/* "id" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol ID */
	0x70727463,	/* "prtc" */
	0x76657273,	/* "vers" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol Version */
	0x70727463,	/* "prtc" */
	0x72657673,	/* "revs" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol Revision */
	0x70727463,	/* "prtc" */
	0x73746e73,	/* "stns" */
	0x76000001,	/* "v" R 1 */
	0x00000000,	/* Immediate value, Network Protocol Settings */
};

static const uuid_t network_dir_uuid =
	UUID_INIT(0xc66189ca, 0x1cce, 0x4195,
		  0xbd, 0xb8, 0x49, 0x59, 0x2e, 0x5f, 0x5a, 0x4f);

static void tb_test_property_parse(struct kunit *test)
{
	struct tb_property_dir *dir, *network_dir;
	struct tb_property *p;

	dir = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, dir);

	p = tb_property_find(dir, "foo", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NULL(test, p);

	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_STREQ(test, p->value.text, "Apple Inc.");

	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0xa27);

	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_STREQ(test, p->value.text, "Macintosh");

	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0xa);

	p = tb_property_find(dir, "missing", TB_PROPERTY_TYPE_DIRECTORY);
	KUNIT_ASSERT_NULL(test, p);

	p = tb_property_find(dir, "network", TB_PROPERTY_TYPE_DIRECTORY);
	KUNIT_ASSERT_NOT_NULL(test, p);

	network_dir = p->value.dir;
	KUNIT_EXPECT_TRUE(test, uuid_equal(network_dir->uuid, &network_dir_uuid));

	p = tb_property_find(network_dir, "prtcid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcvers", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcrevs", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcstns", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x0);

	p = tb_property_find(network_dir, "deviceid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_EXPECT_TRUE(test, !p);
	p = tb_property_find(network_dir, "deviceid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_EXPECT_TRUE(test, !p);

	tb_property_free_dir(dir);
}

static void tb_test_property_format(struct kunit *test)
{
	struct tb_property_dir *dir;
	ssize_t block_len;
	u32 *block;
	int ret, i;

	dir = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, dir);

	ret = tb_property_format_dir(dir, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, ARRAY_SIZE(root_directory));

	block_len = ret;

	block = kunit_kzalloc(test, block_len * sizeof(u32), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, block);

	ret = tb_property_format_dir(dir, block, block_len);
	KUNIT_EXPECT_EQ(test, ret, 0);

	for (i = 0; i < ARRAY_SIZE(root_directory); i++)
		KUNIT_EXPECT_EQ(test, root_directory[i], block[i]);

	tb_property_free_dir(dir);
}

static void compare_dirs(struct kunit *test, struct tb_property_dir *d1,
			 struct tb_property_dir *d2)
{
	struct tb_property *p1, *p2, *tmp;
	int n1, n2, i;

	if (d1->uuid) {
		KUNIT_ASSERT_NOT_NULL(test, d2->uuid);
		KUNIT_ASSERT_TRUE(test, uuid_equal(d1->uuid, d2->uuid));
	} else {
		KUNIT_ASSERT_NULL(test, d2->uuid);
	}

	n1 = 0;
	tb_property_for_each(d1, tmp)
		n1++;
	KUNIT_ASSERT_NE(test, n1, 0);

	n2 = 0;
	tb_property_for_each(d2, tmp)
		n2++;
	KUNIT_ASSERT_NE(test, n2, 0);

	KUNIT_ASSERT_EQ(test, n1, n2);

	p1 = NULL;
	p2 = NULL;
	for (i = 0; i < n1; i++) {
		p1 = tb_property_get_next(d1, p1);
		KUNIT_ASSERT_NOT_NULL(test, p1);
		p2 = tb_property_get_next(d2, p2);
		KUNIT_ASSERT_NOT_NULL(test, p2);

		KUNIT_ASSERT_STREQ(test, &p1->key[0], &p2->key[0]);
		KUNIT_ASSERT_EQ(test, p1->type, p2->type);
		KUNIT_ASSERT_EQ(test, p1->length, p2->length);

		switch (p1->type) {
		case TB_PROPERTY_TYPE_DIRECTORY:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.dir);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.dir);
			compare_dirs(test, p1->value.dir, p2->value.dir);
			break;

		case TB_PROPERTY_TYPE_DATA:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.data);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.data);
			KUNIT_ASSERT_TRUE(test,
				!memcmp(p1->value.data, p2->value.data,
					p1->length * 4)
			);
			break;

		case TB_PROPERTY_TYPE_TEXT:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.text);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.text);
			KUNIT_ASSERT_STREQ(test, p1->value.text, p2->value.text);
			break;

		case TB_PROPERTY_TYPE_VALUE:
			KUNIT_ASSERT_EQ(test, p1->value.immediate,
					p2->value.immediate);
			break;
		default:
			KUNIT_FAIL(test, "unexpected property type");
			break;
		}
	}
}

static void tb_test_property_copy(struct kunit *test)
{
	struct tb_property_dir *src, *dst;
	u32 *block;
	int ret, i;

	src = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, src);

	dst = tb_property_copy_dir(src);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	/* Compare the structures */
	compare_dirs(test, src, dst);

	/* Compare the resulting property block */
	ret = tb_property_format_dir(dst, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, ARRAY_SIZE(root_directory));

	block = kunit_kzalloc(test, sizeof(root_directory), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, block);

	ret = tb_property_format_dir(dst, block, ARRAY_SIZE(root_directory));
	KUNIT_EXPECT_TRUE(test, !ret);

	for (i = 0; i < ARRAY_SIZE(root_directory); i++)
		KUNIT_EXPECT_EQ(test, root_directory[i], block[i]);

	tb_property_free_dir(dst);
	tb_property_free_dir(src);
}

static struct kunit_case tb_test_cases[] = {
	KUNIT_CASE(tb_test_path_basic),
	KUNIT_CASE(tb_test_path_not_connected_walk),
	KUNIT_CASE(tb_test_path_single_hop_walk),
	KUNIT_CASE(tb_test_path_daisy_chain_walk),
	KUNIT_CASE(tb_test_path_simple_tree_walk),
	KUNIT_CASE(tb_test_path_complex_tree_walk),
	KUNIT_CASE(tb_test_path_max_length_walk),
	KUNIT_CASE(tb_test_path_not_connected),
	KUNIT_CASE(tb_test_path_not_bonded_lane0),
	KUNIT_CASE(tb_test_path_not_bonded_lane1),
	KUNIT_CASE(tb_test_path_not_bonded_lane1_chain),
	KUNIT_CASE(tb_test_path_not_bonded_lane1_chain_reverse),
	KUNIT_CASE(tb_test_path_mixed_chain),
	KUNIT_CASE(tb_test_path_mixed_chain_reverse),
	KUNIT_CASE(tb_test_tunnel_pcie),
	KUNIT_CASE(tb_test_tunnel_dp),
	KUNIT_CASE(tb_test_tunnel_dp_chain),
	KUNIT_CASE(tb_test_tunnel_dp_tree),
	KUNIT_CASE(tb_test_tunnel_dp_max_length),
	KUNIT_CASE(tb_test_tunnel_3dp),
	KUNIT_CASE(tb_test_tunnel_port_on_path),
	KUNIT_CASE(tb_test_tunnel_usb3),
	KUNIT_CASE(tb_test_tunnel_dma),
	KUNIT_CASE(tb_test_tunnel_dma_rx),
	KUNIT_CASE(tb_test_tunnel_dma_tx),
	KUNIT_CASE(tb_test_tunnel_dma_chain),
	KUNIT_CASE(tb_test_tunnel_dma_match),
	KUNIT_CASE(tb_test_credit_alloc_legacy_not_bonded),
	KUNIT_CASE(tb_test_credit_alloc_legacy_bonded),
	KUNIT_CASE(tb_test_credit_alloc_pcie),
	KUNIT_CASE(tb_test_credit_alloc_without_dp),
	KUNIT_CASE(tb_test_credit_alloc_dp),
	KUNIT_CASE(tb_test_credit_alloc_usb3),
	KUNIT_CASE(tb_test_credit_alloc_dma),
	KUNIT_CASE(tb_test_credit_alloc_dma_multiple),
	KUNIT_CASE(tb_test_credit_alloc_all),
	KUNIT_CASE(tb_test_property_parse),
	KUNIT_CASE(tb_test_property_format),
	KUNIT_CASE(tb_test_property_copy),
	{ }
};

static struct kunit_suite tb_test_suite = {
	.name = "thunderbolt",
	.test_cases = tb_test_cases,
};

kunit_test_suite(tb_test_suite);
