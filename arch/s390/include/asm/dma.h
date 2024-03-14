/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_S390_DMA_H
#define _ASM_S390_DMA_H

#include <linux/io.h>

/*
 * MAX_DMA_ADDRESS is ambiguous because on s390 its completely unrelated
 * to DMA. It _is_ used for the s390 memory zone split at 2GB caused
 * by the 31 bit heritage.
 */
#define MAX_DMA_ADDRESS		__va(0x80000000)

#endif /* _ASM_S390_DMA_H */
