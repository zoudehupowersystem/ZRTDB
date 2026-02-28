# example_rs

Rust examples that access the same ZRTDB shared-memory layout as the C/C++ demos.

## Prerequisites

1. Build/install ZRTDB and run `zrtdb_model` first so DAT-based Rust bindings are generated:

```bash
zrtdb_model
# Generates /usr/local/ZRTDB/header/rust/<APP>.rs
```

2. Default APP is `CONTROLER` (matches the C++ example). You can switch APP with:

```bash
export ZRTDB_APP=SIMULATION
```

## Binaries

This crate now provides **at least two Rust processes** that mirror `example/policy_gen_cpp.cpp` and `example/policy_exec_cpp.cpp`:

- `policy_gen_rs`: producer/publisher process writing COMMANDS rows and publishing LV
- `policy_exec_rs`: consumer/executor process watching LV, reading latest row, and marking status done

There is also a tiny `zrtdb_example_rs` binary (`src/main.rs`) for basic mapping smoke checks.

## Build

```bash
cd example_rs
cargo build
```

## Run two Rust processes (recommended)

Terminal 1:

```bash
cd example_rs
ZRTDB_DEMO_LOOPS=8 cargo run --bin policy_gen_rs
```

Terminal 2:

```bash
cd example_rs
ZRTDB_DEMO_LOOPS=16 cargo run --bin policy_exec_rs
```

You should see `publish: ...` logs in generator and `consume: ...` logs in executor.

## Optional smoke run

```bash
cd example_rs
cargo run --bin zrtdb_example_rs
```

## Optional static root override

```bash
export ZRTDB_STATIC_ROOT=/your/static/root
cargo build
```
