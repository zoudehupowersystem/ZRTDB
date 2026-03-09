# 零拷贝实时数据总线 ZRTDB

ZRTDB（Zero-copy Real-Time Data Bus）是面向实时控制、SCADA、工业边缘计算等需求，以共享内存为载体、以静态建模为契约的实时数据平面。它的核心取舍是：**把结构复杂度前移到建模/实例化阶段**，运行期只做“固定偏移 + 直接读写”，从而获得可预测的时延与可运维的现场定位能力。ZRTDB不是Redis/SQLite 的替代品，而是同机实时数据的可运维基础设施。

与传统数据库或消息队列不同，ZRTDB重点确保强确定性与可预测时延。ZRTDB 的三个关键特性是：

- 零拷贝：多个进程映射同一 `.sec` 分区文件，读写直接作用于共享页，避免运行期序列化/反序列化。
- 强确定性：字段布局在建模阶段固化；运行期访问等价于“`base + offset`”，避免动态分配与复杂容器。
- 工程可运维：提供 `zrtdb_tool` 运维交互工具，支持运行期巡检、字段读取/写入、表格视图与快照。

工程使用者/开发者通常涉及三类工作：

1) 建模：用 `DAT/APPDAT.json`文件描述共享内存数据结构；由 `zrtdb_model` 编译并实例化运行期产物  
2) 运维：用 `zrtdb_tool` 对运行期数据进行查询/修改/翻页查看，并理解其视图与键盘交互逻辑  
3) 业务集成：应用程序链接 `libzrtdb.a`，引入 `zrtdb_model` 生成的头文件（每个 APP 一个 `header/inc/<APP>.h`），按生成的 context/helper 或裸指针/结构体访问数据
   Rust 应用可直接使用 `zrtdb_model` 生成的 `header/rust/<APP>.rs` 访问同一套共享内存布局。

本项目当前版本的软硬件环境默认 **Linux x86-64 + gcc/clang（C++20）**，当前版本暂时不考虑跨平台。未来可考虑移植到Windows平台、增加Rust语言的接口等。

---

## 1. 目录与产物：静态建模 vs 运行期实例

### 1.1 仓库目录（源码）

- `zrtdb_lib/`：运行期库（meta 读取、mmap 管理、快照、工具共用能力）
- `include/`：公共头文件（C API 在 `zrtdb.h`）
- `zrtdb_model/`：建模编译 + 实例化工具（解析 `DAT/APPDAT.json`，生成 meta 与 `.sec`）
- `zrtdb_tool/`：运维交互工具（查看/修改/翻页/快照/status）
- `zrtdb_watchdog/`：守护进程（日志/严重越界检测）
- `DAT/`：示例 DAT/APPDAT.json（可按工程重构）
- `example/`：示例工程（实际工程可参考）

### 1.2 两套根目录

ZRTDB 把文件/目录明确分成两类：

A) 安装期（静态根目录，默认 `/usr/local/ZRTDB`）  
包含：`libzrtdb.a`、公共头文件、示例 `DAT/APPDAT.json`、`zrtdb_model` 生成的头文件与 DB/APP 定义文件。

B) 运行期（动态根目录，默认 `/var/ZRTDB`）  
包含：每个 APP 的实例化目录（`.sec` 分区文件、meta、快照目录）。

通过环境变量可以显式指定：

- `MMDB_STATIC_ROOT`：静态根目录（默认 `/usr/local/ZRTDB`）
- `MMDB_RUNTIME_ROOT`：运行期根目录（默认 `/var/ZRTDB`）

### 1.3 运行期目录结构

每个 APP 在运行期根目录下有一个独立目录（APP 名会被归一化为大写）：

```
${MMDB_RUNTIME_ROOT:-/var/ZRTDB}/<APP>/
├── meta/
│   └── apps/
│       ├── <APP>.sec          # 静态建模 meta（clone meta）
│       └── <APP>_NEW.sec      # 运行期实例 meta（runtime meta）
├── zrtdb_data/
│   ├── <DB1>/
│   │   ├── <PART_A>.sec       # 分区文件：每个 Partition 一个 .sec
│   │   ├── <PART_B>.sec
│   │   └── ...
│   ├── <DB2>/
│   │   ├── <PART_C>.sec
│   │   └── ...
│   └── ...
├── <APP>_YYYYMMDD-HHMMSS.mmm/ # 快照目录（由 SNAP/SaveSnapshot_ 产生）
│   ├── meta/apps/<APP>.sec
│   ├── meta/apps/<APP>_NEW.sec
│   ├── zrtdb_data/<DB>/<PART>.sec ...
│   └── manifest.json
└── .snaplock                  # 进程间快照锁文件（内部使用）
```


---

## 2. 工具链与典型工作流

ZRTDB 主要可执行工具：

- `zrtdb_model`：扫描静态根目录下的 `DAT/`，编译所有 `*.DAT` + `APPDAT.json`，生成建模产物并实例化每个 APP 的运行期目录
- `zrtdb_tool`：运行期交互工具（按 DB/Record/Field 维度浏览与写入；支持 status 与快照）
- 业务进程：通常直接 include `header/inc/<APP>.h`，调用生成的 `zrtdb_app_<app>_init()` 一次完成 `RegisterApp_ + MapMemory_`；也可手工调用底层 C API

典型工作流：

1) 在静态根目录准备/修改 `DAT/APPDAT.json`  
2) 运行 `zrtdb_model`：生成头文件、DB/APPDEF，并在运行期根目录实例化 `.sec` 与 meta  
3) 启动业务进程：推荐直接调用 `zrtdb_model` 生成的 `zrtdb_app_<app>_init()`；若需细粒度控制，也可手工 `RegisterApp_()` + `MapMemory_()` 读取 meta 并映射所需分区  
4) 运维：用 `zrtdb_tool <APP> [DB]` 进入交互界面，做巡检、定位、快照与离线回放

---

## 3. 建模文件格式（DAT / APPDAT.json）

### 3.1 DAT 文件：DB 主定义

DAT 的语法是“指令行 + 注释”的工程化子集，常用指令如下：

- `/DBNAME:<DB>`：声明 DB 名（必需）
- `/RECORD:<REC> DIM:<N>`：声明记录（二维表）与最大容量（MX）
- `/PARTITION:<PART>`：声明分区（Partition）；之后的字段会归属该分区
- `/FIELD:<NAME> TYPE:<T>`：声明字段
- `COMMENT:<text>`：可选属性，用于声明“机器可读取的说明文本”（运维工具可视化展示）

注释规则：

- `// ...` 与 `/* ... */` 是**真正的注释**，编译器与工具都会忽略（与 C/C++ 一致）
- 字段/记录的说明请使用 `COMMENT:` 属性，不要再依赖 `//` 注释

一个示例：

```
/*
真正的注释（编译器忽略）
*/
/DBNAME:CONTROL
/RECORD:COMMANDS DIM:1000  COMMENT:控制指令
/PARTITION:CONTROLPTR
/FIELD:SELFMAC         TYPE:string64  COMMENT:控制对象 MAC
/FIELD:SELFIP          TYPE:string64  COMMENT:控制对象 IP
/FIELD:INFO_COMMANDS   TYPE:string64  COMMENT:指令描述（绑定 COMMANDS 记录）
/FIELD:ID_COMMANDS     TYPE:int       COMMENT:指令点号（绑定 COMMANDS 记录）
/FIELD:VAL_COMMANDS    TYPE:float     COMMENT:输出值（绑定 COMMANDS 记录）
```

字段绑定（Record 归属）规则：

- 若字段名存在后缀 `_<RECORD_NAME>`，则该字段属于该记录（存储为数组，维度为该记录的 MX）
- 否则属于全局域（标量/定长字符串），存储为单值


### 3.2 类型映射（实现约定）

当前编译器支持如下类型关键字（不区分大小写）：

- `int` / `int32` → 4 字节
- `long` / `int64` → 8 字节
- `float` → 4 字节
- `double` → 8 字节
- `string<N>`（如 `string64`）→ 固定字节数组（实现对 N 做上限约束，N≤120）

### 3.3 分区与对齐机制

实现层面，`.sec` 文件的粒度是 **Partition**，而不是“每个 Record 一个文件”。一个分区内可以同时包含：

- 全局字段（标量/定长字符串）
- 记录字段（数组，维度为记录 MX）

为了满足工程侧的 mmap 与页对齐需求，`zrtdb_model` 会在编译阶段对分区做如下处理：

- 强制创建“主分区”：主分区名称等于 DB 名（即 `<DB>` 本身）；若你的 DAT 里只有 `/FIELD`，它们默认落在主分区
- 每个分区会插入分区头字段（同名 `int32`），并对分区大小做 4KiB 对齐（必要时插入 padding 字段）
- 对每个 Record，会在主分区额外生成“当前有效行数（LV）”字段。运行期 meta 与 `zrtdb_tool` 仍沿用历史显示名 `LV$<REC>`；但生成的 C/C++ 头文件会把它转成合法标识符 `LV_<REC>`（例如 `LV_COMMANDS`）。
  工程约束：建议 `LV <= MX`；当 `LV >= MX` 时，`zrtdb_tool` 启动会给出中英文严重告警并提示扩容 DAT；
  当 `LV > 3*MX` 时，`zrtdb_watchdog` 将认为已越过物理缓冲上限并强制杀掉全部映射该 APP 的进程（避免进一步破坏共享内存）。

工程实践建议：应用程序与工具均应以 **生成的头文件** 为准访问布局，不要“自行手算偏移”。

### 3.4 APPDAT：应用组织

APPDAT 现在采用**单一 JSON 文件**：`DAT/APPDAT.json`，集中声明所有 APP 以及每个 APP 关联的 DB 列表。

示例：

```json
{
  "version": 1,
  "apps": [
    {"app": "CONTROLER", "dbs": ["MODEL", "CONTROL"]},
    {"app": "SIMULATION", "dbs": ["MODEL", "SIMU"]}
  ]
}
```

含义：

- 创建两个 APP：`CONTROLER`、`SIMULATION`
- `CONTROLER` 关联 `MODEL` 与 `CONTROL` 两个 DB
- `SIMULATION` 关联 `MODEL` 与 `SIMU` 两个 DB
- APP 与 APP 在运行期目录上完全隔离；不同 APP 可以关联相同的 DB 结构（共享建模定义），但运行期 `.sec` 文件仍然按 APP 实例化


---

## 4. 运行期 `.sec` / meta 的语义与路径

### 4.1 `.sec` 文件语义

当前实现中，分区文件路径为：

```
${MMDB_RUNTIME_ROOT:-/var/ZRTDB}/<APP>/zrtdb_data/<DB>/<PART>.sec
```

其中 `<PART>` 是分区名，`<DB>` 是该分区所属 DB 名（均会归一化为大写）。主分区的一个常见形态是：

```
.../<APP>/zrtdb_data/CONTROL/CONTROL.sec
```

### 4.2 meta 文件的分工

每个 APP 都有两份 meta（均位于 `meta/apps/`）：

- `<APP>.sec`：静态建模 meta（clone meta，描述 DB/Partition/Field 的建模结果）
- `<APP>_NEW.sec`：运行期实例 meta（runtime meta，描述 APP 关联 DB、以及 field/record 的运行期索引表）

`MmdbManager::setContext(APP)` 会同时加载这两份 meta；但 **不会自动 mmap 分区文件**，分区映射由 `MapMemory_()`/`mapPartition()` 按需触发。

### 4.3 分区名参数约定：`PART` 与 `PART/DB`

为了在一个 APP 内区分“同名分区来自哪个 DB”的情况，分区名参数支持两种写法：

- `PART`：省略 DB 时，默认 `DB = PART`（通常用于主分区：`CONTROL`）
- `PART/DB`：显式指定分区所属 DB

`zrtdb_model` 生成的头文件会为每个分区提供形如 `ZRTDB_PART_<DB>_<PART>` 的字符串常量（例如 `ZRTDB_PART_CONTROL_CONTROLPTR`），其值采用 `PART/DB` 形式。建议业务侧直接使用这些常量，避免手写分区名字符串造成歧义。

---

## 5. 编译与安装（推荐使用 CMake install）

### 5.1 编译

```
cd ZRTDB
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 5.2 安装（默认安装到 /usr/local/ZRTDB）

```
sudo cmake --install build
```

安装布局由两个变量控制：

- `ZRTDB_INSTALL_ROOT`：静态根目录（默认 `${CMAKE_INSTALL_PREFIX}/ZRTDB`，即 `/usr/local/ZRTDB`）
- `ZRTDB_INSTALL_BINDIR`：可执行文件目录（默认 `/bin`；你也可以设为 `/usr/local/bin`）

示例（把工具安装到 `/usr/local/bin`）：

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   -DZRTDB_INSTALL_BINDIR=/usr/local/bin
sudo cmake --install build
```

安装完成后，常用可执行文件包括：

- `zrtdb_model`：编译 DAT/APPDAT.json + 实例化运行期目录
- `zrtdb_tool`：运维交互工具
- `zrtdb_watchdog`：运行期守护进程（通常由 tool/业务进程自动拉起；需保证在 PATH 中可执行）

### 5.3 运行前的环境变量建议

建议显式设置：

```
export MMDB_STATIC_ROOT=/usr/local/ZRTDB
export MMDB_RUNTIME_ROOT=/var/ZRTDB
```

注意：`zrtdb_model` 需要在运行期根目录创建 `<APP>/meta` 与 `<APP>/zrtdb_data`，因此需具备写权限（常见做法是以 root 运行或调整目录属主/权限）。

---

## 6. zrtdb_model：编译 + 实例化

`zrtdb_model` 当前无命令行参数，启动后会做三件事：

1) 扫描 `${MMDB_STATIC_ROOT}/DAT`  
2) 编译所有 `*.DAT` 生成 DBDEF，并生成 C 头文件到 `${MMDB_STATIC_ROOT}/header/inc`  
3) 编译 `APPDAT.json` 生成全部 APP 的 APPDEF，并为每个 APP 在 `${MMDB_RUNTIME_ROOT}/<APP>/...` 下创建 meta 与 `.sec`

生成的静态建模产物（重要）：

- `${MMDB_STATIC_ROOT}/header/.datdef/<DB>.DBDEF`
- `${MMDB_STATIC_ROOT}/header/.datdef/<APP>.APPDEF`
- `${MMDB_STATIC_ROOT}/header/inc/*`（供业务侧 include）

布局一致性（layout fingerprint）：

- `zrtdb_model` 会基于 DB/Record/Field 逻辑结构、分区字节布局、类型映射结果与代码生成版本计算统一 fingerprint；
- fingerprint 会写入 `.DBDEF/.APPDEF`、runtime meta（`<APP>.sec` 与 `<APP>_NEW.sec`）、生成的 C/Rust 头文件常量；
- 同时为每个分区 `.sec` 生成同目录侧车 `*.sec.manifest`（包含 fingerprint）；
- 运行期 `RegisterApp_()/MapMemory_()` 会校验 fingerprint，不兼容时直接拒绝映射，而不是“建议重启”。

---

## 7. zrtdb_watchdog：审计日志 + 越界保险丝

`zrtdb_watchdog` 是一个“无需显式配置文件”的运行期守护进程，定位是：

1) 记录审计日志（tool 交互、库接口调用的关键动作）
2) 周期性监测每个 record 的 `LV$<REC>` 与建模维度 `MX`
3) 提供硬保险丝：当任何 record 的 `LV > 3*MX` 时，先做紧急快照并记录触发原因，再立刻杀掉所有映射该 APP 的进程，防止进一步破坏共享内存

### 7.1 自动拉起（无需用户手工启动）

- 当 `zrtdb_tool` 启动时，会 best-effort 自动拉起 `zrtdb_watchdog` 并登记本工具进程
- 当业务进程调用 `RegisterApp_(APP)`（或使用 `MmdbManager::setContext(APP)`）时，也会 best-effort 自动拉起

如果你确实希望禁用 watchdog（例如单元测试），可临时设置环境变量：

```
export ZRTDB_WATCHDOG_DISABLE=1
```

### 7.2 日志路径与格式

watchdog 以 JSONL（每行一个 JSON）追加写日志，路径固定为：

```
${MMDB_RUNTIME_ROOT:-/var/ZRTDB}/<APP>/meta/zrtdb_watchdog.log
```

日志内容包含：tool 的每次输入行（含翻页键）、库 API 的审计事件（audit）、以及 watchdog 自身的告警/杀进程记录。

### 7.3 容量策略（MX、CAP、LV）与处置规则

本版本采用“保守缓冲”的物理容量策略：建模期每个 record 的物理容量 `CAP` 取 `3*MX`。

- `MX`：DAT 中定义的逻辑维度（DIM），业务侧应将其视为“可接受的最大规模”
- `CAP`：`.sec` 文件里实际为该 record 预留的物理容量（默认 `3*MX`，用于越界缓冲/保险丝）
- `LV`：运行期有效行数（由 `LV$<REC>` 维护）

处置规则：

- 当 `LV >= MX`：`zrtdb_tool` 启动立即给出“严重告警/CRITICAL WARNING”，并列出异常 record，建议将 DAT 中对应 `DIM(MX)` 扩容到 `>= 2*LV`
- 当 `LV > 3*MX`：`zrtdb_watchdog` 判定已越过物理缓冲上限（通常意味着越界写入风险已发生），将强制 kill 所有映射该 APP 的进程，并写入 FATAL 日志

注意：watchdog 的“杀进程”是最后手段，设计目标是把潜在的数据破坏限制在最小范围；工程上仍应把 `LV <= MX` 作为强约束。

---

## 8. zrtdb_tool：运维交互指南（以当前实现为准）

### 8.1 启动方式

`zrtdb_tool` 需要至少一个参数：APP 名；可选第二个参数指定 DB：

```
zrtdb_tool <APP> [DB]
```

- 若不指定 `[DB]`，工具默认选择该 APP 关联的第一个 DB  
- 当前版本不支持在交互会话内切换 DB；如需切换 DB，请重新启动工具并指定 DB

启动后会打印 APP 信息，并进入提示符。提示符形式为：

```
<DB>  RH>
```

其中 `RH>` 是历史遗留的固定提示符（当前实现未把 APP 名显示在提示符里；启动 banner 会显示 APP）。

### 8.2 命令总览（`H` 查看）

当前版本的核心命令是围绕“Record / Global / Field”的浏览与写入：

- `LI`：列出当前 DB 的记录（record）列表
- `LIG` 或 `li`：列出全局域（ITEM）字段列表
- `P/RECNAME`：切换当前记录；`P/item` 切回全局域（ITEM）
- `+n` / `-n`：移动当前行指针（slot）
- `/FIELD`：查看当前行的字段值（FIELD 指当前记录/全局域中的某个字段名）
- `/FIELD(3:9)`、`/FIELD(3:)`：查看指定范围行
- `/FIELD(:)=220`：批量写入范围
- `LV$REC = 3`：设置记录的 LV（有效行数，<= MX）
- `show/RECNAME`：表格视图（默认 10×10），支持键盘翻页
- `show/item`：全局域的表格视图
- `SEL <expr> [COLS ...] [SORT ...] [LIMIT n|ALL] [OFFSET n] [GOTO first|k]`：轻量筛选查询（非 SQL），按条件过滤当前记录/全局域，并可排序/投影输出
- `FIND <expr> [...]`：在当前记录/全局域中查找第一条命中并定位到该行（等价于默认 `LIMIT 1 GOTO first`）
- `NEXT` / `N`：在“上一次 SEL/FIND 的命中集合”中跳到下一条，并自动 `show current`（便于巡检定位）
- `PREV` / `B`：在“上一次 SEL/FIND 的命中集合”中跳到上一条
- `status`：扫描运行期根目录并汇总（meta、`.sec` 文件、映射进程；受权限影响可能不完整）
- `DB <name>`：会话内切换当前 DB
- `EXPORTJSON [file]`：导出当前作用域（ITEM 或当前记录）为 JSON，默认 `./zrtdb_export.json`
- `SNAP [note...]`：保存快照到 `/var/ZRTDB/<APP>/<APP>_YYYYMMDD-HHMMSS.mmm/`
- `LSNAP`：列出当前 APP 的快照
- `LOADSNAP <dir>`：恢复某个快照（可传相对名或绝对路径）
- `Q` / `EX`：退出

### 8.3 表格视图的翻页键

在 `show/...` 进入表格视图后，支持以下按键（输入后回车生效）：

- `v` / `^`：下翻/上翻一页（按行）
- `>` / `<`：右翻/左翻一屏（按列）
- `li`：退出视图并回到全局域列表


### 8.4 轻量查询与排序：SEL / FIND / NEXT / PREV（非 SQL）

本版本在 `zrtdb_tool` 中新增了一个“工程化轻量查询层”，目标是解决运行期常见的两类运维痛点：

- 快速定位：在一个 record（二维表）里按条件找出“异常行/关注行”，并一键跳转到该行继续用 `/FIELD`、`show/...` 等原生命令巡检；
- 排序浏览：把“最大/最小/最新/最异常”的若干行排到前面，减少翻页成本。

设计边界（刻意不做数据库）：

1) 不引入 SQL 语法；命令是“管道式”的固定关键字：`SEL <expr> ...`  
2) 不做 join / group by / 子查询；只在**当前 scope**（当前 record 或 ITEM 全局域）内做筛选、排序与投影  
3) 不建索引；实现是线性扫描（O(N)），因此默认 `LIMIT=100`，避免一次性打印过大结果导致交互退化

#### 8.4.1 基本语法

在 record 作用域（例如你已执行 `P/COMMANDS`）：

```
SEL <expr> [COLS f1,f2,...|*] [SORT k1[:asc|desc],k2[:asc|desc] ...] [LIMIT n|ALL] [OFFSET n] [GOTO first|k]
```

在 ITEM 全局域（你已执行 `P/item`）：

```
SEL <expr> ...
```

其中：

- `<expr>`：布尔表达式（WHERE），可用字段名、`_IDX`（行号，1-based）与常量构造
- `COLS`：列选择（projection）。建议显式列出关心字段；`*` 表示尽量输出全部字段（字段过多时不建议）
- `SORT`：多键排序（稳定排序）。`k:desc` 或 `-k` 表示降序；默认升序
- `LIMIT/OFFSET`：结果窗口（分页/截断）
- `GOTO`：把当前行指针定位到命中集合中的第 `k` 条；`first` 等价于 `1`

`FIND` 是语法糖：

```
FIND <expr> [COLS ...] [SORT ...]
```

默认行为等价于：`SEL <expr> LIMIT 1 GOTO first`。当命中时，会把 `slot` 定位到该行并输出该行（便于马上继续 `/FIELD` 查看其它字段）。

`NEXT/PREV` 用于在“上一次 SEL/FIND 的命中集合”里前后跳转：

- `NEXT` 或 `N`：跳到下一条命中  
- `PREV` 或 `B`：跳到上一条命中  

它们会更新 `slot` 并自动打印当前行（本质是把查询结果集当作“书签列表”来用）。

#### 8.4.2 表达式（expr）说明

支持的运算符：

- 比较：`= != < <= > >=`
- 字符串匹配：`~`（匹配/包含，支持 `*` 通配）；`!~`（不匹配）
- 逻辑：`AND` / `OR` / `NOT`（也支持 `&&` / `||` / `!`）
- 括号：`(...)`

常量：

- 数值：`12`、`-3`、`0.25`（按 double 语义比较）
- 字符串：建议使用双引号，例如 `"AA:BB:*"`、`"BUS_10"`  
  （对包含 `:`、`*`、空格等字符的字符串，必须加引号）

特别字段：

- `_IDX`：行号（从 1 开始）。用于“按范围快速定位”非常实用：`_IDX>=500 AND _IDX<=520`

行数范围（record 的扫描上限）：

- 优先使用 `LV$<REC>` 作为有效行数（LV）；若不存在则退化为 `MX`  
- 工程上仍建议维持 `LV<=MX` 的约束；当 `LV` 异常时，工具会在启动时告警（见前文容量策略）

#### 8.4.3 排序与类型规则（工程约定）

- 数值字段：按 double 比较（`int/float/double` 统一投影到 double）  
- 字符串字段（例如 `string64`）：按字典序比较  
- 多键排序：按 `SORT` 指定顺序逐键比较；当第一键相等时再比较第二键，以此类推  
- 稳定性：同键值的行保持原始相对顺序（便于做“筛选后不打乱业务自然顺序”的巡检）

#### 8.4.4 结果集与定位语义

- `SEL` 会打印表格结果，并把“命中行号列表”缓存为会话状态（与当前 scope 绑定）
- `FIND` 会缓存同样的结果集，但默认只取第一条并自动定位
- 当你切换 record（`P/xxx`）或切换到 ITEM，再使用 `NEXT/PREV`，工具会提示“当前 scope 无可用结果集”，避免跨域误跳转

---

### 8.5 典型用法示例（建议直接复制试用）

假设你在 `P/COMMANDS`（记录名仅示例；字段名请按你的 DAT 实际为准）：

1) 找出输出值异常偏大的行，按值降序显示前 20 条：

```
SEL VAL_COMMANDS > 0.8 SORT -VAL_COMMANDS LIMIT 20
```

2) 只看几个关键列，并按点号升序排列（适合做“点号区间巡检”）：

```
SEL ID_COMMANDS >= 100 COLS _IDX,SELFMAC,ID_COMMANDS,VAL_COMMANDS SORT ID_COMMANDS:ASC LIMIT 50
```

3) 用 `_IDX` 快速定位某一段行（对大表非常实用）：

```
SEL _IDX >= 500 AND _IDX <= 520
```

4) 查找字符串

```
SEL NAME_GENERATOR == '测试'
```

5) 组合条件 + 括号（逻辑优先级可读性更好）：

```
SEL (VAL_COMMANDS > 0.8 AND INFO_COMMANDS ~ "V/F") OR (ID_COMMANDS >= 9000)
```

6) “查到一条后连续巡检”：先 FIND 定位，然后 NEXT/PREV 在命中之间跳转：

```
FIND VAL_COMMANDS > 0.8 SORT -VAL_COMMANDS
NEXT
NEXT
PREV
```

命中后，你可以继续使用原生接口查看/修改：

- `/VAL_COMMANDS` 查看该字段值  
- `/VAL_COMMANDS(:)=0` 或 `/VAL_COMMANDS(3:9)=0` 批量修正（高风险操作，建议配合快照）  
- `show/COMMANDS` 进入表格视图，从当前 slot 附近开始上下翻页巡检  

---

### 8.6 性能与工程建议

1) 复杂筛选优先显式加 `LIMIT`。本查询层不建索引，扫描成本与 LV 线性相关；默认 `LIMIT=100` 是为了避免“工具可用性”被大表拖垮。  
2) 优先在表达式里用“先粗后细”的条件：例如先按 `_IDX` 或 `ID` 范围缩小，再按字符串匹配做精筛。  
3) `SORT` 会对命中集合做排序；若命中集合本身很大，排序开销会显著增加。工程上建议把 `SORT` 与 `LIMIT` 配合使用（典型是 Top-K）。  
4) 该层的定位是“运维巡检辅助”，不是业务逻辑依赖。任何“长期稳定的数据查询接口”仍应在业务侧以头文件布局 + 明确遍历逻辑实现，并配合 watchdog/audit 做安全约束。


---

## 9. 运行期映射接口与业务侧调用

### 9.1 C 接口（推荐作为“最小稳定 ABI”）

公共头文件：`include/zrtdb.h`

常用函数族：

```
int RegisterApp_(const char* app_name);                 // 设置 APP 上下文并加载 meta
int MapMemory_(const char* part_nm, char** part_addr); // mmap 分区（part_nm 支持 PART 或 PART/DB）
int free_MapMemory_();                                  // 释放 APP 上下文与已映射分区（本进程内）

int SnapshotReadLock_();                                // 写者进入一个“写入批次”
int SnapshotReadUnlock_();                              // 写者结束该写入批次
int SaveSnapshot_(char* out_path, int out_len);         // 保存快照；可返回快照目录路径
int LoadSnapshot_(const char* snapshot_name_or_path);   // 下装/恢复快照（低频运维）
```

如果使用 `zrtdb_model` 生成的每 APP 头文件，通常还会直接使用：

```
int zrtdb_app_<app>_init(zrtdb_app_<app>_ctx_t* ctx);
```

它是对 `RegisterApp_()` + 多次 `MapMemory_()` 的薄封装，便于业务进程一次完成上下文初始化。

推荐调用顺序：

1) 进程启动后调用一次 `RegisterApp_(APP)`  
2) 按需对每个分区调用 `MapMemory_(PART$, &addr)`（或 `MapMemory_("PART/DB", &addr)`）  
3) 进程退出或需要释放时调用 `free_MapMemory_()`

### 9.2 C++ 运行期管理（可选）

库内提供 `mmdb::MmdbManager`（位于 `zrtdb_lib/`），其语义等价于：

- `setContext(APP)`：加载 meta
- `mapPartition(PART[, readOnly])`：mmap 分区并返回地址

如果你的工程希望保持 ABI 稳定，建议优先使用 C 接口；C++ API 更适合库内部与工具侧开发。

### 9.3 一致性与快照锁（必须理解的工程约束）

快照与下装通过进程间 `pthread_rwlock` 实现“短暂停写入”的协作式一致性：

- **业务写者**在一个写入批次外层持 `SnapshotReadLock_()`（读锁）  
- **快照保存/下装**持写锁（写锁会阻塞新读锁进入，从而短暂停止写入）

重要建议：

- 不要在硬实时闭环里触发 `SaveSnapshot_()` / `LoadSnapshot_()` / `SNAP` / `LOADSNAP`  
- 快照属于低频运维动作，应在独立线程或独立进程中执行  
- 当前实现对“写者不配合锁”的情况不会强行阻止其写入；锁的语义是工程协作约束，而非内核强制隔离

---


## 10. Rust 接口（DAT 驱动自动生成）

### 10.1 设计目标

Rust 接口的目标不是“手写一套并行模型”，而是**复用现有 DAT 编译产物**，确保 Rust/C/C++ 对同一 `.sec` 布局有一致解释。

具体原则：

1) 以 `zrtdb_model` 为唯一模型编译器：DAT 改动后重新运行 `zrtdb_model`，同时生成 C 头与 Rust 绑定。  
2) Rust 绑定仅承载 ABI/布局，不引入额外运行期协议。  
3) Rust 与 C/C++ 通过同一 `RegisterApp_` / `MapMemory_` 访问同一共享内存文件。

### 10.2 生成位置与触发时机

- 生成目录：`/usr/local/ZRTDB/header/rust/`（可由静态根目录配置影响）。
- 生成文件：每个 APP 一个 `header/rust/<APP>.rs`。
- 触发时机：执行 `zrtdb_model` 编译 APP 配置（`APPDAT.json` 或 legacy `.APPDAT`）时自动生成。

### 10.3 生成内容说明

每个 `header/rust/<APP>.rs` 主要包含：

- `unsafe extern "C"` 声明：`RegisterApp_`、`MapMemory_`；
- APP/分区/容量常量（与 DAT 编译结果一致）；
- `ZRTDB_LAYOUT_FINGERPRINT` 常量（可用于运行时一致性核对）；
- `#[repr(C, packed)]` 的分区结构体定义（字段顺序与字节布局匹配 C 侧）；
- 每个分区结构体自动生成 `get_*/set_*` 安全访问器：
  - 标量字段：使用 `read_unaligned/write_unaligned`；
  - 数组字段：自动做索引范围检查（越界返回 `OutOfRange`）；
  - 定长字符串字段：按字节数组安全读写（不暴露未对齐引用）；
- `zrtdb_app_<app>_ctx_t` 上下文结构体（分区指针集合）；
- `ZrtdbRo/ZrtdbRw` typed view 包装与 `*_ro/*_rw` 访问函数（避免直接裸指针游走）；
- `zrtdb_app_<app>_init(...)`：对 `RegisterApp_ + MapMemory_` 的 Rust 侧便捷封装。

> 说明：仍保留 raw `#[repr(C, packed)]` 作为 FFI 真相；推荐业务层优先使用自动生成的访问器与 typed view，而不是直接对 packed 字段取引用。

### 10.4 推荐工作流（Rust）

```bash
# 1) 更新 DAT 后，重新生成模型与 Rust 绑定
zrtdb_model

# 2) 进入 Rust 示例工程（或你的业务工程）
cd example_rs

# 3) 编译并运行
cargo build
cargo run
```

`example_rs/build.rs` 会在编译时从 `${ZRTDB_STATIC_ROOT:-/usr/local/ZRTDB}/header/rust/<APP>.rs` 拷贝生成绑定文件到 `OUT_DIR`，并链接 `libzrtdb.a`。

### 10.5 与 C/C++ 的协同边界

- **模型源头统一**：DAT / APPDAT 是唯一契约源，不建议手改生成的 `.rs/.h` 文件。  
- **版本切换规则一致**：DAT 变更后旧进程可能因 fingerprint 不匹配而被拒绝映射；仍建议工程上执行“重建 + 重启/重映射”以保持行为一致。  
- **运维工具通用**：Rust 进程写入的数据可直接被 `zrtdb_tool`、快照与 watchdog 观察和审计。

---

## 11. 快照（Snapshot）补充说明

快照目录位于该 APP 根目录下，并包含：

- `meta/apps/`：两份 meta（clone + runtime）
- `zrtdb_data/`：按 `<DB>/<PART>.sec` 结构保存的分区文件
- `manifest.json`：快照清单与 note

实现层面，快照优先尝试：

- reflink（CoW 克隆，若文件系统支持）  
- 否则退化为复制；同时对稀疏文件做优化（按 data/hole 段拷贝）

`LOADSNAP`/`LoadSnapshot_` 会在原路径下 **就地覆盖**目标 `.sec` 文件（`O_TRUNC` 不改变 inode），其设计意图是：尽可能维持既有 mmap 的“文件身份”稳定；但仍建议对下装行为配合停业务或重启映射进程来消除解释不一致风险。

---

## 12. 常见问题与排障

1) `Meta file not found for APP=...`  
说明该 APP 尚未被实例化。检查：是否运行过 `zrtdb_model`，以及 `${MMDB_RUNTIME_ROOT}/<APP>/meta/apps/` 是否存在 `<APP>.sec` 与 `<APP>_NEW.sec`。

2) `status` 的 mapped-by 列表不完整  
`status` 通过扫描 `/proc/<pid>/maps` 识别映射者；受权限与系统安全策略影响（如 `ptrace_scope`），非特权用户可能读不到其他用户进程的 maps。

3) `.sec` 重建后的兼容性  
重建会改变布局与 meta 索引。工程上应把它视为“版本切换”：建议停业务/重启所有映射进程，避免旧进程按旧布局解释新数据。

---

## 13. 选型建议

如果客户要跨网络、要QoS、要发现与互操作：DDS/zenoh/eCAL属于更合适的“体系型”选择，但请接受复杂度与学习成本。

如果客户只要高性能IPC底座，并且愿意在上层自建数据字典与运维：iceoryx/iceoryx2是行业级底座。

如果客户有如下需求：
1. 可控性优先：代码总行数1万行以内、可审计、可裁剪、可快速定位问题。
2. 确定性优先：固定偏移、无动态分配、可预测的读写路径。
3. 可运维巡检：zrtdb_tool不是锦上添花，而是“可运维”的核心工具，同时有watchdog守护进程作为兜底。
4. 建模产物与运行期实例分离：这是工程化的分水岭。中间件生态往往忽略这一点，但实时仿真/控制现场极其在意版本与实例的可追溯。

那么ZRTDB这类“模型先行的共享内存数据平面”更贴合，并且更容易落地与长期维护。
