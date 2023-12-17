//stat.rs测试文件
//用于测试stat.rs里的函数正确性
use crate::{
    clock::*, factory::RrosElement, factory::RrosFactory, factory::RustFile, lock::*, sched::*,
    timer::*, RROS_OOB_CPUS, factory, list::*,
};
use core::{
    borrow::{Borrow, BorrowMut},
    cell::{RefCell, UnsafeCell},
    ops::{Deref, DerefMut},
    mem::{align_of, size_of},
    todo,
};
use kernel::{
    bindings, c_types, cpumask::CpumaskT, double_linked_list::*, file_operations::FileOperations,
    ktime::*, percpu, percpu_defs, prelude::*, premmpt, spinlock_init, str::CStr, sync::Guard,
    sync::Lock, sync::SpinLock, sysfs, timekeeping,
};

// CFG为!CONFIG_RROS_RUNSTATS未测试，理论可行
