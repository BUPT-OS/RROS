
use crate::{
    rbtree::{RBTree, RBTreeNode},
    prelude::*,sync::SpinLock,
    vmalloc, mm, premmpt, spinlock_init, c_types,
};
use crate::{bindings, Result};
use core::{borrow::BorrowMut, mem::size_of, mem::zeroed, ptr::addr_of_mut, ptr::addr_of};
use crate::timekeeping::*;

const PAGE_SIZE: u32 = 4096 as u32;
const RROS_HEAP_PAGE_SHIFT: u32 = 9; /* 2^9 => 512 bytes */
const RROS_HEAP_PAGE_SIZE: u32 =	(1 << RROS_HEAP_PAGE_SHIFT);
const RROS_HEAP_PAGE_MASK: u32 =	(!(RROS_HEAP_PAGE_SIZE - 1));
const RROS_HEAP_MIN_LOG2: u32 = 	4; /* 16 bytes */
/*
 * Use bucketed memory for sizes between 2^RROS_HEAP_MIN_LOG2 and
 * 2^(RROS_HEAP_PAGE_SHIFT-1).
 */
const RROS_HEAP_MAX_BUCKETS: u32 = (RROS_HEAP_PAGE_SHIFT - RROS_HEAP_MIN_LOG2);
const RROS_HEAP_MIN_ALIGN: u32 = (1 << RROS_HEAP_MIN_LOG2);//16
/* Maximum size of a heap (4Gb - PAGE_SIZE). */
const RROS_HEAP_MAX_HEAPSZ: u32 = (4294967295 - PAGE_SIZE + 1);
/* Bits we need for encoding a page # */
const RROS_HEAP_PGENT_BITS: u32 = (32 - RROS_HEAP_PAGE_SHIFT);
/* Each page is represented by a page map entry. */
// const RROS_HEAP_PGMAP_BYTES	sizeof(struct rros_heap_pgentry)
const CONFIG_RROS_NR_THREADS: usize = 256;
const CONFIG_RROS_NR_MONITORS: usize = 512;
pub type size_t = usize;

extern "C" {
    fn rust_helper_rb_link_node(
        node: *mut bindings::rb_node,
        parent: *const bindings::rb_node,
        rb_link: *mut *mut bindings::rb_node,
    );
    fn rust_helper_ilog2(
        size: size_t
    ) -> c_types::c_int;
    fn rust_helper_align(
        x: size_t,
        a: u32,
    ) -> c_types::c_ulong;
    fn rust_helper_ffs(
        x: u32,
    ) -> c_types::c_int;
}

#[no_mangle]
pub fn __rros_sys_heap_alloc(size: usize, align: usize) -> *mut u8 {
    // pr_info!("__rros_sys_heap_alloc: begin");
    unsafe {
        rros_system_heap.rros_alloc_chunk(size).unwrap()
    }
}

#[no_mangle]
pub fn __rros_sys_heap_dealloc(ptr: *mut u8, size: usize, align: usize) {
    unsafe { rros_system_heap.rros_free_chunk(ptr); }
}

#[no_mangle]
pub fn __rros_sys_heap_realloc(ptr: *mut u8, old_size: usize, align: usize, new_size: usize) -> *mut u8 {
    unsafe {
        rros_system_heap.rros_realloc_chunk(ptr, old_size, new_size).unwrap()
    }
}

#[no_mangle]
pub fn __rros_sys_heap_alloc_zerod(size: usize, align: usize) -> *mut u8 {
    unsafe {
        rros_system_heap.rros_alloc_chunk_zeroed(size).unwrap()
    }
}

struct rros_user_window {
    state: u32,
    info: u32,
    pp_pending: u32,
}

#[repr(C)]
pub union pginfo {
    pub map: u32,
    pub bsize: u32,
}

pub struct rros_heap_pgentry {
    pub prev: u32,
    pub next: u32,
    pub page_type: u32,
    pub pginfo: pginfo,
}

pub struct rros_heap_range {
    pub addr_node: bindings::rb_node,
    pub size_node: bindings::rb_node,
    pub size: size_t,
}

pub fn new_rros_heap_range(addr: *mut u8, size: size_t) -> *mut rros_heap_range{
    let addr = addr as *mut rros_heap_range;
    unsafe{
        (*addr).addr_node = bindings::rb_node::default();
        (*addr).size_node = bindings::rb_node::default();
        (*addr).size = size;
    }
    return addr;
}

#[inline]
pub fn addr_add_size(addr: *mut u8, size: size_t) -> *mut u8 {
    (addr as u64 + size as u64) as *mut u8
}

// impl rros_heap_range {
//     pub fn new(size: size_t) -> Self {
//         Self {
//             addr_node: bindings::rb_node::default(),
//             size_node: bindings::rb_node::default(),
//             size: size,
//         }
//     }

//     pub fn into_node(&mut self) {
//         unsafe { addr_of_mut!(self.addr_node).write(bindings::rb_node::default()) };
//         // SAFETY: `node_ptr` is valid, and so are its fields.
//         unsafe { addr_of_mut!(self.size_node).write(bindings::rb_node::default()) };
//         // SAFETY: `node_ptr` is valid, and so are its fields.
//         unsafe { addr_of_mut!(self.size).write(self.size) };
//     }
// }

pub struct rros_heap {
    pub membase: *mut u8,
    pub addr_tree: Option<bindings::rb_root>, //根据首地址找大小
    pub size_tree: Option<bindings::rb_root>, //根据大小找首地址
    pub pagemap: Option<*mut rros_heap_pgentry>,
    pub usable_size: size_t,
    pub used_size: size_t,
    pub buckets: [u32; RROS_HEAP_MAX_BUCKETS as usize],
    pub lock: Option<SpinLock<i32>>,
}

impl rros_heap {
    pub fn init(&mut self, membase: *mut u8, size: size_t) -> Result<usize>{
        premmpt::running_inband()?;
        mm::page_aligned(size)?;
        if (size as u32) > RROS_HEAP_MAX_HEAPSZ {
            return Err(crate::Error::EINVAL);
        }

        let mut spinlock = unsafe{ SpinLock::new(1) };
        let pinned = unsafe { Pin::new_unchecked(&mut spinlock) };
        spinlock_init!(pinned, "spinlock");
        self.lock = Some(spinlock);
        
        for i in self.buckets.iter_mut() {
            *i = u32::MAX;
        }

        let nrpages = size >> RROS_HEAP_PAGE_SHIFT;
        let a: u64 = size_of::<rros_heap_pgentry>() as u64;
        let kzalloc_res = vmalloc::c_kzalloc(a * nrpages as u64);
        match kzalloc_res {
            Some(x) => self.pagemap = Some(x as *mut rros_heap_pgentry),
            None => {
                return Err(crate::Error::ENOMEM);
            }
        }

        self.membase = membase;
        self.usable_size = size;
        self.used_size = 0;
        
        self.size_tree = Some(bindings::rb_root::default());
        self.addr_tree = Some(bindings::rb_root::default());
        self.release_page_range(membase, size);
        
        Ok(0)
    }

    pub fn release_page_range(&mut self, page: *mut u8, size: size_t) {
        // pr_info!("release_page_range: 1");
        let mut freed = page as *mut rros_heap_range;
        let mut addr_linked = false;
        
        unsafe{ (*freed).size = size; }
        // pr_info!("release_page_range: 2");
        let left_op = self.search_left_mergeable(freed);
        match left_op {
            Some(left) => {
                let node_links = unsafe {addr_of_mut!((*left).size_node)};
                let mut root = self.size_tree.as_mut().unwrap();
                unsafe{bindings::rb_erase(node_links, root);}
                unsafe{ (*left).size += (*freed).size; }
                freed = left;
                addr_linked = true;
            },
            None => (),
        }
        // pr_info!("release_page_range: 3");
        let right_op = self.search_right_mergeable(freed);
        match right_op {
            Some(right) => {
                let mut node_links = unsafe{addr_of_mut!((*right).size_node)};
                let mut root = self.size_tree.as_mut().unwrap();
                unsafe{bindings::rb_erase(node_links, root);}
                unsafe{ (*freed).size += (*right).size; }
                node_links = unsafe{addr_of_mut!((*right).addr_node)};
                root = self.addr_tree.as_mut().unwrap();
                if addr_linked {
                    unsafe{bindings::rb_erase(node_links, root)};
                } else {
                    let freed_node_links = unsafe{addr_of_mut!((*freed).addr_node)};
                    unsafe{bindings::rb_replace_node(node_links, freed_node_links, root)};
                }
            },
            None => {
                if !addr_linked {
                    self.insert_range_byaddr(freed);
                }
            },
        }
        // pr_info!("release_page_range: 4");
        self.insert_range_bysize(freed);
        // pr_info!("release_page_range: 5");
    }

    pub fn search_left_mergeable(&self, r: *mut rros_heap_range) -> Option<*mut rros_heap_range> {
        let mut node: *mut bindings::rb_node = self.addr_tree.clone().unwrap().rb_node;
        while !node.is_null() {
            let p = crate::container_of!(node, rros_heap_range, addr_node);
            unsafe {
                if addr_add_size(p as *mut u8, (*p).size) as u64 == r as u64 {
                    return Some(p as *mut rros_heap_range);
                }
                let addr_node_addr = addr_of_mut!((*r).addr_node);
                if (addr_node_addr as u64) < (node as u64) {
                    node = (*node).rb_left;
                }else {
                    node = (*node).rb_right;
                }
            }
        }
        None
    }

    pub fn search_right_mergeable(&self, r: *mut rros_heap_range) -> Option<*mut rros_heap_range> {
        let mut node: *mut bindings::rb_node = self.addr_tree.clone().unwrap().rb_node;
        while !node.is_null() {
            let p = crate::container_of!(node, rros_heap_range, addr_node);
            unsafe {
                if addr_add_size(r as *mut u8, (*r).size) as u64  == p as u64 {
                    return Some(p as *mut rros_heap_range);
                }
                let addr_node_addr = addr_of_mut!((*r).addr_node);
                if (addr_node_addr as u64) < (node as u64) {
                    node = (*node).rb_left;
                }else {
                    node = (*node).rb_right;
                }
            }
        }
        None
    }

    pub fn insert_range_byaddr(&mut self, r: *mut rros_heap_range) {
        unsafe {
            let node_links = addr_of_mut!((*r).addr_node);
            let mut root = self.addr_tree.as_mut().unwrap();
            let mut new_link: &mut *mut bindings::rb_node = &mut root.rb_node;
            let mut parent = core::ptr::null_mut();
            while !new_link.is_null() {
                let p = crate::container_of!(*new_link, rros_heap_range, addr_node);
                parent = *new_link;
                if (r as u64) < (p as u64) {
                    new_link = &mut (*parent).rb_left;
                } else {            
                    new_link = &mut (*parent).rb_right;
                }
            }
            rust_helper_rb_link_node(node_links, parent, new_link);
            bindings::rb_insert_color(node_links, root);
        }
    }

    pub fn insert_range_bysize(&mut self, r: *mut rros_heap_range) {
        unsafe {
            let node_links = addr_of_mut!((*r).size_node);
            let mut root = self.size_tree.as_mut().unwrap();
            let mut new_link: &mut *mut bindings::rb_node = &mut root.rb_node;
            let mut parent = core::ptr::null_mut();
            while !new_link.is_null() {
                let p = crate::container_of!(*new_link, rros_heap_range, size_node);
                parent = *new_link;
                if (r as u64) < (p as u64) {
                    new_link = &mut (*parent).rb_left;
                } else {            
                    new_link = &mut (*parent).rb_right;
                }
            }
            rust_helper_rb_link_node(node_links, parent, new_link);
            bindings::rb_insert_color(node_links, root);
        }
    }

    #[no_mangle]
    pub fn rros_alloc_chunk(&mut self, size: size_t) -> Option<*mut u8> {
        //pr_info!("rros_alloc_chunk: time1 is {} size is {}",ktime_get_real_fast_ns(), size);
        // pr_info!("rros_alloc_chunk: begin");
        // pr_info!("rros_alloc_chunk: alloc size is {}",size);
        let mut log2size:i32;
        let mut ilog: i32;
        let mut pg: i32;
        let mut b: i32 = -1;
        let mut flags: u32;
        let mut bsize: size_t;
        
        let mut block: Option<*mut u8>;
        if size == 0 {
            return None;
        }

        if size < (RROS_HEAP_MIN_ALIGN as size_t) {
            bsize = RROS_HEAP_MIN_ALIGN as size_t;
            log2size = RROS_HEAP_MIN_LOG2 as i32;
        } else {
            log2size = unsafe{ rust_helper_ilog2(size) };//down int size
            //pr_info!("rros_alloc_chunk: time1.1.0 is {}",ktime_get_real_fast_ns());
            if (log2size < (RROS_HEAP_PAGE_SHIFT as i32)) {//9 2^4-2^8
                //pr_info!("rros_alloc_chunk: time1.1.1 is {}",ktime_get_real_fast_ns());
                if size & (size - 1) != 0 {
                    //pr_info!("rros_alloc_chunk: time1.1.2 is {}",ktime_get_real_fast_ns());
                    log2size += 1;
                }
                //pr_info!("rros_alloc_chunk: time1.1.3 is {}",ktime_get_real_fast_ns());
                bsize = 1 << log2size;
                //pr_info!("rros_alloc_chunk: time1.2.0 is {}",ktime_get_real_fast_ns());
            } else{
                bsize = unsafe{ rust_helper_align(size, RROS_HEAP_PAGE_SIZE) as size_t };//512 up to a int times to 512
                //pr_info!("rros_alloc_chunk: time1.3 is {}",ktime_get_real_fast_ns());
            }   
        }
        // pr_info!("rros_alloc_chunk: time2 is {}",ktime_get_real_fast_ns());
        //上锁
        if bsize >= (RROS_HEAP_PAGE_SIZE as usize) {
            block = self.add_free_range(bsize, 0);
            // pr_info!("rros_alloc_chunk: time2.1 is {}",ktime_get_real_fast_ns());
        } else {
            ilog = log2size - RROS_HEAP_MIN_LOG2 as i32;
            // pr_info!("rros_alloc_chunk: ilog is {}", ilog);
            pg = self.buckets[ilog as usize] as i32;
            // pr_info!("rros_alloc_chunk: pg is {}", pg);
            unsafe{
                if pg < 0 {
                    block = self.add_free_range(bsize, log2size);
                    pr_info!("rros_alloc_chunk: block is {:p}",block.clone().unwrap());
                    // pr_info!("rros_alloc_chunk: time2.2 is {}",ktime_get_real_fast_ns());
                } else {
                    let pagemap = self.get_pagemap(pg);
                    if (*pagemap).pginfo.map == u32::MAX {
                        block = self.add_free_range(bsize, log2size);
                        // pr_info!("rros_alloc_chunk: time2.3 is {}",ktime_get_real_fast_ns());
                    } else {
                        let x = (*pagemap).pginfo.map;
                        pr_info!("rros_alloc_chunk: x is {}",x);
                        b = rust_helper_ffs(!x) - 1;
                        pr_info!("rros_alloc_chunk: b is {}",b);
                        // pr_info!("rros_alloc_chunk: time3 is {}",ktime_get_real_fast_ns());
                        (*pagemap).pginfo.map |= (1 << b);
                        let t1 = ktime_get_real_fast_ns(); //pr_info!("rros_alloc_chunk: time3.1 is {}",ktime_get_real_fast_ns());
                        self.used_size += bsize;
                        let t2 = ktime_get_real_fast_ns();
                        // pr_info!("rros_alloc_chunk: time3.1 is {}",t2 - t1);
                        block = Some(addr_add_size(self.membase, ((pg << RROS_HEAP_PAGE_SHIFT) + (b << log2size)) as size_t));
                        // pr_info!("rros_alloc_chunk: time3.3 is {}",ktime_get_real_fast_ns());
                        if (*pagemap).pginfo.map == u32::MAX {
                            // pr_info!("rros_alloc_chunk: time3.12 is {}",ktime_get_real_fast_ns());
                            self.move_page_back(pg, log2size);
                        }
                    }
                }
            }
            // pr_info!("rros_alloc_chunk: time4 is {}",ktime_get_real_fast_ns());
        }
        //解锁
        // pr_info!("rros_alloc_chunk: time5 is {}",ktime_get_real_fast_ns());
        return block;
    }

    //将申请的内存空间初始化为0
    pub fn rros_alloc_chunk_zeroed(&mut self, size: size_t) -> Option<*mut u8> {
        let block = self.rros_alloc_chunk(size);
        match block {
            Some(x) => {
                unsafe{bindings::memset(x as *mut c_types::c_void, 0, size as c_types::c_ulong)};
                return Some(x);
            },
            None => return None,
        }
        None
    }

    //重新分配空间
    pub fn rros_realloc_chunk(&mut self, raw: *mut u8, old_size: size_t, new_size: size_t) -> Option<*mut u8> {
        //开辟新空间
        let ptr_op = self.rros_alloc_chunk(new_size);
        match ptr_op {
            Some(ptr) => {
                unsafe{ bindings::memcpy(ptr as *mut c_types::c_void, raw as *mut c_types::c_void, old_size as c_types::c_ulong)  };
                self.rros_free_chunk(raw);
                return Some(ptr);
            },
            None => return None,
        }
        None
    }
    
    #[inline]
    fn addr_to_pagenr(&mut self, p: *mut u8) -> i32 {
        ( (p as u32 - self.membase as u32) >> RROS_HEAP_PAGE_SHIFT ) as i32
    }

    //检查、无误
    fn add_free_range(&mut self, bsize:size_t, log2size: i32) -> Option<*mut u8> {
        let pg_op = self.reserve_page_range(unsafe{rust_helper_align(bsize, RROS_HEAP_PAGE_SIZE)} as size_t);
        let pg: i32;
        match pg_op {
            Some(x) => {
                if x < 0 {
                    return None;
                }
                pg = x;
            },
            None => return None,
        }

        let pagemap = self.get_pagemap(pg);
        if log2size != 0 {
            unsafe {
                (*pagemap).page_type = log2size as u32;
                (*pagemap).pginfo.map = !gen_block_mask(log2size) | 1;
                self.add_page_front(pg, log2size);
            }
        } else {
            unsafe {
                (*pagemap).page_type = 0x02;
                (*pagemap).pginfo.bsize = bsize as u32;
            }
        }

        self.used_size += bsize;
        return Some(self.pagenr_to_addr(pg));
    }

    #[inline]
    fn pagenr_to_addr(&mut self, pg: i32) -> *mut u8 {
        addr_add_size(self.membase, (pg as size_t) << RROS_HEAP_PAGE_SHIFT) as *mut u8
    }

    //检查：逻辑完整、暂未找到优化点 检查2、无误
    fn search_size_ge(&mut self, size: size_t) -> Option<*mut rros_heap_range> {
        let mut rb = self.size_tree.as_mut().unwrap().rb_node;
        let mut deepest = core::ptr::null_mut();
        while !rb.is_null() {
            deepest = rb;
            unsafe {
                let mut r = crate::container_of!(rb, rros_heap_range, size_node);
                if size < (*r).size {
                    rb = (*rb).rb_left;
                    continue;
                }
                if size > (*r).size {
                    rb = (*rb).rb_right;
                    continue;
                }
                return Some(r as *mut rros_heap_range);
            }
        }
        rb = deepest;
        while !rb.is_null() {
            unsafe {
                let mut r = crate::container_of!(rb, rros_heap_range, size_node);
                if size <= (*r).size {
                    return Some(r as *mut rros_heap_range);
                }
                rb = bindings::rb_next(rb as *const bindings::rb_node);
            }
        }
        None
    }

    //检查、无误
    fn reserve_page_range(&mut self, size: size_t) -> Option<i32> {
        let new_op = self.search_size_ge(size);
        let mut new;
        match new_op {
            Some(x) => new = x,
            None => return None,
        }
        let mut node_links = unsafe{addr_of_mut!((*new).size_node)};
        let mut root = self.size_tree.as_mut().unwrap();
        unsafe{bindings::rb_erase(node_links, root)};

        if (unsafe{(*new).size == size}) {
            node_links = unsafe{addr_of_mut!((*new).addr_node)};
            root = self.addr_tree.as_mut().unwrap();
            unsafe{bindings::rb_erase(node_links, root)};
            return Some(self.addr_to_pagenr(new as *mut u8));
        }

        let mut splitr = new;
        unsafe{(*splitr).size -= size};
        new = unsafe{ addr_add_size(new as *mut u8, (*splitr).size) as *mut rros_heap_range };
        self.insert_range_bysize(splitr);
        return Some(self.addr_to_pagenr(new as *mut u8));
    }

    fn move_page_back(&mut self, pg: i32, log2size: i32) {
        pr_info!("move_page_back: in");
        let old = self.get_pagemap(pg);
        if pg == unsafe{(*old).next as i32} {
            pr_info!("move_page_back: return");
            return;
        }

        self.remove_page(pg, log2size);

        let ilog = (log2size as u32) - RROS_HEAP_MIN_LOG2;
        let head = self.get_pagemap(self.buckets[ilog as usize] as i32);
        let last = self.get_pagemap( unsafe{(*head).prev as i32} );
        unsafe {
            (*old).prev = (*head).prev;
            (*old).next = (*last).next;
            let next = self.get_pagemap((*old).next as i32);
            (*next).prev = pg as u32;
            (*last).next = pg as u32;
        }
        pr_info!("move_page_back: out");
    }

    fn move_page_front(&mut self, pg: i32, log2size: i32) {
        let ilog = (log2size as u32) - RROS_HEAP_MIN_LOG2;

        if self.buckets[ilog as usize] == (pg as u32) {
            return;
        }

        self.remove_page(pg, log2size);
        self.add_page_front(pg, log2size);
    }

    fn remove_page(&mut self, pg: i32, log2size: i32) {
        // pr_info!("remove_page: in");
        let ilog = ((log2size as u32) - RROS_HEAP_MIN_LOG2) as usize;
        let old = self.get_pagemap(pg);
        if pg == unsafe{(*old).next as i32} {
            pr_info!("remove_page: u32::MAX");
            self.buckets[ilog] = u32::MAX;
        } else {
            if pg == (self.buckets[ilog] as i32) {
                self.buckets[ilog] = unsafe{(*old).next};
            }
            unsafe {
                let prev = self.get_pagemap((*old).prev as i32);
                (*prev).next = (*old).next;
                let next = self.get_pagemap((*old).next as i32);
                (*next).prev = (*old).prev;
            }
        }
        // pr_info!("remove_page: out");
    }

    //检查、无误
    fn add_page_front(&mut self, pg: i32, log2size: i32) {
        let ilog = ((log2size as u32) - RROS_HEAP_MIN_LOG2) as usize;
        // pr_info!("add_page_front: ilog is {}",ilog);
        let new = self.get_pagemap(pg);
        if self.buckets[ilog] == u32::MAX {
            pr_info!("add_page_front: if");
            self.buckets[ilog] = pg as u32;
            unsafe {
                (*new).prev = pg as u32;
                (*new).next = pg as u32;
            }
            // pr_info!("add_page_front: pg is {}",pg);
        } else {
            pr_info!("add_page_front: else");
            let head = self.get_pagemap(self.buckets[ilog] as i32);
            unsafe {
                (*new).prev = self.buckets[ilog];
                (*new).next = (*head).next;
                let next = self.get_pagemap((*new).next as i32);
                (*next).prev = pg as u32;
                (*head).next = pg as u32;
                self.buckets[ilog] = pg as u32;
            }
        }
    }

    #[inline]
    pub fn get_pagemap(&self, pg: i32) -> *mut rros_heap_pgentry {
        addr_add_size(self.pagemap.clone().unwrap() as *mut u8, 
                        ((pg as u32) * (size_of::<rros_heap_pgentry>() as u32)) as size_t) 
                            as *mut rros_heap_pgentry
    }

    pub fn rros_free_chunk(&mut self, block: *mut u8) {
        let pgoff = (block as u32) - (self.membase as u32);
	    let pg = (pgoff >> RROS_HEAP_PAGE_SHIFT) as i32;
        let pagemap = self.get_pagemap(pg);
        let page_type = unsafe { (*pagemap).page_type };
        let bsize: size_t;
        if page_type == 0x02 {
            bsize = unsafe{ (*pagemap).pginfo.bsize as usize };
            let addr = self.pagenr_to_addr(pg);
            self.release_page_range(addr, bsize);
        } else {
            let log2size = page_type as i32;
            bsize = (1 << log2size);
            let boff = pgoff & !RROS_HEAP_PAGE_MASK; //页内偏移
            if (boff & ((bsize - 1) as u32)) != 0 {
                return;
                //解锁
                //raw_spin_unlock_irqrestore(&heap->lock, flags);
            }
            let n = boff >> log2size;
            let oldmap = unsafe{ (*pagemap).pginfo.map };
            unsafe{ (*pagemap).pginfo.map &= !((1 as u32) << n) };//置原map对应的位置为0，表示释放
            unsafe { pr_info!("rros_free_chunk: pg is {}, log2size is {}, oldmap is {}, newmap is {}",pg, log2size, oldmap, (*pagemap).pginfo.map); }
            if unsafe{ (*pagemap).pginfo.map == !gen_block_mask(log2size) } {//释放后页为空
                pr_info!("rros_free_chunk: 1");
                self.remove_page(pg, log2size);
                // pr_info!("rros_free_chunk: 1.2");
                let addr = self.pagenr_to_addr(pg);
                // pr_info!("rros_free_chunk: 1.3");
                self.release_page_range(addr, RROS_HEAP_PAGE_SIZE as size_t);
                // pr_info!("rros_free_chunk: 2");
            } else if oldmap == u32::MAX {
                pr_info!("rros_free_chunk: 3");
                self.move_page_front(pg, log2size);
                // pr_info!("rros_free_chunk: 4");
            }
        }
        self.used_size -= bsize;
	    //raw_spin_unlock_irqrestore(&heap->lock, flags);
    }

    pub fn rros_destroy_heap(&mut self) {
        let res = premmpt::running_inband();
        match res {
            Err(_) => {
                pr_info!("warning: rros_destroy_heap not inband");
            },
            Ok(_) => (),
        }
        vmalloc::c_kzfree(self.pagemap.clone().unwrap() as *const c_types::c_void);
    }

}

pub fn cleanup_shared_heap() {
    unsafe {
        rros_shared_heap.rros_destroy_heap();
        vmalloc::c_vfree(rros_shared_heap.membase as *const c_types::c_void);
    }
}

pub fn cleanup_system_heap() {
    unsafe {
        rros_system_heap.rros_destroy_heap();
        vmalloc::c_vfree(rros_system_heap.membase as *const c_types::c_void);
    }
}

//检查、无误
//log2size is 8: -1U >>（32-512/2^8）= -1U >>30 = 11 ;...; 5: -1U >>（32-512/2^5）= -1U>>16 = 16 1; 4: -1U >>（32-512/2^4）= -1U>>0 = -1U; 
pub fn gen_block_mask(log2size: i32) -> u32 {
    return u32::MAX >> (32 - (RROS_HEAP_PAGE_SIZE >> log2size));
} 

pub static mut rros_system_heap: rros_heap = rros_heap {
    membase: 0 as *mut u8,
    addr_tree: None,
    size_tree: None,
    pagemap: None,
    usable_size: 0,
    used_size: 0,
    buckets: [0; RROS_HEAP_MAX_BUCKETS as usize],
    lock: None,
};

pub static mut rros_shared_heap: rros_heap = rros_heap {
    membase: 0 as *mut u8,
    addr_tree: None,
    size_tree: None,
    pagemap: None,
    usable_size: 0,
    used_size: 0,
    buckets: [0; RROS_HEAP_MAX_BUCKETS as usize],
    lock: None,
};

pub static mut rros_shm_size: usize = 0;

pub fn init_system_heap() -> Result<usize> {
    let size = 2048 * 1024;
    let system = vmalloc::c_vmalloc(size as c_types::c_ulong);
    match system {
        Some(x) => {
            let ret = unsafe{rros_system_heap.init(x as *mut u8, size as usize)};
            match ret {
                Err(_) => {
                    vmalloc::c_vfree(x);
                    return Err(crate::Error::ENOMEM);
                },
                Ok(_) => (),
            }
        },
        None => return Err(crate::Error::ENOMEM),
    }
    pr_info!("rros_mem: init_system_heap success");
    Ok(0)
}

pub fn init_shared_heap() -> Result<usize> {
    let mut size: usize = CONFIG_RROS_NR_THREADS * size_of::<rros_user_window>()
        + CONFIG_RROS_NR_MONITORS * 40;
    size = mm::page_align(size)?;
    mm::page_aligned(size)?;
    let shared = vmalloc::c_kzalloc(size as u64);
    match shared {
        Some(x) => {
            let ret = unsafe{rros_shared_heap.init(x as *mut u8, size as usize)};
            match ret {
                Err(_e) => {
                    vmalloc::c_kzfree(x);
                    return Err(_e);
                },
                Ok(_) => (),
            }
        },
        None => return Err(crate::Error::ENOMEM),
    }
    unsafe{ rros_shm_size = size };
    Ok(0)
}

pub fn rros_init_memory() -> Result<usize> {
    let mut ret = init_system_heap();
    match ret {
        Err(_) => return ret,
        Ok(_) => (),
    }
    ret = init_shared_heap();
    match ret {
        Err(_) => {
            cleanup_system_heap();
            return ret;
        },
        Ok(_) => (),
    }
    Ok(0)
}

pub fn rros_cleanup_memory() {
    cleanup_shared_heap();
	cleanup_system_heap();
}