use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zrtdb_example_rs::*;

fn now_text() -> String {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => format!("unix:{}", d.as_secs()),
        Err(_) => "unix:0".to_string(),
    }
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

    let mx = ZRTDB_CONTROL_MX_COMMANDS;

    println!("[GEN][pid={}] loops={} mx={} (run policy_exec_rs in another terminal)", pid, loops, mx);

    for seq in 1..=loops {
        let row = (seq - 1) % mx;

        with_snapshot_write(|| {
            write_command_row(
                &ctx,
                row,
                &CommandRow {
                    status: 0,
                    val: std::f32::consts::PI,
                    sig: seq as i32,
                    id: 200,
                    info: now_text() + " publish command",
                },
            );
            publish_lv(&ctx, (row + 1) as i32);
        });

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
