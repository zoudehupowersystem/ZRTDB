use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zrtdb_example_rs::*;

const STR_BYTES: usize = 120;

fn now_text() -> String {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => format!("unix:{}", d.as_secs()),
        Err(_) => "unix:0".to_string(),
    }
}

unsafe fn write_info(controlptr: *mut zrtdb_control_controlptr_t, row: usize, text: &str) {
    let info_ptr = std::ptr::addr_of_mut!((*controlptr).INFO_COMMANDS) as *mut [core::ffi::c_char; STR_BYTES];
    let slot = info_ptr.add(row);
    let mut buf = [0 as core::ffi::c_char; STR_BYTES];
    let bytes = text.as_bytes();
    let n = bytes.len().min(STR_BYTES - 1);
    for i in 0..n {
        buf[i] = bytes[i] as core::ffi::c_char;
    }
    std::ptr::write_unaligned(slot, buf);
}

fn main() {
    let pid = std::process::id();
    let ctx = app_init();
    let loops: usize = std::env::var("ZRTDB_DEMO_LOOPS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(6);

    if ctx.CONTROL_CONTROLPTR.is_null() || ctx.CONTROL_CONTROL.is_null() {
        eprintln!("CONTROL mapping is null");
        std::process::exit(2);
    }

    let controlptr = ctx.CONTROL_CONTROLPTR;
    let control = ctx.CONTROL_CONTROL;
    let mx = ZRTDB_CONTROL_MX_COMMANDS;

    println!("[GEN][pid={}] loops={} mx={} (run policy_exec_rs in another terminal)", pid, loops, mx);

    for seq in 1..=loops {
        let row = (seq - 1) % mx;
        unsafe {
            SnapshotReadLock_();

            let status_base = std::ptr::addr_of_mut!((*controlptr).STATUS_COMMANDS) as *mut i32;
            let val_base = std::ptr::addr_of_mut!((*controlptr).VAL_COMMANDS) as *mut f32;
            let sig_base = std::ptr::addr_of_mut!((*controlptr).SIG_COMMANDS) as *mut i32;
            let id_base = std::ptr::addr_of_mut!((*controlptr).ID_COMMANDS) as *mut i32;

            std::ptr::write_unaligned(status_base.add(row), 0);
            std::ptr::write_unaligned(val_base.add(row), std::f32::consts::PI);
            std::ptr::write_unaligned(sig_base.add(row), seq as i32);
            std::ptr::write_unaligned(id_base.add(row), 200);

            write_info(controlptr, row, &(now_text() + " publish command"));

            std::sync::atomic::fence(std::sync::atomic::Ordering::Release);
            let lv_ptr = std::ptr::addr_of_mut!((*control).LV_COMMANDS);
            std::ptr::write_unaligned(lv_ptr, (row + 1) as i32);

            SnapshotReadUnlock_();
        }

        println!("[GEN][pid={}] publish seq={:04} lv={:04} row={:04}", pid, seq, row + 1, row + 1);

        if seq == 6 {
            let mut path = [0 as core::ffi::c_char; 512];
            unsafe {
                let rc = SaveSnapshot_(path.as_mut_ptr() as *mut i8, path.len() as i32);
                if rc > 0 {
                    let bytes: Vec<u8> = path
                        .iter()
                        .take_while(|&&c| c != 0)
                        .map(|&c| c as u8)
                        .collect();
                    println!("[GEN][pid={}] snapshot={}", pid, String::from_utf8_lossy(&bytes));
                }
            }
        }

        thread::sleep(Duration::from_millis(800));
    }
}
