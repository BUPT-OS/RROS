#[repr(C)]
pub struct ListHead {
    pub next: *mut ListHead,
    pub prev: *mut ListHead,
}
use core::ptr::null_mut;

impl Default for ListHead {
    fn default() -> Self {
        Self {
            next: null_mut(),
            prev: null_mut(),
        }
    }
}

impl ListHead {
    //添加节点到self和next之间
    #[allow(dead_code)]
    pub fn add(&mut self, new: *mut ListHead) {
        if self.is_empty() {
            self.prev = new;
            unsafe {
                (*new).next = self as *mut ListHead;
                (*new).prev = self as *mut ListHead;
            }
        } else {
            unsafe {
                (*self.next).prev = new;
                (*new).next = self.next;
                (*new).prev = self as *mut ListHead;
            }
        }
        self.next = new;
    }

    //空双向链表next和prev都指向自己
    #[allow(dead_code)]
    pub fn is_empty(&self) -> bool {
        self.next == null_mut() && self.prev == null_mut()
    }

    //list是不是head的最后一个节点
    #[allow(dead_code)]
    pub fn last_is(&self, list: *mut ListHead) -> bool {
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
