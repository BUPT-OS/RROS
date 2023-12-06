use kernel::{
    bindings, c_types, prelude::*, ktime,str::CStr, c_str,double_linked_list::*,sync::{SpinLock, Lock, Guard},memory_rros::*, spinlock_init,};
use crate::{
    sched::*,
    clock::*,
	thread::*,
    timer::*,
    timeout::*,
    fifo::*,
};
use core::ptr::NonNull;
use core::mem::size_of;

pub static mut rros_sched_tp:rros_sched_class = rros_sched_class{
    sched_init:	Some(tp_init),
    sched_enqueue:	Some(tp_enqueue),
    sched_dequeue:	Some(tp_dequeue),
    sched_requeue:	Some(tp_requeue),
    sched_pick:	Some(tp_pick),
    sched_migrate:	Some(tp_migrate),
    sched_chkparam:	Some(tp_chkparam),
    sched_setparam:	Some(tp_setparam),
    sched_getparam:	Some(tp_getparam),
    sched_trackprio: Some(tp_trackprio),
    sched_ceilprio:	Some(tp_ceilprio),
    sched_declare:	Some(tp_declare),
    sched_forget:	Some(tp_forget),
    sched_show:	Some(tp_show),
    sched_control:	Some(tp_control),
    sched_kick: None,
    sched_tick: None,
    nthreads: 0,
    next: 0 as *mut rros_sched_class,
    weight: 3 * RROS_CLASS_WEIGHT_FACTOR,
    policy:	SCHED_TP,
    name:	"tp",
    flag:4,
};

pub const CONFIG_RROS_SCHED_TP_NR_PART: i32 = 5;// 先默认设置为5
pub const RROS_TP_MAX_PRIO:i32  = RROS_FIFO_MAX_PRIO;
pub const RROS_TP_MIN_PRIO:i32 = RROS_FIFO_MIN_PRIO;
pub const RROS_TP_NR_PRIO:i32 =	RROS_TP_MAX_PRIO - RROS_TP_MIN_PRIO + 1;

type ktime_t = i64;

extern "C"{
    fn rust_helper_timespec64_to_ktime(ts:bindings::timespec64) -> ktime_t;
}


pub struct rros_tp_rq {
    pub runnable: rros_sched_queue,
}
impl rros_tp_rq{
    pub fn new() -> Result<Self>{
        Ok(rros_tp_rq { 
            runnable: rros_sched_queue::new()?,
        })
    }
}

pub struct rros_tp_window {
	pub w_offset:ktime_t,
	pub w_part:i32,
}

pub struct rros_tp_schedule {
	pub pwin_nr:i32,
	pub tf_duration:ktime_t,
	pub refcount:*mut bindings::atomic_t,
	pub pwins:[rros_tp_window;1 as usize],
}

pub struct Rros_sched_tp {
	pub partitions:Option<[rros_tp_rq; CONFIG_RROS_SCHED_TP_NR_PART as usize]>,
	pub idle:rros_tp_rq,
	pub tps:*mut rros_tp_rq,
	pub tf_timer:Option<Arc<SpinLock<RrosTimer>>>,
	pub gps:*mut rros_tp_schedule,
	pub wnext:i32,
	pub tf_start:ktime_t,
	pub threads:Option<List<Arc<SpinLock<rros_thread>>>>,
}
impl Rros_sched_tp{
    pub fn new() -> Result<Self>{
        unsafe{
        Ok(Rros_sched_tp { 
            partitions: None, 
            idle: rros_tp_rq::new()?, 
            tps: 0 as *mut rros_tp_rq,
            tf_timer: None, 
            gps: 0 as *mut rros_tp_schedule, 
            wnext: 0, 
            tf_start: 0, 
            threads: None,
        })
        }
    }
}

pub fn tp_schedule_next(tp:&mut Rros_sched_tp) -> Result<usize>{
    let mut w  = 0 as *mut rros_tp_window;
    let mut rq = 0 as *mut rros_rq;
    let mut t:ktime_t = 0; 
    let mut now:ktime_t = 0;
    let mut p_next:i32 = 0;

    rq = kernel::container_of!(tp as *mut Rros_sched_tp, rros_rq, tp) as *mut rros_rq;
    // assert_hard_lock(&rq->lock);
    unsafe{
        w = &mut ((*tp.gps).pwins[tp.wnext as usize]) as *mut rros_tp_window;
        p_next = (*w).w_part;
        if p_next < 0{
            tp.tps = &mut tp.idle as *mut rros_tp_rq;
        }else{
            tp.tps = &mut tp.partitions.as_mut().unwrap()[p_next as usize] as *mut rros_tp_rq;
        }
        tp.wnext = (tp.wnext + 1) % (*tp.gps).pwin_nr;
        w = &mut (*tp.gps).pwins[tp.wnext as usize] as *mut rros_tp_window;
        t = ktime::ktime_add(tp.tf_start, (*w).w_offset);
    }

    loop{
        unsafe{
            now = rros_read_clock(&RROS_MONO_CLOCK);
            if ktime::ktime_compare(now, t) <= 0{
                break;
            }
            t = ktime::ktime_add(tp.tf_start, (*tp.gps).tf_duration);
            tp.tf_start = t;
            tp.wnext = 0;
        }
    }

    unsafe{rros_start_timer(tp.tf_timer.as_mut().unwrap().clone(), t, RROS_INFINITE)};
    rros_set_resched(Some(rq));
    Ok(0)
}

pub fn tp_tick_handler(timer:*mut RrosTimer){
    unsafe{
        // 这里的container_of有问题
        let rq = kernel::container_of!(timer, rros_rq, tp.tf_timer) as *mut rros_rq;
        let mut tp = &mut (*rq).tp;

        // raw_spin_lock(&rq->lock);
        if tp.wnext + 1 == (*tp.gps).pwin_nr{
            tp.tf_start = ktime::ktime_add(tp.tf_start, (*tp.gps).tf_duration);
        }
        tp_schedule_next(tp);

        // raw_spin_unlock(&rq->lock);
    }
}

pub fn tp_init(rq:*mut rros_rq) -> Result<usize>{
    unsafe{
        let mut tp = &mut (*rq).tp;
        let r1 = rros_tp_rq::new()?;
        let r2 = rros_tp_rq::new()?;
        let r3 = rros_tp_rq::new()?;
        let r4 = rros_tp_rq::new()?;
        let r5 = rros_tp_rq::new()?;
        let mut temp :[rros_tp_rq;CONFIG_RROS_SCHED_TP_NR_PART as usize] = [r1,r2,r3,r4,r5];
        for n in 0..CONFIG_RROS_SCHED_TP_NR_PART{
            // temp[n as usize].runnable.head = Some(List::new(Arc::try_new(SpinLock::new(rros_thread::new()?))?));
            let mut tmp = Arc::<SpinLock<rros_thread>>::try_new_uninit()?;
            let mut tmp = unsafe{
                core::ptr::write_bytes(Arc::get_mut_unchecked(&mut tmp), 0, 1);
                tmp.assume_init()
            };
            let pinned = unsafe{
               Pin::new_unchecked(Arc::get_mut_unchecked(&mut tmp))
            };
            spinlock_init!(pinned,"tp kthread");

            // let mut thread = SpinLock::new(rros_thread::new()?);
            // let pinned = Pin::new_unchecked(&mut thread);
            // spinlock_init!(pinned, "rros_threads");
            // Arc::get_mut(&mut tmp).unwrap().write(thread);
            temp[n as usize].runnable.head =  Some(List::new(tmp));//Arc::try_new(thread)?
            (*temp[n as usize].runnable.head.as_mut().unwrap().head.value.locked_data().get()).init()?;
            // let pinned = Pin::new_unchecked(&mut *(Arc::into_raw( temp[n as usize].runnable.head.as_mut().unwrap().head.value.clone()) as *mut SpinLock<rros_thread>));
            // // &mut *Arc::into_raw( *(*rq_ptr).root_thread.clone().as_mut().unwrap()) as &mut SpinLock<rros_thread>
            // spinlock_init!(pinned, "rros_threads");
        }
        tp.partitions = Some(temp);
        tp.tps = 0 as *mut rros_tp_rq;
        tp.gps = 0 as *mut rros_tp_schedule;
        tp.tf_timer = Some(Arc::try_new(SpinLock::new(RrosTimer::new(0)))?);

        unsafe {
            let mut tf_timer = SpinLock::new(RrosTimer::new(2));
            let pinned_p =  Pin::new_unchecked(&mut tf_timer);
            spinlock_init!(pinned_p, "ptimer");
            tp.tf_timer = Some(Arc::try_new(tf_timer)?);
        }

        rros_init_timer_on_rq(tp.tf_timer.clone().as_mut().unwrap().clone(), 
            &mut RROS_MONO_CLOCK, Some(tp_tick_handler),
            rq, c_str!("[tp-tick]"),RROS_TIMER_IGRAVITY);
        // rros_set_timer_name(&tp->tf_timer, "[tp-tick]");
        pr_info!("tp_init ok");
        Ok(0)
    }
}

pub fn tp_setparam(thread: Option<Arc<SpinLock<rros_thread>>>,p:Option<Arc<SpinLock<rros_sched_param>>>) -> Result<usize>{
    unsafe{
        let thread_clone = thread.clone();
        let thread_clone = thread_clone.unwrap();
        let rq = (*thread_clone.locked_data().get()).rq.unwrap();
        let p = p.unwrap();
        (*thread_clone.locked_data().get()).tps = &mut (*rq).tp.partitions.as_mut().unwrap()[(*p.locked_data().get()).tp.ptid as usize] as *mut rros_tp_rq;
        (*thread_clone.locked_data().get()).state &= !T_WEAK;
        let prio = (*p.locked_data().get()).tp.prio;
        return rros_set_effective_thread_priority(thread.clone(), prio);
        pr_info!("tp_setparam success!");
    }
}

pub fn tp_getparam(thread: Option<Arc<SpinLock<rros_thread>>>, p: Option<Arc<SpinLock<rros_sched_param>>>){
    let thread = thread.unwrap();
    let p = p.unwrap();
    unsafe{
        (*p.locked_data().get()).tp.prio = (*thread.locked_data().get()).cprio;
        let p1 = (*thread.locked_data().get()).tps;
        let p2 = &mut (*(*thread.locked_data().get()).rq.unwrap()).tp.partitions.as_mut().unwrap()[0] as *mut rros_tp_rq;
        (*p.locked_data().get()).tp.ptid = p1.offset_from(p2) as i32;
    }
}

pub fn tp_trackprio(thread: Option<Arc<SpinLock<rros_thread>>>, p: Option<Arc<SpinLock<rros_sched_param>>>){
    let thread = thread.unwrap();
    unsafe{
        if p.is_some(){
            // RROS_WARN_ON(CORE,
            //     thread->base_class == &rros_sched_tp &&
            //     thread->tps - rros_thread_rq(thread)->tp.partitions
            //     != p->tp.ptid);
            let p = p.unwrap();
            (*thread.locked_data().get()).cprio = (*p.locked_data().get()).tp.prio;
        } else{
            (*thread.locked_data().get()).cprio = (*thread.locked_data().get()).bprio;
        }
    }
}

pub fn tp_ceilprio(thread: Arc<SpinLock<rros_thread>>, mut prio: i32){
    if prio > RROS_TP_MAX_PRIO{
        prio = RROS_TP_MAX_PRIO;
    }

    unsafe{(*thread.locked_data().get()).cprio = prio};
}

pub fn tp_chkparam(thread: Option<Arc<SpinLock<rros_thread>>>, p: Option<Arc<SpinLock<rros_sched_param>>>) -> Result<i32>{
    unsafe{
        let thread = thread.unwrap();
        let p = p.unwrap();
        let rq = (*thread.locked_data().get()).rq.unwrap();
        let tp = &(*rq).tp;

        let prio = (*p.locked_data().get()).tp.prio;
        let ptid = (*p.locked_data().get()).tp.ptid;
        pr_info!("in tp_chkparam,gps = {:p}",tp.gps);
        pr_info!("in tp_chkparam,prio = {}",prio);
        pr_info!("in tp_chkparam,ptid = {}",ptid);
        if (tp.gps == 0 as *mut rros_tp_schedule||
            prio < RROS_TP_MIN_PRIO ||
            prio > RROS_TP_MAX_PRIO ||
            ptid < 0 ||
            ptid >= CONFIG_RROS_SCHED_TP_NR_PART){
            pr_warn!("tp_chkparam error");
            return Err(kernel::Error::EINVAL);
        }
        // if tp.gps == 0 as *mut rros_tp_schedule{
        //     pr_warn!("in tp_chkparam,tp.gps == 0 as *mut rros_tp_schedule");
        //     return Err(kernel::Error::EINVAL);
        // }
    }
    pr_info!("tp_chkparam success");
    Ok(0)
}

pub fn tp_declare(thread: Option<Arc<SpinLock<rros_thread>>>,p:Option<Arc<SpinLock<rros_sched_param>>>) -> Result<i32>{
    let thread = thread.unwrap();
    // let p = p.unwrap();
    unsafe{
        let rq = (*thread.locked_data().get()).rq.unwrap();
        (*thread.locked_data().get()).tp_link = Some(Node::new(Arc::try_new(SpinLock::new(rros_thread::new()?))?));
        let mut tp_link = (*thread.locked_data().get()).tp_link.clone();
        (*rq).tp.threads = Some(List::new(Arc::try_new(SpinLock::new(rros_thread::new()?))?));
        if (*rq).tp.threads.clone().as_mut().unwrap().is_empty(){
            pr_info!("tp.threads is empty!");
        }
        (*rq).tp.threads.clone().as_mut().unwrap().add_tail(tp_link.clone().as_mut().unwrap().value.clone());
    }
    pr_info!("tp_declare success!");
    Ok(0)
}

pub fn tp_forget(thread:Arc<SpinLock<rros_thread>>) -> Result<usize>{
    unsafe{
        (*thread.locked_data().get()).tp_link.clone().as_mut().unwrap().remove();
        (*thread.locked_data().get()).tps = 0 as *mut rros_tp_rq;
    }
    Ok(0)
}

pub fn tp_enqueue(thread: Arc<SpinLock<rros_thread>>) -> Result<i32>{
    unsafe{
        let mut head =  (*((*thread.locked_data().get()).tps)).runnable.head.as_mut().unwrap();
        if head.is_empty() {
            let node = Node::new(Arc::try_new(SpinLock::new(rros_thread::new()?))?);
            let mut box_node = Box::try_new(node).unwrap();
            let ptr = Box::into_raw(box_node);
            (*thread.locked_data().get()).rq_next = Some(NonNull::new(ptr).unwrap());
            let mut rq_next = (*thread.locked_data().get()).rq_next.clone();
            head.add_head(rq_next.clone().as_mut().unwrap().as_mut().value.clone());
        }else{
            let mut flag = 1; // flag指示是否到头
            for i in head.len()..=1{
                let thread_cprio = (*thread.locked_data().get()).cprio;
                let cprio_in_list = (*head.get_by_index(i).unwrap().value.clone().locked_data().get()).cprio;
                if thread_cprio <= cprio_in_list{
                    flag = 0;
                    let rq_next = (*thread.locked_data().get()).rq_next.clone();
                    head.enqueue_by_index(i,rq_next.clone().as_mut().unwrap().as_mut().value.clone());
                    break;
                }
            }
            if flag == 1{
                let rq_next = (*thread.locked_data().get()).rq_next.clone();
                head.add_head(rq_next.clone().as_mut().unwrap().as_mut().value.clone());
            }
        }
        Ok(0)
    }
}

pub fn tp_dequeue(thread: Arc<SpinLock<rros_thread>>){
    unsafe{
        (*thread.locked_data().get()).rq_next.as_mut().unwrap().as_mut().remove();
    }
}

pub fn tp_requeue(thread: Arc<SpinLock<rros_thread>>){
    unsafe{
        let mut head =  (*((*thread.locked_data().get()).tps)).runnable.head.as_mut().unwrap();
        if head.is_empty(){
            let rq_next = (*thread.locked_data().get()).rq_next.clone();
            head.add_head(rq_next.clone().as_mut().unwrap().as_mut().value.clone());
        }else{
            let mut flag = 1; // flag指示是否到头
            for i in head.len()..=1{
                let thread_cprio = (*thread.locked_data().get()).cprio;
                let cprio_in_list = (*head.get_by_index(i).unwrap().value.clone().locked_data().get()).cprio;
                if thread_cprio < cprio_in_list{
                    flag = 0;
                    let rq_next = (*thread.locked_data().get()).rq_next.clone();
                    head.enqueue_by_index(i,rq_next.clone().as_mut().unwrap().as_mut().value.clone());
                    break;
                }
            }
            if flag == 1{
                let rq_next = (*thread.locked_data().get()).rq_next.clone();
                head.add_head(rq_next.clone().as_mut().unwrap().as_mut().value.clone());
            }
        }
    }
}

pub fn tp_pick(rq: Option<*mut rros_rq>) -> Result<Arc<SpinLock<rros_thread>>>{
    let rq = rq.unwrap();
    unsafe{
        let timer = Arc::into_raw((*rq).tp.tf_timer.as_mut().unwrap().clone()) as *mut SpinLock<RrosTimer> as *mut RrosTimer;
        if rros_timer_is_running (timer) == false{
            return Err(kernel::Error::EINVAL);
        }
        let head = (*(*rq).tp.tps).runnable.head.as_mut().unwrap();
        if head.is_empty(){
            return Err(kernel::Error::EINVAL);
        }

		let __item = head.get_head().unwrap().value.clone();	
        (*__item.locked_data().get()).rq_next.as_mut().unwrap().as_mut().remove();
        return Ok(__item);						
    }
}

pub fn tp_migrate(thread: Arc<SpinLock<rros_thread>>, rq: *mut rros_rq) -> Result<usize>{
    let mut param = rros_sched_param::new();
    unsafe{
        param.fifo.prio = (*thread.locked_data().get()).cprio;
        rros_set_thread_schedparam_locked(thread.clone(), Some(&rros_sched_fifo), Some(Arc::try_new(SpinLock::new(param))?));
    }
    Ok(0)
}

pub fn tp_show(
    thread: *mut rros_thread,
    buf: *mut c_types::c_char,
    count: ssize_t,
) -> Result<usize>{

    unsafe{
        let p1 = (*thread).tps;
        let p2 = &mut (*(*thread).rq.unwrap()).tp.partitions.as_mut().unwrap()[0] as *mut rros_tp_rq;
        let ptid = p1.offset_from(p2) as i32;
        // return snprintf(buf, count, "%d\n", ptid);
        Ok(0)
    }
}

pub fn start_tp_schedule(rq:*mut rros_rq){
    unsafe{
        let mut tp = &mut (*rq).tp;

        // assert_hard_lock(&rq->lock);

        if tp.gps == 0 as *mut rros_tp_schedule{
            return;
        }
        tp.wnext = 0;
        tp.tf_start = rros_read_clock(&RROS_MONO_CLOCK);
        tp_schedule_next(tp);
    }
}

pub fn stop_tp_schedule(rq:*mut rros_rq) -> Result<usize>{
    unsafe{
        let tp = &mut (*rq).tp;
        // assert_hard_lock(&rq->lock);
        if tp.gps != 0 as *mut rros_tp_schedule{
            rros_stop_timer(tp.tf_timer.as_mut().unwrap().clone());
        }
        Ok(0)
    }
}

pub fn set_tp_schedule(rq:*mut rros_rq, gps:*mut rros_tp_schedule) -> Result<*mut rros_tp_schedule>{
    unsafe{
        let mut tp = &mut (*rq).tp;
        let mut thread = Arc::try_new(SpinLock::new(rros_thread::new()?))?;
        let mut old_gps = 0 as *mut rros_tp_schedule;
        let mut param = rros_sched_param::new();
        // assert_hard_lock(&rq->lock);
        // if (RROS_WARN_ON(CORE, gps != NULL &&
        //     (gps->pwin_nr <= 0 || gps->pwins[0].w_offset != 0)))
        //     return tp->gps;
        stop_tp_schedule(rq);
        if tp.threads.clone().as_mut().unwrap().is_empty() == true{
            old_gps = tp.gps;
            tp.gps = gps;
            return Ok(old_gps);
        }

        for i in 1..=tp.threads.clone().as_mut().unwrap().len(){
            thread = tp.threads.clone().as_mut().unwrap().get_by_index(i).unwrap().value.clone();
            param.fifo.prio = (*thread.locked_data().get()).cprio;
            rros_set_thread_schedparam_locked(thread.clone(), Some(&rros_sched_fifo), Some(Arc::try_new(SpinLock::new(param))?));
        }
        old_gps = tp.gps;
        tp.gps = gps;
        return Ok(old_gps);
    }
}

pub fn get_tp_schedule(rq:*mut rros_rq) -> *mut rros_tp_schedule{
    let gps = unsafe{(*rq).tp.gps};

    // assert_hard_lock(&rq->lock);

    if gps == 0 as *mut rros_tp_schedule{
        return 0 as *mut rros_tp_schedule;
    }

    unsafe{atomic_inc((*gps).refcount)};

    return gps;
}

pub fn put_tp_schedule(gps:*mut rros_tp_schedule){
    unsafe{
        if atomic_dec_and_test((*gps).refcount) != false{
            rros_system_heap.rros_free_chunk(gps as *mut u8);
        }
    }
}

pub fn tp_control(cpu: i32, ctlp: *mut rros_sched_ctlparam, infp: *mut rros_sched_ctlinfo) -> Result<ssize_t>{
    let pt = unsafe{&(*ctlp).tp};
    let mut offset:ktime_t = 0;
    let mut duration:ktime_t = 0;
    let mut next_offset:ktime_t = 0;
    let mut gps = None;
    let mut ogps = 0 as *mut rros_tp_schedule;
    let mut p= 0 as *mut __rros_tp_window;
    let mut pp= 0 as *mut __rros_tp_window;
    let mut w = 0 as *mut rros_tp_window;
    let mut pw = 0 as *mut rros_tp_window;
    let mut it = 0 as *mut rros_tp_ctlinfo;
    let flags:u32 = 0;
    let mut rq = 0 as *mut rros_rq;
    let mut n:i32 = 0; 
    let mut nr_windows:i32 = 0;


    if (cpu < 0 || !cpu_present(cpu) || !is_threading_cpu(cpu)){
        return Err(kernel::Error::EINVAL);
    }

    rq = rros_cpu_rq(cpu);

    // raw_spin_lock_irqsave(&rq->lock, flags);
    unsafe{
    match (pt.op){
    0 =>{
        if pt.nr_windows > 0{
            // raw_spin_unlock_irqrestore(&rq->lock, flags);
            // TODO Sizeof
            gps = rros_system_heap.rros_alloc_chunk((8 + pt.nr_windows * 8) as usize);
            if gps == None{
                return Err(kernel::Error::ENOMEM);
            }
            let mut gps = gps.unwrap() as *mut rros_tp_schedule;
            let mut loop_n = 0;
            loop{
                if loop_n == 0{
                    n = 0;
                    p = pt.windows;
                    w = &mut (*gps).pwins[0] as *mut rros_tp_window;
                    next_offset = 0;
                }else{
                    n += 1;
                    p = p.offset(1);
                    w = w.offset(1);
                }
                if n >= pt.nr_windows{
                    break;
                }
                offset = u_timespec_to_ktime((*p).offset);
                if offset != next_offset{
                    rros_system_heap.rros_free_chunk(gps as *mut u8);
                    return Err(kernel::Error::EINVAL);
                }

                duration = u_timespec_to_ktime((*p).duration);
                if duration <= 0{
                    rros_system_heap.rros_free_chunk(gps as *mut u8);
                    return Err(kernel::Error::EINVAL);
                }

                if (*p).ptid < -1 || (*p).ptid >= CONFIG_RROS_SCHED_TP_NR_PART {
                    rros_system_heap.rros_free_chunk(gps as *mut u8);
                    return Err(kernel::Error::EINVAL);
                }

                (*w).w_offset = next_offset;
                (*w).w_part = (*p).ptid;
                next_offset = ktime::ktime_add(next_offset, duration);
                loop_n += 1;
            }
            atomic_set((*gps).refcount, 1);
            (*gps).pwin_nr = n;
            (*gps).tf_duration = next_offset;
            // raw_spin_lock_irqsave(&rq->lock, flags);

            ogps = set_tp_schedule(rq, gps).unwrap();
            // raw_spin_unlock_irqrestore(&rq->lock, flags);
            if ogps != 0 as *mut rros_tp_schedule{
                put_tp_schedule(ogps);
            }
            rros_schedule();
            return Ok(0);
        }
        let mut gps = 0 as *mut rros_tp_schedule;
        ogps = set_tp_schedule(rq, gps).unwrap();
        // raw_spin_unlock_irqrestore(&rq->lock, flags);
        if ogps != 0 as *mut rros_tp_schedule{
            put_tp_schedule(ogps);
        }
        rros_schedule();
        return Ok(0);
    },
    1 =>{
        let mut gps = 0 as *mut rros_tp_schedule;
        ogps = set_tp_schedule(rq, gps).unwrap();
        // raw_spin_unlock_irqrestore(&rq->lock, flags);
        if ogps != 0 as *mut rros_tp_schedule{
            put_tp_schedule(ogps);
        }
        rros_schedule();
        return Ok(0);
    },
    2 =>{
        start_tp_schedule(rq);
        // raw_spin_unlock_irqrestore(&rq->lock, flags);
        rros_schedule();
        return Ok(0);
    },
    3 =>{
        stop_tp_schedule(rq);
        // raw_spin_unlock_irqrestore(&rq->lock, flags);
        rros_schedule();
        return Ok(0);
    },
    4 =>(),
    _ =>
        return Err(kernel::Error::EINVAL),
    }

    let mut gps = get_tp_schedule(rq);
    // raw_spin_unlock_irqrestore(&rq->lock, flags);
    if gps == 0 as *mut rros_tp_schedule{
        rros_schedule();
        return Ok(0);
    }

    if infp == 0 as *mut rros_sched_ctlinfo {
        put_tp_schedule(gps);
        return Err(kernel::Error::EINVAL);
    }

    it = &mut (*infp).tp as *mut rros_tp_ctlinfo;
    if pt.nr_windows < (*gps).pwin_nr{
        nr_windows = pt.nr_windows;
    }else{
        nr_windows = (*gps).pwin_nr;
    }
    (*it).nr_windows = (*gps).pwin_nr;
    let mut loop_n = 0;
    loop{
        if loop_n == 0{
            n = 0;
            p = (*it).windows;
            pp = p;
            w = &mut (*gps).pwins[0] as *mut rros_tp_window;
            pw = w;
        }else{
            pp = p;
            p = p.offset(1);
            pw = w;
            w = w.offset(1);
            n += 1;
        }
        if n >= nr_windows{
            break;
        }
        (*p).offset = ktime_to_u_timespec((*w).w_offset);
        (*pp).duration = ktime_to_u_timespec(ktime::ktime_sub((*w).w_offset, (*pw).w_offset));
        (*p).ptid = (*w).w_part;
        loop_n += 1;
    }

    (*pp).duration = ktime_to_u_timespec(ktime::ktime_sub((*gps).tf_duration, (*pw).w_offset));
    put_tp_schedule(gps);
    let ret = size_of::<rros_tp_ctlinfo>() + size_of::<__rros_tp_window>() * nr_windows as usize;
    return Ok(ret as i64);
    }
}

// 下面几个函数应移动到clock.rs中
pub fn u_timespec_to_ktime(u_ts: *mut __rros_timespec ) -> ktime_t{
    unsafe{
        let ts64 = bindings::timespec64{
            tv_sec: (*u_ts).tv_sec,
            tv_nsec: (*u_ts).tv_nsec,
        };
        return timespec64_to_ktime(ts64);
    }
}

pub fn timespec64_to_ktime(ts:bindings::timespec64) -> ktime_t{
	unsafe{return rust_helper_timespec64_to_ktime(ts);}
}

pub fn ktime_to_u_timespec(t:ktime_t) -> *mut __rros_timespec{
	let ts64 = ktime_to_timespec64(t);

	return &mut __rros_timespec{
		tv_sec: ts64.tv_sec,
		tv_nsec: ts64.tv_nsec,
	} as *mut __rros_timespec;
}

pub fn ktime_to_timespec64(kt:ktime_t) -> bindings::timespec64{
    unsafe{return bindings::ns_to_timespec64(kt)};
}

pub fn rros_timer_is_running(timer: *mut RrosTimer) -> bool{
    unsafe{
        if (*timer).get_status() & RROS_TIMER_RUNNING != 0{
            return true;
        }else{
            return false;
        }
    }
}


pub fn test_tp(){
    // pr_info!("test_tp in ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
	
}

#[cfg(CONFIG_SMP)]
fn cpu_present(cpu:i32) -> bool{
	return cpu == 0;
}


#[cfg(not(CONFIG_SMP))]
fn cpu_present(cpu:i32) -> bool{
	return cpu == 0;
}