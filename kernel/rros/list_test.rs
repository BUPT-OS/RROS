//list.rs测试文件！
use crate::list::*;
use kernel::prelude::*;
use crate::{list_first_entry,list_entry,list_last_entry};
struct ListTest {
    num: i32,
    head: list_head,
}

impl ListTest {
    fn new(num: i32, head: list_head) -> ListTest {
        ListTest { num, head }
    }
}

fn print_info() {}

fn traverse_list(head: &list_head) -> i32 {
    let mut count = 0;
    if head as *const list_head == 0 as *const list_head {
    } else if head.is_empty() {
        count = count + 1;
    } else {
        count = count + 1;
        let mut p: *mut list_head = head.next;
        while p as *const list_head != head as *const list_head {
            count = count + 1;
            unsafe {
                p = (*p).next;
            }
        }
    }
    //pr_info!("list count is {}",count);
    return count;
}

fn test_list_method() {
    let mut head = list_head::default();
    let mut t1 = list_head::default();
    let mut t2 = list_head::default();

    //测试add
    head.add(&mut t1 as *mut list_head);
    head.add(&mut t2 as *mut list_head);
    if traverse_list(&head) == 3 {
        pr_info!("test_list_add success");
    } else {
        pr_info!("test_list_add failed");
    }

    //测试list_drop
    unsafe {
        (*head.next).list_drop();
    }
    //head.next = &mut t2 as *mut list_head;
    if traverse_list(&head) == 2 {
        pr_info!("test_list_drop success");
    } else {
        pr_info!("test_list_drop failed");
    }

    //测试last_is
    if head.last_is(&mut t1 as *mut list_head) {
        pr_info!("test_list_last_is success");
    } else {
        pr_info!("test_list_last_is failed");
    }

    unsafe {
        (*head.next).list_drop();
    }

    //测试empty
    if head.is_empty() {
        pr_info!("test_list_is_empty success");
    } else {
        pr_info!("test_list_is_empty failed");
    }
}

pub fn test_entry() {
    let mut t1 = ListTest::new(111, list_head::default());
    let mut t2 = ListTest::new(222, list_head::default());
    let mut t3 = ListTest::new(333, list_head::default());
    t1.head.add(&mut t2.head as *mut list_head);
    t1.head.add(&mut t3.head as *mut list_head);
    let _t1 = list_entry!(&mut t1.head as *mut list_head, ListTest, head);
    unsafe {
        if (*_t1).num == t1.num {
            pr_info!("test_list_entry success!");
        } else {
            pr_info!("test_list_entry failed!");
        }
    }
    let _t2 = list_first_entry!(t1.head.next, ListTest, head);
    unsafe {
        if (*_t2).num == t2.num {
            pr_info!("test_list_first_entry success!");
        } else {
            pr_info!("test_list_first_entry failed!");
        }
    }
    let _t3 = list_last_entry!(t1.head.prev, ListTest, head);
    unsafe {
        if (*_t3).num == t3.num {
            pr_info!("test_list_last_entry success!");
        } else {
            pr_info!("test_list_last_entry failed!");
        }
    }
}

pub fn test_list() {
    test_list_method();
    test_entry();
}
