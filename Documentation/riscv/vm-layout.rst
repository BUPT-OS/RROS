.. SPDX-License-Identifier: GPL-2.0

=====================================
Virtual Memory Layout on RISC-V Linux
=====================================

:Author: Alexandre Ghiti <alex@ghiti.fr>
:Date: 12 February 2021

This document describes the virtual memory layout used by the RISC-V Linux
Kernel.

RISC-V Linux Kernel 32bit
=========================

RISC-V Linux Kernel SV32
------------------------

TODO

RISC-V Linux Kernel 64bit
=========================

The RISC-V privileged architecture document states that the 64bit addresses
"must have bits 63–48 all equal to bit 47, or else a page-fault exception will
occur.": that splits the virtual address space into 2 halves separated by a very
big hole, the lower half is where the userspace resides, the upper half is where
the RISC-V Linux Kernel resides.

RISC-V Linux Kernel SV39
------------------------

::

  ========================================================================================================================
      Start addr    |   Offset   |     End addr     |  Size   | VM area description
  ========================================================================================================================
                    |            |                  |         |
   0000000000000000 |    0       | 0000003fffffffff |  256 GB | user-space virtual memory, different per mm
  __________________|____________|__________________|_________|___________________________________________________________
                    |            |                  |         |
   0000004000000000 | +256    GB | ffffffbfffffffff | ~16M TB | ... huge, almost 64 bits wide hole of non-canonical
                    |            |                  |         |     virtual memory addresses up to the -256 GB
                    |            |                  |         |     starting offset of kernel mappings.
  __________________|____________|__________________|_________|___________________________________________________________
                                                              |
                                                              | Kernel-space virtual memory, shared between all processes:
  ____________________________________________________________|___________________________________________________________
                    |            |                  |         |
   ffffffc6fea00000 | -228    GB | ffffffc6feffffff |    6 MB | fixmap
   ffffffc6ff000000 | -228    GB | ffffffc6ffffffff |   16 MB | PCI io
   ffffffc700000000 | -228    GB | ffffffc7ffffffff |    4 GB | vmemmap
   ffffffc800000000 | -224    GB | ffffffd7ffffffff |   64 GB | vmalloc/ioremap space
   ffffffd800000000 | -160    GB | fffffff6ffffffff |  124 GB | direct mapping of all physical memory
   fffffff700000000 |  -36    GB | fffffffeffffffff |   32 GB | kasan
  __________________|____________|__________________|_________|____________________________________________________________
                                                              |
                                                              |
  ____________________________________________________________|____________________________________________________________
                    |            |                  |         |
   ffffffff00000000 |   -4    GB | ffffffff7fffffff |    2 GB | modules, BPF
   ffffffff80000000 |   -2    GB | ffffffffffffffff |    2 GB | kernel
  __________________|____________|__________________|_________|____________________________________________________________


RISC-V Linux Kernel SV48
------------------------

::

 ========================================================================================================================
      Start addr    |   Offset   |     End addr     |  Size   | VM area description
 ========================================================================================================================
                    |            |                  |         |
   0000000000000000 |    0       | 00007fffffffffff |  128 TB | user-space virtual memory, different per mm
  __________________|____________|__________________|_________|___________________________________________________________
                    |            |                  |         |
   0000800000000000 | +128    TB | ffff7fffffffffff | ~16M TB | ... huge, almost 64 bits wide hole of non-canonical
                    |            |                  |         | virtual memory addresses up to the -128 TB
                    |            |                  |         | starting offset of kernel mappings.
  __________________|____________|__________________|_________|___________________________________________________________
                                                              |
                                                              | Kernel-space virtual memory, shared between all processes:
  ____________________________________________________________|___________________________________________________________
                    |            |                  |         |
   ffff8d7ffea00000 |  -114.5 TB | ffff8d7ffeffffff |    6 MB | fixmap
   ffff8d7fff000000 |  -114.5 TB | ffff8d7fffffffff |   16 MB | PCI io
   ffff8d8000000000 |  -114.5 TB | ffff8f7fffffffff |    2 TB | vmemmap
   ffff8f8000000000 |  -112.5 TB | ffffaf7fffffffff |   32 TB | vmalloc/ioremap space
   ffffaf8000000000 |  -80.5  TB | ffffef7fffffffff |   64 TB | direct mapping of all physical memory
   ffffef8000000000 |  -16.5  TB | fffffffeffffffff | 16.5 TB | kasan
  __________________|____________|__________________|_________|____________________________________________________________
                                                              |
                                                              | Identical layout to the 39-bit one from here on:
  ____________________________________________________________|____________________________________________________________
                    |            |                  |         |
   ffffffff00000000 |   -4    GB | ffffffff7fffffff |    2 GB | modules, BPF
   ffffffff80000000 |   -2    GB | ffffffffffffffff |    2 GB | kernel
  __________________|____________|__________________|_________|____________________________________________________________


RISC-V Linux Kernel SV57
------------------------

::

 ========================================================================================================================
      Start addr    |   Offset   |     End addr     |  Size   | VM area description
 ========================================================================================================================
                    |            |                  |         |
   0000000000000000 |   0        | 00ffffffffffffff |   64 PB | user-space virtual memory, different per mm
  __________________|____________|__________________|_________|___________________________________________________________
                    |            |                  |         |
   0100000000000000 | +64     PB | feffffffffffffff | ~16K PB | ... huge, almost 64 bits wide hole of non-canonical
                    |            |                  |         | virtual memory addresses up to the -64 PB
                    |            |                  |         | starting offset of kernel mappings.
  __________________|____________|__________________|_________|___________________________________________________________
                                                              |
                                                              | Kernel-space virtual memory, shared between all processes:
  ____________________________________________________________|___________________________________________________________
                    |            |                  |         |
   ff1bfffffea00000 | -57     PB | ff1bfffffeffffff |    6 MB | fixmap
   ff1bffffff000000 | -57     PB | ff1bffffffffffff |   16 MB | PCI io
   ff1c000000000000 | -57     PB | ff1fffffffffffff |    1 PB | vmemmap
   ff20000000000000 | -56     PB | ff5fffffffffffff |   16 PB | vmalloc/ioremap space
   ff60000000000000 | -40     PB | ffdeffffffffffff |   32 PB | direct mapping of all physical memory
   ffdf000000000000 |  -8     PB | fffffffeffffffff |    8 PB | kasan
  __________________|____________|__________________|_________|____________________________________________________________
                                                              |
                                                              | Identical layout to the 39-bit one from here on:
  ____________________________________________________________|____________________________________________________________
                    |            |                  |         |
   ffffffff00000000 |  -4     GB | ffffffff7fffffff |    2 GB | modules, BPF
   ffffffff80000000 |  -2     GB | ffffffffffffffff |    2 GB | kernel
  __________________|____________|__________________|_________|____________________________________________________________


Userspace VAs
--------------------
To maintain compatibility with software that relies on the VA space with a
maximum of 48 bits the kernel will, by default, return virtual addresses to
userspace from a 48-bit range (sv48). This default behavior is achieved by
passing 0 into the hint address parameter of mmap. On CPUs with an address space
smaller than sv48, the CPU maximum supported address space will be the default.

Software can "opt-in" to receiving VAs from another VA space by providing
a hint address to mmap. A hint address passed to mmap will cause the largest
address space that fits entirely into the hint to be used, unless there is no
space left in the address space. If there is no space available in the requested
address space, an address in the next smallest available address space will be
returned.

For example, in order to obtain 48-bit VA space, a hint address greater than
:code:`1 << 47` must be provided. Note that this is 47 due to sv48 userspace
ending at :code:`1 << 47` and the addresses beyond this are reserved for the
kernel. Similarly, to obtain 57-bit VA space addresses, a hint address greater
than or equal to :code:`1 << 56` must be provided.
