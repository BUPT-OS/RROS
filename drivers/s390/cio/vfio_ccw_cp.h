/* SPDX-License-Identifier: GPL-2.0 */
/*
 * channel program interfaces
 *
 * Copyright IBM Corp. 2017
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 */

#ifndef _VFIO_CCW_CP_H_
#define _VFIO_CCW_CP_H_

#include <asm/cio.h>
#include <asm/scsw.h>

#include "orb.h"
#include "vfio_ccw_trace.h"

/*
 * Max length for ccw chain.
 * XXX: Limit to 256, need to check more?
 */
#define CCWCHAIN_LEN_MAX	256

/**
 * struct channel_program - manage information for channel program
 * @ccwchain_list: list head of ccwchains
 * @orb: orb for the currently processed ssch request
 * @initialized: whether this instance is actually initialized
 *
 * @ccwchain_list is the head of a ccwchain list, that contents the
 * translated result of the guest channel program that pointed out by
 * the iova parameter when calling cp_init.
 */
struct channel_program {
	struct list_head ccwchain_list;
	union orb orb;
	bool initialized;
	struct ccw1 *guest_cp;
};

int cp_init(struct channel_program *cp, union orb *orb);
void cp_free(struct channel_program *cp);
int cp_prefetch(struct channel_program *cp);
union orb *cp_get_orb(struct channel_program *cp, struct subchannel *sch);
void cp_update_scsw(struct channel_program *cp, union scsw *scsw);
bool cp_iova_pinned(struct channel_program *cp, u64 iova, u64 length);

#endif
