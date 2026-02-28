pub mod generated {
    #![allow(non_snake_case)]
    #![allow(non_camel_case_types)]
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/zrtdb_generated.rs"));
}

pub use generated::*;

pub fn app_init() -> zrtdb_app_controler_ctx_t {
    let mut ctx = std::mem::MaybeUninit::<zrtdb_app_controler_ctx_t>::zeroed();
    let rc = unsafe { zrtdb_app_controler_init(ctx.as_mut_ptr()) };
    if rc < 0 {
        eprintln!("zrtdb_app_controler_init failed");
        std::process::exit(1);
    }
    unsafe { ctx.assume_init() }
}
