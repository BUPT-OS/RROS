/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TOOLS_MMZONE_H
#define _TOOLS_MMZONE_H

#include <linux/atomic.h>

struct pglist_data *first_online_pgdat(void);
struct pglist_data *next_online_pgdat(struct pglist_data *pgdat);

#define for_each_online_pgdat(pgdat)			\
	for (pgdat = first_online_pgdat();		\
	     pgdat;					\
	     pgdat = next_online_pgdat(pgdat))

enum zone_type {
	__MAX_NR_ZONES
};

#define MAX_NR_ZONES __MAX_NR_ZONES
#define MAX_ORDER 10
#define MAX_ORDER_NR_PAGES (1 << MAX_ORDER)

#define pageblock_order		MAX_ORDER
#define pageblock_nr_pages	BIT(pageblock_order)
#define pageblock_align(pfn)	ALIGN((pfn), pageblock_nr_pages)
#define pageblock_start_pfn(pfn)	ALIGN_DOWN((pfn), pageblock_nr_pages)

struct zone {
	atomic_long_t		managed_pages;
};

typedef struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];

} pg_data_t;

#endif
