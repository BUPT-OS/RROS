//stat.rs测试文件
//用于测试stat.rs里的函数正确性
use crate::factory;
use crate::list::*;
use crate::{
    clock::*, factory::RrosElement, factory::RrosFactory, factory::RustFile, lock::*, sched::*,
    timer::*, RROS_OOB_CPUS,
};
use core::borrow::{Borrow, BorrowMut};
use core::cell::RefCell;
use core::cell::UnsafeCell;
use core::ops::Deref;
use core::ops::DerefMut;
use core::{mem::align_of, mem::size_of, todo};
use kernel::{
    bindings, c_types, cpumask::CpumaskT, double_linked_list::*, file_operations::FileOperations,
    ktime::*, percpu, percpu_defs, prelude::*, premmpt, spinlock_init, str::CStr, sync::Guard,
    sync::Lock, sync::SpinLock, sysfs, timekeeping,
};

// CFG为!CONFIG_RROS_RUNSTATS未测试，理论可行
