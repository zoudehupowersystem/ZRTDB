pub mod generated {
    #![allow(non_snake_case)]
    #![allow(non_camel_case_types)]
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/zrtdb_generated.rs"));
}

pub use generated::*;

pub const COMMAND_INFO_BYTES: usize = 120;

pub fn app_init() -> zrtdb_app_controler_ctx_t {
    let mut ctx = std::mem::MaybeUninit::<zrtdb_app_controler_ctx_t>::zeroed();
    let rc = unsafe { zrtdb_app_controler_init(ctx.as_mut_ptr()) };
    if rc < 0 {
        eprintln!("zrtdb_app_controler_init failed");
        std::process::exit(1);
    }
    unsafe { ctx.assume_init() }
}

#[derive(Clone, Debug)]
pub struct CommandRow {
    pub id: i32,
    pub val: f32,
    pub sig: i32,
    pub status: i32,
    pub info: String,
}

struct SnapshotWriteGuard;

impl SnapshotWriteGuard {
    fn new() -> Self {
        unsafe { SnapshotReadLock_() };
        Self
    }
}

impl Drop for SnapshotWriteGuard {
    fn drop(&mut self) {
        unsafe { SnapshotReadUnlock_() };
    }
}

pub fn with_snapshot_write<R>(f: impl FnOnce() -> R) -> R {
    let _guard = SnapshotWriteGuard::new();
    f()
}

pub fn write_command_row(ctx: &zrtdb_app_controler_ctx_t, row: usize, cmd: &CommandRow) {
    unsafe {
        let controlptr = ctx.CONTROL_CONTROLPTR;
        let status_base = std::ptr::addr_of_mut!((*controlptr).STATUS_COMMANDS) as *mut i32;
        let val_base = std::ptr::addr_of_mut!((*controlptr).VAL_COMMANDS) as *mut f32;
        let sig_base = std::ptr::addr_of_mut!((*controlptr).SIG_COMMANDS) as *mut i32;
        let id_base = std::ptr::addr_of_mut!((*controlptr).ID_COMMANDS) as *mut i32;

        std::ptr::write_unaligned(status_base.add(row), cmd.status);
        std::ptr::write_unaligned(val_base.add(row), cmd.val);
        std::ptr::write_unaligned(sig_base.add(row), cmd.sig);
        std::ptr::write_unaligned(id_base.add(row), cmd.id);
        write_info(controlptr, row, &cmd.info);
    }
}

pub fn read_command_row(ctx: &zrtdb_app_controler_ctx_t, row: usize) -> CommandRow {
    unsafe {
        let controlptr = ctx.CONTROL_CONTROLPTR;
        let status_base = std::ptr::addr_of!((*controlptr).STATUS_COMMANDS) as *const i32;
        let val_base = std::ptr::addr_of!((*controlptr).VAL_COMMANDS) as *const f32;
        let sig_base = std::ptr::addr_of!((*controlptr).SIG_COMMANDS) as *const i32;
        let id_base = std::ptr::addr_of!((*controlptr).ID_COMMANDS) as *const i32;

        CommandRow {
            status: std::ptr::read_unaligned(status_base.add(row)),
            val: std::ptr::read_unaligned(val_base.add(row)),
            sig: std::ptr::read_unaligned(sig_base.add(row)),
            id: std::ptr::read_unaligned(id_base.add(row)),
            info: read_info(controlptr, row),
        }
    }
}

pub fn publish_lv(ctx: &zrtdb_app_controler_ctx_t, lv_1based: i32) {
    std::sync::atomic::fence(std::sync::atomic::Ordering::Release);
    unsafe {
        let lv_ptr = std::ptr::addr_of_mut!((*ctx.CONTROL_CONTROL).LV_COMMANDS);
        std::ptr::write_unaligned(lv_ptr, lv_1based);
    }
}

pub fn load_lv(ctx: &zrtdb_app_controler_ctx_t) -> i32 {
    let lv =
        unsafe { std::ptr::read_unaligned(std::ptr::addr_of!((*ctx.CONTROL_CONTROL).LV_COMMANDS)) };
    std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
    lv
}

pub fn mark_command_done(ctx: &zrtdb_app_controler_ctx_t, row: usize) {
    unsafe {
        let controlptr = ctx.CONTROL_CONTROLPTR;
        let status_base = std::ptr::addr_of_mut!((*controlptr).STATUS_COMMANDS) as *mut i32;
        std::ptr::write_unaligned(status_base.add(row), 2);
    }
}

fn write_info(controlptr: *mut zrtdb_control_controlptr_t, row: usize, text: &str) {
    unsafe {
        let info_ptr = std::ptr::addr_of_mut!((*controlptr).INFO_COMMANDS)
            as *mut [core::ffi::c_char; COMMAND_INFO_BYTES];
        let slot = info_ptr.add(row);
        let mut buf = [0 as core::ffi::c_char; COMMAND_INFO_BYTES];
        let bytes = text.as_bytes();
        let n = bytes.len().min(COMMAND_INFO_BYTES - 1);
        for i in 0..n {
            buf[i] = bytes[i] as core::ffi::c_char;
        }
        std::ptr::write_unaligned(slot, buf);
    }
}

fn read_info(controlptr: *mut zrtdb_control_controlptr_t, row: usize) -> String {
    unsafe {
        let info_ptr = std::ptr::addr_of!((*controlptr).INFO_COMMANDS)
            as *const [core::ffi::c_char; COMMAND_INFO_BYTES];
        let raw = std::ptr::read_unaligned(info_ptr.add(row));
        let bytes: Vec<u8> = raw
            .iter()
            .take_while(|&&c| c != 0)
            .map(|&c| c as u8)
            .collect();
        String::from_utf8_lossy(&bytes).to_string()
    }
}
