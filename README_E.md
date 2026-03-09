# ZRTDB (Zero-copy Real-Time Data Bus)

ZRTDB (Zero-copy Real-Time Data Bus) is a model-first real-time data plane designed for control systems, SCADA, and industrial edge workloads. The key engineering tradeoff is:

> move structural complexity into the modeling/instantiation phase, so runtime access becomes fixed-offset direct memory read/write.

This gives predictable latency and high operational transparency. ZRTDB is not intended to replace Redis/SQLite or network middleware; it is a same-host deterministic shared-memory foundation.

Main characteristics:

- **Zero-copy**: multiple processes map the same `.sec` partition files.
- **Strong determinism**: data layout is frozen during modeling; runtime path is effectively `base + offset`.
- **Operational tooling**: `zrtdb_tool` supports runtime inspection, query, write, paging view, and snapshots.

Typical roles in a project:

1. **Modeling**: describe schema in `DAT/` + `APPDAT.json`, compile with `zrtdb_model`.
2. **Operations**: inspect/modify runtime data via `zrtdb_tool`.
3. **Business integration**: C/C++/Rust applications map and access partitions using generated headers/bindings.

---

## 1. Directory Model and Artifacts: Static vs Runtime

### 1.1 Source repository directories

- `zrtdb_lib/`: runtime library (meta loading, mmap management, snapshots, shared helpers)
- `include/`: public headers (`zrtdb.h` is the C ABI entry)
- `zrtdb_model/`: model compiler + runtime instantiator
- `zrtdb_tool/`: operations and inspection CLI
- `zrtdb_watchdog/`: watchdog/audit daemon
- `DAT/`: example DAT and APPDAT definitions
- `example/`: C/C++ examples
- `example_rs/`: Rust example

### 1.2 Two roots

A) **Static root** (default `/usr/local/ZRTDB`)  
Contains `libzrtdb.a`, public headers, DAT examples, and generated model outputs (headers/defs/Rust bindings).

B) **Runtime root** (default `/var/ZRTDB`)  
Contains instantiated app directories (`.sec` partitions, meta, snapshots).

Environment variables:

- `MMDB_STATIC_ROOT` (legacy `ZRTDB_HOME` also supported in code paths)
- `MMDB_RUNTIME_ROOT`

### 1.3 Runtime directory shape

Per app:

```text
${MMDB_RUNTIME_ROOT:-/var/ZRTDB}/<APP>/
├── meta/
│   └── apps/
│       ├── <APP>.sec
│       └── <APP>_NEW.sec
├── zrtdb_data/
│   ├── <DB1>/<PART_A>.sec ...
│   └── <DB2>/<PART_B>.sec ...
├── <APP>_YYYYMMDD-HHMMSS.mmm/   # snapshot dir
│   ├── meta/apps/...
│   ├── zrtdb_data/...
│   └── manifest.json
└── .snaplock
```

---

## 2. Toolchain and Typical Workflow

Main executables:

- `zrtdb_model`: scans DAT, compiles definitions, and instantiates runtime artifacts
- `zrtdb_tool`: runtime operations (query/view/write/snapshot/status)
- business process: usually includes generated app header/binding and initializes mapping once

Recommended workflow:

1. Edit `DAT/*.DAT` and `DAT/APPDAT.json`.
2. Run `zrtdb_model`.
3. Start business processes (C/C++/Rust).
4. Use `zrtdb_tool` for operations and diagnosis.

---

## 3. Modeling Formats (DAT / APPDAT.json)

### 3.1 DAT directives

Common directives:

- `/DBNAME:<DB>`
- `/RECORD:<REC> DIM:<N>`
- `/PARTITION:<PART>`
- `/FIELD:<NAME> TYPE:<T>`
- `COMMENT:<text>` for machine-readable descriptions

Comments:

- `// ...` and `/* ... */` are true comments and are ignored by parser/tooling.
- Field/record descriptions should use `COMMENT:`.

Field binding rule:

- If field name has suffix `_<RECORD_NAME>`, it is bound to that record (array with record dimension).
- Otherwise it is a global scalar/fixed-length field.

### 3.2 Type mapping

Current implementation supports (case-insensitive):

- `int` / `int32` -> 4 bytes
- `long` / `int64` -> 8 bytes
- `float` -> 4 bytes
- `double` -> 8 bytes
- `string<N>` -> fixed byte array (implementation currently constrains upper bound)

### 3.3 Partition and alignment behavior

Runtime file granularity is **partition** (not record).

Compiler behavior includes:

- force-create a main partition named `<DB>`
- insert partition header/sentinel fields where required by model rules
- pad partition size to 4KiB boundaries
- generate LV (logical valid rows) helpers for records

Engineering recommendation: always rely on generated headers/bindings instead of manually calculating offsets.

### 3.4 APPDAT.json

`DAT/APPDAT.json` defines apps and referenced DBs centrally, for example:

```json
{
  "version": 1,
  "apps": [
    {"app": "CONTROLER", "dbs": ["MODEL", "CONTROL"]},
    {"app": "SIMULATION", "dbs": ["MODEL", "SIMU"]}
  ]
}
```

---

## 4. Runtime `.sec` and meta semantics

### 4.1 Partition file path

```text
${MMDB_RUNTIME_ROOT:-/var/ZRTDB}/<APP>/zrtdb_data/<DB>/<PART>.sec
```

### 4.2 Two meta files per app

In `meta/apps/`:

- `<APP>.sec`: static/clone meta
- `<APP>_NEW.sec`: runtime/instance meta

`setContext(APP)` loads both meta files.

### 4.3 Partition name forms: `PART` and `PART/DB`

For disambiguation in multi-DB apps:

- `PART` defaults to `DB=PART` in conventional cases
- `PART/DB` explicitly identifies DB ownership

Generated constants `ZRTDB_PART_<DB>_<PART>` use `PART/DB` form.

---

## 5. Build and Install (CMake)

### 5.1 Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 5.2 Install

```bash
sudo cmake --install build
```

Default static root install artifacts:

- `/usr/local/ZRTDB/libzrtdb.a`
- `/usr/local/ZRTDB/include/*.h`
- `/usr/local/ZRTDB/DAT/*`
- `/usr/local/ZRTDB/header/inc/<APP>.h`
- `/usr/local/ZRTDB/header/rust/<APP>.rs`

### 5.3 Environment setup recommendation

Set explicit roots in deployment scripts to avoid ambiguity:

```bash
export MMDB_STATIC_ROOT=/usr/local/ZRTDB
export MMDB_RUNTIME_ROOT=/var/ZRTDB
```

---

## 6. `zrtdb_model`: compile + instantiate

`zrtdb_model` does three things:

1. scan DAT directory
2. compile DB/APP definitions (`.DBDEF`, `.APPDEF`)
3. instantiate runtime app directories and partition files

It also generates:

- C headers: `header/inc/<APP>.h`
- Rust bindings: `header/rust/<APP>.rs`

---

## 7. `zrtdb_watchdog`: audit and overflow fuse

Watchdog focuses on operational safety:

- audit events for key runtime operations
- severe overflow detection/fuse actions according to model/runtime constraints

Use it as a guardrail layer, not a replacement for business-level validation.

---

## 8. `zrtdb_tool`: operations guide

`zrtdb_tool <APP> [DB]` provides runtime maintenance features including:

- status display (mapped partitions, process visibility, metadata)
- field query/update
- tabular page view for record-oriented inspection
- expression filtering and sorting
- snapshot creation/load operations

Practical recommendations:

1. use `LIMIT` for expensive scans
2. filter coarse-to-fine in expressions
3. combine `SORT` with `LIMIT`
4. keep business-critical logic in application code, not operations CLI scripts

---

## 9. Runtime mapping API and business-side integration

### 9.1 C ABI (`include/zrtdb.h`)

Main functions:

```c
int RegisterApp_(const char* app_name);
int MapMemory_(const char* part_nm, char** part_addr);
int free_MapMemory_();

int SnapshotReadLock_();
int SnapshotReadUnlock_();
int SaveSnapshot_(char* out_path, int out_len);
int LoadSnapshot_(const char* snapshot_name_or_path);
```

Generated app helpers:

```c
int zrtdb_app_<app>_init(zrtdb_app_<app>_ctx_t* ctx);
```

### 9.2 C++ helper layer

`mmdb::MmdbManager` provides context and partition mapping APIs internally used by runtime/tooling.
For long-term ABI stability, prefer C ABI for external integrations.

### 9.3 Snapshot lock cooperation

Writers should use `SnapshotReadLock_/Unlock_` around write batches.
Snapshot save/load obtains writer-side lock and is a low-frequency operations action.

---

## 10. Rust Interface (DAT-driven auto generation)

### 10.1 Design goals

Rust interface reuses DAT compiler outputs, ensuring Rust/C/C++ interpret the same `.sec` layout consistently.

### 10.2 Path and trigger

- output directory: `header/rust/`
- output file: `header/rust/<APP>.rs`
- generated automatically when `zrtdb_model` compiles app definitions

### 10.3 Generated content

Each generated Rust file contains:

- `unsafe extern "C"` declarations (`RegisterApp_`, `MapMemory_`)
- app/partition/capacity constants
- `#[repr(C, packed)]` structs matching partition layout
- app context struct (mapped partition pointers)
- Rust helper `zrtdb_app_<app>_init(...)`

> Note: packed structs can imply unaligned field access. In Rust business code, use safe unaligned-read patterns where needed.

### 10.4 Recommended Rust workflow

```bash
zrtdb_model
cd example_rs
cargo build

# Terminal A: producer
ZRTDB_DEMO_LOOPS=16 cargo run --bin policy_gen_rs

# Terminal B: consumer
ZRTDB_DEMO_LOOPS=32 cargo run --bin policy_exec_rs
```

`example_rs/build.rs` copies generated binding source from `${ZRTDB_STATIC_ROOT:-/usr/local/ZRTDB}/header/rust/<APP>.rs` to `OUT_DIR` and links `libzrtdb.a`.

Note: `policy_gen_rs` and `policy_exec_rs` are different binaries. Running them separately creates two independent OS processes (not a single process with different parameters).

### 10.5 Interop boundaries with C/C++

- DAT/APPDAT remains the single contract source.
- Do not manually edit generated `.h`/`.rs` files.
- After layout change, restart mapping processes to avoid mixed-version interpretation.

---

## 11. Snapshot notes

Snapshot directory includes meta and partition data plus `manifest.json`.
Implementation may use reflink where available, otherwise copy fallback.

`LoadSnapshot_` performs in-place overwrite on target paths. Treat load operations as maintenance activities and coordinate with process lifecycle.

---

## 12. FAQ / Troubleshooting

1. **`Meta file not found for APP=...`**  
   Run `zrtdb_model` and verify runtime meta files exist.

2. **Incomplete mapped-by list in `status`**  
   Usually due to `/proc` visibility or permission policies.

3. **Incompatibility after `.sec` rebuild**  
   Treat as a version switch; restart all mapping processes.

4. **`Layout fingerprint mismatch for sec manifest` / `ZRTDB init failed`**  
   This means the process model layout (code side) and on-disk `*.sec.manifest` (data side) are out of sync. A common case is rebuilding examples after DAT changes without regenerating runtime data. Recommended sequence:
   - run `zrtdb_model` again so `meta/` and manifests match current DAT;
   - clean/rebuild the target app runtime directory (default `/var/ZRTDB/<APP>/`);
   - restart all processes mapping that app, then run `example/policy_exec_cpp`;
   - for compile-chain verification only, run `example/policy_gen_cpp` first, then `policy_exec_cpp`.

5. **No `bin/` directory under `example/`**  
   The example CMake currently emits executables directly into the build directory (e.g. `example/policy_exec_cpp`), not a `bin/` subdirectory. Run binaries directly, or use out-of-source build: `cmake -S example -B build_example && cmake --build build_example`.

---

## 13. Technology positioning

Choose ZRTDB when you need:

1. high controllability and auditability
2. deterministic fixed-layout in-memory access
3. strong runtime observability (tool + watchdog)
4. clear separation between model artifacts and runtime instances

If you need cross-host discovery/interoperability and QoS-rich networking, technologies like DDS/zenoh/eCAL are often a better fit.
