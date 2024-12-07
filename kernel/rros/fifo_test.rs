use crate::{fifo::*, sched::*, thread::*, timer::*, RROS_OOB_CPUS};
use core::clone::Clone;
use core::ops::Deref;
use kernel::{c_str, cpumask::CpumaskT, prelude::*, new_spinlock, sync::Lock, sync::SpinLock};

#[allow(dead_code)]
pub fn test_init_thread(thread: Arc<Pin<Box<SpinLock<RrosThread>>>>, prio: i32) -> Result<usize> {
    let mut iattr = RrosInitThreadAttr::new();
    unsafe {
        iattr.affinity = &RROS_OOB_CPUS as *const CpumaskT;
        iattr.sched_class = Some(&RROS_SCHED_FIFO);
        let sched_param = Arc::try_new(Box::pin_init(new_spinlock!(RrosSchedParam::new())).unwrap())?;
        (*sched_param.locked_data().get()).fifo.prio = prio;
        (*sched_param.locked_data().get()).idle.prio = prio;
        (*sched_param.locked_data().get()).weak.prio = prio;
        iattr.sched_param = Some(sched_param);
    }
    rros_init_thread(&Some(thread), iattr, this_rros_rq(), c_str!("bw1"))?;
    Ok(0)
}

#[allow(dead_code)]
pub fn test_rros_enqueue_fifo_thread() -> Result<usize> {
    pr_debug!("~~~test_rros_enqueue_fifo_thread begin~~~");
    unsafe {
        let mut length;

        // create thread1
        let mut t1 = Box::pin_init(new_spinlock!(RrosThread::new().unwrap(),"create_thread1")).unwrap();
        // let pinned = Pin::new_unchecked(&mut t1);
        // spinlock_init!(pinned, "create_thread1");
        let thread1 = Arc::try_new(t1)?;

        let mut r1 = Box::pin_init(new_spinlock!(RrosTimer::new(1), "rtimer_1")).unwrap();
        // let pinned_r1 = Pin::new_unchecked(&mut r1);
        // spinlock_init!(pinned_r1, "rtimer_1");
        let mut p1 = Box::pin_init(new_spinlock!(RrosTimer::new(1), "ptimer_1")).unwrap();
        // let pinned_p = Pin::new_unchecked(&mut p1);
        // spinlock_init!(pinned_p, "ptimer_1");
        thread1.lock().rtimer = Some(Arc::try_new(r1).unwrap());
        thread1.lock().ptimer = Some(Arc::try_new(p1).unwrap());

        test_init_thread(thread1.clone(), 22)?;

        // create thread2
        let mut t2 = Box::pin_init(new_spinlock!(RrosThread::new().unwrap(), "create_thread1")).unwrap();
        // let pinned = Pin::new_unchecked(&mut t2);
        // spinlock_init!(pinned, "create_thread1");
        let thread2 = Arc::try_new(t2)?;

        let mut r2 = Box::pin_init(new_spinlock!(RrosTimer::new(1), "rtimer_2")).unwrap();
        // let pinned_r2 = Pin::new_unchecked(&mut r2);
        // spinlock_init!(pinned_r2, "rtimer_2");
        let mut p2 = Box::pin_init(new_spinlock!(RrosTimer::new(1), "ptimer_2")).unwrap();
        // let pinned_p = Pin::new_unchecked(&mut p2);
        // spinlock_init!(pinned_p, "ptimer_2");
        thread2.lock().rtimer = Some(Arc::try_new(r2).unwrap());
        thread2.lock().ptimer = Some(Arc::try_new(p2).unwrap());

        test_init_thread(thread2.clone(), 33)?;

        // // create thread3
        let mut t3 = Box::pin_init(new_spinlock!(RrosThread::new().unwrap(), "create_thread1")).unwrap();
        // let pinned = Pin::new_unchecked(&mut t3);
        // spinlock_init!(pinned, "create_thread1");
        let thread3 = Arc::try_new(t3)?;

        let mut r3 = Box::pin_init(new_spinlock!(RrosTimer::new(1), "rtimer_3")).unwrap();
        // let pinned_r3 = Pin::new_unchecked(&mut r3);
        // spinlock_init!(pinned_r3, "rtimer_3");
        let mut p3 = Box::pin_init(new_spinlock!(RrosTimer::new(1), "ptimer_3")).unwrap();
        // let pinned_p = Pin::new_unchecked(&mut p3);
        // spinlock_init!(pinned_p, "ptimer_3");
        thread3.lock().rtimer = Some(Arc::try_new(r3).unwrap());
        thread3.lock().ptimer = Some(Arc::try_new(p3).unwrap());

        test_init_thread(thread3.clone(), 44)?;

        let rq_ptr1;
        match thread1.lock().rq.clone() {
            Some(rq) => rq_ptr1 = rq,
            None => return Err(kernel::Error::EINVAL),
        }

        __rros_enqueue_fifo_thread(thread1.clone())?;

        length = (*rq_ptr1).fifo.runnable.head.clone().unwrap().len();
        pr_debug!("test_rros_enqueue_fifo_thread: length is  {}", length);

        __rros_enqueue_fifo_thread(thread2.clone())?;

        length = (*rq_ptr1).fifo.runnable.head.clone().unwrap().len();
        pr_debug!("test_rros_enqueue_fifo_thread: length is  {}", length);

        __rros_enqueue_fifo_thread(thread3.clone())?;

        length = (*rq_ptr1).fifo.runnable.head.clone().unwrap().len();
        pr_debug!("test_rros_enqueue_fifo_thread: length is  {}", length);
        pr_debug!("~~~test_rros_enqueue_fifo_thread end~~~");

        //__rros_dequeue_fifo_thread passed test.
        pr_debug!("~~~test_rros_dequeue_fifo_thread begin~~~");

        __rros_dequeue_fifo_thread(thread1.clone())?;
        length = (*rq_ptr1).fifo.runnable.head.clone().unwrap().len();
        pr_debug!("test_rros_enqueue_fifo_thread: length1 is  {}", length);

        __rros_dequeue_fifo_thread(thread2.clone())?;
        length = (*rq_ptr1).fifo.runnable.head.clone().unwrap().len();
        pr_debug!("test_rros_enqueue_fifo_thread: length2 is  {}", length);

        __rros_dequeue_fifo_thread(thread3.clone())?;
        length = (*rq_ptr1).fifo.runnable.head.clone().unwrap().len();
        pr_debug!("test_rros_enqueue_fifo_thread: length3 is  {}", length);

        pr_debug!("~~~test_rros_dequeue_fifo_thread end~~~");
    }
    Ok(0)
}

// TODO: Add a config for test functions.
#[allow(dead_code)]
pub fn test_rros_dequeue_fifo_thread() -> Result<usize> {
    Ok(0)
}
