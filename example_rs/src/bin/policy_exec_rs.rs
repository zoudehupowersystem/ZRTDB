use std::thread;
use std::time::Duration;
use zrtdb_example_rs::*;

const STR_BYTES: usize = 120;

unsafe fn read_info(controlptr: *mut zrtdb_control_controlptr_t, row: usize) -> String {
    let info_ptr = std::ptr::addr_of!((*controlptr).INFO_COMMANDS) as *const [core::ffi::c_char; STR_BYTES];
    let raw = std::ptr::read_unaligned(info_ptr.add(row));
    let bytes: Vec<u8> = raw
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    String::from_utf8_lossy(&bytes).to_string()
}

fn main() {
    let ctx = app_init();
    let loops: usize = std::env::var("ZRTDB_DEMO_LOOPS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(12);

    if ctx.CONTROL_CONTROLPTR.is_null() || ctx.CONTROL_CONTROL.is_null() {
        eprintln!("CONTROL mapping is null");
        std::process::exit(2);
    }

    let controlptr = ctx.CONTROL_CONTROLPTR;
    let control = ctx.CONTROL_CONTROL;

    let mut prev_lv = 0i32;
    let mut last_sig = -1i32;

    for _ in 0..loops {
        let lv_now = unsafe { std::ptr::read_unaligned(std::ptr::addr_of!((*control).LV_COMMANDS)) };
        std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);

        if lv_now != prev_lv {
            prev_lv = lv_now;
            let row = (lv_now - 1).max(0) as usize;
            if lv_now > 0 && row < ZRTDB_CONTROL_MX_COMMANDS {
                unsafe {
                    let status_base = std::ptr::addr_of_mut!((*controlptr).STATUS_COMMANDS) as *mut i32;
                    let val_base = std::ptr::addr_of!((*controlptr).VAL_COMMANDS) as *const f32;
                    let sig_base = std::ptr::addr_of!((*controlptr).SIG_COMMANDS) as *const i32;
                    let id_base = std::ptr::addr_of!((*controlptr).ID_COMMANDS) as *const i32;

                    let status = std::ptr::read_unaligned(status_base.add(row));
                    if status != 2 {
                        let val = std::ptr::read_unaligned(val_base.add(row));
                        let sig = std::ptr::read_unaligned(sig_base.add(row));
                        let id = std::ptr::read_unaligned(id_base.add(row));
                        let info = read_info(controlptr, row);

                        println!(
                            "consume: row={} id={} val={} sig={} status={} info={}",
                            row + 1,
                            id,
                            val,
                            sig,
                            status,
                            info
                        );

                        if last_sig >= 0 && sig <= last_sig {
                            eprintln!(
                                "WARN: non-increasing SIG observed (sig={}, last_sig={})",
                                sig, last_sig
                            );
                        }
                        last_sig = sig;

                        std::ptr::write_unaligned(status_base.add(row), 2);
                    }
                }
            }
        }

        thread::sleep(Duration::from_millis(500));
    }
}
