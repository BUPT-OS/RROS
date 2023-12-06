use core::cell::UnsafeCell;
use core::default::Default;
use core::mem::transmute;
use core::ops::{Deref, DerefMut};
use core::option::Option::None;
use core::pin::Pin;
use core::ptr::NonNull;
use core::result::Result::Ok;
use core::sync::atomic::{Ordering, AtomicBool};
use kernel::sync::Lock;
use kernel::{bindings, init_static_sync, spinlock_init, Result,pr_info};
use kernel::{sync::SpinLock};
use kernel::ktime::KtimeT;
use crate::clock::RROS_MONO_CLOCK;
use crate::sched::rros_schedule;
use crate::timeout::{rros_tmode, RROS_NONBLOCK};
use crate::wait::RROS_WAIT_PRIO;
use crate::work::RrosWork;
use super::device::NetDevice;
use super::socket::RrosSocket;
// use super::{socket::RrosSocket, input::RROSNetHandler};
struct CloneControl{
    pub(crate) queue : bindings::list_head,
    pub(crate) count : i32,
}
unsafe impl Sync for CloneControl{}
unsafe impl Send for CloneControl{}

struct RecyclingWork{
    pub(crate) work : RrosWork,
    pub(crate) count : i32,
    pub(crate) queue : bindings::list_head,
}

unsafe impl Sync for RecyclingWork{}
unsafe impl Send for RecyclingWork{}

init_static_sync! {
    static clone_queue: SpinLock<CloneControl> = CloneControl{
        queue: bindings::list_head{
            next: core::ptr::null_mut() as *mut bindings::list_head,
            prev: core::ptr::null_mut() as *mut bindings::list_head,
        },
        count: 0,
    };
    static recycler_work : SpinLock<RecyclingWork> = RecyclingWork{
        work : RrosWork::new(),
        count : 0,
        queue : bindings::list_head{
            next: core::ptr::null_mut() as *mut bindings::list_head,
            prev: core::ptr::null_mut() as *mut bindings::list_head,
        },
    };
}

const SKB_RECYCLING_THRESHOLD : usize = 64;
const NET_CLONES : usize = 128;
pub struct RrosNetCb{
    pub handler : fn (skb:RrosSkBuff),
    pub origin : *mut bindings::sk_buff,
    pub dma_addr : bindings::dma_addr_t,
    pub tracker : *mut RrosSocket,
}

fn maybe_kick_recycler(){
    unsafe{
        if (*recycler_work.locked_data().get()).count > SKB_RECYCLING_THRESHOLD as i32{
            (*recycler_work.locked_data().get()).work.call_inband();
        }
    }
}


/// Wraps the pointer of kernel's `struct sk_buff`.
pub struct RrosSkBuff(pub(crate) NonNull<bindings::sk_buff>);

impl Deref for RrosSkBuff{
    type Target = bindings::sk_buff;
    fn deref(&self) -> &Self::Target {
        unsafe{self.0.as_ref()}
    }
}

impl DerefMut for RrosSkBuff{
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe{self.0.as_mut()}
    }
}

impl RrosSkBuff{
    // rros_net_dev_alloc_skb
    // fn new(dev:*mut bindings::net_device,timeout:)
    pub fn from_raw_ptr(ptr:*mut bindings::sk_buff) -> Self{
        unsafe{Self(NonNull::new_unchecked(ptr))}
    }
    #[inline]
    pub fn net_cb_mut(&mut self) -> &mut RrosNetCb{
        unsafe{
            transmute(&mut self.0.as_mut().cb[0])
        }
    }
    pub fn net_clone_skb(&mut self) -> Option<Self>{
        extern "C"{
            fn rust_helper_list_empty(list:*const bindings::list_head)->bool;
            fn rust_helper_skb_morph_oob_skb(n:*mut bindings::sk_buff,skb:*mut bindings::sk_buff);
        }
        let flags = clone_queue.irq_lock_noguard();
        let clone_data_control = unsafe{&mut *clone_queue.locked_data().get()};
        let clone : *mut bindings::sk_buff = if unsafe{!rust_helper_list_empty(&clone_data_control.queue)}{
            clone_data_control.count -=1;
            pr_info!("clone skb count:{}",clone_data_control.count);
            list_get_entry!(&mut clone_data_control.queue,bindings::sk_buff,__bindgen_anon_1.list)
        }else{
            panic!("No more skb to clone");
        };
        clone_queue.irq_unlock_noguard(flags);

        unsafe{
            rust_helper_skb_morph_oob_skb(clone,self.0.as_ptr());
        }
        let mut clone = RrosSkBuff::from_raw_ptr(clone);
        let origin = self.net_cb_mut().origin;
        if origin == core::ptr::null_mut(){
            clone.net_cb_mut().origin = self.0.as_ptr();
        }else{
            clone.net_cb_mut().origin = origin;
        }
        return Some(clone);
    }
    pub fn alloc_one_skb(dev : &mut NetDevice)-> Option<Self>{
        extern "C"{
            // fn rust_helper_netdev_is_oob_capable(dev:*mut bindings::net_device)->bool;
            fn rust_helper_netdev_alloc_oob_skb(dev:*mut bindings::net_device,dma_addr:*mut bindings::dma_addr_t)->*mut bindings::sk_buff;
        }
        if !dev.is_oob_capable(){
            let est = unsafe{dev.dev_state_mut().as_mut()};
            let skb = unsafe{
                bindings::__netdev_alloc_oob_skb(dev.0.as_mut(), est.rstate.buf_size, bindings::GFP_KERNEL)
            };
            return Some(Self::from_raw_ptr(skb))
        }
        let mut dma_addr = unsafe {
            bindings::dma_addr_t::default()
        };
        let mut skb = unsafe{
            rust_helper_netdev_alloc_oob_skb(dev.0.as_ptr() ,&mut dma_addr)
        };
        if !skb.is_null(){
            let mut skb = Self::from_raw_ptr(skb);
            unsafe{skb.rros_control_cb().as_mut().dma_addr = dma_addr;}
            return Some(skb);
        }else {
            return None;
        }

    }
    pub fn dev_alloc_skb(dev:&mut NetDevice,timeout:KtimeT,tmode:rros_tmode)->Option<Self>{
        // rros_net_dev_alloc_skb
        extern "C"{
            fn rust_helper_list_empty(list:*const bindings::list_head)->bool;
        }
        if dev.is_vlan_dev(){
            return None;
        }
        let rst = unsafe{dev.dev_state_mut().as_mut()};
        let mut ret_skb = None;
        let mut flags = 0;
        loop{
            flags = unsafe{rust_helper_raw_spin_lock_irqsave(&mut rst.rstate.pool_wait.lock as *const _ as *mut bindings::hard_spinlock_t)};
            
            if !unsafe{rust_helper_list_empty(&mut rst.rstate.free_skb_pool)}{
                let skb = list_get_entry!(&mut rst.rstate.free_skb_pool,bindings::sk_buff,__bindgen_anon_1.list);
                rst.rstate.pool_free-=1;
                ret_skb = Some(Self::from_raw_ptr(skb));
                break;
            }

            if timeout == RROS_NONBLOCK{
                break;
            }
            pr_info!("I am in the dev_alloc_skb loop");
            rst.rstate.pool_wait.locked_add(timeout, tmode);
            pr_info!("I am in the dev_alloc_skb loop after adding");
            unsafe{rust_helper_raw_spin_unlock_irqrestore(&mut rst.rstate.pool_wait.lock as *const _ as *mut bindings::hard_spinlock_t, flags);}
            let ret = rst.rstate.pool_wait.wait_schedule();
            if ret !=0{
                break;
            }
        }
        unsafe{rust_helper_raw_spin_unlock_irqrestore(&mut rst.rstate.pool_wait.lock as *const _ as *mut bindings::hard_spinlock_t, flags);}
        return ret_skb;
    }
    #[inline]
    pub fn dev(&self) -> Option<NetDevice>{
        unsafe{
            NetDevice::from_ptr(self.0.as_ref().__bindgen_anon_1.__bindgen_anon_1.__bindgen_anon_1.dev)
        }
    }

    #[inline]
    pub fn set_dev(&mut self,dev:*mut bindings::net_device){
        unsafe{
            self.0.as_mut().__bindgen_anon_1.__bindgen_anon_1.__bindgen_anon_1.dev = dev;
        }
    }
    #[inline]
    pub fn is_oob(&self) -> bool{
        unsafe{
            self.0.as_ref().oob() != 0
        }
    }
    #[inline]
    pub fn set_oob_bit(&mut self, val:bool){
        unsafe{
            if val{
                self.0.as_mut().set_oob(1);
            }else{
                self.0.as_mut().set_oob(0);
            }
        }
    }
    #[inline]
    pub fn has_oob_clone(&self) -> bool{
        unsafe{
            self.0.as_ref().oob_cloned() != 0
        }
    }

    #[inline]
    pub fn is_oob_clone(&self) -> bool{
        unsafe{
            self.0.as_ref().oob_clone() != 0
        }
    }


    #[inline]
    pub fn list_mut(&mut self) -> *mut bindings::list_head{
        unsafe{
            &mut self.0.as_mut().__bindgen_anon_1.list as *mut bindings::list_head
        }
    }
    #[inline]
    pub fn rros_control_cb(&mut self) -> NonNull<RrosNetCb>{
        unsafe{
            NonNull::new(&self.0.as_mut().cb as *const _ as *mut RrosNetCb).unwrap()
        }
    }

    // #[inline]
    // pub fn oob_recycle(&mut self) -> bool{
    //     if !self.is_oob() || self.dev().is_none(){
    //         return false;
    //     }
    //     self.free();
    //     return true;
    // }
    
    #[inline]
    pub fn put(&mut self,len:u32){
        unsafe{
            bindings::skb_put(self.0.as_ptr(), len);
        }
    }
    
    pub fn free(&mut self){
        // rros_net_free_skb
        extern "C"{
           fn rust_helper_hard_irqs_disabled() -> bool;
        }
        unsafe{rust_helper_hard_irqs_disabled()};
        self.free_skb();

        unsafe{rros_schedule();}
        maybe_kick_recycler();
    }
    
    fn free_skb_inband(&mut self){
        extern  "C"{
            fn rust_helper_list_add(new: *mut bindings::list_head, head: *mut bindings::list_head);
        }
        let flags = recycler_work.irq_lock_noguard();
        unsafe{
            rust_helper_list_add(self.list_mut(), &mut (*recycler_work.locked_data().get()).queue);
        }
        unsafe{(*recycler_work.locked_data().get()).count += 1};
        recycler_work.irq_unlock_noguard(flags);

    }

    fn free_clone(&mut self){
        // free_skb_clone
        extern "C"{
            fn rust_helper_list_add(new: *mut bindings::list_head, head: *mut bindings::list_head);
        }
        let flags = clone_queue.irq_lock_noguard();
        unsafe{rust_helper_list_add(self.list_mut() as *mut bindings::list_head, &mut (*clone_queue.locked_data().get()).queue)};
        unsafe{(*clone_queue.locked_data().get()).count+=1};
        unsafe{pr_info!("free skb count:{}",(*clone_queue.locked_data().get()).count)};
        clone_queue.irq_unlock_noguard(flags);
    }

    fn free_to_dev(&mut self){
        // free_skb_to_dev
        extern  "C"{
            fn rust_helper_raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> u64;
            fn rust_helper_raw_spin_unlock_irqrestore(lock: *mut bindings::hard_spinlock_t, flags: u64);
        }
        let mut dev = self.dev().expect("free_to_dev: dev is none");
        let rst = unsafe{dev.dev_state_mut().as_mut()};
        let flags = unsafe{
            rust_helper_raw_spin_lock_irqsave(&mut rst.rstate.pool_wait.lock)
        };        
        list_add!(self.list_mut(),&mut rst.rstate.free_skb_pool);
        rst.rstate.pool_free += 1;
        if rst.rstate.pool_wait.is_active(){
            rst.rstate.pool_wait.wake_up(core::ptr::null_mut(), 0);
        }
        unsafe{
            rust_helper_raw_spin_unlock_irqrestore(&mut rst.rstate.pool_wait.lock, flags);
        }
    	// // rros_signal_poll_events(&est->poll_head,	POLLOUT|POLLWRNORM); // rros poll

    }
    fn free_skb(&mut self){
        // 对应 free_skb
        extern  "C"{
            fn skb_release_oob_skb(skb:*mut bindings::sk_buff,dref :*mut i32) -> bool;
            fn netdev_reset_oob_skb(dev:*mut bindings::net_device,skb:*mut bindings::sk_buff);
        }
        let dev = self.dev();
        if dev.is_none(){
            return;
        }
        let mut dev = dev.unwrap();
        if dev.is_vlan_dev(){
            return
        }
        /* TODO:
        * We might receive requests to free regular skbs, or
        * associated to devices for which diversion was just turned
        * off: pass them on to the in-band stack if so.
        * FIXME: should we receive that?
	    */
        // if (unlikely(!netif_oob_diversion(skb->dev)))
        // skb->oob = false;
        if (!self.is_oob()){
            if (!self.has_oob_clone()){
                self.free_skb_inband();
            }
            return;
        }

        let mut dref = 0;

        if !unsafe{skb_release_oob_skb(self.0.as_ptr(),&mut dref)}{
            return;
        }
        // if !skb_release_oob_skb // TODO:
        if (self.is_oob_clone()){
            let origin = unsafe{self.rros_control_cb().as_mut().origin};
            let mut origin = RrosSkBuff::from_raw_ptr(origin);
            origin.free_clone();
            if (!origin.is_oob()){
                assert!(dref == 1);
                if dref == 1{
                    origin.free_skb_inband();
                }else{
    				// RROS_WARN_ON(NET, dref < 1);
                }
                return
            }
            if (dref!=0){
                unsafe{netdev_reset_oob_skb(dev.0.as_mut(),origin.0.as_mut())};
                origin.free_to_dev();
            }
            return
        }
        if (dref==0){
            unsafe{netdev_reset_oob_skb(dev.0.as_mut(),self.0.as_mut())};
            self.free_to_dev();
        }
    }

    #[inline]
    pub fn reset_mac_header(&mut self){
        extern "C"{
            fn rust_helper_skb_reset_mac_header(skb: *mut bindings::sk_buff);
        }
        unsafe{rust_helper_skb_reset_mac_header(self.0.as_mut())};
    }
}


pub fn free_skb_list(list : *mut bindings::list_head){
    // rros_net_free_skb_list
    extern "C"{
        fn rust_helper_hard_irqs_disabled() -> bool;
        fn rust_helper_list_empty(list: *const bindings::list_head) -> bool;
     }
     unsafe{rust_helper_hard_irqs_disabled()};
     let list_is_empty = unsafe{
        rust_helper_list_empty(list)
     };
     if list_is_empty{
        return;
     }
     list_for_each_entry_safe!(skb,next,list,bindings::sk_buff,{
        let mut sk_buff = RrosSkBuff::from_raw_ptr(skb);
        sk_buff.free();
     },__bindgen_anon_1.list);

     unsafe{rros_schedule();}
     maybe_kick_recycler();
}



// pub struct RROSNetSkbQueue{
//     pub queue : bindings::list_head,
//     pub lock : SpinLock<()>, //不直接封装在上面是因为不够灵活，以后可以重新封装下
//     // phantom : core::marker::PhantomData<RrosSkBuff>,
// }


pub struct RrosSkbQueueInner{
    is_init : AtomicBool,
    list : bindings::list_head
}

unsafe impl Send for RrosSkbQueueInner{}
unsafe impl Sync for RrosSkbQueueInner{}

pub type RrosSkbQueue = SpinLock<RrosSkbQueueInner>;

impl RrosSkbQueueInner{
    #[inline]
    pub const fn default() -> Self{
        Self{
            is_init: AtomicBool::new(false),
            list: bindings::list_head{
            next : core::ptr::null_mut(),
            prev : core::ptr::null_mut(),
        }}
    }
    #[inline]
    pub fn init(&mut self){
        init_list_head!(&mut self.list);
        self.is_init.store(true, Ordering::Relaxed);
    }

    #[inline]
    pub fn is_empty(&self) -> bool{
        extern  "C"{
            fn rust_helper_list_empty(list: *const bindings::list_head) -> bool;
        }
        unsafe{
            rust_helper_list_empty(&self.list as *const bindings::list_head)
        }
    }

    #[inline]
    pub fn destory(&mut self){
        // rros_net_destroy_skb_queue
        free_skb_list(&mut self.list as *mut bindings::list_head);
    }
    pub fn get(&mut self) -> Option<RrosSkBuff>{
        // rros_net_get_skb_queue
        extern  "C"{
            fn rust_helper_list_empty(list: *const bindings::list_head) -> bool;
        }

        let list_is_empty = unsafe{
            rust_helper_list_empty(&self.list as *const bindings::list_head)
        };
        let skb  =if !list_is_empty{
            Some(RrosSkBuff::from_raw_ptr(list_get_entry!(&mut self.list as *mut bindings::list_head,bindings::sk_buff,__bindgen_anon_1.list)))
        }else{
            None
        };
        return skb;
    }
    pub fn move_queue(&mut self,list:*mut bindings::list_head)->bool{
        // rros_net_move_skb_queue
        extern "C"{
            fn rust_helper_list_empty(list:*mut bindings::list_head)->bool;
            fn rust_helper_list_splice_init(list:*mut bindings::list_head,head:*mut bindings::list_head);
        }
        let v = self.is_init.load(Ordering::Relaxed);
        if !v{
            self.init();
        }
        // move the node on self.queue to list
        let ret = unsafe{
            rust_helper_list_splice_init(&mut self.list,list);
            !rust_helper_list_empty(list)
        };
        ret
        // drop(guard);
    }

    pub fn add(&mut self, skb: &mut RrosSkBuff){
        // rros_net_add_skb_queue
        extern "C"{
            fn rust_helper_list_add_tail(new: *mut bindings::list_head, head: *mut bindings::list_head);
        }
        unsafe{
            rust_helper_list_add_tail(skb.list_mut(),&mut self.list);
        }
    }
}



pub fn rros_net_dev_build_pool(dev:&mut NetDevice) ->i32{
    extern "C"{
        fn rust_helper_list_add(new: *mut bindings::list_head, head: *mut bindings::list_head);
    }
    if dev.is_vlan_dev(){
        return -(bindings::EINVAL as i32);
    }
    if dev.netif_oob_diversion(){
        return -(bindings::EBUSY as i32);
    }
    let est = unsafe{dev.dev_state_mut().as_mut()};
    init_list_head!(&mut est.rstate.free_skb_pool);


    for n in 0..est.rstate.pool_max{
        let skb = RrosSkBuff::alloc_one_skb(dev);
        if let Some(mut skb) = skb{
            unsafe{
                rust_helper_list_add(skb.list_mut(),&mut est.rstate.free_skb_pool);
            }
        }else{
            unimplemented!();
			// TODO:rros_net_dev_purge_pool(dev);
            return -(bindings::ENOMEM as i32);

        }
    }

    est.rstate.pool_free = est.rstate.pool_max;
    unsafe{
        est.rstate.pool_wait.init(&mut RROS_MONO_CLOCK, RROS_WAIT_PRIO as i32);
    }
	// rros_init_poll_head(&est->poll_head);
    0
}

fn skb_recycler(work :&mut RrosWork)->i32{
    extern "C"{
        fn rust_helper_list_splice_init(list:*mut bindings::list_head,head:*mut bindings::list_head);
        fn rust_helper_skb_list_del_init(skb:*mut bindings::sk_buff)->bool; 
        fn rust_helper_dev_kfree_skb(skb:*mut bindings::sk_buff);
    }
    let mut list = bindings::list_head::default();
    init_list_head!(&mut list);

    let flags = recycler_work.irq_lock_noguard();
    unsafe{
        rust_helper_list_splice_init(&mut (*recycler_work.locked_data().get()).queue, &mut list);
    }
    list_for_each_entry_safe!(skb,next,&mut list,bindings::sk_buff,{
        unsafe{
            rust_helper_skb_list_del_init(skb);
            rust_helper_dev_kfree_skb(skb);
        }
    },__bindgen_anon_1.list);
    recycler_work.irq_unlock_noguard(flags);
    0
}

/// 初始化函数
pub fn rros_net_init_pools() -> Result<()>{
    extern "C"{
        fn rust_helper_list_add(new: *mut bindings::list_head, head: *mut bindings::list_head);
        fn rust_helper_INIT_LIST_HEAD(list: *mut bindings::list_head);
    }
    unsafe{rust_helper_INIT_LIST_HEAD(&mut (*(*clone_queue.locked_data()).get()).queue)};
    unsafe{(*(*clone_queue.locked_data()).get()).count = NET_CLONES as i32};
    let head = unsafe{
        &mut (*(*clone_queue.locked_data()).get()).queue
    };
    // clone_queue.
    for n in 0..NET_CLONES{
        let clone =unsafe{
            bindings::skb_alloc_oob_head(bindings::GFP_KERNEL)
        };
        if clone.is_null(){
            unimplemented!()
            // failed
        }
        unsafe{
            rust_helper_list_add(RrosSkBuff::from_raw_ptr(clone).list_mut(),head);
        }
    }

    unsafe{(&mut *recycler_work.locked_data().get()).work.init(skb_recycler)};
    unsafe{
        rust_helper_INIT_LIST_HEAD(&mut (*(*recycler_work.locked_data()).get()).queue);
    }

    Ok(())
    // TODO: failed
}

extern "C" {
    fn rust_helper_raw_spin_lock_irqsave(lock: *mut bindings::hard_spinlock_t) -> u64;
    fn rust_helper_raw_spin_unlock_irqrestore(lock: *mut bindings::hard_spinlock_t, flags: u64);
}

#[no_mangle]
pub fn skb_oob_recycle(skb : *mut bindings::sk_buff) -> bool{
    let mut skb = RrosSkBuff::from_raw_ptr(skb);
    if !skb.is_oob() || skb.dev().is_none(){
        return false;
    }
    skb.free();
    true
}