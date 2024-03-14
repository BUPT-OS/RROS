#
# gdb helper commands and functions for Linux kernel debugging
#
#  loader module
#
# Copyright (c) Siemens AG, 2012, 2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + "/scripts/gdb")

try:
    gdb.parse_and_eval("0")
    gdb.execute("", to_string=True)
except:
    gdb.write("NOTE: gdb 7.2 or later required for Linux helper scripts to "
              "work.\n")
else:
    import linux.constants
    if linux.constants.LX_CONFIG_DEBUG_INFO_REDUCED:
        raise gdb.GdbError("Reduced debug information will prevent GDB "
                           "from having complete types.\n")
    import linux.utils
    import linux.symbols
    import linux.modules
    import linux.dmesg
    import linux.tasks
    import linux.config
    import linux.cpus
    import linux.lists
    import linux.rbtree
    import linux.proc
    import linux.timerlist
    import linux.clk
    import linux.genpd
    import linux.device
    import linux.vfs
    import linux.pgtable
    import linux.radixtree
    import linux.interrupts
    import linux.mm
    import linux.stackdepot
    import linux.page_owner
    import linux.slab
    import linux.vmalloc
