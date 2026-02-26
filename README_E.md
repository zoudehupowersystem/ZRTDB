# ZRTDB (Zero-copy Real-Time Data Bus)

ZRTDB is a model-first shared-memory data plane for real-time control, SCADA, and industrial edge workloads.
Its core idea is to move structural complexity into the modeling/build phase, so runtime access becomes fixed-offset read/write with predictable latency.

ZRTDB is **not** a replacement for network databases or general-purpose message middleware.
It is a practical infrastructure layer for **same-host, deterministic, inspectable** real-time data sharing.

## 1. Core Features

- **Zero-copy data path**: multiple processes map the same `.sec` partition files.
- **Deterministic layout**: schema is frozen by DAT/APPDAT compilation.
- **Operational tooling**: built-in `zrtdb_tool` for inspection, query, editing, paging, and snapshots.
- **Safety guardrails**: `zrtdb_watchdog` auditing and overflow fuse behavior.

## 2. Repository Layout

- `zrtdb_lib/`: runtime library and shared capabilities
- `include/`: public C API headers (notably `zrtdb.h`)
- `zrtdb_model/`: model compiler + runtime instantiator
- `zrtdb_tool/`: operations/inspection CLI
- `zrtdb_watchdog/`: watchdog/audit service
- `DAT/`: sample DAT and APPDAT definitions
- `example/`: C/C++ examples
- `example_rs/`: Rust example

## 3. Static vs Runtime Roots

ZRTDB uses two roots:

1. **Static root** (default: `/usr/local/ZRTDB`)
   - library, headers, DAT, generated app headers, generated Rust bindings
2. **Runtime root** (default: `/var/ZRTDB`)
   - app instances, partition files, runtime meta, snapshots

Environment variables:

- `MMDB_STATIC_ROOT` (or legacy `ZRTDB_HOME`) for static root
- `MMDB_RUNTIME_ROOT` for runtime root

## 4. Data Modeling

### 4.1 DAT

A DAT file defines DB schema using directives such as:

- `/DBNAME:<DB>`
- `/RECORD:<REC> DIM:<N>`
- `/PARTITION:<PART>`
- `/FIELD:<NAME> TYPE:<T> [COMMENT:<text>]`

C/C++-style comments (`//`, `/* ... */`) are ignored.

### 4.2 APPDAT.json

`DAT/APPDAT.json` defines apps and DB dependencies, for example:

```json
{
  "version": 1,
  "apps": [
    {"app": "CONTROLER", "dbs": ["MODEL", "CONTROL"]},
    {"app": "SIMULATION", "dbs": ["MODEL", "SIMU"]}
  ]
}
```

## 5. Build and Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build
```

Key generated/install outputs include:

- `/usr/local/ZRTDB/libzrtdb.a`
- `/usr/local/ZRTDB/include/*.h`
- `/usr/local/ZRTDB/DAT/*`
- `/usr/local/ZRTDB/header/inc/<APP>.h`
- `/usr/local/ZRTDB/header/rust/<APP>.rs`

## 6. Typical Workflow

1. Edit DAT and `APPDAT.json` in static root.
2. Run `zrtdb_model` to compile definitions and instantiate runtime data.
3. Start business applications that call `RegisterApp_` / `MapMemory_` (or generated helper init).
4. Use `zrtdb_tool` for inspection and operations.

## 7. Runtime API (C ABI)

Public API is in `include/zrtdb.h`:

- `RegisterApp_(const char* app_name)`
- `MapMemory_(const char* part_nm, char** part_addr)`
- `free_MapMemory_()`
- `SnapshotReadLock_()` / `SnapshotReadUnlock_()`
- `SaveSnapshot_(char* out_path, int out_len)`
- `LoadSnapshot_(const char* snapshot_name_or_path)`

Generated app headers also provide:

- `zrtdb_app_<app>_init(zrtdb_app_<app>_ctx_t* ctx)`

## 8. Rust Interface (Auto-generated from DAT)

Rust bindings are generated automatically by `zrtdb_model`:

- output path: `header/rust/<APP>.rs`
- generated from the same DAT/APPDAT model source as C headers
- includes:
  - `unsafe extern "C"` declarations (`RegisterApp_`, `MapMemory_`)
  - partition/app constants
  - `#[repr(C, packed)]` partition structs
  - app context struct and Rust init helper

Recommended Rust flow:

```bash
zrtdb_model
cd example_rs
cargo build
cargo run
```

`example_rs/build.rs` copies generated bindings from `${ZRTDB_STATIC_ROOT:-/usr/local/ZRTDB}/header/rust/<APP>.rs` into `OUT_DIR` and links `libzrtdb.a`.

## 9. Snapshots and Operational Notes

- Snapshots are low-frequency operations; avoid invoking in hard real-time loops.
- Snapshot/load uses cooperative lock semantics with writers.
- For schema/layout changes, restart mapping processes to avoid mixed-version interpretation.

## 10. Troubleshooting

- **Meta not found**: run `zrtdb_model` and confirm `<APP>.sec` / `<APP>_NEW.sec` in runtime meta path.
- **Incomplete mapped-by list in tool**: likely OS permission/`/proc` visibility limitations.
- **Layout mismatch after DAT change**: treat as version switch; rebuild/re-instantiate and restart apps.

## 11. When to Choose ZRTDB

ZRTDB is a strong fit when you need:

- single-host, high-throughput deterministic IPC
- fixed schema with strict layout control
- operational visibility and fast root-cause workflows
- model artifacts separated from runtime instances for traceability

If you need cross-host discovery, QoS-rich networking, and broad interoperability, middleware like DDS/zenoh/eCAL may be more appropriate.
