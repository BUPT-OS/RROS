use alloc::rc::Rc;
use core::{
    cell::RefCell,
    mem::MaybeUninit
};

use kernel::{bindings, c_types::c_void, irq_work::IrqWork, container_of, Result, pr_info};

use crate::factory::RrosElement;
pub struct RrosWork{
    irq_work : IrqWork,
    wq_work : bindings::work_struct,
    wq : *mut bindings::workqueue_struct,
    pub handler : Option<fn (arg:&mut RrosWork) -> i32>,
    // element : Rc<RefCell<RrosElement>>
    element : Option<Rc<RefCell<RrosElement>>>
}


fn do_wq_work(wq_work : *mut bindings::work_struct){
    let work = container_of!(wq_work, RrosWork, wq_work);
    let handler =unsafe{
        (*work).handler.unwrap()
    };
    let work = unsafe{&mut *(work as *mut RrosWork)};
    handler(work);

    // TODO:     
    // if (work->element)
    // rros_put_element(work->element);
}

unsafe extern "C" fn do_irq_work(irq_work : *mut bindings::irq_work){
    extern "C"{
        fn rust_helper_queue_work(wq:*mut bindings::workqueue_struct,work:*mut bindings::work_struct) -> bool;
    }
    let work = container_of!(irq_work, RrosWork, irq_work) as *mut RrosWork;
    if unsafe { !bindings::queue_work_on(bindings::WORK_CPU_UNBOUND as _, (*work).wq, &mut (*work).wq_work as *mut bindings::work_struct) && (*work).element.is_some() } {
        pr_info!("uncompleted rros_put_element()");
    }
    // TODO: 没有实现rros_put_element
    // if unsafe{rust_helper_queue_work((*work).wq,&mut (*work).wq_work)} && unsafe{(*)}
    // if (!queue_work(work->wq, &work->wq_work) && work->element)
    // rros_put_element(work->element);
}

impl RrosWork{
    pub const fn new() -> Self{
        unsafe{
            core::mem::transmute::<[u8; core::mem::size_of::<Self>()], Self>( [0; core::mem::size_of::<Self>()])
        }
        // RrosWork{
        //     element : None,
        //     // element: Rc::try_new(RefCell::new(RrosElement::new().unwrap())).unwrap(),
        //     handler : None,
        //     wq : core::ptr::null_mut(),
        //     wq_work : bindings::work_struct{
        //         data : bindings::atomic64_t { counter: 0 },
        //         entry : bindings::list_head{
        //             next : core::ptr::null_mut(),
        //             prev : core::ptr::null_mut(),
        //         },
        //         // func : Some(0 as extern "C" fn(*mut bindings::work_struct)),
        //         func: None
        //     },
        //     irq_work : IrqWork::new()
        // }
    }
    pub fn init(&mut self,handler : fn (arg:&mut RrosWork) -> i32){
        extern "C"{
            fn rust_helper_init_work(work:*mut bindings::work_struct,func:fn(*mut bindings::work_struct));
        }
        self.irq_work.init_irq_work(do_irq_work);
        unsafe{rust_helper_init_work(&mut self.wq_work, do_wq_work)};
        self.handler = Some(handler);
        self.element = Some(Rc::try_new(RefCell::new(RrosElement::new().unwrap())).unwrap());

    }
    pub fn init_safe(&mut self,handler : fn (arg:&mut RrosWork) -> i32, element: Rc<RefCell<RrosElement>>){
        extern "C"{
            fn rust_helper_init_work(work:*mut bindings::work_struct,func:fn(*mut bindings::work_struct));
        }
        self.irq_work.init_irq_work(do_irq_work);
        unsafe{rust_helper_init_work(&mut self.wq_work, do_wq_work)};
        self.handler = Some(handler);
        self.element = Some(element);

    }
    pub fn call_inband_from(&mut self,wq:*mut bindings::workqueue_struct){
        self.wq = wq;
        // TODO: 没有实现rros_put_element
        // if (work->element)
        if self.element.is_some() {
            pr_info!("uncompleted rros_get_element()");
        }
		// rros_get_element(work->element);
        if self.irq_work.irq_work_queue().is_err() && self.element.is_some() {
            pr_info!("uncompleted rros_put_element()")
        }
        // if (!irq_work_queue(&work->irq_work) && work->element)
		// rros_put_element(work->element);
        // unsafe{rust_helper_queue_work(wq,&mut self.wq_work)};
    }

    #[inline]
    pub fn call_inband(&mut self){
        self.call_inband_from(unsafe{bindings::system_wq});
    }
}

