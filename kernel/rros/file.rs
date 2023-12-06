use alloc::rc::Rc;

use core::{
    borrow::{ Borrow,BorrowMut },
    cell::{ RefCell, Ref },
    marker::PhantomData,
    ptr::{self, NonNull}, ops::{Deref, DerefMut},
};

use crate::{
    crossing::{ RrosCrossing, rros_down_crossing, rros_init_crossing, rros_pass_crossing, rros_up_crossing },
    factory::RrosElement,
    list,
    sched,
};

use kernel::{
    bindings,
    c_str,
    c_types,
    double_linked_list::*,
    file::File,
    prelude::*,
    rbtree,
    str::CStr,
    sync::{ SpinLock, Lock, Guard },
    task::Task, init_static_sync,
};

pub struct RrosFileBinding {
    pub rfile: Rc<RefCell<RrosFile>>,
    pub element:*mut RrosElement,
}

impl RrosFileBinding {
    pub fn new() -> Self {
        Self {
            rfile: Rc::try_new(RefCell::new(RrosFile::new())).unwrap(),
            element: ptr::null_mut(),
        }
    }
}

#[repr(C)]
pub struct RrosFile {
    pub filp: *mut bindings::file,
    pub crossing: RrosCrossing,
}

impl RrosFile {
    pub fn new() -> Self {
        Self {
            filp: ptr::null_mut(),
            crossing: RrosCrossing::new(),
        }
    }

    pub fn rros_open_file(&mut self, filp: *mut bindings::file) -> Result<usize> {
        self.filp = filp;
    
        pr_info!("the address of self is {:p}", self);
        unsafe { (*filp).oob_data = self as *const RrosFile as _;}
        pr_info!("the address of filp oob_data is {:p}", (*filp).oob_data);
        self.crossing.init();
        Ok(0)
    }

    pub fn rros_release_file(&mut self) -> Result<usize> {
        self.crossing.pass();
        Ok(0)
    }

    pub fn flags(&self) -> u32 {
        unsafe { (*self.filp).f_flags }
    }
    
    pub fn from_ptr(ptr: *mut bindings::file) -> Self {
        Self {
            filp: ptr,
            crossing: RrosCrossing::new(),
        }
    }

    pub fn put_file(&mut self) {
        self.crossing.up();
    }
}

impl Drop for RrosFile {
    fn drop(&mut self) {
        // SAFETY: The type invariants guarantee that `RrosFile::filp` has a non-zero reference count.
        pr_info!("I am the RrosFile drop");
    }
}

pub struct RrosFd {
    pub fd: u32,
    rfilp: NonNull<RrosFile>,
    files: *mut bindings::files_struct,
    pub poll_nodes: list::list_head,
}

impl RrosFd {
    pub fn new(fd:u32,files:*mut bindings::files_struct,rfilp:*mut RrosFile) -> Self {
        RrosFd {
            fd: fd,
            rfilp: NonNull::new(rfilp).unwrap(),
            files: files,
            poll_nodes: list::list_head::default(),
        }
    }
    
    pub fn get_rfile(&self) -> &RrosFile {
        unsafe { self.rfilp.as_ref() }
    }

    pub fn get_rfile_mut(&mut self) -> &mut RrosFile {
        unsafe { self.rfilp.as_mut() }
    }
}



struct FdTree(rbtree::RBTree<u32, RrosFd>);
impl Deref for FdTree {
    type Target = rbtree::RBTree<u32, RrosFd>;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
impl DerefMut for FdTree {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}
unsafe impl Send for FdTree{}
init_static_sync! {
    static FD_TREE: SpinLock<FdTree> = FdTree(rbtree::RBTree::new());
}



// pub static mut FD_TREE: Option<Arc<SpinLock<rbtree::RBTree<u32, RrosFd>>>> = Some(init_rbtree().unwrap());

/// Insert the given rfd to static rbtree FD_TREE.
pub fn index_rfd(rfd: RrosFd, filp: *mut bindings::file) -> Result<usize> {
    let flags = FD_TREE.irq_lock_noguard();
    unsafe{
        (*FD_TREE.locked_data().get()).try_insert(rfd.fd, rfd)?;
    }
    FD_TREE.irq_unlock_noguard(flags);
    Ok(0)
    // unlock FD_TREE here
}

/// Search rfd in rbtree FD_TREE by fd.
///
/// Returns a reference to the rfd corresponding to the fd.
pub fn lookup_rfd(fd: u32, files: *mut bindings::files_struct) -> Option<*mut RrosFd> {
    let flags = FD_TREE.irq_lock_noguard();
    // get_mut和锁的get_mut重名了，所以用unsafe了
    if let Some(rfd) =  unsafe{(*FD_TREE.locked_data().get()).get_mut(&fd)}{
        FD_TREE.irq_unlock_noguard(flags);
        return Some(rfd as *mut RrosFd);
    } else {
        FD_TREE.irq_unlock_noguard(flags);
        return None;
    }
}

/// Removes the node with the given key from the rbtree.
///
/// It returns the value that was removed if rfd exists, or ['None'] otherwise.
pub fn unindex_rfd(fd:u32, files: *mut bindings::files_struct) -> Option<RrosFd> {
    let flags = FD_TREE.irq_lock_noguard();
    pr_info!("unindex_rfd 1");
    let ret = unsafe{(*FD_TREE.locked_data().get()).remove(&fd)};
    pr_info!("unindex_rfd 2");
    if ret.is_none(){
        pr_info!("unindex_rfd 3");
        FD_TREE.irq_unlock_noguard(flags);
        return None;
    }else{
        pr_info!("unindex_rfd 4");
        FD_TREE.irq_unlock_noguard(flags);
        return Some(ret.unwrap());
    }
}

// in-band, caller may hold files->file_lock
#[no_mangle]
unsafe extern "C" fn install_inband_fd(fd: u32, filp: *mut bindings::file, files: *mut bindings::files_struct){
    if unsafe{(*filp).oob_data.is_null()} {
        return;
    }
    pr_info!("filp is {:p}", filp);
    pr_info!("install_inband_fd 1");
    pr_info!("the address of filp is {:p}, the filp_oob is {:p}, fd is {}", filp, (*filp).oob_data, fd);
    let mut rfd = RrosFd::new(fd,files,unsafe{(*filp).oob_data as *const _ as *mut RrosFile});
    rfd.rfilp = unsafe { NonNull::new_unchecked((*filp).oob_data as *mut RrosFile) };
    let ret = index_rfd(rfd, filp);
    pr_info!("install_inband_fd 2");
    if ret.is_err(){
        pr_warn!("install_inband_fd: index_rfd failed\n");
    }
    // let a = lookup_rfd(fd, files);
    // if a.is_none(){
    //     pr_warn!("install_inband_fd: lookup_rfd failed\n");
    // }
}

// fdt_lock held, irqs off. CAUTION: resched required on exit.
// static void drop_watchpoints(struct rros_fd *efd)
// {
// 	if (!list_empty(&efd->poll_nodes))
// 		rros_drop_watchpoints(&efd->poll_nodes);
// }

// in-band, caller holds files->file_lock
#[no_mangle]
unsafe extern "C" fn rust_uninstall_inband_fd(fd: u32, filp: *mut bindings::file, files: *mut bindings::files_struct){
    // pr_info!("the filp is {:p}",filp);
    // pr_info!("the filp oob_data is {:p}",(*filp).oob_data);
    if unsafe { (*filp).oob_data.is_null() } {
        return;
    }
    pr_info!("uninstall_inband_fd 1 {:p}", &filp as *const _ as *mut u8);
    pr_info!("uninstall_inband_fd 1 {:p} {:p}",files, &files as *const _ as *mut u8);
    pr_info!("the address of filp is {:p}, the filp_oob is {:p}, fd is {}", filp, (*filp).oob_data, fd);

    let rfd = unindex_rfd(fd, files);
    pr_info!("uninstall_inband_fd 2");
    match rfd {
        Some(rfd) => (), // drop_watchpoints(rfd);
        None => (),
    }
    unsafe { sched::rros_schedule(); }

    // Ok(0)
}
// in-band, caller holds files->file_lock
#[no_mangle]
unsafe extern "C" fn replace_inband_fd(fd: u32, filp: *mut bindings::file, files: *mut bindings::files_struct){
    if unsafe { (*filp).oob_data.is_null() } {
        return;
    }

    let rfd = lookup_rfd(fd, files);
    match rfd {
        Some(rfd) => {
            // drop_watchpoints(rfd);
            unsafe { (*rfd).rfilp = NonNull::new((*filp).oob_data as *const _ as *mut RrosFile).unwrap() };
            unsafe{sched::rros_schedule();} 
        },
        None => {
            unsafe{install_inband_fd(fd, filp, files);}
        }
    }

    // Ok(0)
}

pub fn rros_get_fileref(rfilp: &mut RrosFile) -> Result<usize> {
    rros_down_crossing(&mut rfilp.crossing);
    Ok(0)
}

pub fn rros_get_file(fd: u32) -> Option<NonNull<RrosFile>> { // TODO: 暂时改成NonNull
    let rfd = lookup_rfd(fd, unsafe { (*Task::current_ptr()).files });

    match rfd {
        Some(rfd) => {
            unsafe {
                // rros_get_fileref((*rfd).rfilp);
                // Some(Arc::try_new((*rfd).rfilp.unwrap()).unwrap())
                rros_get_fileref((*rfd).get_rfile_mut());
                Some((*rfd).rfilp)
            }
        },
        None => None,
    }
}

// pub fn rros_watch_fd(fd: u32, node: RrosPollNode) -> Option<Arc<RrosFile>> {
//     let rfd = lookup_rfd(fd, unsafe { (*Task::current_ptr()).files });

//     match rfd {
//         Some(rfd) => {
//             unsafe {
//                 rros_get_fileref((*rfd).rfilp.as_mut().unwrap());
//                 Some(Arc::from_raw((*rfd).rfilp.as_ref().unwrap() as *const RrosFile))
//                 // (*rfd).poll_nodes.add(node.next);
//             }
//         },
//         None => None,
//     }
// }

// pub fn rros_ignore_fd(node: RrosPollNode) -> Result<usize> {
//     // list_del(node.next);
//     Ok(0)
// }

/// Rros_open_file - Open new file with oob capabilities
///
/// Called by chrdev with oob capabilities when a new @rfilp is opened.
/// @rfilp is paired with the in-band file struct @filp
pub fn rros_open_file(rfilp: &mut RrosFile, filp: *mut bindings::file) -> Result<usize> {
    rfilp.filp = filp;

    // mark filp as oob-capable.
    unsafe { (*filp).oob_data = rfilp as *const RrosFile as _; }
    rros_init_crossing(&mut rfilp.crossing);

    Ok(0)
}

/// Rros_release_file - Drop an oob-capable file
///
/// Called by chrdev with oob capabilities when @rfilp is about to be released.
/// Must be called from a fops->release() handler, and paired with a previous
/// call to rros_open_file() from the fops->open() handler.
pub fn rros_release_file(rfilp: &mut RrosFile) -> Result<usize> {
    // Release the orginal reference on @rfilp. If oob references
	// are still pending (e.g. some thread is still blocked in
    // fops->oob_read()), we must wait for them to be dropped
    // before allowing the in-band code to dismantle @efilp->filp.

    // NOTE: In-band and out-of-band fds are working together in
    // lockstep mode via dovetail_install/uninstall_fd() calls.
    // Therefore, we can't livelock with rros_get_file() as @efilp
    // was removed from the fd tree before fops->release() called us.

    rros_pass_crossing(&mut rfilp.crossing);

    Ok(0)
}


/// Rros_put_file - oob

pub fn rros_put_file(rfilp: &mut RrosFile) -> Result<usize> {
    rros_up_crossing(&mut rfilp.crossing);

    Ok(0)
}