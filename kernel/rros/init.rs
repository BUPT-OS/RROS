#![no_std]
#![feature(allocator_api, global_asm)]
use alloc::vec;
use kernel::{cpumask, irqstage, prelude::*, str::CStr,
     ThisModule, c_str, chrdev, file_operations::FileOperations,
    kthread, c_types, dovetail, bindings, percpu};

use core::str;
use core::sync::atomic::{AtomicU8, Ordering};
use core::{mem::size_of, todo, mem::align_of};

mod idle;
mod queue;
mod sched;
use sched::rros_init_sched;
mod thread;
// mod weak;
mod fifo;
mod tick;
use tick::rros_enable_tick;

mod timeout;
mod stat;

mod clock;
mod clock_test;
use clock::rros_clock_init;
#[macro_use]
mod list;
mod list_test;
mod lock;
mod memory;
mod monitor;
mod timer;
mod timer_test;
mod thread_test;
mod test;
mod double_linked_list_test;
use memory::init_memory;
mod factory;
use factory::rros_early_init_factories;

use crate::sched::this_rros_rq;
mod file;
mod crossing;
mod wait;
#[cfg(CONFIG_NET)]
mod net;
module! {
    type: Rros,
    name: b"rros",
    author: b"Hongyu Li",
    description: b"A rust realtime os",
    license: b"GPL v2",
    params: {
        oobcpus_arg: str {
            default: b"0\0",
            permissions: 0o444,
            description: b"which cpus in the oob",
        },
        init_state_arg: str {
            default: b"enabled",
            permissions: 0o444,
            description: b"inital state of rros",
        },
        sysheap_size_arg: u32{
            default: 0,
            permissions: 0o444,
            description: b"system heap size",
        },
    },
}

pub struct RrosMachineCpuData{}

pub static mut RROS_MACHINE_CPUDATA: *mut RrosMachineCpuData = 0 as *mut RrosMachineCpuData;

enum RrosRunStates {
    RrosStateDisabled = 1,
    RrosStateRunning = 2,
    RrosStateStopped = 3,
    RrosStateTeardown = 4,
    RrosStateWarmup = 5,
}
struct Rros
{
    factory: Pin<Box<chrdev::Registration<{factory::NR_FACTORIES}>>>,
}

struct InitState {
    label: &'static str,
    state: RrosRunStates,
}

static RUN_FLAG: AtomicU8 = AtomicU8::new(0);
static SCHED_FLAG: AtomicU8 = AtomicU8::new(0);
// static RUN_FLAG: AtomicU8 = AtomicU8::new(0);
static RROS_RUNSTATE: AtomicU8 = AtomicU8::new(RrosRunStates::RrosStateWarmup as u8);
static mut RROS_OOB_CPUS: cpumask::CpumaskT = cpumask::CpumaskT::from_int(1 as u64);

fn setup_init_state(init_state: &'static str) {
    let warn_bad_state: &str = "invalid init state '{}'\n";

    let init_states: [InitState; 3] = [
        InitState {
            label: "disabled",
            state: RrosRunStates::RrosStateDisabled,
        },
        InitState {
            label: "stopped",
            state: RrosRunStates::RrosStateStopped,
        },
        InitState {
            label: "enabled",
            state: RrosRunStates::RrosStateWarmup,
        },
    ];

    for InitState in init_states {
        if InitState.label == init_state {
            set_rros_state(InitState.state);
            pr_info!("{}", init_state);
            return;
        }
    }
    pr_warn!("{} {}", warn_bad_state, init_state);
}

fn set_rros_state(state: RrosRunStates) {
    RROS_RUNSTATE.store(state as u8, Ordering::Relaxed);
}

fn init_core() -> Result<Pin<Box<chrdev::Registration<{factory::NR_FACTORIES}>>>> {
    let res =
        irqstage::enable_oob_stage(CStr::from_bytes_with_nul("rros\0".as_bytes())?.as_char_ptr());
    pr_info!("hello");
    match res {
        Ok(_o) => (),
        Err(_e) => {
            pr_warn!("rros cannot be enabled");
            return Err(kernel::Error::EINVAL);
        }
    }
    pr_info!("hella");
    // let res = init_memory(*sysheap_size_arg.read());
    // match res {
    //     Ok(_o) => (),
    //     Err(_e) => {
    //         pr_warn!("memory init wrong");
    //         return Err(_e);
    //     }
    // }
    let res = rros_early_init_factories(&THIS_MODULE);
    let fac_reg;
    match res {
        Ok(_o) => fac_reg = _o,
        Err(_e) => {
            pr_warn!("factory init wrong");
            return Err(_e);
        }
    }
    pr_info!("haly");
    
    let res = rros_clock_init();
    match res {
        Ok(_o) => (),
        Err(_e) => {
            pr_warn!("clock init wrong");
            return Err(_e);
        }
    }
    
    let res = rros_init_sched();
    match res {
        Ok(_o) => (),
        Err(_e) => {
            pr_warn!("sched init wrong");
            return Err(_e);
        }
    }

    let rq = this_rros_rq();
    pr_info!("rq add is {:p}", this_rros_rq());
    let res = rros_enable_tick();
    match res {
        Ok(_o) => (),
        Err(_e) => {
            pr_warn!("tick enable wrong");
            return Err(_e);
        }
    }

    let res = dovetail::dovetail_start();
    match res {
        Ok(_o) => (),
        Err(_e) => {
            pr_warn!("dovetail start wrong");
        }
    }
    
    Ok(fac_reg)
}

fn test_clock() {
    //clock_test::test_do_clock_tick();
    //clock_test::test_adjust_timer();
    clock_test::test_rros_adjust_timers();
    //clock_test::test_rros_stop_timers();
}

fn test_timer() {
    // timer_test::test_timer_at_front();
    // timer_test::test_rros_timer_deactivate();
    // timer_test::test_rros_get_timer_gravity();
    // timer_test::test_rros_update_timer_date();
    // timer_test::test_rros_get_timer_next_date();
    // timer_test::test_rros_get_timer_expiry();
    //timer_test::test___rros_get_timer_delta();
    //timer_test::test_rros_get_timer_delta();
    //timer_test::test_rros_get_timer_date();
    //timer_test::test_rros_insert_tnode();
    //timer_test::test_rros_enqueue_timer();
    // timer_test::test_program_timer();
    // timer_test::test_rros_start_timer();
    // timer_test::test_stop_timer_locked();
    // timer_test::test_rros_destroy_timer();
    //timer_test::test_get_handler();
}

fn test_double_linked_list() {
    double_linked_list_test::test_enqueue_by_index();
}

fn test_thread() {
    thread_test::test_thread_context_switch();
}

impl KernelModule for Rros {
    fn init() -> Result<Self> {
        pr_info!("Hello world from rros!\n");
        let init_state_arg_str = str::from_utf8(init_state_arg.read())?;
        setup_init_state(init_state_arg_str);

        if RROS_RUNSTATE.load(Ordering::Relaxed) != RrosRunStates::RrosStateWarmup as u8 {
            pr_warn!("disabled on kernel command line\n");
            return Err(kernel::Error::EINVAL);
        }

        let cpu_online_mask = unsafe { cpumask::read_cpu_online_mask() };
        //size_of 为0，align_of为4，alloc报错
        // unsafe {RROS_MACHINE_CPUDATA = 
        //     percpu::alloc_per_cpu(size_of::<RrosMachineCpuData>() as usize, 
        //                   align_of::<RrosMachineCpuData>() as usize) as *mut RrosMachineCpuData};
        unsafe {RROS_MACHINE_CPUDATA = 
            percpu::alloc_per_cpu(4 as usize, 4 as usize) as *mut RrosMachineCpuData};
        if str::from_utf8(oobcpus_arg.read())? != "" {
            let res = unsafe {
                cpumask::cpulist_parse(
                    CStr::from_bytes_with_nul(oobcpus_arg.read())?.as_char_ptr(),
                    RROS_OOB_CPUS.as_cpumas_ptr(),
                )
            };
            match res {
                Ok(_o) => (pr_info!("load parameters {}\n", str::from_utf8(oobcpus_arg.read())?)),
                Err(_e) => {
                    pr_warn!("wrong oobcpus_arg");
                    unsafe {
                        cpumask::cpumask_copy(RROS_OOB_CPUS.as_cpumas_ptr(), &cpu_online_mask);
                    }
                }
            }
        } else {
            unsafe {
                cpumask::cpumask_copy(RROS_OOB_CPUS.as_cpumas_ptr(), &cpu_online_mask);
            }
        }

        let res = init_core(); //*sysheap_size_arg.read()
        let fac_reg;

        // test_timer();
        // test_double_linked_list();
        
        // test_clock();
        // test_thread();
        //test_double_linked_list();
        match res {
            Ok(_o) => {
                pr_info!("Success boot the rros.");
                fac_reg = _o;
            },
            Err(_e) => {
                pr_warn!("Boot failed!\n");
                return Err(_e);
            }
        }

        // let mut evl_kthread1 = evl_kthread::new(fn1);
        // let mut evl_kthread2 = evl_kthread::new(fn2);
        // pr_info!("before thread 1");
        // kthread::kthread_run(Some(threadfn), &mut evl_kthread1 as *mut evl_kthread as *mut c_types::c_void, c_str!("%s").as_char_ptr(),
        //  format_args!("hongyu1"));

        //  pr_info!("between 1 and 2");

        // kthread::kthread_run(Some(threadfn), &mut evl_kthread2 as *mut evl_kthread as *mut c_types::c_void, c_str!("%s").as_char_ptr(),
        //  format_args!("hongyu2"));

        Ok(Rros{factory: fac_reg})
    }
}

#[no_mangle]
unsafe extern "C" fn helloworld() {
    pr_info!("hello world! from C to rust");
}

impl Drop for Rros {
    fn drop(&mut self) {
        pr_info!("Bye world from rros!\n");
    }
}
