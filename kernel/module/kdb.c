// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Module kdb support
 *
 * Copyright (C) 2010 Jason Wessel
 */

#include <linux/module.h>
#include <linux/kdb.h>
#include "internal.h"

/*
 * kdb_lsmod - This function implements the 'lsmod' command.  Lists
 *	currently loaded kernel modules.
 *	Mostly taken from userland lsmod.
 */
int kdb_lsmod(int argc, const char **argv)
{
	struct module *mod;

	if (argc != 0)
		return KDB_ARGCOUNT;

	kdb_printf("Module                  Size  modstruct     Used by\n");
	list_for_each_entry(mod, &modules, list) {
		if (mod->state == MODULE_STATE_UNFORMED)
			continue;

		kdb_printf("%-20s%8u", mod->name, mod->mem[MOD_TEXT].size);
		kdb_printf("/%8u", mod->mem[MOD_RODATA].size);
		kdb_printf("/%8u", mod->mem[MOD_RO_AFTER_INIT].size);
		kdb_printf("/%8u", mod->mem[MOD_DATA].size);

		kdb_printf("  0x%px ", (void *)mod);
#ifdef CONFIG_MODULE_UNLOAD
		kdb_printf("%4d ", module_refcount(mod));
#endif
		if (mod->state == MODULE_STATE_GOING)
			kdb_printf(" (Unloading)");
		else if (mod->state == MODULE_STATE_COMING)
			kdb_printf(" (Loading)");
		else
			kdb_printf(" (Live)");
		kdb_printf(" 0x%px", mod->mem[MOD_TEXT].base);
		kdb_printf("/0x%px", mod->mem[MOD_RODATA].base);
		kdb_printf("/0x%px", mod->mem[MOD_RO_AFTER_INIT].base);
		kdb_printf("/0x%px", mod->mem[MOD_DATA].base);

#ifdef CONFIG_MODULE_UNLOAD
		{
			struct module_use *use;

			kdb_printf(" [ ");
			list_for_each_entry(use, &mod->source_list,
					    source_list)
				kdb_printf("%s ", use->target->name);
			kdb_printf("]\n");
		}
#endif
	}

	return 0;
}
