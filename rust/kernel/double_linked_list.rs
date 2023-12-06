// SPDX-License-Identifier: GPL-2.0

//! Double Linked lists.
//!
//! TODO: This module is a work in progress.
use crate::prelude::*;
use alloc::boxed::Box;
use core::ptr::NonNull;
use alloc::alloc_rros::*;

#[derive(Debug, Clone, Copy)]
pub struct Node<T> {
    pub next: Option<NonNull<Node<T>>>,
    pub prev: Option<NonNull<Node<T>>>,
    pub value: T,
}

impl<T> Node<T> {
    pub fn new(v: T) -> Self {
        Node {
            next: None,
            prev: None,
            value: v,
        }
    }

    //self---n---next
    pub fn add(&mut self, next: *mut Node<T>, n: T) {
        // Box::try_new_in(123, RrosMem);
        // let mut node = Box::try_new_in(Node::new(n), RrosMem).unwrap();
        let mut node = Box::try_new(Node::new(n)).unwrap();
        node.next = NonNull::new(next);
        node.prev = NonNull::new(self as *mut Node<T>);
        let node = NonNull::new(Box::into_raw(node));

        self.next = node;
        unsafe {
            (*next).prev = node;
        }
    }

    pub fn remove(&mut self) {
        if self.next.is_some() && self.prev.is_some() {
            unsafe {
                let next = self.next.unwrap().as_ptr();
                let prev = self.prev.unwrap().as_ptr();
                if next == prev {
                    //处理list中只有一个元素的情况
                    (*next).prev = None;
                    (*next).next = None;
                } else {
                    (*next).prev = self.prev;
                    (*prev).next = self.next;
                }
            }
        }
        unsafe{Box::from_raw(self as *mut Node<T>);}
    }

    pub fn into_val(self: Box<Self>) -> T {
        self.value
    }
}

#[derive(Clone, Copy)]
pub struct List<T> {
    pub head: Node<T>,
}

impl<T> List<T> {
    pub fn new(v: T) -> Self {
        List {
            head: Node::new(v), //头节点
        }
    }

    // 从队头入队
    pub fn add_head(&mut self, v: T) {
        if self.is_empty() {
            let x = &mut self.head as *mut Node<T>;
            unsafe {
                self.head.add(x, v);
            }
        } else {
            unsafe {
                self.head.add(self.head.next.unwrap().as_ptr(), v);
            }
        }
    }

    //入队尾
    pub fn add_tail(&mut self, v: T) {
        if self.is_empty() {
            let x = &mut self.head as *mut Node<T>;
            unsafe {
                self.head.add(x, v);
            }
        } else {
            unsafe {
                let prev = self.head.prev.unwrap().as_mut();
                prev.add(&mut self.head as *mut Node<T>, v);
            }
        }
        // pr_info!("after add tail, the length is {}", self.len());
    }

    //得到队头
    pub fn get_head<'a>(&self) -> Option<&'a mut Node<T>> {
        if self.is_empty() {
            return None;
        } else {
            Some(unsafe { self.head.next.unwrap().as_mut() })
        }
    }

    //得到队尾
    pub fn get_tail<'a>(&self) -> Option<&'a mut Node<T>> {
        if self.is_empty() {
            return None;
        } else {
            Some(unsafe { self.head.prev.unwrap().as_mut() })
        }
    }

    //按index取node
    pub fn get_by_index<'a>(&mut self, index: u32) -> Option<&'a mut Node<T>> {
        if index <= self.len() {
            let mut p = self.head.next;
            for _ in 1..index {
                p = unsafe { p.unwrap().as_ref().next };
            }
            return Some(unsafe { p.unwrap().as_mut() });
        } else {
            return None;
        }
    }

    //入到index之后 0表示队头
    pub fn enqueue_by_index(&mut self, index: u32, v: T) {
        //测试通过
        if index >= 0 && index <= self.len() {
            if index == 0 {
                self.add_head(v);
            } else if index == self.len() {
                self.add_tail(v);
            } else {
                let x = self.get_by_index(index).unwrap();
                let next = self.get_by_index(index + 1).unwrap();
                x.add(next as *mut Node<T>, v);
            }
        }
    }

    //按index出队
    pub fn dequeue(&mut self, index: u32) {
        if self.len() == 1 && index == 1 {
            unsafe{Box::from_raw(self.head.next.as_mut().unwrap().as_ptr() as *mut Node<T>);}
            self.head.next = None;
            self.head.prev = None;
        } else if index <= self.len() {
            self.get_by_index(index).unwrap().remove();
        }
    }

    //出队头
    pub fn de_head(&mut self) {
        self.dequeue(1);
    }

    //出队尾
    pub fn de_tail(&mut self) {
        self.dequeue(self.len());
    }

    // 计算链表长度
    pub fn len(&self) -> u32 {
        let mut ans = 0;
        if !self.is_empty() {
            ans = 1;
            let mut p = self.head.next;
            while p.unwrap().as_ptr() != self.head.prev.unwrap().as_ptr() {
                ans = ans + 1;
                unsafe {
                    p = p.unwrap().as_ref().next;
                }
            }
        }
        ans
    }

    //判空
    pub fn is_empty(&self) -> bool {
        self.head.next.is_none() && self.head.prev.is_none()
    }
}

//用于测试
impl<i32: core::fmt::Display> List<i32> {
    pub fn traverse(&self) {
        if self.is_empty() {
            return;
        }
        let mut p = self.head.next;
        unsafe {
            pr_info!("x is {}", p.unwrap().as_ref().value);
        }
        while p.unwrap().as_ptr() != self.head.prev.unwrap().as_ptr() {
            unsafe {
                p = p.unwrap().as_ref().next;
                pr_info!("x is {}", p.unwrap().as_ref().value);
            }
        }
    }
}
