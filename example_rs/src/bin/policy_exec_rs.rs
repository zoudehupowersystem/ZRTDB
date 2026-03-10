use std::thread;
use std::time::Duration;
use zrtdb_example_rs::*;

fn main() {
    let pid = std::process::id();
    let ctx = app_init();
    let loops: usize = std::env::var("ZRTDB_DEMO_LOOPS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(12);

    if ctx.CONTROL_CONTROLPTR.is_null() || ctx.CONTROL_CONTROL.is_null() {
        eprintln!("CONTROL mapping is null");
        std::process::exit(2);
    }

    let mut prev_lv = 0i32;
    let mut last_sig = -1i32;

    println!("[EXEC][pid={}] loops={} (consumer polling)", pid, loops);

    for tick in 1..=loops {
        let lv_now = load_lv(&ctx);

        if lv_now != prev_lv {
            prev_lv = lv_now;
            let row = (lv_now - 1).max(0) as usize;
            if lv_now > 0 && row < ZRTDB_CONTROL_MX_COMMANDS {
                let cmd = read_command_row(&ctx, row);
                if cmd.status != 2 {
                    println!(
                        "[EXEC][pid={}] tick={:04} consume row={:04} id={} val={:.7} sig={:04} status={} info={}",
                        pid,
                        tick,
                        row + 1,
                        cmd.id,
                        cmd.val,
                        cmd.sig,
                        cmd.status,
                        cmd.info
                    );

                    if last_sig >= 0 && cmd.sig <= last_sig {
                        eprintln!(
                            "[EXEC][pid={}] WARN: non-increasing SIG observed (sig={}, last_sig={})",
                            pid, cmd.sig, last_sig
                        );
                    }
                    last_sig = cmd.sig;

                    mark_command_done(&ctx, row);
                }
            }
        }

        thread::sleep(Duration::from_millis(500));
    }
}
