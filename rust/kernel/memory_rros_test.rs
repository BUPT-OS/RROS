/*
测试内存分配的正确性
*/

use crate::{
    rbtree::{RBTree, RBTreeNode},
    prelude::*,sync::SpinLock,
    vmalloc, mm, premmpt, spinlock_init, c_types,
    double_linked_list::*,
};
use crate::memory_rros::*;
use crate::random;
use crate::{bindings, Result};

pub fn main_memory_rros_test() {
    pr_info!("main_memory_rros_test: begin");
    // get_random_test();
    small_chunk_alloc_test();
}

//判断某个地址是否有效
fn test_addr_valid(addr: *mut u8, size: size_t) -> bool {
    return false;
}

//得到随机数，左闭右开
//[1,16)
fn get_random(start:u32, end:u32) -> u32 {
    let mut t: [u8;4] = [1,2,3,4];
    let res = random::getrandom(&mut t);
    let num;
    match res {
        Ok(_) => {
            let ptr :*const u8 = t.as_ptr();
            let ptr :*const u32 = ptr as *const u32;
            num = unsafe{ *ptr};
        },
        Err(_) => {
            pr_info!("get_random err");
            return 0;
        }
    }
    return start + num % (end - start);
}

fn get_random_test() {
    for i in 1..20 {
        let num = get_random(1,100);
        pr_info!("num is {}",num);
    }
}

struct pair {
    addr: *mut u8,
    size: u32,
}
impl pair {
    pub fn new(addr: *mut u8, size: u32) -> Self {
        pair{
            addr,
            size,
        }
    }
}

//计算当前buckets中的pg链表的数量
fn calcuate_buckets() {
    unsafe {
        for i in 0..5 {
            let mut sum = 0;
            let pg = rros_system_heap.buckets[i];
            if pg != u32::MAX {
                pr_info!("calcuate_buckets: in");
                let mut page = rros_system_heap.get_pagemap(pg as i32);
                loop {
                    let x = 32>>i;
                    let pgmap = (*page).pginfo.map;
                    for j in 0..x {
                        if pgmap & 1<<j != 0x0 {
                            sum += 1;
                        }
                    }
                    pr_info!("calcuate_buckets: i is {}, sum is {}",i,sum);
                    if (*page).next == pg {
                        break;
                    }
                    page = rros_system_heap.get_pagemap((*page).next as i32);
                }
            }
            // pr_info!("calcuate_buckets: i is {}, sum is {}",i,sum);
        }
    }
}

//输入分配范围，进行连续分配测试
fn mem_alloc_range(start:u32, end:u32, repeat: u32) {
    pr_info!("mem_alloc_range: begin");
    let base = pair::new(1 as *mut u8, 0);
    let mut link_head = List::new(base);
    let mut sum = 0;
    unsafe {pr_info!("mem_alloc_range: heap size:{}, heap used:{}", rros_system_heap.usable_size, rros_system_heap.used_size); }
    //进行分配
    for i in 0..repeat {
        let num = get_random(start, end);
        sum += num;
        let x = unsafe{rros_system_heap.rros_alloc_chunk(num as usize)};
        match x {
            Some(a) => {
                let p = pair::new(a,num);
                link_head.add_tail(p);
            },
            None => {
                pr_info!("mem_alloc_range: rros_alloc_chunk err");
                // return ;
            }
        }
        pr_info!("i is {}, num is {}",i,num);
    }
    pr_info!("mem_alloc_range: has alloced: {}",sum);
    unsafe {pr_info!("mem_alloc_range: heap size:{}, heap used:{}", rros_system_heap.usable_size, rros_system_heap.used_size); }
    // calcuate_buckets();
    //进行回收
    let length = link_head.len()+1;
    for i in 1..length {
        let x = link_head.get_by_index(i).unwrap().value.addr;
        let y = link_head.get_by_index(i).unwrap().value.size;
        pr_info!("i is {}, mem_alloc_range: size is {:?}",i,y);
        unsafe{rros_system_heap.rros_free_chunk(x);}
    }
    unsafe {pr_info!("mem_alloc_range: heap size:{}, heap used:{}", rros_system_heap.usable_size, rros_system_heap.used_size); }
    unsafe {
        for i in 0..5 {
            let x = rros_system_heap.buckets[i];
            if x != u32::MAX {
                pr_info!("mem_alloc_range: *i != u32::MAX, i is {}, x = {}",i,x);
                unsafe {
                    let page = rros_system_heap.get_pagemap(x as i32);
                    pr_info!("page map is {}",(*page).pginfo.map);

                }
            }
        }
    }
    pr_info!("mem_alloc_range: end");
}

//随机分配与回收
fn random_mem_alloc_range(start:u32, end:u32, repeat: u32) {
    pr_info!("random_mem_alloc_range: begin");
    let base = pair::new(1 as *mut u8, 0);
    let mut link_head = List::new(base);
    unsafe {pr_info!("random_mem_alloc_range: heap size:{}, heap used:{}", rros_system_heap.usable_size, rros_system_heap.used_size); }
    //进行分配
    for i in 1..repeat {
        let r = get_random(0, 2);
        if r == 0 {//0表示分配
            let num = get_random(start, end);
            let x = unsafe{rros_system_heap.rros_alloc_chunk(num as usize)};
            match x {
                Some(a) => {
                    let p = pair::new(a,num);
                    link_head.add_tail(p);
                },
                None => {
                    pr_info!("random_mem_alloc_range: rros_alloc_chunk err");
                    // return ;
                }
            }
            pr_info!("random_mem_alloc_range: alloc chunk i is {}, num is {}",i,num);
        } else { //1表示回收
            let length = link_head.len();
            if length > 0 {
                let x = link_head.get_by_index(1).unwrap().value.addr;
                pr_info!("random_mem_alloc_range: free chunk addr is {:?}",x);
                unsafe{rros_system_heap.rros_free_chunk(x);}
                link_head.de_head();
            }
        }
    }
    unsafe {pr_info!("random_mem_alloc_range: heap size:{}, heap used:{}", rros_system_heap.usable_size, rros_system_heap.used_size); }
    
    //进行回收
    let length = link_head.len() + 1;
    for i in 1..length {
        let x = link_head.get_by_index(i).unwrap().value.addr;
        pr_info!("random_mem_alloc_range: free chunk i is {}, addr is {:?}",i,x);
        unsafe{rros_system_heap.rros_free_chunk(x);}
    }
    unsafe {pr_info!("random_mem_alloc_range: heap size:{}, heap used:{}", rros_system_heap.usable_size, rros_system_heap.used_size); }
    unsafe {
        for i in rros_system_heap.buckets.iter_mut() {
            if *i != u32::MAX {
                pr_info!("random_mem_alloc_range: *i != u32::MAX, *i = {}",*i);
            }
        }
    }
    pr_info!("random_mem_alloc_range: end");
}

fn small_chunk_alloc_test() {
    mem_alloc_range(1,257,100);//连续分配小内存
    mem_alloc_range(257,2048,100);//连续分配大内存
    random_mem_alloc_range(1,2049,100);//随机分配内存
}