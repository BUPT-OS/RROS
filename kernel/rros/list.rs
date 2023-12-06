use kernel::{bindings, container_of, percpu_defs, prelude::*, str::CStr, sync::SpinLock};
pub struct list_head {
    pub next: *mut list_head,
    pub prev: *mut list_head,
}
use core::ptr::{null, null_mut};

impl Default for list_head {
    fn default() -> Self {
        Self {
            next: null_mut(),
            prev: null_mut(),
        }
    }
}

impl list_head {
    //添加节点到self和next之间
    pub fn add(&mut self, new: *mut list_head) {
        if self.is_empty() {
            self.prev = new;
            unsafe {
                (*new).next = self as *mut list_head;
                (*new).prev = self as *mut list_head;
            }
        } else {
            unsafe {
                (*self.next).prev = new;
                (*new).next = self.next;
                (*new).prev = self as *mut list_head;
            }
        }
        self.next = new;
    }

    //空双向链表next和prev都指向自己
    pub fn is_empty(&self) -> bool {
        self.next == null_mut() && self.prev == null_mut()
    }

    //list是不是head的最后一个节点
    pub fn last_is(&self, list: *mut list_head) -> bool {
        self.prev == list
    }

    //释放本身节点
    pub fn list_drop(&mut self) {
        if !self.is_empty() {
            if self.next == self.prev {
                unsafe {
                    (*self.next).next = null_mut();
                    (*self.next).prev = null_mut();
                }
            } else {
                unsafe {
                    (*self.next).prev = self.prev;
                    (*self.prev).next = self.next;
                }
            }
        }
    }
}
