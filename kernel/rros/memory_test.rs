use core::sync::atomic::AtomicUsize;
use core::{borrow::BorrowMut, mem::size_of, mem::zeroed};

use kernel::{
    init_static_sync, mm,
    prelude::*,
    premmpt, spinlock_init,
    sync::{self, Mutex, SpinLock},
    vmalloc, memory_rros::*,
    rbtree::{RBTree, RBTreeNode},
    memory_rros::*,container_of,memory_rros_test::*,
};

use alloc::alloc::*;
use alloc::alloc_rros::*;
use alloc::boxed::*;

use crate::{list, monitor};

pub fn mem_test() {
    // mem_test1();
    // mem_test2();
    // test_rbtree();
    // test_init_system_heap();
    // test_insert_system_heap();
    // test_alloc_chunk();
    // test_box_allocator();
    // test_chunk();
    // test_arc();
    // test_buckets();
    main_memory_rros_test();
}

fn test_buckets() {
    test_213();
    unsafe {
        pr_info!("test_buckets: xxx is {}",rros_system_heap.buckets[1]);
    }
}

fn test_213() {
    unsafe {
        rros_system_heap.buckets[1] = 22;
    }
}

fn test_arc() {
    pr_info!("test_arc: begin");
    let b = 5;
    let a1;
    let b1;
    let c1;
    let d1;
    let a = Arc::try_new_in(111, RrosMem);
    let b = Arc::try_new_in(222, RrosMem);
    let c = Arc::try_new_in(333, RrosMem);
    let d = Arc::try_new_in(444, RrosMem);
    match a {
        Ok(x) => {a1 = x;},
        Err(_) => {
            pr_info!("test_arc: arc alloc err");
            return;
        },
    }
    match b {
        Ok(x) => {b1 = x;},
        Err(_) => {
            pr_info!("test_arc: arc alloc err");
            return;
        },
    }
    match c {
        Ok(x) => {c1 = x;},
        Err(_) => {
            pr_info!("test_arc: arc alloc err");
            return;
        },
    }
    match d {
        Ok(x) => {d1 = x;},
        Err(_) => {
            pr_info!("test_arc: arc alloc err");
            return;
        },
    }
    pr_info!("test_arc: a is {}",a1);
    pr_info!("test_arc: b is {}",b1);
    pr_info!("test_arc: c is {}",c1);
    pr_info!("test_arc: d is {}",d1);

    pr_info!("test_arc: end");
}

fn test_fn(x: Arc<i32,RrosMem>) {
    pr_info!("test_fn x is {}",x);
}

fn mem_test1() {
    let b = 5;
    let x = Box::try_new_in(123, Global);
    match x {
        Err(_) => {
            pr_info!("alloc error");
        },
        Ok(y) => {
            let z =y;
            pr_info!("z is {}",z);
        },
    }
    pr_info!("alloc success");
}
struct mem_testxy {
    x: i32,
    y: i32,
    z: i32,
}
//测试申请到的内存直接转换为结构体指针：结论是可以直接使用
pub fn mem_test2() -> Result<usize>{
    let vmalloc_res = vmalloc::c_vmalloc(1024 as u64);
    let memptr;
    match vmalloc_res {
        Some(ptr) => memptr = ptr,
        None => return Err(kernel::Error::ENOMEM),
    }
    let xxx = memptr as *mut mem_testxy;
    unsafe {
        (*xxx).x = 11;
        (*xxx).y = 22;
        (*xxx).z = 33;
        pr_info!("mem_test2: z is {}",(*xxx).z);
        pr_info!("mem_test2: x addr is {:p}",&mut (*xxx).x as *mut i32);
        pr_info!("mem_test2: y addr is {:p}",&mut (*xxx).y as *mut i32);
        pr_info!("mem_test2: z addr is {:p}",&mut (*xxx).z as *mut i32);
    }
    Ok(0)
}

struct pageinfo {
    membase:u32,
    size:u32,
}

//测试完成：
fn test_rbtree() -> Result<usize>{
    pr_info!("~~~test_rbtree begin~~~");
    let mut root: RBTree<u32, pageinfo> = RBTree::new();

    let mut x1 = pageinfo{
        membase:100,
        size:200,
    };
    let mut x2 = pageinfo{
        membase:101,
        size:200,
    };
    let mut x3 = pageinfo{
        membase:102,
        size:200,
    };
    
    let mut node1 = RBTree::try_allocate_node(100,x1)?;
    // let mut node1: = RBTree::try_allocate_node(300,x2)?;
    let mut node2 = RBTree::try_allocate_node(101,x2)?;
    let mut node3 = RBTree::try_allocate_node(102,x3)?;
    root.insert(node1);
    root.insert(node2);
    root.insert(node3);
    //遍历红黑树方式：
    for item in root.iter() {
        pr_info!("item.0 is {}",item.0);
        pr_info!("item.1.size is {}",item.1.size);
    }
    pr_info!("~~~test_rbtree end~~~");
    Ok(0)
}

//测试初始化系统堆
fn test_init_system_heap() {
    init_system_heap();
}

//测试系统堆插入节点——测试通过
fn test_insert_system_heap() -> Result<usize> {
    pr_info!("~~~test_insert_system_heap begin~~~");
    init_system_heap();
    
    unsafe{
        let membase = rros_system_heap.membase;
        pr_info!("test_insert_system_heap: membase is {:p}",membase);
        let mut x1 = new_rros_heap_range(membase, 1024);
        let mut x2 = new_rros_heap_range(addr_add_size(membase,1024), 2048);
        let mut x3 = new_rros_heap_range(addr_add_size(membase,2048), 4096);

        pr_info!("test_insert_system_heap: 1");
        rros_system_heap.insert_range_byaddr(x1);
        rros_system_heap.insert_range_byaddr(x2);
        rros_system_heap.insert_range_byaddr(x3);
        pr_info!("test_insert_system_heap: 2");
        let mut rb_node = rros_system_heap.addr_tree.clone().unwrap().rb_node;
        if rb_node.is_null() {
            pr_info!("test_insert_system_heap: root is null");
        } else {
            let p = container_of!(rb_node, rros_heap_range, addr_node);
            pr_info!("test_insert_system_heap root size is {}",(*p).size);
        }
        pr_info!("test_insert_system_heap: 3");
    }
    Ok(0)
}

//测试小内存的分配与回收
fn test_small_chunk() {

}

//多次分配回收
fn test_chunk() {
    pr_info!("~~~test_chunk: begin~~~");
    let x = __rros_sys_heap_alloc(1025,0);
    pr_info!("~~~test_chunk: 1~~~");
    let y = __rros_sys_heap_alloc(4,0);
    pr_info!("~~~test_chunk: end~~~");
}

//测试分配chunk
fn test_alloc_chunk() {
    pr_info!("~~~test_alloc_chunk begin~~~");
    unsafe {
        //查看当前rros_system_heap size树的根
        let mut rb_node = rros_system_heap.size_tree.clone().unwrap().rb_node;
        let mut p = container_of!(rb_node, rros_heap_range, size_node);
        let raw_size = (*p).size;
        pr_info!("test_insert_system_heap root size is {}", raw_size);
        let membase = rros_system_heap.membase;
        pr_info!("test_alloc_chunk: membase is {}",membase as u32);
        let res = rros_system_heap.rros_alloc_chunk(1024);
        let mut x:u32 = 0;
        let mut addr = 0 as *mut u8;
        match res {
            Some(a) => {
                addr = a;
                x = a as u32;
                pr_info!("test_alloc_chunk: alloc addr is {}",x as u32);
            },
            None => {
                pr_info!("test_alloc_chunk: alloc err");
            }
        }
        pr_info!("test_alloc_chunk: membase - alloc = {}",x as u32 - membase as u32);
        p = container_of!(rb_node, rros_heap_range, size_node);
        let mut new_size = (*p).size;
        pr_info!("test_insert_system_heap root size is {}", new_size);
        pr_info!("test_alloc_chunk: raw_size - new_size = {}", raw_size - new_size);
        //测试回收
        pr_info!("~~~test_alloc_chunk: test free begin~~~");
        rros_system_heap.rros_free_chunk(addr);
        p = container_of!(rb_node, rros_heap_range, size_node);
        new_size = (*p).size;
        pr_info!("test_insert_system_heap root size is {}", new_size);
        pr_info!("~~~test_alloc_chunk: test free end~~~");
    }
    pr_info!("~~~test_alloc_chunk end~~~");

}

//测试box的自定义分配器
fn test_box_allocator() {
    pr_info!("test_box_allocator: begin");
    let b = 5;
    let x = Box::try_new_in(123, RrosMem);
    match x {
        Err(_) => {
            pr_info!("test_box_allocator: alloc error");
            return ;
        },
        Ok(_x) => {
            unsafe {
                let mut rb_node = rros_system_heap.size_tree.clone().unwrap().rb_node;
                let mut p = container_of!(rb_node, rros_heap_range, size_node);
                let raw_size = (*p).size;
                pr_info!("test_box_allocator: root size is {}", raw_size);
            }
            pr_info!("test_box_allocator: x is {}",_x);
        },
    }
    unsafe {
        let mut rb_node = rros_system_heap.size_tree.clone().unwrap().rb_node;
        let mut p = container_of!(rb_node, rros_heap_range, size_node);
        let raw_size = (*p).size;
        pr_info!("test_box_allocator: root size is {}", raw_size);
    }
    pr_info!("test_box_allocator: alloc success");
}