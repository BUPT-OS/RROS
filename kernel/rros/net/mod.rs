use core::result::Result::Ok;

use kernel::{Result, bindings::sock_register};
mod socket;
mod packet;
mod skb;
mod input;
mod output;
mod ethernet;
mod constant;
// mod qdisc;
mod device;

pub use device::netif_oob_switch_port;

use self::{skb::rros_net_init_pools, ethernet::input::{rros_net_store_vlans, rros_show_vlans}, socket::rros_family_ops, output::rros_init_tx_irqwork};
use kernel::bindings;

const RROS_DOMAIN_HASH_BITS: u8 = 8;

#[macro_export]
macro_rules! sk_prot{
	($x:ident) => {
		unsafe{(*$x).__sk_common.skc_prot}
    };
}

#[macro_export]
macro_rules! RROS_NET_CB{
    ($skb:ident) => {
        unsafe{
            &(*$skb).cb[0] as *const _ as *mut skb::RrosNetCb
        }
    };
}

pub fn set_42_as_oob_port(){
    let mut msg: [u8; 6] = [0; 6];
    msg.copy_from_slice("42\0\0\0\0".as_bytes());
    rros_net_store_vlans(&msg as *const _ as *const u8, 3);
}

pub fn init()->Result<()>{
    rros_net_init_pools()?;
    set_42_as_oob_port();
    rros_show_vlans();
    rros_init_tx_irqwork();
	// rros_net_init_tx();

	// rros_net_init_qdisc();
    // init_rros_af_oob_proto();
    // let ret = unsafe{bindings::proto_register(&mut rros_af_oob_proto, 0)};
    // if ret !=0{
    //     panic!()
    // }
    unsafe{
        sock_register(rros_family_ops.get_ptr());
    }
    Ok(())
}
// bitmap
// #[macro_export]
// macro_rules! DECLARE_BITMAP {
//     ($name,$bits) => {

//         static mut $name: [usize;rust_helper_BITS_TO_LONGS($bits)] = [0;rust_helper_BITS_TO_LONGS($bits)];
//     };
// }



/**
 * VLAN
 */
#[macro_export]
macro_rules! skb_vlan_tag_present {
    ($skb:ident) => {
        (*$skb)._bitfield_3.0[0]
    };
}

