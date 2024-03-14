/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DOVETAIL_NET_H
#define _DOVETAIL_NET_H

/*
 * Placeholder for a network descriptor extension otherwise defined by
 * the out-of-band network stack.
 */
struct oob_net_state {
};

static inline void net_init_oob_state(struct oob_net_state *nets) { }
static inline void net_cleanup_oob_state(struct oob_net_state *nets) { }

#endif /* !_DOVETAIL_NET_H */
