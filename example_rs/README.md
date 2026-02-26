# example_rs

Rust 示例：访问与 C/C++ 示例同一套 ZRTDB 共享内存数据结构。

## 前置步骤

1. 先编译安装并执行 `zrtdb_model`，使其根据 `DAT/` 生成 Rust 接口文件：

```bash
zrtdb_model
# 会生成 /usr/local/ZRTDB/header/rust/<APP>.rs
```

2. 默认示例使用 `CONTROLER` APP，可通过环境变量切换：

```bash
export ZRTDB_APP=SIMULATION
```

## 运行

```bash
cd example_rs
cargo run
```

如需自定义静态根目录（默认 `/usr/local/ZRTDB`）：

```bash
export ZRTDB_STATIC_ROOT=/your/static/root
cargo run
```


## 自测（建议）

```bash
# 1) 先确保模型已生成 Rust 绑定
zrtdb_model

# 2) 构建并运行 Rust 示例
cd example_rs
cargo build
cargo run
```
