// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/pci.h>
#include <asm/div64.h>
#include "cxlpci.h"
#include "cxl.h"

#define CXL_RCRB_SIZE	SZ_8K

struct cxl_cxims_data {
	int nr_maps;
	u64 xormaps[] __counted_by(nr_maps);
};

/*
 * Find a targets entry (n) in the host bridge interleave list.
 * CXL Specification 3.0 Table 9-22
 */
static int cxl_xor_calc_n(u64 hpa, struct cxl_cxims_data *cximsd, int iw,
			  int ig)
{
	int i = 0, n = 0;
	u8 eiw;

	/* IW: 2,4,6,8,12,16 begin building 'n' using xormaps */
	if (iw != 3) {
		for (i = 0; i < cximsd->nr_maps; i++)
			n |= (hweight64(hpa & cximsd->xormaps[i]) & 1) << i;
	}
	/* IW: 3,6,12 add a modulo calculation to 'n' */
	if (!is_power_of_2(iw)) {
		if (ways_to_eiw(iw, &eiw))
			return -1;
		hpa &= GENMASK_ULL(51, eiw + ig);
		n |= do_div(hpa, 3) << i;
	}
	return n;
}

static struct cxl_dport *cxl_hb_xor(struct cxl_root_decoder *cxlrd, int pos)
{
	struct cxl_cxims_data *cximsd = cxlrd->platform_data;
	struct cxl_switch_decoder *cxlsd = &cxlrd->cxlsd;
	struct cxl_decoder *cxld = &cxlsd->cxld;
	int ig = cxld->interleave_granularity;
	int iw = cxld->interleave_ways;
	int n = 0;
	u64 hpa;

	if (dev_WARN_ONCE(&cxld->dev,
			  cxld->interleave_ways != cxlsd->nr_targets,
			  "misconfigured root decoder\n"))
		return NULL;

	hpa = cxlrd->res->start + pos * ig;

	/* Entry (n) is 0 for no interleave (iw == 1) */
	if (iw != 1)
		n = cxl_xor_calc_n(hpa, cximsd, iw, ig);

	if (n < 0)
		return NULL;

	return cxlrd->cxlsd.target[n];
}

struct cxl_cxims_context {
	struct device *dev;
	struct cxl_root_decoder *cxlrd;
};

static int cxl_parse_cxims(union acpi_subtable_headers *header, void *arg,
			   const unsigned long end)
{
	struct acpi_cedt_cxims *cxims = (struct acpi_cedt_cxims *)header;
	struct cxl_cxims_context *ctx = arg;
	struct cxl_root_decoder *cxlrd = ctx->cxlrd;
	struct cxl_decoder *cxld = &cxlrd->cxlsd.cxld;
	struct device *dev = ctx->dev;
	struct cxl_cxims_data *cximsd;
	unsigned int hbig, nr_maps;
	int rc;

	rc = eig_to_granularity(cxims->hbig, &hbig);
	if (rc)
		return rc;

	/* Does this CXIMS entry apply to the given CXL Window? */
	if (hbig != cxld->interleave_granularity)
		return 0;

	/* IW 1,3 do not use xormaps and skip this parsing entirely */
	if (is_power_of_2(cxld->interleave_ways))
		/* 2, 4, 8, 16 way */
		nr_maps = ilog2(cxld->interleave_ways);
	else
		/* 6, 12 way */
		nr_maps = ilog2(cxld->interleave_ways / 3);

	if (cxims->nr_xormaps < nr_maps) {
		dev_dbg(dev, "CXIMS nr_xormaps[%d] expected[%d]\n",
			cxims->nr_xormaps, nr_maps);
		return -ENXIO;
	}

	cximsd = devm_kzalloc(dev, struct_size(cximsd, xormaps, nr_maps),
			      GFP_KERNEL);
	if (!cximsd)
		return -ENOMEM;
	cximsd->nr_maps = nr_maps;
	memcpy(cximsd->xormaps, cxims->xormap_list,
	       nr_maps * sizeof(*cximsd->xormaps));
	cxlrd->platform_data = cximsd;

	return 0;
}

static unsigned long cfmws_to_decoder_flags(int restrictions)
{
	unsigned long flags = CXL_DECODER_F_ENABLE;

	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_TYPE2)
		flags |= CXL_DECODER_F_TYPE2;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_TYPE3)
		flags |= CXL_DECODER_F_TYPE3;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_VOLATILE)
		flags |= CXL_DECODER_F_RAM;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_PMEM)
		flags |= CXL_DECODER_F_PMEM;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_FIXED)
		flags |= CXL_DECODER_F_LOCK;

	return flags;
}

static int cxl_acpi_cfmws_verify(struct device *dev,
				 struct acpi_cedt_cfmws *cfmws)
{
	int rc, expected_len;
	unsigned int ways;

	if (cfmws->interleave_arithmetic != ACPI_CEDT_CFMWS_ARITHMETIC_MODULO &&
	    cfmws->interleave_arithmetic != ACPI_CEDT_CFMWS_ARITHMETIC_XOR) {
		dev_err(dev, "CFMWS Unknown Interleave Arithmetic: %d\n",
			cfmws->interleave_arithmetic);
		return -EINVAL;
	}

	if (!IS_ALIGNED(cfmws->base_hpa, SZ_256M)) {
		dev_err(dev, "CFMWS Base HPA not 256MB aligned\n");
		return -EINVAL;
	}

	if (!IS_ALIGNED(cfmws->window_size, SZ_256M)) {
		dev_err(dev, "CFMWS Window Size not 256MB aligned\n");
		return -EINVAL;
	}

	rc = eiw_to_ways(cfmws->interleave_ways, &ways);
	if (rc) {
		dev_err(dev, "CFMWS Interleave Ways (%d) invalid\n",
			cfmws->interleave_ways);
		return -EINVAL;
	}

	expected_len = struct_size(cfmws, interleave_targets, ways);

	if (cfmws->header.length < expected_len) {
		dev_err(dev, "CFMWS length %d less than expected %d\n",
			cfmws->header.length, expected_len);
		return -EINVAL;
	}

	if (cfmws->header.length > expected_len)
		dev_dbg(dev, "CFMWS length %d greater than expected %d\n",
			cfmws->header.length, expected_len);

	return 0;
}

/*
 * Note, @dev must be the first member, see 'struct cxl_chbs_context'
 * and mock_acpi_table_parse_cedt()
 */
struct cxl_cfmws_context {
	struct device *dev;
	struct cxl_port *root_port;
	struct resource *cxl_res;
	int id;
};

static int cxl_parse_cfmws(union acpi_subtable_headers *header, void *arg,
			   const unsigned long end)
{
	int target_map[CXL_DECODER_MAX_INTERLEAVE];
	struct cxl_cfmws_context *ctx = arg;
	struct cxl_port *root_port = ctx->root_port;
	struct resource *cxl_res = ctx->cxl_res;
	struct cxl_cxims_context cxims_ctx;
	struct cxl_root_decoder *cxlrd;
	struct device *dev = ctx->dev;
	struct acpi_cedt_cfmws *cfmws;
	cxl_calc_hb_fn cxl_calc_hb;
	struct cxl_decoder *cxld;
	unsigned int ways, i, ig;
	struct resource *res;
	int rc;

	cfmws = (struct acpi_cedt_cfmws *) header;

	rc = cxl_acpi_cfmws_verify(dev, cfmws);
	if (rc) {
		dev_err(dev, "CFMWS range %#llx-%#llx not registered\n",
			cfmws->base_hpa,
			cfmws->base_hpa + cfmws->window_size - 1);
		return 0;
	}

	rc = eiw_to_ways(cfmws->interleave_ways, &ways);
	if (rc)
		return rc;
	rc = eig_to_granularity(cfmws->granularity, &ig);
	if (rc)
		return rc;
	for (i = 0; i < ways; i++)
		target_map[i] = cfmws->interleave_targets[i];

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	res->name = kasprintf(GFP_KERNEL, "CXL Window %d", ctx->id++);
	if (!res->name)
		goto err_name;

	res->start = cfmws->base_hpa;
	res->end = cfmws->base_hpa + cfmws->window_size - 1;
	res->flags = IORESOURCE_MEM;

	/* add to the local resource tracking to establish a sort order */
	rc = insert_resource(cxl_res, res);
	if (rc)
		goto err_insert;

	if (cfmws->interleave_arithmetic == ACPI_CEDT_CFMWS_ARITHMETIC_MODULO)
		cxl_calc_hb = cxl_hb_modulo;
	else
		cxl_calc_hb = cxl_hb_xor;

	cxlrd = cxl_root_decoder_alloc(root_port, ways, cxl_calc_hb);
	if (IS_ERR(cxlrd))
		return 0;

	cxld = &cxlrd->cxlsd.cxld;
	cxld->flags = cfmws_to_decoder_flags(cfmws->restrictions);
	cxld->target_type = CXL_DECODER_HOSTONLYMEM;
	cxld->hpa_range = (struct range) {
		.start = res->start,
		.end = res->end,
	};
	cxld->interleave_ways = ways;
	/*
	 * Minimize the x1 granularity to advertise support for any
	 * valid region granularity
	 */
	if (ways == 1)
		ig = CXL_DECODER_MIN_GRANULARITY;
	cxld->interleave_granularity = ig;

	if (cfmws->interleave_arithmetic == ACPI_CEDT_CFMWS_ARITHMETIC_XOR) {
		if (ways != 1 && ways != 3) {
			cxims_ctx = (struct cxl_cxims_context) {
				.dev = dev,
				.cxlrd = cxlrd,
			};
			rc = acpi_table_parse_cedt(ACPI_CEDT_TYPE_CXIMS,
						   cxl_parse_cxims, &cxims_ctx);
			if (rc < 0)
				goto err_xormap;
			if (!cxlrd->platform_data) {
				dev_err(dev, "No CXIMS for HBIG %u\n", ig);
				rc = -EINVAL;
				goto err_xormap;
			}
		}
	}
	rc = cxl_decoder_add(cxld, target_map);
err_xormap:
	if (rc)
		put_device(&cxld->dev);
	else
		rc = cxl_decoder_autoremove(dev, cxld);
	if (rc) {
		dev_err(dev, "Failed to add decode range: %pr", res);
		return rc;
	}
	dev_dbg(dev, "add: %s node: %d range [%#llx - %#llx]\n",
		dev_name(&cxld->dev),
		phys_to_target_node(cxld->hpa_range.start),
		cxld->hpa_range.start, cxld->hpa_range.end);

	return 0;

err_insert:
	kfree(res->name);
err_name:
	kfree(res);
	return -ENOMEM;
}

__mock struct acpi_device *to_cxl_host_bridge(struct device *host,
					      struct device *dev)
{
	struct acpi_device *adev = to_acpi_device(dev);

	if (!acpi_pci_find_root(adev->handle))
		return NULL;

	if (strcmp(acpi_device_hid(adev), "ACPI0016") == 0)
		return adev;
	return NULL;
}

/* Note, @dev is used by mock_acpi_table_parse_cedt() */
struct cxl_chbs_context {
	struct device *dev;
	unsigned long long uid;
	resource_size_t base;
	u32 cxl_version;
};

static int cxl_get_chbs_iter(union acpi_subtable_headers *header, void *arg,
			     const unsigned long end)
{
	struct cxl_chbs_context *ctx = arg;
	struct acpi_cedt_chbs *chbs;

	if (ctx->base != CXL_RESOURCE_NONE)
		return 0;

	chbs = (struct acpi_cedt_chbs *) header;

	if (ctx->uid != chbs->uid)
		return 0;

	ctx->cxl_version = chbs->cxl_version;
	if (!chbs->base)
		return 0;

	if (chbs->cxl_version == ACPI_CEDT_CHBS_VERSION_CXL11 &&
	    chbs->length != CXL_RCRB_SIZE)
		return 0;

	ctx->base = chbs->base;

	return 0;
}

static int cxl_get_chbs(struct device *dev, struct acpi_device *hb,
			struct cxl_chbs_context *ctx)
{
	unsigned long long uid;
	int rc;

	rc = acpi_evaluate_integer(hb->handle, METHOD_NAME__UID, NULL, &uid);
	if (rc != AE_OK) {
		dev_err(dev, "unable to retrieve _UID\n");
		return -ENOENT;
	}

	dev_dbg(dev, "UID found: %lld\n", uid);
	*ctx = (struct cxl_chbs_context) {
		.dev = dev,
		.uid = uid,
		.base = CXL_RESOURCE_NONE,
		.cxl_version = UINT_MAX,
	};

	acpi_table_parse_cedt(ACPI_CEDT_TYPE_CHBS, cxl_get_chbs_iter, ctx);

	return 0;
}

static int add_host_bridge_dport(struct device *match, void *arg)
{
	acpi_status rc;
	struct device *bridge;
	struct cxl_dport *dport;
	struct cxl_chbs_context ctx;
	struct acpi_pci_root *pci_root;
	struct cxl_port *root_port = arg;
	struct device *host = root_port->dev.parent;
	struct acpi_device *hb = to_cxl_host_bridge(host, match);

	if (!hb)
		return 0;

	rc = cxl_get_chbs(match, hb, &ctx);
	if (rc)
		return rc;

	if (ctx.cxl_version == UINT_MAX) {
		dev_warn(match, "No CHBS found for Host Bridge (UID %lld)\n",
			 ctx.uid);
		return 0;
	}

	if (ctx.base == CXL_RESOURCE_NONE) {
		dev_warn(match, "CHBS invalid for Host Bridge (UID %lld)\n",
			 ctx.uid);
		return 0;
	}

	pci_root = acpi_pci_find_root(hb->handle);
	bridge = pci_root->bus->bridge;

	/*
	 * In RCH mode, bind the component regs base to the dport. In
	 * VH mode it will be bound to the CXL host bridge's port
	 * object later in add_host_bridge_uport().
	 */
	if (ctx.cxl_version == ACPI_CEDT_CHBS_VERSION_CXL11) {
		dev_dbg(match, "RCRB found for UID %lld: %pa\n", ctx.uid,
			&ctx.base);
		dport = devm_cxl_add_rch_dport(root_port, bridge, ctx.uid,
					       ctx.base);
	} else {
		dport = devm_cxl_add_dport(root_port, bridge, ctx.uid,
					   CXL_RESOURCE_NONE);
	}

	if (IS_ERR(dport))
		return PTR_ERR(dport);

	return 0;
}

/*
 * A host bridge is a dport to a CFMWS decode and it is a uport to the
 * dport (PCIe Root Ports) in the host bridge.
 */
static int add_host_bridge_uport(struct device *match, void *arg)
{
	struct cxl_port *root_port = arg;
	struct device *host = root_port->dev.parent;
	struct acpi_device *hb = to_cxl_host_bridge(host, match);
	struct acpi_pci_root *pci_root;
	struct cxl_dport *dport;
	struct cxl_port *port;
	struct device *bridge;
	struct cxl_chbs_context ctx;
	resource_size_t component_reg_phys;
	int rc;

	if (!hb)
		return 0;

	pci_root = acpi_pci_find_root(hb->handle);
	bridge = pci_root->bus->bridge;
	dport = cxl_find_dport_by_dev(root_port, bridge);
	if (!dport) {
		dev_dbg(host, "host bridge expected and not found\n");
		return 0;
	}

	if (dport->rch) {
		dev_info(bridge, "host supports CXL (restricted)\n");
		return 0;
	}

	rc = cxl_get_chbs(match, hb, &ctx);
	if (rc)
		return rc;

	if (ctx.cxl_version == ACPI_CEDT_CHBS_VERSION_CXL11) {
		dev_warn(bridge,
			 "CXL CHBS version mismatch, skip port registration\n");
		return 0;
	}

	component_reg_phys = ctx.base;
	if (component_reg_phys != CXL_RESOURCE_NONE)
		dev_dbg(match, "CHBCR found for UID %lld: %pa\n",
			ctx.uid, &component_reg_phys);

	rc = devm_cxl_register_pci_bus(host, bridge, pci_root->bus);
	if (rc)
		return rc;

	port = devm_cxl_add_port(host, bridge, component_reg_phys, dport);
	if (IS_ERR(port))
		return PTR_ERR(port);

	dev_info(bridge, "host supports CXL\n");

	return 0;
}

static int add_root_nvdimm_bridge(struct device *match, void *data)
{
	struct cxl_decoder *cxld;
	struct cxl_port *root_port = data;
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct device *host = root_port->dev.parent;

	if (!is_root_decoder(match))
		return 0;

	cxld = to_cxl_decoder(match);
	if (!(cxld->flags & CXL_DECODER_F_PMEM))
		return 0;

	cxl_nvb = devm_cxl_add_nvdimm_bridge(host, root_port);
	if (IS_ERR(cxl_nvb)) {
		dev_dbg(host, "failed to register pmem\n");
		return PTR_ERR(cxl_nvb);
	}
	dev_dbg(host, "%s: add: %s\n", dev_name(&root_port->dev),
		dev_name(&cxl_nvb->dev));
	return 1;
}

static struct lock_class_key cxl_root_key;

static void cxl_acpi_lock_reset_class(void *dev)
{
	device_lock_reset_class(dev);
}

static void del_cxl_resource(struct resource *res)
{
	kfree(res->name);
	kfree(res);
}

static void cxl_set_public_resource(struct resource *priv, struct resource *pub)
{
	priv->desc = (unsigned long) pub;
}

static struct resource *cxl_get_public_resource(struct resource *priv)
{
	return (struct resource *) priv->desc;
}

static void remove_cxl_resources(void *data)
{
	struct resource *res, *next, *cxl = data;

	for (res = cxl->child; res; res = next) {
		struct resource *victim = cxl_get_public_resource(res);

		next = res->sibling;
		remove_resource(res);

		if (victim) {
			remove_resource(victim);
			kfree(victim);
		}

		del_cxl_resource(res);
	}
}

/**
 * add_cxl_resources() - reflect CXL fixed memory windows in iomem_resource
 * @cxl_res: A standalone resource tree where each CXL window is a sibling
 *
 * Walk each CXL window in @cxl_res and add it to iomem_resource potentially
 * expanding its boundaries to ensure that any conflicting resources become
 * children. If a window is expanded it may then conflict with a another window
 * entry and require the window to be truncated or trimmed. Consider this
 * situation:
 *
 * |-- "CXL Window 0" --||----- "CXL Window 1" -----|
 * |--------------- "System RAM" -------------|
 *
 * ...where platform firmware has established as System RAM resource across 2
 * windows, but has left some portion of window 1 for dynamic CXL region
 * provisioning. In this case "Window 0" will span the entirety of the "System
 * RAM" span, and "CXL Window 1" is truncated to the remaining tail past the end
 * of that "System RAM" resource.
 */
static int add_cxl_resources(struct resource *cxl_res)
{
	struct resource *res, *new, *next;

	for (res = cxl_res->child; res; res = next) {
		new = kzalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		new->name = res->name;
		new->start = res->start;
		new->end = res->end;
		new->flags = IORESOURCE_MEM;
		new->desc = IORES_DESC_CXL;

		/*
		 * Record the public resource in the private cxl_res tree for
		 * later removal.
		 */
		cxl_set_public_resource(res, new);

		insert_resource_expand_to_fit(&iomem_resource, new);

		next = res->sibling;
		while (next && resource_overlaps(new, next)) {
			if (resource_contains(new, next)) {
				struct resource *_next = next->sibling;

				remove_resource(next);
				del_cxl_resource(next);
				next = _next;
			} else
				next->start = new->end + 1;
		}
	}
	return 0;
}

static int pair_cxl_resource(struct device *dev, void *data)
{
	struct resource *cxl_res = data;
	struct resource *p;

	if (!is_root_decoder(dev))
		return 0;

	for (p = cxl_res->child; p; p = p->sibling) {
		struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev);
		struct cxl_decoder *cxld = &cxlrd->cxlsd.cxld;
		struct resource res = {
			.start = cxld->hpa_range.start,
			.end = cxld->hpa_range.end,
			.flags = IORESOURCE_MEM,
		};

		if (resource_contains(p, &res)) {
			cxlrd->res = cxl_get_public_resource(p);
			break;
		}
	}

	return 0;
}

static int cxl_acpi_probe(struct platform_device *pdev)
{
	int rc;
	struct resource *cxl_res;
	struct cxl_port *root_port;
	struct device *host = &pdev->dev;
	struct acpi_device *adev = ACPI_COMPANION(host);
	struct cxl_cfmws_context ctx;

	device_lock_set_class(&pdev->dev, &cxl_root_key);
	rc = devm_add_action_or_reset(&pdev->dev, cxl_acpi_lock_reset_class,
				      &pdev->dev);
	if (rc)
		return rc;

	cxl_res = devm_kzalloc(host, sizeof(*cxl_res), GFP_KERNEL);
	if (!cxl_res)
		return -ENOMEM;
	cxl_res->name = "CXL mem";
	cxl_res->start = 0;
	cxl_res->end = -1;
	cxl_res->flags = IORESOURCE_MEM;

	root_port = devm_cxl_add_port(host, host, CXL_RESOURCE_NONE, NULL);
	if (IS_ERR(root_port))
		return PTR_ERR(root_port);

	rc = bus_for_each_dev(adev->dev.bus, NULL, root_port,
			      add_host_bridge_dport);
	if (rc < 0)
		return rc;

	rc = devm_add_action_or_reset(host, remove_cxl_resources, cxl_res);
	if (rc)
		return rc;

	ctx = (struct cxl_cfmws_context) {
		.dev = host,
		.root_port = root_port,
		.cxl_res = cxl_res,
	};
	rc = acpi_table_parse_cedt(ACPI_CEDT_TYPE_CFMWS, cxl_parse_cfmws, &ctx);
	if (rc < 0)
		return -ENXIO;

	rc = add_cxl_resources(cxl_res);
	if (rc)
		return rc;

	/*
	 * Populate the root decoders with their related iomem resource,
	 * if present
	 */
	device_for_each_child(&root_port->dev, cxl_res, pair_cxl_resource);

	/*
	 * Root level scanned with host-bridge as dports, now scan host-bridges
	 * for their role as CXL uports to their CXL-capable PCIe Root Ports.
	 */
	rc = bus_for_each_dev(adev->dev.bus, NULL, root_port,
			      add_host_bridge_uport);
	if (rc < 0)
		return rc;

	if (IS_ENABLED(CONFIG_CXL_PMEM))
		rc = device_for_each_child(&root_port->dev, root_port,
					   add_root_nvdimm_bridge);
	if (rc < 0)
		return rc;

	/* In case PCI is scanned before ACPI re-trigger memdev attach */
	cxl_bus_rescan();
	return 0;
}

static const struct acpi_device_id cxl_acpi_ids[] = {
	{ "ACPI0017" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, cxl_acpi_ids);

static const struct platform_device_id cxl_test_ids[] = {
	{ "cxl_acpi" },
	{ },
};
MODULE_DEVICE_TABLE(platform, cxl_test_ids);

static struct platform_driver cxl_acpi_driver = {
	.probe = cxl_acpi_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.acpi_match_table = cxl_acpi_ids,
	},
	.id_table = cxl_test_ids,
};

static int __init cxl_acpi_init(void)
{
	return platform_driver_register(&cxl_acpi_driver);
}

static void __exit cxl_acpi_exit(void)
{
	platform_driver_unregister(&cxl_acpi_driver);
	cxl_bus_drain();
}

/* load before dax_hmem sees 'Soft Reserved' CXL ranges */
subsys_initcall(cxl_acpi_init);
module_exit(cxl_acpi_exit);
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(CXL);
MODULE_IMPORT_NS(ACPI);
