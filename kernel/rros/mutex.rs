use core::{
    ops::{Deref, DerefMut},
    ptr::{null_mut, NonNull},
};

use crate::{
    clock::*,
    factory::*,
    fifo::*,
    lock,
    monitor::rros_is_mutex_owner,
    sched::*,
    thread::*,
    timeout::{self, timeout_infinite, RrosTmode},
    timer::rros_get_timer_delta,
    wait::{self, RrosWaitChannel},
};
use kernel::{
    bindings, c_str, c_types,
    double_linked_list::*,
    linked_list::{GetLinks, Links},
    prelude::*,
    premmpt,
    str::CStr,
    sync::{Guard, Lock, SpinLock},
};

pub const RROS_NO_HANDLE: u32 = 0x00000000;
pub const RROS_MUTEX_PI: u32 = 1;
pub const RROS_MUTEX_PP: u32 = 2;
pub const RROS_MUTEX_CLAIMED: u32 = 4;
pub const RROS_MUTEX_CEILING: u32 = 8;
pub const RROS_MUTEX_FLCLAIM: u32 = 0x80000000;
pub const RROS_MUTEX_FLCEIL: u32 = 0x40000000;
pub const EDEADLK: i32 = 35;
pub const EBUSY: i32 = 16;
pub const EIDRM: i32 = 43;
pub const ETIMEDOUT: i32 = 110;
pub const EINTR: i32 = 4;

type ktime_t = i64;

// static mut RROS_NIL: RrosValue = RrosValue::Lval(0);
extern "C" {
    fn rust_helper_atomic_set(v: *mut bindings::atomic_t, i: i32);
    fn rust_helper_atomic_inc(v: *mut bindings::atomic_t);
    fn rust_helper_atomic_inc_return(v: *mut bindings::atomic_t) -> i32;
    fn rust_helper_atomic_dec_and_test(v: *mut bindings::atomic_t) -> bool;
    fn rust_helper_atomic_dec_return(v: *mut bindings::atomic_t) -> i32;
    fn rust_helper_atomic_cmpxchg(v: *mut bindings::atomic_t, old: i32, new: i32) -> i32;
    fn rust_helper_atomic_read(v: *mut bindings::atomic_t) -> i32;
}
pub fn atomic_set(v: *mut bindings::atomic_t, i: i32) {
    unsafe {
        return rust_helper_atomic_set(v, i);
    }
}

pub fn atomic_inc(v: *mut bindings::atomic_t) {
    unsafe {
        return rust_helper_atomic_inc(v);
    }
}

pub fn atomic_inc_return(v: *mut bindings::atomic_t) -> i32 {
    unsafe {
        return rust_helper_atomic_inc_return(v);
    }
}

pub fn atomic_dec_and_test(v: *mut bindings::atomic_t) -> bool {
    unsafe {
        return rust_helper_atomic_dec_and_test(v);
    }
}

pub fn atomic_dec_return(v: *mut bindings::atomic_t) -> i32 {
    unsafe {
        return rust_helper_atomic_dec_return(v);
    }
}

pub fn atomic_cmpxchg(v: *mut bindings::atomic_t, old: i32, new: i32) -> i32 {
    unsafe {
        return rust_helper_atomic_cmpxchg(v, old, new);
    }
}

pub fn atomic_read(v: *mut bindings::atomic_t) -> i32 {
    unsafe {
        return rust_helper_atomic_read(v);
    }
}

#[repr(transparent)]
pub struct RrosMutexWithBoosters(RrosMutex);
impl GetLinks for RrosMutexWithBoosters {
    type EntryType = Self;
    fn get_links(data: &Self::EntryType) -> &Links<Self::EntryType> {
        &data.0.next_booster
    }
}

impl Deref for RrosMutexWithBoosters {
    type Target = RrosMutex;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl DerefMut for RrosMutexWithBoosters {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

#[repr(transparent)]
pub struct RrosMutexWithTrackers(RrosMutex);
impl GetLinks for RrosMutexWithTrackers {
    type EntryType = Self;
    fn get_links(data: &Self::EntryType) -> &Links<Self::EntryType> {
        &data.0.next_tracker
    }
}

impl Deref for RrosMutexWithTrackers {
    type Target = RrosMutex;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl DerefMut for RrosMutexWithTrackers {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

pub struct RrosMutex {
    pub wprio: i32,
    pub flags: i32,
    pub owner: Option<Arc<SpinLock<RrosThread>>>,
    pub clock: *mut RrosClock,
    pub fastlock: *mut bindings::atomic_t,
    pub ceiling_ref: *mut u32,
    pub lock: bindings::hard_spinlock_t,
    pub wchan: RrosWaitChannel,
    // pub next_booster: *mut Node<Arc<SpinLock<RrosMutex>>>,
    pub next_booster: Links<RrosMutexWithBoosters>,
    // pub next_tracker: *mut Node<Arc<SpinLock<RrosMutex>>>,
    pub next_tracker: Links<RrosMutexWithTrackers>,
}
impl RrosMutex {
    pub fn new() -> Self {
        RrosMutex {
            wprio: 0,
            flags: 0,
            owner: None,
            clock: 0 as *mut RrosClock,
            fastlock: &mut bindings::atomic_t { counter: 0 } as *mut bindings::atomic_t,
            ceiling_ref: core::ptr::null_mut::<u32>(),
            // lock: bindings::hard_spinlock_t {
            // 	rlock: bindings::raw_spinlock {
            // 		raw_lock: bindings::arch_spinlock_t {
            // 			__bindgen_anon_1: bindings::qspinlock__bindgen_ty_1 {
            // 				val: bindings::atomic_t { counter: 0 },
            // 			},
            // 		},
            // 	},
            // 	dep_map: bindings::phony_lockdep_map {
            // 		wait_type_outer: 0,
            // 		wait_type_inner: 0,
            // 	},
            // },
            lock: bindings::hard_spinlock_t::default(),
            wchan: RrosWaitChannel::new(),
            next_booster: Links::new(),
            next_tracker: Links::new(),
        }
    }
}

pub struct RrosKMutex {
    pub mutex: *mut RrosMutex,
    pub fastlock: *mut bindings::atomic_t,
}
impl RrosKMutex {
    pub fn new() -> Self {
        RrosKMutex {
            mutex: 0 as *mut RrosMutex,
            fastlock: &mut bindings::atomic_t { counter: 0 } as *mut bindings::atomic_t,
        }
    }
}

extern "C" {
    fn rust_helper_raw_spin_lock_init(lock: *mut bindings::hard_spinlock_t);
    fn rust_helper_raw_spin_lock(lock: *mut bindings::hard_spinlock_t);
    fn rust_helper_raw_spin_unlock(lock: *mut bindings::hard_spinlock_t);
}

pub fn raw_spin_lock_init(lock: *mut bindings::hard_spinlock_t) {
    unsafe { rust_helper_raw_spin_lock_init(lock) };
}

pub fn raw_spin_lock(lock: *mut bindings::hard_spinlock_t) {
    unsafe { rust_helper_raw_spin_lock(lock) };
}

pub fn raw_spin_unlock(lock: *mut bindings::hard_spinlock_t) {
    unsafe { rust_helper_raw_spin_unlock(lock) };
}

// #define for_each_evl_mutex_waiter(__pos, __mutex) \
// 	list_for_each_entry(__pos, &(__mutex)->wchan.wait_list, wait_next)

pub fn get_ceiling_value(mutex: &mut RrosMutex) -> u32 {
    let ceiling_ref = unsafe { *(mutex.ceiling_ref) };
    if ceiling_ref < 1 {
        return 1 as u32;
    } else if ceiling_ref <= RROS_FIFO_MAX_PRIO as u32 {
        return ceiling_ref;
    } else {
        return RROS_FIFO_MAX_PRIO as u32;
    }
}

pub fn disable_inband_switch(curr: *mut RrosThread) {
    unsafe {
        if ((*curr).state & (T_WEAK | T_WOLI)) != 0 {
            (*curr).inband_disable_count.atomic_inc();
        }
    }
}

pub fn enable_inband_switch(curr: *mut RrosThread) -> bool {
    unsafe {
        if ((*curr).state & (T_WEAK | T_WOLI)) == 0 {
            return true;
        }

        if ((*curr).inband_disable_count.atomic_dec_return() >= 0) {
            return true;
        }

        (*curr).inband_disable_count.atomic_set(0);

        if (*curr).state & T_WOLI != 0 {
            rros_notify_thread(curr, RROS_HMDIAG_LKIMBALANCE as u32, RrosValue::Lval(0));
        }
        return false;
    }
}

pub fn raise_boost_flag(owner: Arc<SpinLock<RrosThread>>) {
    unsafe {
        // assert_hard_lock(&owner->lock);
        let lock = &mut (*(*(owner.deref().locked_data().get())).rq.unwrap()).lock;
        // let lock = &mut (*(*owner.locked_data().get()).rq.unwrap()).lock as *mut bindings::hard_spinlock_t;
        lock.raw_spin_lock();

        let state = (*owner.locked_data().get()).state;

        if state & T_BOOST == 0 {
            (*owner.locked_data().get()).bprio = (*owner.locked_data().get()).cprio;
            (*owner.locked_data().get()).state |= T_BOOST;
        }
        lock.raw_spin_unlock();
    }
}

pub fn inherit_thread_priority(
    owner: Arc<SpinLock<RrosThread>>,
    contender: Arc<SpinLock<RrosThread>>,
    originator: Arc<SpinLock<RrosThread>>,
) -> Result<i32> {
    pr_info!("mutex.rs: inherit_thread_priority: into");
    let mut ret: Result<i32> = Ok(0);

    // assert_hard_lock(&owner->lock);
    // assert_hard_lock(&contender->lock);

    rros_track_thread_policy(owner.clone(), contender.clone());

    let func;
    unsafe {
        let wchan = (*owner.deref().locked_data().get()).wchan;
        if (!wchan.is_null()) {
            match (*wchan).reorder_wait {
                Some(f) => func = f,
                None => {
                    pr_warn!("inherit_thread_priority:reorder_wait function error");
                    return Err(kernel::Error::EINVAL);
                }
            }
            ret = unsafe { return func(owner.clone(), originator.clone()) };
        }
    }
    ret
}

pub fn adjust_boost(
    owner: Arc<SpinLock<RrosThread>>,
    contender: Option<Arc<SpinLock<RrosThread>>>,
    origin: *mut RrosMutex,
    originator: Arc<SpinLock<RrosThread>>,
) -> Result<i32> {
    pr_info!("mutex.rs: adjust_boost: into");
    unsafe {
        let mut pprio: u32 = 0;
        let mut ret: Result<i32> = Ok(0);
        // assert_hard_lock(&owner->lock);
        // assert_hard_lock(&origin->lock);
        let mut boosters = &mut (*owner.deref().locked_data().get()).boosters;
        let mut cursor = boosters.cursor_front_mut();
        let mut mutex = cursor.current().unwrap().deref_mut();
        // mutex = Arc::into_raw((*boosters).get_head().unwrap().value.clone())
        //     as *mut SpinLock<RrosMutex> as *mut RrosMutex;
        if mutex as *mut RrosMutex != origin {
            raw_spin_lock(&mut mutex.lock as *mut bindings::hard_spinlock_t);
        }
        let wprio = (*owner.locked_data().get()).wprio;
        if mutex.wprio == wprio {
            if mutex as *mut RrosMutex != origin {
                raw_spin_unlock(&mut mutex.lock as *mut bindings::hard_spinlock_t);
            }
            return Ok(0);
        }

        if mutex.flags & RROS_MUTEX_PP as i32 != 0 {
            pprio = get_ceiling_value(mutex);

            rros_protect_thread_priority(owner.clone(), pprio as i32);

            let wchan = (*owner.deref().locked_data().get()).wchan;
            if (!wchan.is_null()) {
                let func;
                match (*wchan).reorder_wait {
                    Some(f) => func = f,
                    None => {
                        pr_warn!("reorder_wait function error");
                        return Err(kernel::Error::EINVAL);
                    }
                }
                ret = func(owner.clone(), originator.clone());
                if mutex as *mut RrosMutex != origin {
                    raw_spin_unlock(&mut mutex.lock as *mut bindings::hard_spinlock_t);
                }
            }
        } else {
            if (mutex.wchan.wait_list).is_empty() {
                if mutex as *mut RrosMutex != origin {
                    raw_spin_unlock(&mut mutex.lock as *mut bindings::hard_spinlock_t);
                }
                return Ok(0);
            }

            if contender.is_none() {
                let mut wait_list = &mut (*mutex).wchan.wait_list;
                let mut cursor = wait_list.cursor_front_mut();
                let contender = cursor.current().unwrap();
                let contender_lock = &mut (*contender.get_ptr_unlocked()).lock;
                contender_lock.raw_spin_lock();
                Arc::increment_strong_count(
                    contender as *mut RrosThreadWithLock as *mut SpinLock<RrosThread>,
                );
                let contender = Arc::from_raw(
                    contender as *mut RrosThreadWithLock as *mut SpinLock<RrosThread>,
                );
                ret = inherit_thread_priority(owner.clone(), contender.clone(), originator.clone());
                contender_lock.raw_spin_unlock();
            } else {
                ret = inherit_thread_priority(
                    owner.clone(),
                    contender.clone().unwrap(),
                    originator.clone(),
                );
            }
            if mutex as *mut RrosMutex != origin {
                raw_spin_unlock(&mut mutex.lock as *mut bindings::hard_spinlock_t);
            }
        }
        return ret;
    }
}

pub fn ceil_owner_priority(
    mutex: *mut RrosMutex,
    originator: Arc<SpinLock<RrosThread>>,
) -> Result<usize> {
    pr_info!("mutex.rs: ceil_owner_priority: into");
    unsafe {
        let owner = (*mutex).owner.clone().unwrap().clone();
        let wprio: i32;
        // assert_hard_lock(&mutex->lock);
        wprio = rros_calc_weighted_prio(&RROS_SCHED_FIFO, get_ceiling_value(&mut *mutex) as i32);
        (*mutex).wprio = wprio;
        let lock = &mut (*owner.deref().locked_data().get()).lock;
        lock.raw_spin_lock();

        let mut boosters = &mut (*owner.deref().locked_data().get()).boosters;
        Arc::increment_strong_count(mutex as *const RrosMutex);
        let next_booster = Arc::from_raw(mutex as *const RrosMutexWithBoosters);

        // FIXME: `boosters.is_empty()` may be included into `match cursor.current()` -> None path.
        if boosters.is_empty() {
            boosters.push_front(next_booster.clone());
        } else {
            let mut cursor = boosters.cursor_back_mut();
            while let Some(entry) = cursor.current() {
                if ((*mutex).wprio <= (*entry).deref().wprio) {
                    break;
                }
                cursor.move_prev();
            }
            match cursor.current() {
                Some(existing) => {
                    let existing =
                        NonNull::new(existing as *const _ as *mut RrosMutexWithBoosters).unwrap();
                    boosters.insert_after(existing, next_booster.clone())
                }
                None => {
                    boosters.push_front(next_booster.clone());
                }
            }
        }

        raise_boost_flag(owner.clone());
        (*mutex).flags |= RROS_MUTEX_CEILING as i32;

        let owner_wprio = (*owner.locked_data().get()).wprio;
        // TODO: 没进入这个if path
        if wprio > owner_wprio {
            adjust_boost(owner.clone(), None, mutex, originator.clone());
        }
        lock.raw_spin_unlock();
        Ok(0)
    }
}

pub fn untrack_owner(mutex: *mut RrosMutex) {
    unsafe {
        let prev = (*mutex).owner.clone();

        // assert_hard_lock(&mutex->lock);
        if prev.is_some() {
            let flags = lock::raw_spin_lock_irqsave();
            // (*(*mutex).next_tracker).remove();
            lock::raw_spin_unlock_irqrestore(flags);
            // evl_put_element(&prev->element);
            (*mutex).owner = None;
        }
    }
}

pub fn track_owner(mutex: *mut RrosMutex, owner: Arc<SpinLock<RrosThread>>) {
    unsafe {
        let prev = (*mutex).owner.clone();
        // assert_hard_lock(&mutex->lock);
        // if (EVL_WARN_ON_ONCE(CORE, prev == owner))
        // 	return;

        let flags = lock::raw_spin_lock_irqsave();
        if prev.is_some() {
            // TODO:
            // (*(*mutex).next_tracker).remove();
            // smp_wmb();
            // evl_put_element(&prev->element);
        }

        // TODO: ((*((*owner).locked_data().get())).trackers).add_head((*((*mutex).next_tracker)).value.clone());
        lock::raw_spin_unlock_irqrestore(flags);
        (*mutex).owner = Some(owner.clone());
    }
}

pub fn ref_and_track_owner(mutex: *mut RrosMutex, owner: Arc<SpinLock<RrosThread>>) {
    unsafe {
        if ((*mutex).owner.is_none()) {
            // evl_get_element(&owner->element);
            track_owner(mutex, owner.clone());
            return;
        }
        // FIXME: Arc::into_raw leaks memeory.
        let ptr1 = Arc::into_raw((*mutex).owner.clone().unwrap()) as *mut SpinLock<RrosThread>;
        let ptr2 = Arc::into_raw(owner.clone()) as *mut SpinLock<RrosThread>;
        if ptr1 != ptr2 {
            // evl_get_element(&owner->element);
            track_owner(mutex, owner.clone());
        }
    }
}

pub fn fast_mutex_is_claimed(handle: u32) -> bool {
    return handle & RROS_MUTEX_FLCLAIM != 0;
}

pub fn mutex_fast_claim(handle: u32) -> u32 {
    return handle | RROS_MUTEX_FLCLAIM;
}

pub fn mutex_fast_ceil(handle: u32) -> u32 {
    return handle | RROS_MUTEX_FLCEIL;
}

pub fn set_current_owner_locked(mutex: *mut RrosMutex, owner: Arc<SpinLock<RrosThread>>) {
    // assert_hard_lock(&mutex->lock);
    ref_and_track_owner(mutex, owner.clone());
    pr_info!("1111111111111111111111111111111111111111111");
    unsafe {
        if (*mutex).flags & RROS_MUTEX_PP as i32 != 0 {
            pr_info!("2222222222222222222222222222222222222");
            ceil_owner_priority(mutex, owner.clone());
            pr_info!("333333333333333333333333333333333333333");
        }
    }
}

pub fn set_current_owner(mutex: *mut RrosMutex, owner: Arc<SpinLock<RrosThread>>) -> Result<usize> {
    pr_info!("00000000000000000000000000000000000000");
    let flags = lock::raw_spin_lock_irqsave();
    pr_info!("000000000000000.............555000000000000000000000.50.50.5");
    set_current_owner_locked(mutex, owner.clone());
    lock::raw_spin_unlock_irqrestore(flags);
    pr_info!("99999999999999999999999999999999999999");
    Ok(0)
}

pub fn get_owner_handle(mut ownerh: u32, mutex: *mut RrosMutex) -> u32 {
    unsafe {
        if (*mutex).flags & RROS_MUTEX_PP as i32 != 0 {
            ownerh = mutex_fast_ceil(ownerh);
        }
        return ownerh;
    }
}

pub fn clear_boost_locked(
    mutex: *mut RrosMutex,
    owner: Arc<SpinLock<RrosThread>>,
    flag: i32,
) -> Result<i32> {
    pr_info!("mutex.rs: clear_boost_locked: into");
    unsafe {
        // assert_hard_lock(&mutex->lock);
        // assert_hard_lock(&owner->lock);
        (*mutex).flags &= !flag;

        let mut boosters = &mut (*owner.deref().locked_data().get()).boosters;
        // TODO: 不确定是从哪个owner->boosters链表中删除next_booster
        Arc::increment_strong_count(mutex as *const RrosMutex);
        let next_booster = Arc::from_raw(mutex as *const RrosMutexWithBoosters);
        boosters.remove(&next_booster);
        if boosters.is_empty() {
            let lock = &mut (*(*owner.deref().locked_data().get()).rq.unwrap()).lock;
            lock.raw_spin_lock();
            (*owner.deref().locked_data().get()).state &= !T_BOOST;
            lock.raw_spin_unlock();
            inherit_thread_priority(owner.clone(), owner.clone(), owner.clone());
        } else {
            adjust_boost(owner.clone(), None, mutex, owner.clone());
        }
        Ok(0)
    }
}

pub fn clear_boost(
    mutex: *mut RrosMutex,
    owner: Arc<SpinLock<RrosThread>>,
    flag: i32,
) -> Result<usize> {
    let lock = unsafe { &mut (*owner.deref().locked_data().get()).lock };
    lock.raw_spin_lock();
    clear_boost_locked(mutex, owner.clone(), flag);
    lock.raw_spin_unlock();
    Ok(0)
}

pub fn detect_inband_owner(mutex: *mut RrosMutex, curr: *mut RrosThread) {
    unsafe {
        let owner = (*mutex).owner.clone().unwrap();
        let lock = &mut (*(*curr).rq.unwrap()).lock;
        lock.raw_spin_lock();
        let state = (*owner.locked_data().get()).state;
        if (*curr).info & T_PIALERT != 0 {
            (*curr).info &= !T_PIALERT;
        } else if state & T_INBAND != 0 {
            (*curr).info |= T_PIALERT;
            lock.raw_spin_unlock();
            rros_notify_thread(curr, RROS_HMDIAG_LKDEPEND as u32, RrosValue::Lval(0));
            return;
        }

        lock.raw_spin_unlock();
    }
}

pub fn rros_detect_boost_drop() {
    unsafe {
        let curr = rros_current() as *mut RrosThread;
        let mut waiter = 0 as *mut RrosThread;
        let mutex = 0 as *mut RrosMutex;

        let flags = lock::raw_spin_lock_irqsave();

        let mut boosters = &mut (*curr).boosters;
        let mut cursor = boosters.cursor_front_mut();
        while let Some(entry) = cursor.current() {
            raw_spin_lock(&mut (*entry).deref_mut().lock as *mut bindings::hard_spinlock_t);
            let mut wait_list = &mut (*entry).deref_mut().wchan.wait_list;
            let mut cursor = wait_list.cursor_front_mut();
            while let Some(entry) = cursor.current() {
                Arc::increment_strong_count(entry as *const _ as *const SpinLock<RrosThread>);
                let waiter = Arc::from_raw(entry as *const _ as *const SpinLock<RrosThread>);
                if (*waiter.deref().locked_data().get()).state & T_WOLI != 0 {
                    (*waiter.deref().locked_data().get()).lock.raw_spin_lock();
                    (*waiter.locked_data().get()).info |= T_PIALERT;
                    (*waiter.deref().locked_data().get()).lock.raw_spin_unlock();
                    rros_notify_thread(
                        (*Arc::into_raw(waiter)).locked_data().get(),
                        RROS_HMDIAG_LKDEPEND as u32,
                        RrosValue::Lval(0),
                    );
                }
                cursor.move_next();
            }
            raw_spin_unlock(&mut (*entry).deref_mut().lock as *mut bindings::hard_spinlock_t);
        }

        lock::raw_spin_unlock_irqrestore(flags);
    }
}

pub fn __rros_init_mutex(
    mutex: *mut RrosMutex,
    clock: *mut RrosClock,
    fastlock: *mut bindings::atomic_t,
    ceiling_ref: *mut u32,
) {
    unsafe {
        let mut Type: u32 = 0;
        if ceiling_ref.is_null() {
            Type = RROS_MUTEX_PI;
        } else {
            Type = RROS_MUTEX_PP;
        }
        if mutex == 0 as *mut RrosMutex {
            pr_info!("__rros_init_mutex error!");
            return;
        }
        (*mutex).fastlock = fastlock;
        atomic_set(fastlock, RROS_NO_HANDLE as i32);
        (*mutex).flags = (Type & !RROS_MUTEX_CLAIMED) as i32;
        (*mutex).owner = None;
        (*mutex).wprio = -1;
        (*mutex).ceiling_ref = ceiling_ref;
        (*mutex).clock = clock;
        (*mutex).wchan.reorder_wait = Some(rros_reorder_mutex_wait);
        (*mutex).wchan.follow_depend = Some(rros_follow_mutex_depend);
        raw_spin_lock_init(&mut (*mutex).lock as *mut bindings::hard_spinlock_t);
    }
}

pub fn flush_mutex_locked(mutex: *mut RrosMutex, reason: u32) -> Result<usize> {
    let tmp = 0 as *mut RrosThread;
    // assert_hard_lock(&mutex->lock);
    unsafe {
        let mut thread_node = Arc::try_new(SpinLock::new(RrosThread::new()?))?;
        if ((*mutex).wchan.wait_list).is_empty() {
            // EVL_WARN_ON(CORE, mutex->flags & EVL_MUTEX_CLAIMED);
        } else {
            while let Some(waiter) = (*mutex).wchan.wait_list.pop_front() {
                let waiter = unsafe { RrosThreadWithLock::transmute_to_original(waiter) };
                rros_wakeup_thread(waiter.clone(), T_PEND, reason as i32);
            }
            if (*mutex).flags & RROS_MUTEX_CLAIMED as i32 != 0 {
                clear_boost(
                    mutex,
                    (*mutex).owner.clone().unwrap(),
                    RROS_MUTEX_CLAIMED as i32,
                );
            }
        }
    }
    Ok(0)
}

pub fn rros_flush_mutex(mutex: *mut RrosMutex, reason: u32) {
    // trace_evl_mutex_flush(mutex);
    let flags = lock::raw_spin_lock_irqsave();
    flush_mutex_locked(mutex, reason);
    lock::raw_spin_unlock_irqrestore(flags);
}

pub fn rros_destroy_mutex(mutex: *mut RrosMutex) {
    // trace_evl_mutex_destroy(mutex);
    let flags = lock::raw_spin_lock_irqsave();
    untrack_owner(mutex);
    flush_mutex_locked(mutex, T_RMID);
    lock::raw_spin_unlock_irqrestore(flags);
}

pub fn rros_trylock_mutex(mutex: *mut RrosMutex) -> Result<i32> {
    let curr = unsafe { &mut *rros_current() };
    let currh = unsafe { (*(curr.locked_data().get())).element.borrow().fundle };
    let lockp = unsafe { (*mutex).fastlock };
    let mut h: i32 = 0;

    premmpt::running_oob()?;
    // trace_evl_mutex_trylock(mutex);
    pr_info!("mutex.rs: rros_trylock_mutex: lockp here 1\n");
    h = atomic_cmpxchg(
        lockp,
        RROS_NO_HANDLE as i32,
        get_owner_handle(currh, mutex) as i32,
    );

    if h != RROS_NO_HANDLE as i32 {
        if rros_get_index(h as u32) as u32 == currh {
            return Err(Error::EDEADLK);
        } else {
            return Err(Error::EBUSY);
        }
    }

    let curr_arc = unsafe {
        Arc::increment_strong_count(curr as *const SpinLock<RrosThread>);
        Arc::from_raw(curr as *const SpinLock<RrosThread>)
    };
    unsafe { set_current_owner(mutex, curr_arc.clone()) };

    disable_inband_switch(curr.locked_data().get());

    return Ok(0);
}

pub fn wait_mutex_schedule(mutex: *mut RrosMutex) -> Result<i32> {
    let curr = rros_current();
    let flags: u32 = 0;
    let mut ret: Result<i32> = Ok(0);
    let mut info: u32 = 0;

    unsafe { rros_schedule() };

    info = unsafe { (*(*rros_current()).locked_data().get()).info };
    if info & T_RMID != 0 {
        return Err(kernel::Error::EIDRM);
    }

    if info & (T_TIMEO | T_BREAK) != 0 {
        let flags = lock::raw_spin_lock_irqsave();
        let wait_next = unsafe { RrosThreadWithLock::new_from_curr_thread() };
        unsafe {
            (*mutex).wchan.wait_list.remove(&wait_next);
        }

        if info & T_TIMEO != 0 {
            ret = Err(kernel::Error::ETIMEDOUT);
        } else if info & T_BREAK != 0 {
            ret = Err(kernel::Error::EINTR);
        }

        lock::raw_spin_unlock_irqrestore(flags);
    }
    // } else if (IS_ENABLED(CONFIG_EVL_DEBUG_CORE)) {
    // 	bool empty;
    // 	// raw_spin_lock_irqsave(&mutex->lock, flags);
    // 	empty = list_empty(&curr->wait_next);
    // 	// raw_spin_unlock_irqrestore(&mutex->lock, flags);
    // 	// EVL_WARN_ON_ONCE(CORE, !empty);
    // }

    return ret;
}

pub fn finish_mutex_wait(mutex: *mut RrosMutex) {
    unsafe {
        let owner = (*mutex).owner.clone().unwrap();

        // assert_hard_lock(&mutex->lock);

        if (*mutex).flags & RROS_MUTEX_CLAIMED as i32 == 0 {
            return;
        }

        if (*mutex).wchan.wait_list.is_empty() {
            clear_boost(mutex, owner.clone(), RROS_MUTEX_CLAIMED as i32);
            return;
        }

        // let contender = (*mutex).wchan.wait_list.get_head().unwrap().value.clone();
        let mut cursor = (*mutex).wchan.wait_list.cursor_front_mut();
        let contender;
        match cursor.current() {
            Some(entry) => {
                Arc::increment_strong_count(entry as *const _ as *const SpinLock<RrosThread>);
                contender = Arc::from_raw(entry as *const _ as *const SpinLock<RrosThread>);
                let owner_lock = &mut (*owner.deref().locked_data().get()).lock;
                let contender_lock = &mut (*contender.deref().locked_data().get()).lock;
                owner_lock.raw_spin_lock();
                contender_lock.raw_spin_lock();
                (*mutex).wprio = (*contender.deref().locked_data().get()).wprio;
                let mut boosters = &mut (*owner.deref().locked_data().get()).boosters;
                Arc::increment_strong_count(mutex as *const RrosMutex);
                // TODO: 不确定是从哪个owner->boosters链表中删除next_booster
                let next_booster = Arc::from_raw(mutex as *const RrosMutexWithBoosters);
                boosters.remove(&next_booster);

                // FIXME: `boosters.is_empty()` may be included into `match cursor.current()` -> None path.
                if boosters.is_empty() {
                    boosters.push_front(next_booster.clone());
                } else {
                    let mut cursor = boosters.cursor_back_mut();
                    while let Some(entry) = cursor.current() {
                        if ((*mutex).wprio <= (*entry).deref().wprio) {
                            break;
                        }
                        cursor.move_prev();
                    }
                    match cursor.current() {
                        Some(existing) => {
                            let existing =
                                NonNull::new(existing as *const _ as *mut RrosMutexWithBoosters)
                                    .unwrap();
                            boosters.insert_after(existing, next_booster.clone())
                        }
                        None => {
                            boosters.push_front(next_booster.clone());
                        }
                    }
                }
                adjust_boost(owner.clone(), Some(contender.clone()), mutex, owner.clone());
                contender_lock.raw_spin_unlock();
                owner_lock.raw_spin_unlock();
            }
            None => (),
        };
    }
}

pub fn check_lock_chain(
    owner: Arc<SpinLock<RrosThread>>,
    originator: Arc<SpinLock<RrosThread>>,
) -> Result<i32> {
    unsafe {
        let mut wchan = (*owner.deref().deref().locked_data().get()).wchan;
        // assert_hard_lock(&owner->lock);
        // assert_hard_lock(&originator->lock);

        if wchan != null_mut() {
            let func;
            match (*wchan).follow_depend.clone() {
                Some(f) => func = f,
                None => {
                    // TODO: monitor-pi-deadlock failed here.
                    pr_warn!("check_lock_chain:follow_depend function error");
                    return Err(kernel::Error::EINVAL);
                }
            }
            return func(wchan, originator.clone());
        }
        Ok(0)
    }
}

pub fn rros_lock_mutex_timeout(
    mutex: *mut RrosMutex,
    timeout: ktime_t,
    timeout_mode: timeout::RrosTmode,
) -> Result<i32> {
    unsafe {
        let curr = &mut *rros_current();
        // FIXME: code clean here.
        let mut owner = Arc::try_new(SpinLock::new(RrosThread::new().unwrap())).unwrap();
        let lockp = (*mutex).fastlock;
        let mut currh = (*(curr.locked_data().get())).element.borrow().fundle;
        let mut h: FundleT = 0;
        let mut oldh: FundleT = 0;
        let flags: u32 = 0;
        let mut ret: Result<i32>;
        premmpt::running_oob()?;

        // trace_evl_mutex_lock(mutex);
        pr_info!(
            "rros_lock_mutex_timeout rros_current address is {:p}",
            rros_current()
        );
        loop {
            // FIXME: monitor-deadlock failed here, beacause flow into this path.
            // currh get from `curr.fundle`, so the fundle is incurrect.
            // lockp here 9
            pr_info!("mutex.rs: rros_lock_mutex_timeout: lockp here 2\n");
            h = atomic_cmpxchg(
                lockp,
                RROS_NO_HANDLE as i32,
                get_owner_handle(currh, mutex) as i32,
            ) as FundleT;
            if h == RROS_NO_HANDLE {
                let temp = Arc::from_raw(rros_current() as *const SpinLock<RrosThread>);
                Arc::increment_strong_count(rros_current() as *const SpinLock<RrosThread>);
                let test = temp.clone();
                pr_info!("{:p}", test);

                set_current_owner(mutex, temp.clone());

                disable_inband_switch(curr.locked_data().get());

                return Ok(0);
            }

            if rros_get_index(h) == currh {
                return Err(kernel::Error::EDEADLK);
            }

            ret = Ok(0);
            let mut test_no_owner = 0; // goto test_no_owner
            let mut flags = lock::raw_spin_lock_irqsave();
            let curr_lock = &mut (*curr.locked_data().get()).lock;

            curr_lock.raw_spin_lock();
            if fast_mutex_is_claimed(h) == true {
                oldh = atomic_read(lockp) as u32;
                test_no_owner = 1;
            }

            let mut redo = 0;
            loop {
                if test_no_owner == 0 {
                    // lockp here 8
                    pr_info!("mutex.rs: rros_lock_mutex_timeout: lockp here 3\n");
                    oldh = atomic_cmpxchg(lockp, h as i32, mutex_fast_claim(h) as i32) as u32;
                    if oldh == h {
                        break;
                    }
                }
                // test_no_owner
                if oldh == RROS_NO_HANDLE {
                    curr_lock.raw_spin_unlock();
                    lock::raw_spin_unlock_irqrestore(flags);
                    redo = 1;
                    break;
                }
                h = oldh;
                if fast_mutex_is_claimed(h) == true {
                    break;
                }
                test_no_owner = 0;
            }
            if redo == 1 {
                continue;
            }
            // owner = evl_get_factory_element_by_fundle(&evl_thread_factory,evl_get_index(h),struct evl_thread);
            // let owner_ptr = Arc::into_raw(owner.clone()) as *mut SpinLock<RrosThread> as *mut RrosThread;
            let _map = (*RROS_THREAD_FACTORY.locked_data().get())
                .inside
                .as_mut()
                .unwrap()
                .index
                .as_mut()
                .unwrap();
            let fundle = rros_get_index(h);
            let owner_ptr = __rros_get_element_by_fundle(_map, fundle);
            let owner_ptr = (*owner_ptr).pointer as *mut SpinLock<RrosThread>;
            owner = Arc::from_raw(owner_ptr);
            Arc::increment_strong_count(owner_ptr as *const SpinLock<RrosThread>);
            let owner_ptr = (*owner_ptr).locked_data().get();
            if owner_ptr.is_null() {
                untrack_owner(mutex);
                curr_lock.raw_spin_unlock();
                lock::raw_spin_unlock_irqrestore(flags);
                return Err(kernel::Error::EOWNERDEAD);
            }

            if ((*mutex).owner.is_none()
                || (Arc::as_ptr((*mutex).owner.as_ref().unwrap()) != Arc::as_ptr(&owner)))
            {
                track_owner(mutex, owner.clone());
            } else {
                // evl_put_element(&owner->element);
            }
            let owner_lock = &mut (*owner.deref().locked_data().get()).lock;
            owner_lock.raw_spin_lock();
            let state = (*curr.locked_data().get()).state;
            if state & T_WOLI != 0 {
                detect_inband_owner(mutex, curr.locked_data().get());
            }
            let wprio = (*curr.locked_data().get()).wprio;
            let owner_wprio = (*owner.locked_data().get()).wprio;
            pr_info!("mutex.rs: wprio = {}, owner_wprio = {}", wprio, owner_wprio);

            if wprio > owner_wprio {
                let info = (*owner.locked_data().get()).info;
                let wwake = (*owner.locked_data().get()).wwake;
                if info & T_WAKEN != 0 && wwake == &mut (*mutex).wchan as *mut RrosWaitChannel {
                    let temp = Arc::from_raw(curr as *const SpinLock<RrosThread>);
                    Arc::increment_strong_count(curr as *const SpinLock<RrosThread>);
                    set_current_owner_locked(mutex, temp.clone());
                    let owner_rq_lock =
                        &mut (*(*owner.deref().locked_data().get()).rq.unwrap()).lock;
                    owner_rq_lock.raw_spin_lock();
                    (*owner.locked_data().get()).info |= T_ROBBED;
                    owner_rq_lock.raw_spin_unlock();
                    owner_lock.raw_spin_unlock();
                    // grab:
                    disable_inband_switch(curr.locked_data().get());
                    if ((*mutex).wchan.wait_list).is_empty() == false {
                        currh = mutex_fast_claim(currh);
                    }
                    // lockp here 7
                    atomic_set(lockp, get_owner_handle(currh, mutex) as i32);
                    curr_lock.raw_spin_unlock();
                    lock::raw_spin_unlock_irqrestore(flags);
                    return ret;
                }

                if (*mutex).wchan.wait_list.is_empty() {
                    // let wait_next = (*curr.locked_data().get()).wait_next;
                    let wait_next = RrosThreadWithLock::new_from_curr_thread();
                    (*mutex).wchan.wait_list.push_front(wait_next);
                } else {
                    let mut flag = 1; // flag指示是否到头
                    let mut cursor = (*mutex).wchan.wait_list.cursor_front_mut();
                    while let Some(entry) = cursor.current() {
                        let waiter =
                            Arc::from_raw(entry as *const _ as *const SpinLock<RrosThread>);
                        Arc::increment_strong_count(
                            entry as *const _ as *const SpinLock<RrosThread>,
                        );
                        let curr_wprio = (*(*curr).locked_data().get()).wprio;
                        let wprio_in_list = (*waiter.locked_data().get()).wprio;
                        if curr_wprio <= wprio_in_list {
                            flag = 0;
                            let wait_next = RrosThreadWithLock::new_from_curr_thread();
                            (*mutex)
                                .wchan
                                .wait_list
                                .insert_after(NonNull::new(entry).unwrap(), wait_next);
                            break;
                        }
                        cursor.move_next();
                    }
                    if flag == 1 {
                        // let wait_next = (*curr.locked_data().get()).wait_next;
                        let wait_next = RrosThreadWithLock::new_from_curr_thread();
                        (*mutex).wchan.wait_list.push_front(wait_next);
                    }
                }

                if (*mutex).flags & RROS_MUTEX_PI as i32 != 0 {
                    raise_boost_flag(owner.clone());
                    if (*mutex).flags & RROS_MUTEX_CLAIMED as i32 != 0 {
                        let mut boosters = &mut (*owner.deref().locked_data().get()).boosters;
                        Arc::increment_strong_count(mutex as *const RrosMutex);
                        // TODO: 不确定是从哪个owner->boosters链表中删除next_booster
                        let next_booster = Arc::from_raw(mutex as *mut RrosMutexWithBoosters);
                        boosters.remove(&next_booster);
                    } else {
                        (*mutex).flags |= RROS_MUTEX_CLAIMED as i32;
                    }
                    (*mutex).wprio = (*curr.locked_data().get()).wprio;

                    Arc::increment_strong_count(mutex as *const RrosMutex);
                    let next_booster = Arc::from_raw(mutex as *const RrosMutexWithBoosters);
                    // FIXME: `boosters.is_empty()` may be included into `match cursor.current()` -> None path.
                    let mut boosters = &mut (*owner.deref().locked_data().get()).boosters;
                    if boosters.is_empty() {
                        boosters.push_front(next_booster.clone());
                    } else {
                        let mut cursor = boosters.cursor_back_mut();
                        while let Some(entry) = cursor.current() {
                            if ((*mutex).wprio <= (*entry).deref().wprio) {
                                break;
                            }
                            cursor.move_prev();
                        }
                        match cursor.current() {
                            Some(existing) => {
                                let existing = NonNull::new(
                                    existing as *const _ as *mut RrosMutexWithBoosters,
                                )
                                .unwrap();
                                boosters.insert_after(existing, next_booster.clone())
                            }
                            None => {
                                boosters.push_front(next_booster.clone());
                            }
                        }
                    }

                    let temp = Arc::from_raw(curr as *const SpinLock<RrosThread>);
                    Arc::increment_strong_count(curr as *const SpinLock<RrosThread>);
                    ret = inherit_thread_priority(owner.clone(), temp.clone(), temp.clone());
                } else {
                    let temp = Arc::from_raw(curr as *const SpinLock<RrosThread>);
                    Arc::increment_strong_count(curr as *const SpinLock<RrosThread>);
                    ret = check_lock_chain(owner.clone(), temp.clone());
                }
            } else {
                if (*mutex).wchan.wait_list.is_empty() {
                    let wait_next = RrosThreadWithLock::new_from_curr_thread();
                    (*mutex).wchan.wait_list.push_front(wait_next);
                } else {
                    let mut flag = 1; // flag指示是否到头
                    let mut cursor = (*mutex).wchan.wait_list.cursor_front_mut();
                    while let Some(entry) = cursor.current() {
                        let waiter =
                            Arc::from_raw(entry as *const _ as *const SpinLock<RrosThread>);
                        Arc::increment_strong_count(
                            entry as *const _ as *const SpinLock<RrosThread>,
                        );
                        let curr_wprio = (*(*curr).locked_data().get()).wprio;
                        let wprio_in_list = (*waiter.locked_data().get()).wprio;
                        if curr_wprio <= wprio_in_list {
                            flag = 0;
                            let wait_next = RrosThreadWithLock::new_from_curr_thread();
                            (*mutex)
                                .wchan
                                .wait_list
                                .insert_after(NonNull::new(entry).unwrap(), wait_next);

                            break;
                        }
                        cursor.move_next();
                    }

                    if flag == 1 {
                        let wait_next = RrosThreadWithLock::new_from_curr_thread();
                        (*mutex).wchan.wait_list.push_front(wait_next);
                    }
                }
                let temp = Arc::from_raw(curr as *const SpinLock<RrosThread>);
                Arc::increment_strong_count(curr as *const SpinLock<RrosThread>);
                ret = check_lock_chain(owner.clone(), temp.clone());
            }
            owner_lock.raw_spin_unlock();
            if ret == Ok(0) {
                let curr_rq_lock = &mut (*(*curr.locked_data().get()).rq.unwrap()).lock;
                curr_rq_lock.raw_spin_lock();
                rros_sleep_on_locked(
                    timeout,
                    timeout_mode,
                    &(*((*mutex).clock)),
                    &mut (*mutex).wchan as *mut RrosWaitChannel,
                );
                curr_rq_lock.raw_spin_unlock();
                curr_lock.raw_spin_unlock();
                lock::raw_spin_unlock_irqrestore(flags);
                ret = wait_mutex_schedule(mutex);
                flags = lock::raw_spin_lock_irqsave();
            } else {
                curr_lock.raw_spin_unlock();
            }

            finish_mutex_wait(mutex);
            // TODO: monitor-steal lock here.
            curr_lock.raw_spin_lock();
            (*curr.locked_data().get()).wwake = 0 as *mut RrosWaitChannel;
            let curr_rq_lock = &mut (*(*curr.locked_data().get()).rq.unwrap()).lock;
            curr_rq_lock.raw_spin_lock();
            (*curr.locked_data().get()).info &= !T_WAKEN;
            if ret != Ok(0) {
                curr_rq_lock.raw_spin_unlock();
                curr_lock.raw_spin_unlock();
                lock::raw_spin_unlock_irqrestore(flags);
                return ret;
            }
            let info = (*curr.locked_data().get()).info;
            if info & T_ROBBED != 0 {
                curr_rq_lock.raw_spin_unlock();
                if (timeout_mode != RrosTmode::RrosRel
                    || timeout_infinite(timeout)
                    || rros_get_timer_delta((*(curr.locked_data().get())).rtimer.clone().unwrap())
                        != 0)
                {
                    curr_lock.raw_spin_unlock();
                    lock::raw_spin_unlock_irqrestore(flags);
                    continue;
                }
                curr_lock.raw_spin_unlock();
                lock::raw_spin_unlock_irqrestore(flags);
                return Err(kernel::Error::ETIMEDOUT);
            }
            curr_rq_lock.raw_spin_unlock();

            disable_inband_switch(curr.locked_data().get());
            if (*mutex).wchan.wait_list.is_empty() == false {
                currh = mutex_fast_claim(currh);
            }
            // lockp here 6
            pr_info!("mutex.rs: rros_lock_mutex_timeout: lockp here 4\n");
            atomic_set(lockp, get_owner_handle(currh, mutex) as i32);

            curr_lock.raw_spin_unlock();
            lock::raw_spin_unlock_irqrestore(flags);
            return ret;
        } // goto redo
    } // unsafe
}

pub fn transfer_ownership(mutex: *mut RrosMutex, lastowner: Arc<SpinLock<RrosThread>>) {
    unsafe {
        let lockp = (*mutex).fastlock;
        let mut n_ownerh: FundleT = 0;

        // assert_hard_lock(&mutex->lock);

        if (*mutex).wchan.wait_list.is_empty() {
            untrack_owner(mutex);
            // lockp here 5
            pr_info!("mutex.rs: transfer_ownership: lockp here 5\n");
            atomic_set(lockp, RROS_NO_HANDLE as i32);
            return;
        }

        // let n_owner = (*mutex).wchan.wait_list.get_head().unwrap().value.clone();
        let mut n_owner_raw = (*mutex).wchan.wait_list.cursor_front_mut();
        let n_owner = Arc::from_raw(
            n_owner_raw.current().unwrap() as *const _ as *const SpinLock<RrosThread>
        );
        Arc::increment_strong_count(
            n_owner_raw.current().unwrap() as *const _ as *const SpinLock<RrosThread>
        );
        let lock = &mut (*n_owner.deref().locked_data().get()).lock;
        lock.raw_spin_lock();
        (*n_owner.locked_data().get()).wwake = &mut (*mutex).wchan as *mut RrosWaitChannel;
        (*n_owner.locked_data().get()).wchan = null_mut();
        lock.raw_spin_unlock();
        (*mutex)
            .wchan
            .wait_list
            .remove(&RrosThreadWithLock::transmute_to_self(n_owner.clone()));
        set_current_owner_locked(mutex, n_owner.clone());
        rros_wakeup_thread(n_owner.clone(), T_PEND, T_WAKEN as i32);

        if (*mutex).flags & RROS_MUTEX_CLAIMED as i32 != 0 {
            clear_boost_locked(mutex, lastowner.clone(), RROS_MUTEX_CLAIMED as i32);
        }
        // n_ownerh = get_owner_handle(fundle_of(n_owner), mutex);
        let fundle = (*n_owner.deref().locked_data().get())
            .element
            .deref()
            .borrow()
            .deref()
            .fundle;
        n_ownerh = get_owner_handle(fundle, mutex);
        if (*mutex).wchan.wait_list.is_empty() == false {
            n_ownerh = mutex_fast_claim(n_ownerh);
        }
        // lockp here 4
        pr_info!("mutex.rs: transfer_ownership: lockp here 6\n");
        atomic_set(lockp, n_ownerh as i32);
    }
}

pub fn __rros_unlock_mutex(mutex: *mut RrosMutex) -> Result<i32> {
    let mut curr = unsafe { &mut *rros_current() };
    let owner = unsafe { Arc::from_raw(curr as *const SpinLock<RrosThread>) };
    unsafe {
        Arc::increment_strong_count(curr as *const SpinLock<RrosThread>);
    }
    let flags: u32 = 0;
    let mut currh: FundleT = 0;
    let mut h: FundleT = 0;
    let mut lockp = 0 as *mut bindings::atomic_t;

    // trace_evl_mutex_unlock(mutex);
    if (!enable_inband_switch(curr.locked_data().get())) {
        return Ok(0);
    }
    lockp = unsafe { (*mutex).fastlock };

    currh = unsafe { (*(curr.locked_data().get())).element.borrow().fundle };

    let flags = lock::raw_spin_lock_irqsave();
    let lock = unsafe { &mut (*curr.locked_data().get()).lock };
    lock.raw_spin_lock();

    unsafe {
        if (*mutex).flags & RROS_MUTEX_CEILING as i32 != 0 {
            clear_boost_locked(mutex, owner.clone(), RROS_MUTEX_CEILING as i32);
        }
    }
    h = atomic_read(lockp) as u32;
    // lockp here 3
    pr_info!("mutex.rs: __rros_unlock_mutex: lockp here 7\n");
    h = atomic_cmpxchg(lockp, h as i32, RROS_NO_HANDLE as i32) as u32;
    if (h & !RROS_MUTEX_FLCEIL) != currh {
        transfer_ownership(mutex, owner.clone());
    } else {
        if h != currh {
            // lockp here 2
            pr_info!("mutex.rs: __rros_unlock_mutex: lockp here 8\n");
            atomic_set(lockp, RROS_NO_HANDLE as i32);
        }
        untrack_owner(mutex);
    }
    lock.raw_spin_unlock();
    lock::raw_spin_unlock_irqrestore(flags);
    Ok(0)
}

pub fn rros_unlock_mutex(mutex: *mut RrosMutex) -> Result<usize> {
    unsafe {
        let curr = &mut *rros_current();
        // FundleT currh = fundle_of(curr), h;
        let currh: FundleT = (*(curr.locked_data().get())).element.borrow().fundle;
        let mut h: FundleT;

        premmpt::running_inband()?;

        h = rros_get_index(atomic_read((*mutex).fastlock) as u32);
        // h = evl_get_index(atomic_read(mutex->fastlock));
        // if (EVL_WARN_ON_ONCE(CORE, h != currh))
        // 	return;

        __rros_unlock_mutex(mutex);
        rros_schedule();
        Ok(0)
    }
}

// TODO: add trackers to support rros_drop_tracking_mutexes
// pub fn rros_drop_tracking_mutexes(curr: *mut RrosThread) {
//     unsafe {
//         let mut mutex = 0 as *mut RrosMutex;
//         let flags: u32 = 0;
//         let mut h: FundleT = 0;

//         let mut flags = lock::raw_spin_lock_irqsave();

//         while (*curr).trackers.is_empty() == false {
//             mutex = Arc::into_raw((*(*(*curr).trackers).get_head().unwrap()).value.clone())
//                 as *mut SpinLock<RrosMutex> as *mut RrosMutex;
//             lock::raw_spin_unlock_irqrestore(flags);
//             h = rros_get_index(atomic_read((*mutex).fastlock) as FundleT);
//             // if (h == fundle_of(curr)) {
//             // 	__rros_unlock_mutex(mutex);
//             // } else {
//             // 	// raw_spin_lock_irqsave(&mutex->lock, flags);
//             // 	if (*mutex).owner == curr{
//             // 		untrack_owner(mutex);
//             // 	}
//             // 	// raw_spin_unlock_irqrestore(&mutex->lock, flags);
//             // }
//             flags = lock::raw_spin_lock_irqsave();
//         }
//         lock::raw_spin_unlock_irqrestore(flags);
//     }
// }

pub fn wchan_to_mutex(wchan: *mut RrosWaitChannel) -> *mut RrosMutex {
    return kernel::container_of!(wchan, RrosMutex, wchan) as *mut RrosMutex;
}

pub fn rros_reorder_mutex_wait(
    waiter: Arc<SpinLock<RrosThread>>,
    originator: Arc<SpinLock<RrosThread>>,
) -> Result<i32> {
    pr_info!("mutex.rs: rros_reorder_mutex_wait: into");
    unsafe {
        let waiter_ptr = Arc::into_raw(waiter.clone()) as *mut SpinLock<RrosThread>;
        let waiter_ptr = (*waiter_ptr).locked_data().get();
        let originator_ptr = Arc::into_raw(originator.clone()) as *mut SpinLock<RrosThread>;
        let originator_ptr = (*originator_ptr).locked_data().get();

        let mutex = wchan_to_mutex((*waiter_ptr).wchan);
        let owner = (*mutex).owner.clone().unwrap();
        let owner_ptr = Arc::into_raw(owner.clone()) as *mut SpinLock<RrosThread>;
        let owner_ptr = (*owner_ptr).locked_data().get();
        // assert_hard_lock(&waiter->lock);
        // assert_hard_lock(&originator->lock);

        let mutex_lock = &mut (*mutex).lock as *mut bindings::hard_spinlock_t;
        raw_spin_lock(mutex_lock);
        if owner_ptr == originator_ptr {
            raw_spin_unlock(mutex_lock);
            return Err(kernel::Error::EDEADLK);
        }

        (*mutex)
            .wchan
            .wait_list
            .remove(&RrosThreadWithLock::transmute_to_self(waiter.clone()));
        // (*(*waiter_ptr).wait_next).remove();
        if (*mutex).wchan.wait_list.is_empty() {
            (*mutex)
                .wchan
                .wait_list
                .push_front(RrosThreadWithLock::transmute_to_self(waiter.clone()));
        } else {
            let mut flag = 1; // flag指示是否到头
            let mut cursor = (*mutex).wchan.wait_list.cursor_front_mut();
            while let Some(entry) = cursor.current() {
                let waiter_thread = Arc::from_raw(entry as *const _ as *const SpinLock<RrosThread>);
                Arc::increment_strong_count(entry as *const _ as *const SpinLock<RrosThread>);
                let wprio_in_list = (*waiter_thread.locked_data().get()).wprio;
                if (*waiter_ptr).wprio <= wprio_in_list {
                    flag = 0;
                    (*mutex).wchan.wait_list.insert_after(
                        NonNull::new(entry).unwrap(),
                        RrosThreadWithLock::transmute_to_self(waiter.clone()),
                    );
                    break;
                }
                cursor.move_next();
            }
            // for i in (*mutex).wchan.wait_list.len()..=1{
            // 	let wprio_in_list = (*(*mutex).wchan.wait_list.get_by_index(i).unwrap().value.clone().locked_data().get()).wprio;
            // 	if(*waiter_ptr).wprio <= wprio_in_list{
            // 		flag = 0;
            // 		(*mutex).wchan.wait_list.enqueue_by_index(i,(*((*waiter_ptr).wait_next)).value.clone());
            // 		break;
            // 	}
            // }
            if flag == 1 {
                // (*mutex).wchan.wait_list.add_head((*((*waiter_ptr).wait_next)).value.clone());
                (*mutex)
                    .wchan
                    .wait_list
                    .push_front(RrosThreadWithLock::transmute_to_self(waiter.clone()));
            }
        }

        if (*mutex).flags & RROS_MUTEX_PI as i32 == 0 {
            raw_spin_unlock(mutex_lock);
            return Ok(0);
        }

        (*mutex).wprio = (*waiter_ptr).wprio;
        let owner_lock = &mut (*owner.deref().locked_data().get()).lock;
        owner_lock.raw_spin_lock();

        if (*mutex).flags & RROS_MUTEX_CLAIMED as i32 != 0 {
            let boosters = &mut (*owner.deref().locked_data().get()).boosters;
            // TODO: 不确定是从哪个owner->boosters链表中删除next_booster
            Arc::increment_strong_count(mutex as *const RrosMutex);
            let next_booster = Arc::from_raw(mutex as *const RrosMutexWithBoosters);
        } else {
            (*mutex).flags |= RROS_MUTEX_CLAIMED as i32;
            raise_boost_flag(owner.clone());
        }

        Arc::increment_strong_count(mutex as *const RrosMutex);
        let next_booster = Arc::from_raw(mutex as *const RrosMutexWithBoosters);

        // FIXME: `boosters.is_empty()` may be included into `match cursor.current()` -> None path.
        let mut boosters = &mut (*owner.deref().locked_data().get()).boosters;
        if boosters.is_empty() {
            boosters.push_front(next_booster.clone());
        } else {
            let mut cursor = boosters.cursor_back_mut();
            while let Some(entry) = cursor.current() {
                if ((*mutex).wprio <= (*entry).deref().wprio) {
                    break;
                }
                cursor.move_prev();
            }
            match cursor.current() {
                Some(existing) => {
                    let existing =
                        NonNull::new(existing as *const _ as *mut RrosMutexWithBoosters).unwrap();
                    boosters.insert_after(existing, next_booster.clone())
                }
                None => {
                    boosters.push_front(next_booster.clone());
                }
            }
        }

        owner_lock.raw_spin_unlock();
        raw_spin_unlock(mutex_lock);
        return adjust_boost(
            owner.clone(),
            Some(waiter.clone()),
            mutex,
            originator.clone(),
        );
    }
}

pub fn rros_follow_mutex_depend(
    wchan: *mut RrosWaitChannel,
    originator: Arc<SpinLock<RrosThread>>,
) -> Result<i32> {
    // let wchan = Arc::into_raw(wchan) as *mut SpinLock<RrosWaitChannel> as *mut RrosWaitChannel;
    let originator_ref = Arc::into_raw(originator.clone()) as *mut SpinLock<RrosThread>;
    let originator_ref = unsafe { (*originator_ref).locked_data().get() };
    let mutex = wchan_to_mutex(wchan);
    let mut waiter = 0 as *mut RrosThread;
    let mut ret: Result<i32> = Ok(0);

    // assert_hard_lock(&originator->lock);

    let mutex_lock = unsafe { &mut (*mutex).lock as *mut bindings::hard_spinlock_t };
    raw_spin_lock(mutex_lock);
    unsafe {
        let owner_ref = (*mutex).owner.clone().unwrap().deref().locked_data().get();
        if owner_ref == originator_ref {
            raw_spin_unlock(mutex_lock);
            return Err(kernel::Error::EDEADLK);
        }

        let mut cursor = (*mutex).wchan.wait_list.cursor_front_mut();
        while let Some(entry) = cursor.current() {
            let waiter = Arc::from_raw(entry as *const _ as *const SpinLock<RrosThread>);
            Arc::increment_strong_count(entry as *const _ as *const SpinLock<RrosThread>);
            (*waiter.deref().locked_data().get()).lock.raw_spin_lock();
            let mut depend = (*waiter.locked_data().get()).wchan;
            if depend != null_mut() {
                let func;
                match (*depend).follow_depend.clone() {
                    Some(f) => func = f,
                    None => {
                        pr_warn!("rros_follow_mutex_depend:follow_depend function error");
                        return Err(kernel::Error::EINVAL);
                    }
                }
                ret = func(depend, originator.clone());
            }
            (*waiter.deref().locked_data().get()).lock.raw_spin_unlock();
            if ret != Ok(0) {
                break;
            }

            cursor.move_next();
        }
        raw_spin_unlock(mutex_lock);

        cursor.move_next();
    }
    raw_spin_unlock(mutex_lock);

    return ret;
}

pub fn rros_commit_mutex_ceiling(mutex: *mut RrosMutex) -> Result<i32> {
    unsafe {
        let curr = &mut *rros_current();
        let thread = unsafe { Arc::from_raw(curr as *const SpinLock<RrosThread>) };
        Arc::increment_strong_count(curr as *const SpinLock<RrosThread>);
        let lockp = (*mutex).fastlock;
        let flags: u32 = 0;
        let mut oldh: i32 = 0;
        let mut h: i32 = 0;

        let flags = lock::raw_spin_lock_irqsave();

        // if (!rros_is_mutex_owner(lockp, fundle_of(curr)) ||(mutex->flags & EVL_MUTEX_CEILING))
        // 	goto out;
        let fundle = (*curr.locked_data().get())
            .element
            .deref()
            .borrow()
            .deref()
            .fundle;
        if (!rros_is_mutex_owner(lockp, fundle)
            || ((*mutex).flags & RROS_MUTEX_CEILING as i32) != 0)
        {
            lock::raw_spin_unlock_irqrestore(flags);
            return Ok(0);
        }

        ref_and_track_owner(mutex, thread.clone());
        ceil_owner_priority(mutex, thread.clone());

        loop {
            h = atomic_read(lockp);
            // lockp here 1
            pr_info!("mutex.rs: rros_commit_mutex_ceiling: lockp here 9\n");
            oldh = atomic_cmpxchg(lockp, h, mutex_fast_ceil(h as u32) as i32);
            if oldh == h {
                break;
            }
        }
        // out:
        lock::raw_spin_unlock_irqrestore(flags);
        Ok(0)
    }
}

// pub fn test_mutex() -> Result<i32> {
//     pr_info!("mutex test in~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
//     pr_info!("test_mutex rros_current address is {:p}", rros_current());
//     let mut kmutex = RrosKMutex::new();
//     let mut kmutex = &mut kmutex as *mut RrosKMutex;
//     let mut mutex = RrosMutex::new();
//     unsafe { (*kmutex).mutex = &mut mutex as *mut RrosMutex };
//     rros_init_kmutex(kmutex);
//     pr_info!("init ok~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
//     rros_lock_kmutex(kmutex);
//     pr_info!("lock ok~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
//     rros_unlock_kmutex(kmutex);
//     pr_info!("unlock ok~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
//     Ok(0)
// }

pub fn rros_init_kmutex(kmutex: *mut RrosKMutex) {
    unsafe {
        atomic_set((*kmutex).fastlock, 0);
        rros_init_mutex_pi(
            (*kmutex).mutex,
            &mut RROS_MONO_CLOCK as *mut RrosClock,
            (*kmutex).fastlock,
        );
    }
}

pub fn rros_init_mutex_pi(
    mutex: *mut RrosMutex,
    clock: *mut RrosClock,
    fastlock: *mut bindings::atomic_t,
) {
    __rros_init_mutex(mutex, clock, fastlock, core::ptr::null_mut::<u32>());
}

pub fn rros_init_mutex_pp(
    mutex: *mut RrosMutex,
    clock: *mut RrosClock,
    fastlock: *mut bindings::atomic_t,
    ceiling: *mut u32,
) {
    __rros_init_mutex(mutex, clock, fastlock, ceiling);
}

pub fn rros_lock_kmutex(kmutex: *mut RrosKMutex) -> Result<i32> {
    unsafe { return rros_lock_mutex((*kmutex).mutex) };
}

pub fn rros_lock_mutex(mutex: *mut RrosMutex) -> Result<i32> {
    return rros_lock_mutex_timeout(mutex, timeout::RROS_INFINITE, timeout::RrosTmode::RrosRel);
}

pub fn rros_unlock_kmutex(kmutex: *mut RrosKMutex) -> Result<usize> {
    unsafe { return rros_unlock_mutex((*kmutex).mutex) };
}
