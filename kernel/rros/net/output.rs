use core::{ffi::c_void, mem::align_of, ops::Deref};

use kernel::{Result,bindings::{self, _raw_spin_lock_irq}, irq_work::IrqWork, percpu::alloc_per_cpu, sync::{SpinLock, Lock}, init_static_sync, Error, pr_info};
use crate::{list_next_entry, list_entry_is_head, net::{skb::RrosSkbQueueInner, socket::uncharge_socke_wmem}};

use super::{device::{OOBNetdevState}, skb::RrosSkBuff};
use kernel::percpu;

// NOTE:initialize in rros_net_init_tx
// TODO: 这里的实现没用DEFINE_PER_CPU，因为Rust还没有支持静态定义的percpu变量
init_static_sync! {
    static oob_tx_relay : SpinLock<RrosSkbQueueInner> = RrosSkbQueueInner::default();
}
static mut oob_xmit_work : IrqWork = unsafe{core::mem::transmute::<[u8; core::mem::size_of::<IrqWork>()], IrqWork>( [0; core::mem::size_of::<IrqWork>()])};


// fn oob_start_xmit(dev: *mut bindings::net_device, skb: *mut bindings::sk_buff) -> i32 {
//     unsafe{
//         (*(*dev).netdev_ops).ndo_start_xmit.unwrap()(skb, dev)
//     }
// }

// fn do_tx(qdisc: *mut RROSNetQdisc,dev:*mut bindings::net_device,skb:*mut bindings::sk_buff) {
//     rros_unchange_socket_wmem(skb);  //TODO:
//     let result = oob_start_xmit(dev, skb);
//     match result{
//         bindings::netdev_tx_NETDEV_TX_OK => {},
//         _ => {// busy, or whatever
//             unsafe{
//                 (*qdisc).pack_dropped += 1;
//             }
//             rros_net_free_skb(skb); //TODO:
//         }
//     }
// }

// fn rros_net_do_tx(arg: *mut c_void){
//     extern "C"{
//         fn rust_helper_list_del_init(list: *mut bindings::list_head);
//     }
//     let list = bindings::list_head::default();
//     init_as_list_head!(list);
//     let dev = unsafe{
//         arg as *mut bindings::net_device
//     };
//     let est = unsafe{
//         (*dev). // TODO: estate
//     }
//     while !rros_kthread_should_stop(){
        
//         let ret = rros_wait_flag(unsafe{&(*est).flag});
//         if ret{
//             break;
//         }
//         let qdisc : *mut RROSNetQdisc = unsafe{
//             (*est).qdisc
//         };
//         loop{
//             let skb = unsafe{
//                 (*(*qdisc).oob_ops).dequeue(qdisc)
//             };
//             if skb.is_null(){
//                 break;
//             }
//             do_tx(qdisc,dev,skb);
//         }
//         let inband_q = unsafe{
//             &(*qdisc).inband_q
//         };
//         if inband_q.move_queue(&list){
//             // TODO: use macro instead
//             let mut skb = list_first_entry!(&list,bindings::sk_buff,__bindgen_anon_1.list);
//             let mut n = list_next_entry!(pos,bindings::sk_buff,__bindgen_anon_1.list);
//             while !list_entry_is_head!(pos,&list,__bindgen_anon_1.list){
//                 unsafe{
//                     rust_helper_list_del_init(&(*skb).__bindgen_anon_1.list);
//                 }
//                 do_tx(qdisc, dev, skb);
//                 // process next skb
//                 pos = n;
//                 n = list_next_entry!(n,bindings::sk_buff,__bindgen_anon_1.list);
//             }
//         }
//     }
// }

// inband
#[no_mangle]
fn skb_inband_xmit_backlog(){
    extern "C"{
        fn rust_helper_this_cpu_ptr(ptr: *mut c_void) -> *mut c_void;
    }
    let mut list = bindings::list_head::default();
    init_list_head!(&mut list);
    let flags = oob_tx_relay.irq_lock_noguard(); // TODO: 是否需要lock

    if unsafe{(*oob_tx_relay.locked_data().get()).move_queue(&mut list)}{
        list_for_each_entry_safe!(skb,n,&mut list,bindings::sk_buff,{
            let mut ref_skb = RrosSkBuff::from_raw_ptr(skb);
            uncharge_socke_wmem(&mut ref_skb);
            unsafe{bindings::dev_queue_xmit(skb)};
        },__bindgen_anon_1.list);
    }
    oob_tx_relay.irq_unlock_noguard(flags);
}



// fn xmit_oob(dev : *mut bindings::net_device, skb : *mut bindings::sk_buff) -> i32{
//     // TODO: est,skb_inband_xmit_backlog,rros_raise_flag
//     let est = unsafe{
//         let ptr  = (*dev).oob_context.dev_state.wrapper as *mut OOBNetdevState;
//         (*ptr).estate
//     };
//     let ret = rros_net_sched_packet(dev,skb);
//     if ret{
//         ret
//     }
//     rros_raise_flag(&est.tx_flag);
//     return 0;
// }

pub fn rros_net_transmit(mut skb : &mut RrosSkBuff) -> Result<()>{
    let dev = skb.dev();
    if dev.is_none(){
        return Err(Error::EINVAL);
    }
    let dev = dev.unwrap();
    if dev.is_vlan_dev(){
        return Err(Error::EINVAL);
    }

    if unsafe{!skb.__bindgen_anon_2.sk.is_null()}{ // sk_buff->sk
        return Err(Error::EINVAL);
    }

    if !skb.is_oob(){
        return Err(Error::EINVAL);
    }
    // if dev.is_oob_capable(){
    //     return xmit_oob()
    // }

    if kernel::premmpt::running_inband().is_ok(){
        uncharge_socke_wmem(&mut skb);
        unsafe{bindings::dev_queue_xmit(skb.0.as_ptr())};
    }
    
    let flags = oob_tx_relay.irq_lock_noguard();
    unsafe{(*oob_tx_relay.locked_data().get()).add(skb)};
    oob_tx_relay.irq_unlock_noguard(flags);
    unsafe{
        bindings::irq_work_queue(oob_xmit_work.get_ptr());
    }
    Ok(())
}

// fn netif_xmit_oob(skb : *mut bindings::sk_buff) -> i32{
//     if xmit_oob(unsafe{(*skb).__bindgen_anon_1.__bindgen_anon_1.__bindgen_anon_1.dev}, skb){
//         bindings::NET_XMIT_DROP
//     }else{
//         bindings::NET_XMIT_SUCCESS
//     }
// }

unsafe extern "C" fn xmit_inband(work : *mut bindings::irq_work){
    unsafe{
        bindings::__raise_softirq_irqoff(bindings::NET_TX_SOFTIRQ);
    }
}

pub fn rros_init_tx_irqwork(){
    unsafe{
        // oob_tx_relay = alloc_per_cpu(size_of::<RROSNetSkbQueue<*mut bindings::sk_buff>>(),align_of::<RROSNetSkbQueue<*mut bindings::sk_buff>>());
        oob_xmit_work.init_irq_work(xmit_inband).unwrap();
    }
}
// }