/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/node.h>
#include <linux/mmzone.h>
#include <linux/compaction.h>
/*
 * The order of these masks is important. Matching masks will be seen
 * first and the left over flags will end up showing by themselves.
 *
 * For example, if we have GFP_KERNEL before GFP_USER we wil get:
 *
 *  GFP_KERNEL|GFP_HARDWALL
 *
 * Thus most bits set go first.
 */

#define gfpflag_string(flag) {(__force unsigned long)flag, #flag}

#define __def_gfpflag_names			\
	gfpflag_string(GFP_TRANSHUGE),		\
	gfpflag_string(GFP_TRANSHUGE_LIGHT),	\
	gfpflag_string(GFP_HIGHUSER_MOVABLE),	\
	gfpflag_string(GFP_HIGHUSER),		\
	gfpflag_string(GFP_USER),		\
	gfpflag_string(GFP_KERNEL_ACCOUNT),	\
	gfpflag_string(GFP_KERNEL),		\
	gfpflag_string(GFP_NOFS),		\
	gfpflag_string(GFP_ATOMIC),		\
	gfpflag_string(GFP_NOIO),		\
	gfpflag_string(GFP_NOWAIT),		\
	gfpflag_string(GFP_DMA),		\
	gfpflag_string(__GFP_HIGHMEM),		\
	gfpflag_string(GFP_DMA32),		\
	gfpflag_string(__GFP_HIGH),		\
	gfpflag_string(__GFP_IO),		\
	gfpflag_string(__GFP_FS),		\
	gfpflag_string(__GFP_NOWARN),		\
	gfpflag_string(__GFP_RETRY_MAYFAIL),	\
	gfpflag_string(__GFP_NOFAIL),		\
	gfpflag_string(__GFP_NORETRY),		\
	gfpflag_string(__GFP_COMP),		\
	gfpflag_string(__GFP_ZERO),		\
	gfpflag_string(__GFP_NOMEMALLOC),	\
	gfpflag_string(__GFP_MEMALLOC),		\
	gfpflag_string(__GFP_HARDWALL),		\
	gfpflag_string(__GFP_THISNODE),		\
	gfpflag_string(__GFP_RECLAIMABLE),	\
	gfpflag_string(__GFP_MOVABLE),		\
	gfpflag_string(__GFP_ACCOUNT),		\
	gfpflag_string(__GFP_WRITE),		\
	gfpflag_string(__GFP_RECLAIM),		\
	gfpflag_string(__GFP_DIRECT_RECLAIM),	\
	gfpflag_string(__GFP_KSWAPD_RECLAIM),	\
	gfpflag_string(__GFP_ZEROTAGS)

#ifdef CONFIG_KASAN_HW_TAGS
#define __def_gfpflag_names_kasan ,			\
	gfpflag_string(__GFP_SKIP_ZERO),		\
	gfpflag_string(__GFP_SKIP_KASAN)
#else
#define __def_gfpflag_names_kasan
#endif

#define show_gfp_flags(flags)						\
	(flags) ? __print_flags(flags, "|",				\
	__def_gfpflag_names __def_gfpflag_names_kasan			\
	) : "none"

#ifdef CONFIG_MMU
#define IF_HAVE_PG_MLOCK(_name) ,{1UL << PG_##_name, __stringify(_name)}
#else
#define IF_HAVE_PG_MLOCK(_name)
#endif

#ifdef CONFIG_ARCH_USES_PG_UNCACHED
#define IF_HAVE_PG_UNCACHED(_name) ,{1UL << PG_##_name, __stringify(_name)}
#else
#define IF_HAVE_PG_UNCACHED(_name)
#endif

#ifdef CONFIG_MEMORY_FAILURE
#define IF_HAVE_PG_HWPOISON(_name) ,{1UL << PG_##_name, __stringify(_name)}
#else
#define IF_HAVE_PG_HWPOISON(_name)
#endif

#if defined(CONFIG_PAGE_IDLE_FLAG) && defined(CONFIG_64BIT)
#define IF_HAVE_PG_IDLE(_name) ,{1UL << PG_##_name, __stringify(_name)}
#else
#define IF_HAVE_PG_IDLE(_name)
#endif

#ifdef CONFIG_ARCH_USES_PG_ARCH_X
#define IF_HAVE_PG_ARCH_X(_name) ,{1UL << PG_##_name, __stringify(_name)}
#else
#define IF_HAVE_PG_ARCH_X(_name)
#endif

#define DEF_PAGEFLAG_NAME(_name) { 1UL <<  PG_##_name, __stringify(_name) }

#define __def_pageflag_names						\
	DEF_PAGEFLAG_NAME(locked),					\
	DEF_PAGEFLAG_NAME(waiters),					\
	DEF_PAGEFLAG_NAME(error),					\
	DEF_PAGEFLAG_NAME(referenced),					\
	DEF_PAGEFLAG_NAME(uptodate),					\
	DEF_PAGEFLAG_NAME(dirty),					\
	DEF_PAGEFLAG_NAME(lru),						\
	DEF_PAGEFLAG_NAME(active),					\
	DEF_PAGEFLAG_NAME(workingset),					\
	DEF_PAGEFLAG_NAME(slab),					\
	DEF_PAGEFLAG_NAME(owner_priv_1),				\
	DEF_PAGEFLAG_NAME(arch_1),					\
	DEF_PAGEFLAG_NAME(reserved),					\
	DEF_PAGEFLAG_NAME(private),					\
	DEF_PAGEFLAG_NAME(private_2),					\
	DEF_PAGEFLAG_NAME(writeback),					\
	DEF_PAGEFLAG_NAME(head),					\
	DEF_PAGEFLAG_NAME(mappedtodisk),				\
	DEF_PAGEFLAG_NAME(reclaim),					\
	DEF_PAGEFLAG_NAME(swapbacked),					\
	DEF_PAGEFLAG_NAME(unevictable)					\
IF_HAVE_PG_MLOCK(mlocked)						\
IF_HAVE_PG_UNCACHED(uncached)						\
IF_HAVE_PG_HWPOISON(hwpoison)						\
IF_HAVE_PG_IDLE(idle)							\
IF_HAVE_PG_IDLE(young)							\
IF_HAVE_PG_ARCH_X(arch_2)						\
IF_HAVE_PG_ARCH_X(arch_3)

#define show_page_flags(flags)						\
	(flags) ? __print_flags(flags, "|",				\
	__def_pageflag_names						\
	) : "none"

#define DEF_PAGETYPE_NAME(_name) { PG_##_name, __stringify(_name) }

#define __def_pagetype_names						\
	DEF_PAGETYPE_NAME(offline),					\
	DEF_PAGETYPE_NAME(guard),					\
	DEF_PAGETYPE_NAME(table),					\
	DEF_PAGETYPE_NAME(buddy)

#if defined(CONFIG_X86)
#define __VM_ARCH_SPECIFIC_1 {VM_PAT,     "pat"           }
#elif defined(CONFIG_PPC)
#define __VM_ARCH_SPECIFIC_1 {VM_SAO,     "sao"           }
#elif defined(CONFIG_PARISC) || defined(CONFIG_IA64)
#define __VM_ARCH_SPECIFIC_1 {VM_GROWSUP,	"growsup"	}
#elif !defined(CONFIG_MMU)
#define __VM_ARCH_SPECIFIC_1 {VM_MAPPED_COPY,"mappedcopy"	}
#else
#define __VM_ARCH_SPECIFIC_1 {VM_ARCH_1,	"arch_1"	}
#endif

#ifdef CONFIG_MEM_SOFT_DIRTY
#define IF_HAVE_VM_SOFTDIRTY(flag,name) {flag, name },
#else
#define IF_HAVE_VM_SOFTDIRTY(flag,name)
#endif

#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_MINOR
# define IF_HAVE_UFFD_MINOR(flag, name) {flag, name},
#else
# define IF_HAVE_UFFD_MINOR(flag, name)
#endif

#define __def_vmaflag_names						\
	{VM_READ,			"read"		},		\
	{VM_WRITE,			"write"		},		\
	{VM_EXEC,			"exec"		},		\
	{VM_SHARED,			"shared"	},		\
	{VM_MAYREAD,			"mayread"	},		\
	{VM_MAYWRITE,			"maywrite"	},		\
	{VM_MAYEXEC,			"mayexec"	},		\
	{VM_MAYSHARE,			"mayshare"	},		\
	{VM_GROWSDOWN,			"growsdown"	},		\
	{VM_UFFD_MISSING,		"uffd_missing"	},		\
IF_HAVE_UFFD_MINOR(VM_UFFD_MINOR,	"uffd_minor"	)		\
	{VM_PFNMAP,			"pfnmap"	},		\
	{VM_UFFD_WP,			"uffd_wp"	},		\
	{VM_LOCKED,			"locked"	},		\
	{VM_IO,				"io"		},		\
	{VM_SEQ_READ,			"seqread"	},		\
	{VM_RAND_READ,			"randread"	},		\
	{VM_DONTCOPY,			"dontcopy"	},		\
	{VM_DONTEXPAND,			"dontexpand"	},		\
	{VM_LOCKONFAULT,		"lockonfault"	},		\
	{VM_ACCOUNT,			"account"	},		\
	{VM_NORESERVE,			"noreserve"	},		\
	{VM_HUGETLB,			"hugetlb"	},		\
	{VM_SYNC,			"sync"		},		\
	__VM_ARCH_SPECIFIC_1				,		\
	{VM_WIPEONFORK,			"wipeonfork"	},		\
	{VM_DONTDUMP,			"dontdump"	},		\
IF_HAVE_VM_SOFTDIRTY(VM_SOFTDIRTY,	"softdirty"	)		\
	{VM_MIXEDMAP,			"mixedmap"	},		\
	{VM_HUGEPAGE,			"hugepage"	},		\
	{VM_NOHUGEPAGE,			"nohugepage"	},		\
	{VM_MERGEABLE,			"mergeable"	}		\

#define show_vma_flags(flags)						\
	(flags) ? __print_flags(flags, "|",				\
	__def_vmaflag_names						\
	) : "none"

#ifdef CONFIG_COMPACTION
#define COMPACTION_STATUS					\
	EM( COMPACT_SKIPPED,		"skipped")		\
	EM( COMPACT_DEFERRED,		"deferred")		\
	EM( COMPACT_CONTINUE,		"continue")		\
	EM( COMPACT_SUCCESS,		"success")		\
	EM( COMPACT_PARTIAL_SKIPPED,	"partial_skipped")	\
	EM( COMPACT_COMPLETE,		"complete")		\
	EM( COMPACT_NO_SUITABLE_PAGE,	"no_suitable_page")	\
	EM( COMPACT_NOT_SUITABLE_ZONE,	"not_suitable_zone")	\
	EMe(COMPACT_CONTENDED,		"contended")

/* High-level compaction status feedback */
#define COMPACTION_FAILED	1
#define COMPACTION_WITHDRAWN	2
#define COMPACTION_PROGRESS	3

#define compact_result_to_feedback(result)	\
({						\
	enum compact_result __result = result;	\
	(__result == COMPACT_COMPLETE) ? COMPACTION_FAILED : \
		(__result == COMPACT_SUCCESS) ? COMPACTION_PROGRESS : COMPACTION_WITHDRAWN; \
})

#define COMPACTION_FEEDBACK		\
	EM(COMPACTION_FAILED,		"failed")	\
	EM(COMPACTION_WITHDRAWN,	"withdrawn")	\
	EMe(COMPACTION_PROGRESS,	"progress")

#define COMPACTION_PRIORITY						\
	EM(COMPACT_PRIO_SYNC_FULL,	"COMPACT_PRIO_SYNC_FULL")	\
	EM(COMPACT_PRIO_SYNC_LIGHT,	"COMPACT_PRIO_SYNC_LIGHT")	\
	EMe(COMPACT_PRIO_ASYNC,		"COMPACT_PRIO_ASYNC")
#else
#define COMPACTION_STATUS
#define COMPACTION_PRIORITY
#define COMPACTION_FEEDBACK
#endif

#ifdef CONFIG_ZONE_DMA
#define IFDEF_ZONE_DMA(X) X
#else
#define IFDEF_ZONE_DMA(X)
#endif

#ifdef CONFIG_ZONE_DMA32
#define IFDEF_ZONE_DMA32(X) X
#else
#define IFDEF_ZONE_DMA32(X)
#endif

#ifdef CONFIG_HIGHMEM
#define IFDEF_ZONE_HIGHMEM(X) X
#else
#define IFDEF_ZONE_HIGHMEM(X)
#endif

#define ZONE_TYPE						\
	IFDEF_ZONE_DMA(		EM (ZONE_DMA,	 "DMA"))	\
	IFDEF_ZONE_DMA32(	EM (ZONE_DMA32,	 "DMA32"))	\
				EM (ZONE_NORMAL, "Normal")	\
	IFDEF_ZONE_HIGHMEM(	EM (ZONE_HIGHMEM,"HighMem"))	\
				EMe(ZONE_MOVABLE,"Movable")

#define LRU_NAMES		\
		EM (LRU_INACTIVE_ANON, "inactive_anon") \
		EM (LRU_ACTIVE_ANON, "active_anon") \
		EM (LRU_INACTIVE_FILE, "inactive_file") \
		EM (LRU_ACTIVE_FILE, "active_file") \
		EMe(LRU_UNEVICTABLE, "unevictable")

/*
 * First define the enums in the above macros to be exported to userspace
 * via TRACE_DEFINE_ENUM().
 */
#undef EM
#undef EMe
#define EM(a, b)	TRACE_DEFINE_ENUM(a);
#define EMe(a, b)	TRACE_DEFINE_ENUM(a);

COMPACTION_STATUS
COMPACTION_PRIORITY
/* COMPACTION_FEEDBACK are defines not enums. Not needed here. */
ZONE_TYPE
LRU_NAMES

/*
 * Now redefine the EM() and EMe() macros to map the enums to the strings
 * that will be printed in the output.
 */
#undef EM
#undef EMe
#define EM(a, b)	{a, b},
#define EMe(a, b)	{a, b}
