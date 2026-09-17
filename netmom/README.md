# ZRTDB NETMOM

`netmom` is a power-system network object-model layer built on ZRTDB. It keeps the
runtime path deterministic (`mmap + fixed offset + row index`) while adding the
object semantics needed by dispatch/EMS applications.

## 1. Model scope

The initial model contains:

- system metadata: `SYS` (case ID and base MVA);\n- organization hierarchy: `CO -> DV -> ST -> KV`;
- physical electrical nodes: `ND`;
- switching devices: `CBTYP -> CB`;
- explicit zero-impedance connections: `ZBR`;
- AC branches: `LN`;
- transformers / phase shifters: `XF`;
- loads, generators and fixed shunts: `LD`, `UN`, `CP`;
- topology results: `TOPO`, `CALCBUS`, `ISLAND`.

`CBTYP.KIND` uses:

- `1`: circuit breaker;
- `2`: disconnector / isolating switch.

A `CB` joins `ND1` and `ND2` in topology only when `IN_SERVICE=1` and
`CLOSED=1`. Breakers and disconnectors therefore use the same fast topology
primitive while retaining their equipment type.

The BPA importer creates the two default `CBTYP` rows. BPA bus/branch models do
not contain substation-level breaker-and-a-half / double-bus switching detail,
so imported BPA cases normally have no `CB` rows until station connectivity is
added from another source or edited by an application.

## 2. BPA/IPF importer

The executable is:

```bash
zrtdb_bpa_import case.dat
```

Optional arguments:

```bash
zrtdb_bpa_import case.dat --app NETMOM --zero-eps 1e-8 --topology
```

Supported base-case fixed-column records:

- AC buses: `B`, `BC`, `BE`, `BF`, `BG`, `BQ`, `BS`, `BT`, `BV`, `BX`;
- lines and equivalent branches: `L`, `E`;
- transformers and phase shifters: `T`, `TP`.

The importer maps each BPA AC bus to one `ND`, creates `LD`/`UN`/`CP` objects
from the bus-card load, generation and shunt fields, and maps `L/E/T/TP` to
`LN/XF`. Same-voltage line/equivalent records whose `|R|` and `|X|` do not
exceed `--zero-eps` are stored as `ZBR`.

BPA/IPF sometimes represents a normally closed bus tie with a very small
reactance (historically around `0.00020 pu`) rather than mathematical zero.
That is intentionally **not** silently merged by the default `1e-8` tolerance.
For a data set that uses that convention, select an explicit engineering
threshold, for example:

```bash
zrtdb_bpa_import case.dat --zero-eps 0.00025 --topology
```

The parser honors fixed-column implicit decimals (`F6.5`, `F4.3`, etc.).
Continuation (`+`) bus cards and DC `BD/BM/LD/LM` models are currently reported
as warnings and are not imported in this first version. BPA change records
(`M/D/R` in the change-code column) are also skipped; import a consolidated base
case when exact post-change state is required.

The field positions follow the BPA Interactive Power Flow record definitions:
<https://bpa-ipf.readthedocs.io/en/latest/basic/record_formats.html>.

## 3. Calculation-bus formation

Run:

```bash
zrtdb_topology
```

or calculate immediately after import with `zrtdb_bpa_import ... --topology`.

The calculation-bus algorithm uses disjoint-set union (union-find):

1. start with every in-service `ND` as an independent physical node;
2. merge endpoints of closed/in-service `CB` devices;
3. merge endpoints of closed/in-service `ZBR` records;
4. optionally merge same-voltage `LN` records with `|R|,|X| <= zero_impedance_eps`;
5. assign each resulting component a deterministic `CALCBUS` ID.

The per-node result is stored in `TOPO`:

```text
ND_IDX_TOPO -> CALCBUS_ID_TOPO -> ISLAND_ID_TOPO
```

This keeps the static physical node model unchanged while making the current
switching state visible as a derived bus model.

## 4. Electrical-island identification

After calculation buses are formed, the topology engine builds a graph whose
vertices are calculation buses. In-service nonzero `LN` and `XF` objects are
edges. Connected components are electrical islands.

`ISLAND` stores:

- calculation-bus count;
- physical-node count;
- in-service generator count;
- in-service load count;
- `ENERGIZED=1` when at least one in-service generator is present.

This distinction is important:

```text
closed breaker / disconnector / ZBR
        => merge physical nodes into one calculation bus

line / transformer
        => connect calculation buses inside an electrical island
```

## 5. Build and initialize

Build ZRTDB normally:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

`DAT/APPDAT.json` now contains the `NETMOM` APP and `DAT/NETMOM.DAT` defines the
database. Re-run `zrtdb_model` after installation so the NETMOM meta and `.sec`
partitions are instantiated before using the importer:

```bash
sudo zrtdb_model
zrtdb_bpa_import case.dat --topology
zrtdb_tool NETMOM NETMOM
```

Useful records to inspect with `zrtdb_tool` are `ND`, `CB`, `ZBR`, `LN`, `XF`,
`TOPO`, `CALCBUS`, and `ISLAND`.

## 6. C++ API

Public declarations are in:

```cpp
#include <netmom/netmom.hpp>
```

The API exposes:

- `netmom::Database`: dynamic record/field access over ZRTDB runtime meta;
- `netmom::BpaParser`: pure fixed-column BPA parser;
- `netmom::TopologyEngine`: pure in-memory topology calculation;
- `netmom::write_bpa_case`: persist a parsed BPA case to ZRTDB;
- `netmom::load_network`: read the active NETMOM model from ZRTDB;
- `netmom::store_topology`: persist calculation-bus/island results.

The parser and topology engine are independent of shared-memory runtime state,
so their core behavior can be unit-tested without creating a ZRTDB instance.
