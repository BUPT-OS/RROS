use crate::sched;
use alloc::rc::Rc;
use core::cell::RefCell;
use kernel::{bindings, prelude::*, Error};

extern "C" {
    fn rust_helper_INIT_LIST_HEAD(list: *mut sched::list_head);
    fn rust_helper_list_del(list: *mut sched::list_head);
}

// pub fn rros_init_schedq(q: Rc<RefCell<sched::rros_sched_queue>>) {
//     let mut q_ptr = q.borrow_mut();
//     init_list_head(&mut q_ptr.head as *mut sched::list_head);
// }

// fn init_list_head(list: *mut sched::list_head) {
//     unsafe { rust_helper_INIT_LIST_HEAD(list) };
// }

// pub fn rros_get_schedq(struct rros_sched_queue *q){
// 	if (list_empty(&q->head))
// 		return NULL;

// 	return list_get_entry(&q->head, struct rros_thread, rq_next);
// }
