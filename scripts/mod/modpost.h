/* SPDX-License-Identifier: GPL-2.0 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>

#include "list.h"
#include "elfconfig.h"

/* On BSD-alike OSes elf.h defines these according to host's word size */
#undef ELF_ST_BIND
#undef ELF_ST_TYPE
#undef ELF_R_SYM
#undef ELF_R_TYPE

#if KERNEL_ELFCLASS == ELFCLASS32

#define Elf_Ehdr    Elf32_Ehdr
#define Elf_Shdr    Elf32_Shdr
#define Elf_Sym     Elf32_Sym
#define Elf_Addr    Elf32_Addr
#define Elf_Section Elf32_Half
#define ELF_ST_BIND ELF32_ST_BIND
#define ELF_ST_TYPE ELF32_ST_TYPE

#define Elf_Rel     Elf32_Rel
#define Elf_Rela    Elf32_Rela
#define ELF_R_SYM   ELF32_R_SYM
#define ELF_R_TYPE  ELF32_R_TYPE
#else

#define Elf_Ehdr    Elf64_Ehdr
#define Elf_Shdr    Elf64_Shdr
#define Elf_Sym     Elf64_Sym
#define Elf_Addr    Elf64_Addr
#define Elf_Section Elf64_Half
#define ELF_ST_BIND ELF64_ST_BIND
#define ELF_ST_TYPE ELF64_ST_TYPE

#define Elf_Rel     Elf64_Rel
#define Elf_Rela    Elf64_Rela
#define ELF_R_SYM   ELF64_R_SYM
#define ELF_R_TYPE  ELF64_R_TYPE
#endif

#if KERNEL_ELFDATA != HOST_ELFDATA

static inline void __endian(const void *src, void *dest, unsigned int size)
{
	unsigned int i;
	for (i = 0; i < size; i++)
		((unsigned char*)dest)[i] = ((unsigned char*)src)[size - i-1];
}

#define TO_NATIVE(x)						\
({								\
	typeof(x) __x;						\
	__endian(&(x), &(__x), sizeof(__x));			\
	__x;							\
})

#else /* endianness matches */

#define TO_NATIVE(x) (x)

#endif

#define NOFAIL(ptr)   do_nofail((ptr), #ptr)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

void *do_nofail(void *ptr, const char *expr);

struct buffer {
	char *p;
	int pos;
	int size;
};

void __attribute__((format(printf, 2, 3)))
buf_printf(struct buffer *buf, const char *fmt, ...);

void
buf_write(struct buffer *buf, const char *s, int len);

struct module {
	struct list_head list;
	struct list_head exported_symbols;
	struct list_head unresolved_symbols;
	bool is_gpl_compatible;
	bool from_dump;		/* true if module was loaded from *.symvers */
	bool is_vmlinux;
	bool seen;
	bool has_init;
	bool has_cleanup;
	struct buffer dev_table_buf;
	char	     srcversion[25];
	// Missing namespace dependencies
	struct list_head missing_namespaces;
	// Actual imported namespaces
	struct list_head imported_namespaces;
	char name[];
};

struct elf_info {
	size_t size;
	Elf_Ehdr     *hdr;
	Elf_Shdr     *sechdrs;
	Elf_Sym      *symtab_start;
	Elf_Sym      *symtab_stop;
	unsigned int export_symbol_secndx;	/* .export_symbol section */
	char         *strtab;
	char	     *modinfo;
	unsigned int modinfo_len;

	/* support for 32bit section numbers */

	unsigned int num_sections; /* max_secindex + 1 */
	unsigned int secindex_strings;
	/* if Nth symbol table entry has .st_shndx = SHN_XINDEX,
	 * take shndx from symtab_shndx_start[N] instead */
	Elf32_Word   *symtab_shndx_start;
	Elf32_Word   *symtab_shndx_stop;
};

/* Accessor for sym->st_shndx, hides ugliness of "64k sections" */
static inline unsigned int get_secindex(const struct elf_info *info,
					const Elf_Sym *sym)
{
	unsigned int index = sym->st_shndx;

	/*
	 * Elf{32,64}_Sym::st_shndx is 2 byte. Big section numbers are available
	 * in the .symtab_shndx section.
	 */
	if (index == SHN_XINDEX)
		return info->symtab_shndx_start[sym - info->symtab_start];

	/*
	 * Move reserved section indices SHN_LORESERVE..SHN_HIRESERVE out of
	 * the way to UINT_MAX-255..UINT_MAX, to avoid conflicting with real
	 * section indices.
	 */
	if (index >= SHN_LORESERVE && index <= SHN_HIRESERVE)
		return index - SHN_HIRESERVE - 1;

	return index;
}

/* file2alias.c */
void handle_moddevtable(struct module *mod, struct elf_info *info,
			Elf_Sym *sym, const char *symname);
void add_moddevtable(struct buffer *buf, struct module *mod);

/* sumversion.c */
void get_src_version(const char *modname, char sum[], unsigned sumlen);

/* from modpost.c */
char *read_text_file(const char *filename);
char *get_line(char **stringp);
void *sym_get_data(const struct elf_info *info, const Elf_Sym *sym);

enum loglevel {
	LOG_WARN,
	LOG_ERROR,
	LOG_FATAL
};

void modpost_log(enum loglevel loglevel, const char *fmt, ...);

/*
 * warn - show the given message, then let modpost continue running, still
 *        allowing modpost to exit successfully. This should be used when
 *        we still allow to generate vmlinux and modules.
 *
 * error - show the given message, then let modpost continue running, but fail
 *         in the end. This should be used when we should stop building vmlinux
 *         or modules, but we can continue running modpost to catch as many
 *         issues as possible.
 *
 * fatal - show the given message, and bail out immediately. This should be
 *         used when there is no point to continue running modpost.
 */
#define warn(fmt, args...)	modpost_log(LOG_WARN, fmt, ##args)
#define error(fmt, args...)	modpost_log(LOG_ERROR, fmt, ##args)
#define fatal(fmt, args...)	modpost_log(LOG_FATAL, fmt, ##args)
