use zrtdb_example_rs::app_init;

fn main() {
    let ctx = app_init();
    if ctx.MODEL_MODELPTR.is_null() {
        eprintln!("MODEL/MODELPTR mapping is null");
        std::process::exit(2);
    }

    let status = unsafe { std::ptr::read_unaligned(std::ptr::addr_of!((*ctx.MODEL_MODELPTR).STATUS)) };
    println!("MODEL.STATUS = {}", status);
}
