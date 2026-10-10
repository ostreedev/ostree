#[no_mangle]
pub extern "C" fn _ostreesys_bump_mtime(fd: libc::c_int) -> libc::c_int {
    let fd = unsafe { std::os::fd::BorrowedFd::borrow_raw(fd) };
    let now = rustix::fs::Timespec {
        tv_sec: 0,
        tv_nsec: rustix::fs::UTIME_NOW,
    };
    let ts = rustix::fs::Timestamps {
        last_access: now.clone(),
        last_modification: now.clone(),
    };
    if let Err(e) = rustix::fs::utimensat(fd, "ostree/deploy", &ts, rustix::fs::AtFlags::empty()) {
        e.raw_os_error().into()
    } else {
        0
    }
}
