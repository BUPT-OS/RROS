/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __OF_ADDRESS_H
#define __OF_ADDRESS_H
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/io.h>

struct of_bus;

struct of_pci_range_parser {
	struct device_node *node;
	struct of_bus *bus;
	const __be32 *range;
	const __be32 *end;
	int na;
	int ns;
	int pna;
	bool dma;
};
#define of_range_parser of_pci_range_parser

struct of_pci_range {
	union {
		u64 pci_addr;
		u64 bus_addr;
	};
	u64 cpu_addr;
	u64 size;
	u32 flags;
};
#define of_range of_pci_range

#define for_each_of_pci_range(parser, range) \
	for (; of_pci_range_parser_one(parser, range);)
#define for_each_of_range for_each_of_pci_range

/*
 * of_range_count - Get the number of "ranges" or "dma-ranges" entries
 * @parser:	Parser state initialized by of_range_parser_init()
 *
 * Returns the number of entries or 0 if none.
 *
 * Note that calling this within or after the for_each_of_range() iterator will
 * be inaccurate giving the number of entries remaining.
 */
static inline int of_range_count(const struct of_range_parser *parser)
{
	if (!parser || !parser->node || !parser->range || parser->range == parser->end)
		return 0;
	return (parser->end - parser->range) / (parser->na + parser->pna + parser->ns);
}

/* Translate a DMA address from device space to CPU space */
extern u64 of_translate_dma_address(struct device_node *dev,
				    const __be32 *in_addr);
extern const __be32 *of_translate_dma_region(struct device_node *dev, const __be32 *addr,
					     phys_addr_t *start, size_t *length);

#ifdef CONFIG_OF_ADDRESS
extern u64 of_translate_address(struct device_node *np, const __be32 *addr);
extern int of_address_to_resource(struct device_node *dev, int index,
				  struct resource *r);
extern void __iomem *of_iomap(struct device_node *device, int index);
void __iomem *of_io_request_and_map(struct device_node *device,
				    int index, const char *name);

/* Extract an address from a device, returns the region size and
 * the address space flags too. The PCI version uses a BAR number
 * instead of an absolute index
 */
extern const __be32 *__of_get_address(struct device_node *dev, int index, int bar_no,
				      u64 *size, unsigned int *flags);

int of_property_read_reg(struct device_node *np, int idx, u64 *addr, u64 *size);

extern int of_pci_range_parser_init(struct of_pci_range_parser *parser,
			struct device_node *node);
extern int of_pci_dma_range_parser_init(struct of_pci_range_parser *parser,
			struct device_node *node);
extern struct of_pci_range *of_pci_range_parser_one(
					struct of_pci_range_parser *parser,
					struct of_pci_range *range);
extern int of_pci_address_to_resource(struct device_node *dev, int bar,
				      struct resource *r);
extern int of_pci_range_to_resource(struct of_pci_range *range,
				    struct device_node *np,
				    struct resource *res);
extern int of_range_to_resource(struct device_node *np, int index,
				struct resource *res);
extern bool of_dma_is_coherent(struct device_node *np);
#else /* CONFIG_OF_ADDRESS */
static inline void __iomem *of_io_request_and_map(struct device_node *device,
						  int index, const char *name)
{
	return IOMEM_ERR_PTR(-EINVAL);
}

static inline u64 of_translate_address(struct device_node *np,
				       const __be32 *addr)
{
	return OF_BAD_ADDR;
}

static inline const __be32 *__of_get_address(struct device_node *dev, int index, int bar_no,
					     u64 *size, unsigned int *flags)
{
	return NULL;
}

static inline int of_property_read_reg(struct device_node *np, int idx, u64 *addr, u64 *size)
{
	return -ENOSYS;
}

static inline int of_pci_range_parser_init(struct of_pci_range_parser *parser,
			struct device_node *node)
{
	return -ENOSYS;
}

static inline int of_pci_dma_range_parser_init(struct of_pci_range_parser *parser,
			struct device_node *node)
{
	return -ENOSYS;
}

static inline struct of_pci_range *of_pci_range_parser_one(
					struct of_pci_range_parser *parser,
					struct of_pci_range *range)
{
	return NULL;
}

static inline int of_pci_address_to_resource(struct device_node *dev, int bar,
				             struct resource *r)
{
	return -ENOSYS;
}

static inline int of_pci_range_to_resource(struct of_pci_range *range,
					   struct device_node *np,
					   struct resource *res)
{
	return -ENOSYS;
}

static inline int of_range_to_resource(struct device_node *np, int index,
				       struct resource *res)
{
	return -ENOSYS;
}

static inline bool of_dma_is_coherent(struct device_node *np)
{
	return false;
}
#endif /* CONFIG_OF_ADDRESS */

#ifdef CONFIG_OF
extern int of_address_to_resource(struct device_node *dev, int index,
				  struct resource *r);
void __iomem *of_iomap(struct device_node *node, int index);
#else
static inline int of_address_to_resource(struct device_node *dev, int index,
					 struct resource *r)
{
	return -EINVAL;
}

static inline void __iomem *of_iomap(struct device_node *device, int index)
{
	return NULL;
}
#endif
#define of_range_parser_init of_pci_range_parser_init

static inline const __be32 *of_get_address(struct device_node *dev, int index,
					   u64 *size, unsigned int *flags)
{
	return __of_get_address(dev, index, -1, size, flags);
}

static inline const __be32 *of_get_pci_address(struct device_node *dev, int bar_no,
					       u64 *size, unsigned int *flags)
{
	return __of_get_address(dev, -1, bar_no, size, flags);
}

static inline int of_address_count(struct device_node *np)
{
	struct resource res;
	int count = 0;

	while (of_address_to_resource(np, count, &res) == 0)
		count++;

	return count;
}

#endif /* __OF_ADDRESS_H */
