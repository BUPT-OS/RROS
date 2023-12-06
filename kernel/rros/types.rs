use kernel::bindings::{self, _raw_spin_unlock_irqrestore};
use kernel::c_types::c_ulong;
use kernel::{container_of,init_static_sync, Opaque};
use kernel::sync::Mutex;

/// Unofficial Wrap for some binding struct.
/// Now are mostly used in net module.
/// Here's a simple list:
/// * macro for list_head
/// 
/// 



/// List macro and method
/// 
#[macro_export]
macro_rules! list_entry {
    ($ptr:expr, $type:ty, $($f:tt)*) => {{
        unsafe{kernel::container_of!($ptr, $type, $($f)*) as *mut $type}
    }}
}

#[macro_export]
macro_rules! list_first_entry {
    ($ptr:expr, $type:ty, $($f:tt)*) => {{
        list_entry!((*$ptr).next, $type, $($f)*)
    }}
}

#[macro_export]
macro_rules! list_last_entry {
    ($ptr:expr, $type:ty, $($f:tt)*) => {{
        list_entry!((*$ptr).prev, $type, $($f)*)
    }}
}

#[macro_export]
macro_rules! list_next_entry {
    ($pos:expr, $type:ty,$($f:tt)*) => {
        unsafe {
            let ptr = ((*$pos).$($f)*).next;
            list_entry!(ptr,$type, $($f)*)
        }
    };
}

#[macro_export]
macro_rules! list_prev_entry {
    ($pos:expr, $type:ty,$($f:tt)*) => {
        unsafe {
            let ptr = ((*$pos).$($f)*).prev;
            list_entry!(ptr,$type, $($f)*)
        }
    };
}

#[macro_export]
macro_rules! list_entry_is_head {
    ($pos:expr,$head:expr,$($f:tt)*) => {
        unsafe{
            core::ptr::eq(&(*$pos).$($f)*,$head)
        }
    };
}


#[macro_export]
macro_rules! init_list_head {
    ($list:expr) => {
        extern "C"{
            fn rust_helper_INIT_LIST_HEAD(list: *mut $crate::bindings::list_head);
        }
        unsafe{
            rust_helper_INIT_LIST_HEAD($list as *mut $crate::bindings::list_head);
        }
    };
}

#[inline]
pub fn list_empty(list:*const bindings::list_head) -> bool{
    unsafe{
        (*list).next as *const bindings::list_head == list
    }
}

#[macro_export]
macro_rules! list_empty {
    ($list_head_ptr:expr) => {
        extern "C"{
            fn rust_helper_list_empty(list: *const $crate::bindings::list_head) -> bool;
        }
        unsafe{
            rust_helper_list_empty($list_head_ptr as *const $crate::bindings::list_head)
        }
    };
}

#[macro_export]
macro_rules! list_del {
    ($list:expr) => {
        extern "C"{
            fn rust_helper_list_del(list: *mut $crate::bindings::list_head);
        }
        unsafe{
            rust_helper_list_del($list as *mut $crate::bindings::list_head);
        }
    };
}

#[macro_export]
macro_rules! list_del_init {
    ($list:expr) => {
        extern "C"{
            fn rust_helper_list_del_init(list: *mut $crate::bindings::list_head);
        }
        unsafe{
            rust_helper_list_del_init($list as *mut $crate::bindings::list_head);
        }
    };
}


/// 获取当前链表节点，并将其从链表中移出去
#[macro_export]
macro_rules! list_get_entry{
    ($head:expr,$type:ty,$($f:tt)*) => {
        {
            let item = $crate::list_first_entry!($head,$type,$($f)*);
            $crate::list_del!(&mut (*item).$($f)*);
            item
        }
    };
}
#[macro_export]
macro_rules! list_add_tail{
    ($new:expr,$head:expr) => {
        extern "C"{
            fn rust_helper_list_add_tail(new: *mut $crate::bindings::list_head, head: *mut $crate::bindings::list_head);
        }
        unsafe{
            rust_helper_list_add_tail($new as *mut $crate::bindings::list_head,$head as *mut $crate::bindings::list_head);
        }
    };
}

#[macro_export]
macro_rules! list_add{
    ($new:expr,$head:expr) => {
        extern "C"{
            fn rust_helper_list_add(new: *mut $crate::bindings::list_head, head: *mut $crate::bindings::list_head);
        }
        unsafe{
            rust_helper_list_add($new as *mut $crate::bindings::list_head,$head as *mut $crate::bindings::list_head);
        }
    };
}

// 常规实现
#[macro_export]
macro_rules! list_for_each_entry{
    ($pos:ident,$head:expr,$type:ty,$e:block,$($f:tt)*) => {
        let mut $pos = list_first_entry!($head,$type,$($f)*);
        while !list_entry_is_head!($pos,$head,$($f)*){
            $e;
            $pos = list_next_entry!($pos,$type,$($f)*);
        }
    };
}

#[macro_export]
macro_rules! list_for_each_entry_safe {
    ($pos:ident,$n:ident,$head:expr,$type:ty,$e:block,$($f:tt)*) => {
        let mut $pos = list_first_entry!($head,$type,$($f)*);
        let mut $n = list_next_entry!($pos,$type,$($f)*);
        while !list_entry_is_head!($pos,$head,$($f)*){
            $e;
            $pos = $n;
            $n = list_next_entry!($n,$type,$($f)*);
        }
    };
}

#[macro_export]
macro_rules! list_for_each_entry_reverse {
    ($pos:ident,$head:expr,$type:ty,$e:block,$($f:tt)*) => {
        let mut $pos = list_last_entry!($head,$type,$($f)*);
        while !list_entry_is_head!($pos,$head,$($f)*){
            $e;
            $pos = list_prev_entry!($pos,$type,$($f)*);
        }
    };
}

#[macro_export]
macro_rules! list_add_priff {
    ($new:expr,$head:expr, $member_pri:ident,$member_next:ident,$tp:ty) => {
        {
            extern "C"{
                fn rust_helper_list_add(new: *mut $crate::bindings::list_head, head: *mut $crate::bindings::list_head);
            }
            let next = unsafe{(*$head).next};
            if core::ptr::eq(next,$head){
                unsafe{rust_helper_list_add(unsafe{&mut (*$new).$member_next} as *mut $crate::bindings::list_head,$head as *mut $crate::bindings::list_head)};
            }else{
                let mut pos : *mut $tp;
                $crate::list_for_each_entry_reverse!(pos,$head,$tp,{
                    if unsafe{(*$new).$member_pri} <= unsafe{(*pos).$member_pri} {
                        break;
                    }
                },$member_next);
                unsafe{rust_helper_list_add(unsafe{&mut (*$new).$member_next} as *mut $crate::bindings::list_head,unsafe{&mut (*pos).$member_next} as *mut $crate::bindings::list_head)};
            }
        }        
    };
}



pub struct Hashtable<const N:usize>{
    pub table : [bindings::hlist_head; N]
}

unsafe impl<const N:usize> Sync for Hashtable<N> {}
unsafe impl<const N:usize> Send for Hashtable<N> {}
impl<const N:usize> Hashtable<N>{
    pub const fn new() -> Self{
        let table = [bindings::hlist_head{first:core::ptr::null_mut()}; N];
        Self{
            table:table
        }
    }
    pub fn add(&mut self,node:&mut bindings::hlist_node, key: u32) {
        extern "C" {
            fn rust_helper_hash_add(ht: *mut bindings::hlist_head,length:usize,node: *mut bindings::hlist_node,key: u32);
        }
        unsafe {
            rust_helper_hash_add(&self.table as *const _ as *mut bindings::hlist_head,N,node as *mut bindings::hlist_node,key);
        }
    }

    pub fn del(&self,node:&mut bindings::hlist_node){
        extern "C"{
            fn rust_helper_hash_del(node:*mut bindings::hlist_node);
        }
        unsafe{
            rust_helper_hash_del(node as *mut bindings::hlist_node);
        }
    }
    pub fn head(&mut self,key :u32)->*const bindings::hlist_head{
        extern "C"{
            fn rust_helper_get_hlist_head(ht: *const bindings::hlist_head,length:usize,key:u32)->*const bindings::hlist_head;
        }
        unsafe{
            rust_helper_get_hlist_head(&self.table as *const bindings::hlist_head,N,key)
        }
    }
}

#[macro_export]
macro_rules! initialize_lock_hashtable{
    ($name:ident,$bits_to_shift:expr) => {
        kernel::init_static_sync! {
            static $name: kernel::sync::Mutex<Hashtable::<$bits_to_shift>> = Hashtable::<$bits_to_shift>::new();
        }
    }
}

#[macro_export]
macro_rules! hlist_entry{
    ($ptr:expr,$type:ty,$($f:tt)*) =>{
        kernel::container_of!($ptr,$type,$($f)*)
    }
}
#[macro_export]
macro_rules! hlist_entry_safe{
    ($ptr:expr,$type:ty,$($f:tt)*) =>{
        if ($ptr).is_null(){
            core::ptr::null()
        }else{
            kernel::container_of!($ptr,$type,$($f)*)
        }
    }
}

#[macro_export]
macro_rules! hash_for_each_possible {
    ($pos:ident,$head:expr,$type:ty,$member:ident,{ $($block:tt)* } ) => {
        let mut $pos = $crate::hlist_entry_safe!(unsafe{(*$head).first},$type,$member);
        while(!$pos.is_null()){
            // $code
            $($block)*
            $pos = $crate::hlist_entry_safe!(unsafe{(*$pos).$member.next},$type,$member);
        }
    };
}

macro_rules! bits_to_long {
    ($bits:expr) => {
        ((($bits) + (64) - 1) / (64)) 
    };
}

#[macro_export]
macro_rules! DECLARE_BITMAP {
    ($name:ident,$bits:expr) => {
        static mut $name: [usize;bits_to_long!($bits)] = [0;bits_to_long!($bits)];
    };
}

// pub struct HardSpinlock<T>{
//     spin_lock: Opaque<bindings::spinlock>,
//     flags:usize,
//     _pin: PhantomPinned,
//     data: UnsafeCell<T>,
// }

// unsafe impl Sync for HardSpinlock {}
// unsafe impl Send for HardSpinlock {}

// impl<T> HardSpinlock<T>{
//     pub fn new(data:T) -> Self{
//         extern "C"{
//             fn rust_helper_raw_spin_lock_init(lock:*mut bindings::spinlock_t);
//         }
//         let t = bindings::hard_spinlock_t::default();
//         unsafe{
//             rust_helper_raw_spin_lock_init(&t as *const _ as *mut bindings::spinlock_t);
//         }
//         Self{
//             spin_lock : Opaque(t),
//             flags:0,
//             _pin:PhantomPinned,
//             data:UnsafeCell::new(data),
//         }
//     }

//     pub fn lock(&mut self) -> usize{
//         unsafe{
//             _raw_spin_lock_irqsave(&mut self.0 as *const _ as *mut bindings::raw_spinlock_t) as usize
//         }
//     }

//     pub fn unlock(&mut self,flags:usize){
//         unsafe{
//             _raw_spin_unlock_irqrestore(&mut self.0 as *const _ as *mut bindings::raw_spinlock_t,flags as c_ulong);
//         }
//     }
// }