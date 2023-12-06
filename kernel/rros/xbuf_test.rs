use alloc::rc::Rc;
use core::cell::RefCell;

use crate::{
    factory::{rros_init_element, RrosElement, RROS_CLONE_PRIVATE},
    xbuf::*,
};

use kernel::{
    prelude::*,
};

fn test_xbuf() {
    test_create_xbuf();
    test_read_xbuf();
}

fn test_create_xbuf() -> Result<usize>{
    let mut xbuf = RrosXbuf::new()?;
    let e = xbuf.element.clone();

    rros_init_element(e, unsafe { &mut RROS_XBUF_FACTORY }, RROS_CLONE_PRIVATE); 

    Ok(0)
}

fn test_read_xbuf() -> Result<usize> {
    
    Ok(0)
}