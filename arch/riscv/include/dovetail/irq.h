/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_DOVETAIL_IRQ_H
#define _EVL_DOVETAIL_IRQ_H

#ifdef CONFIG_EVL
#include <asm-generic/evl/irq.h>
#else
#include_next <dovetail/irq.h>
#endif

#endif /* !_EVL_DOVETAIL_IRQ_H */
