// SPDX-License-Identifier: GPL-2.0

#include <linux/efi.h>
#include <asm/efi.h>
#include <asm/string.h>

#include "efistub.h"

#ifdef CONFIG_KASAN
#undef memcpy
#undef memmove
#undef memset
void *__memcpy(void *__dest, const void *__src, size_t __n) __alias(memcpy);
void *__memmove(void *__dest, const void *__src, size_t count) __alias(memmove);
void *__memset(void *s, int c, size_t count) __alias(memset);
#endif

void *memcpy(void *dst, const void *src, size_t len)
{
	efi_bs_call(copy_mem, dst, src, len);
	return dst;
}

extern void *memmove(void *dst, const void *src, size_t len) __alias(memcpy);

void *memset(void *dst, int c, size_t len)
{
	efi_bs_call(set_mem, dst, len, c & U8_MAX);
	return dst;
}

/**
 * memcmp - Compare two areas of memory
 * @cs: One area of memory
 * @ct: Another area of memory
 * @count: The size of the area.
 */
#undef memcmp
int memcmp(const void *cs, const void *ct, size_t count)
{
	const unsigned char *su1, *su2;
	int res = 0;

	for (su1 = cs, su2 = ct; 0 < count; ++su1, ++su2, count--)
		if ((res = *su1 - *su2) != 0)
			break;
	return res;
}
