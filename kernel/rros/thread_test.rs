use crate::{
    thread, timer, clock,tick, 
    sched::{self, this_rros_rq}
};
use kernel::{bindings, prelude::*, c_str, mutex_init, sync::{Mutex, Lock, Guard}, c_types, };
use alloc::rc::Rc;
use core::cell::RefCell;

struct KthreadRunner {
    pub kthread: thread::RrosKthread
    // struct evl_kthread kthread;
	// struct evl_flag barrier;
	// ktime_t start_time;
	// struct latmus_runner runner;
}



impl KthreadRunner {
    pub fn new(kfn: fn()) -> Self{
        // let mut thread = unsafe{Mutex::new(thread::RrosKthread::new(kfn))};
        // let pinned = unsafe{Pin::new_unchecked(&mut thread)};

        // unsafe{Self { kthread: Arc::try_new(thread).unwrap()}}
        unsafe{Self { kthread: thread::RrosKthread::new(kfn)}}
    }
}

// static mut kthread_runner_1:Option<> = ;
static mut kthread_runner_1:Option<KthreadRunner> = None;
static mut kthread_runner_2:Option<KthreadRunner> = None;

pub fn test_thread_context_switch() {
    let rq = this_rros_rq();
    
    // let 
    
    let rq_len = unsafe{(*rq).fifo.runnable.head.clone().unwrap().len()};
    pr_info!("the init xxx length0 {}", rq_len);

    // unsafe{Arc::try_new(Mutex::new(rros_thread::new().unwrap())).unwrap()},
    unsafe{
        kthread_runner_1 = Some(KthreadRunner::new(kfn_1));
        let mut thread = Mutex::new(sched::rros_thread::new().unwrap());
        let pinned = Pin::new_unchecked(&mut thread);
        mutex_init!(pinned, "test_threads1");
        kthread_runner_1.as_mut().unwrap().kthread.thread =  Some(Arc::try_new(thread).unwrap());

        let mut r = Mutex::new(timer::RrosTimer::new(1));
        let pinned_r =  Pin::new_unchecked(&mut r);
        mutex_init!(pinned_r, "rtimer_1");
        
        let mut p = Mutex::new(timer::RrosTimer::new(1));
        let pinned_p =  Pin::new_unchecked(&mut p);
        mutex_init!(pinned_p, "ptimer_1");

        kthread_runner_1.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().rtimer = Some(Arc::try_new(r).unwrap());
        kthread_runner_1.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().ptimer = Some(Arc::try_new(p).unwrap());
        
        // let mut tmb = timer::rros_percpu_timers(&clock::RROS_MONO_CLOCK, 0);
        // kthread_runner_1.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().rtimer.as_mut().unwrap().lock().set_base(tmb);
        // kthread_runner_1.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().ptimer.as_mut().unwrap().lock().set_base(tmb);

        thread::rros_run_kthread(&mut kthread_runner_1.as_mut().unwrap().kthread, c_str!("hongyu1")) ;
    }
    
    let rq_len = unsafe{(*rq).fifo.runnable.head.clone().unwrap().len()};
    pr_info!("length1 {}", rq_len);

    // // let 
    unsafe{
        kthread_runner_2 = Some( KthreadRunner::new(kfn_2));
        let mut thread = Mutex::new(sched::rros_thread::new().unwrap());
        let pinned = Pin::new_unchecked(&mut thread);
        mutex_init!(pinned, "test_threads2");
        kthread_runner_2.as_mut().unwrap().kthread.thread =  Some(Arc::try_new(thread).unwrap());

        let mut r = Mutex::new(timer::RrosTimer::new(1));
        let pinned_r =  Pin::new_unchecked(&mut r);
        mutex_init!(pinned_r, "rtimer_2");
        
        let mut p = Mutex::new(timer::RrosTimer::new(1));
        let pinned_p =  Pin::new_unchecked(&mut p);
        mutex_init!(pinned_p, "ptimer_2");

        kthread_runner_2.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().rtimer = Some(Arc::try_new(r).unwrap());
        kthread_runner_2.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().ptimer = Some(Arc::try_new(p).unwrap());

        // let mut tmb = timer::rros_percpu_timers(&clock::RROS_MONO_CLOCK, 0);
        // kthread_runner_2.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().rtimer.as_mut().unwrap().lock().set_base(tmb);
        // kthread_runner_2.as_mut().unwrap().kthread.thread.as_mut().unwrap().lock().ptimer.as_mut().unwrap().lock().set_base(tmb);

        thread::rros_run_kthread(&mut kthread_runner_2.as_mut().unwrap().kthread, c_str!("hongyu2"));
    }

    let rq_len = unsafe{(*rq).fifo.runnable.head.clone().unwrap().len()};
    pr_info!("length2 {}", rq_len);

    // let mut a = 0 as u64;
    // while 1==1 {
    //     a += 1;
        // if a == 10000000000{
        //     break;
        // }
        // let a = 1;
        // pr_info!("good");
    // }
    // let mut RrosKthread1 = kthread::RrosKthread::new(fn1);
    // let mut RrosKthread2 = kthread::RrosKthread::new(fn2);
    // kthread::kthread_run(Some(threadfn), &mut RrosKthread1 as *mut kthread::RrosKthread as *mut c_types::c_void, c_str!("%s").as_char_ptr(),
    //  format_args!("hongyu1"));
    // kthread::kthread_run(Some(threadfn), &mut RrosKthread2 as *mut kthread::RrosKthread as *mut c_types::c_void, c_str!("%s").as_char_ptr(),
    //  format_args!("hongyu2"));
}

// unsafe extern "C" fn threadfn(arg: *mut c_types::c_void) -> c_types::c_int {
//     // let c;
//     // unsafe{
//     //     c = CStr::from_char_ptr(str as *const c_types::c_char ).as_bytes();
//     // }

//     // let a = str::from_utf8(c);
//     // match a {
//     //     Ok(hello_str)=>{
//     //         pr_info!("{}\n", hello_str);
//     //     },
//     //     Err(err) => {
//     //         pr_info!("{}", err);
//     //         return 0 as c_types::c_int;
//     //     },
//     // }
//     let mut kthread = arg as *mut kthread::RrosKthread;
// //////////////////////////////////////////////
//     unsafe {
//         bindings::dovetail_init_altsched(


//             &mut (*kthread).altsched as *mut bindings::dovetail_altsched_context,
//         )
//     };
//     unsafe{
//     let kthread_fn = (*kthread).kthread_fn;
//     match kthread_fn{
//         Some(a) => a(),
//         None => (),
//     }}
// //////////////////////////////////////////////
//     1 as c_types::c_int
// }




pub fn kfn_1() {
    while 1==1 {
        // unsafe{rust_helper_hard_local_irq_enable()};
        thread::rros_sleep(1000000000);
        unsafe{
            let mut tmb = timer::rros_this_cpu_timers(&clock::RROS_MONO_CLOCK);
            if (*tmb).q.is_empty() == true {
                // tick
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                pr_info!("empty/n");
                // tick::proxy_set_next_ktime(1000000, 0 as *mut bindings::clock_event_device);
            }
            // tick::proxy_set_next_ktime(1000000, 0 as *mut bindings::clock_event_device);
        }
        // unsafe{
        //     let this_rq = this_rros_rq();
        //     tick::rros_notify_proxy_tick(this_rq);
        // }
        // pr_info!("hello! from rros~~~~~~~~~~~~");
        pr_emerg!("hello! from rros~~~~~~~~~~~~");
        
    }
}

pub fn kfn_2() {
    while 1==1 {
        thread::rros_sleep(1000000000);
        pr_info!("world! from rros~~~~~~~~~~~~");
        
    }
}