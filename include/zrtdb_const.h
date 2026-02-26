// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#ifndef _ZRTDB_CONST_H_
#define _ZRTDB_CONST_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*
ZRTDB 的核心思想：把“数据结构”在建模阶段固化为一组确定的偏移/索引表，
运行期由多个进程通过 mmap 映射同一份 .sec 分区文件，从而实现零拷贝数据交换。

本头文件包含两类“元数据（meta）结构”：

1) StaticModelConfig
   - 静态建模产物（compile-time model），通常由 zrtdb_model 在实例化阶段写入：
     /var/ZRTDB/<APP>/meta/<APP>.sec
   - 描述：DB/分区/记录/字段的全局编号体系与固定布局信息（不含运行期地址）。

2) RuntimeAppConfig
   - 运行期实例元数据（runtime instance），通常由 zrtdb_model 实例化并在运行期加载：
     /var/ZRTDB/<APP>/meta/<APP>_NEW.sec
   - 描述：在 StaticModelConfig 的基础上补齐“当前进程的映射地址”等运行期信息。
*/

// 最大数量限制（历史遗留：用于建模阶段约束/预分配上限；运行期不再按此分配固定大数组）
constexpr int MXDBNUM = 24;
constexpr int MXPRTNUM = 128;
constexpr int MXRECNUM = 1024;
constexpr int MXFLDNUM = 10240;

constexpr int MXRELATDNUM = 2048;
constexpr int MXCHKNUM = 2048;
constexpr int MXOPNUM = 2048;
/* -----------------------------
 * RuntimeAppConfig（运行期 meta）
 * ----------------------------- */

struct RuntimeAppConfig {
    /*
    全局计数（均为 0-based 计数值）：
    - db_count / partition_count / record_count / field_count / table_count
      分别表示 DB、分区、记录、字段、记录-字段映射表的条目数。
    */
    int db_count = 0;
    int partition_count = 0;
    int record_count = 0;
    int field_count = 0;
    int table_count = 0;

    /*
    partition_base_addrs[i]：
    - 第 i 个分区文件映射到“当前进程”后的基址（进程虚拟地址）。
    - 未映射则为 0。
    - 该数组由 MmdbManager::mapPartition() 在运行期填充。
    */
    std::vector<long> partition_base_addrs;

    /*
    DB 维度索引（prefix array 形式）：
    - db_ids.size() == db_count
    - db_partition_prefix.size() == db_count + 1
    - db_record_prefix.size() == db_count + 1

    prefix array 约定：
    - 对任意 i ∈ [0, db_count)，对应范围为 [prefix[i], prefix[i+1])（半开区间，0-based）。
    - 例如：DB i 的分区全局索引范围为
      [db_partition_prefix[i], db_partition_prefix[i+1])。
    */
    std::vector<std::string> db_ids;
    std::vector<short> db_partition_prefix;
    std::vector<short> db_record_prefix;

    /*
    分区表（全局）：
    - partition_ids.size() == partition_count
    - partition_bytes.size() == partition_count
    - partition_db_1based.size() == partition_count
    - partition_field_prefix.size() == partition_count + 1（prefix array）

    注意：
    - partition_db_1based 为 1-based（历史兼容），取值范围：
      0 表示未绑定；1..db_count 表示属于第 (db-1) 个 DB。
    */
    std::vector<std::string> partition_ids;
    std::vector<int> partition_bytes;
    std::vector<short> partition_db_1based;
    std::vector<short> partition_field_prefix;

    /*
    记录表（全局）：
    - record_ids.size() == record_count
    - record_max_dim.size() == record_count
    - record_physical_dim.size() == record_count（可选：物理分配容量，默认=record_max_dim）
    - record_table_prefix.size() == record_count + 1（prefix array）

    record_lv_* 用于 LV$（记录级“当前序号/游标”）的定位：
    - record_lv_partition_1based[r]：LV$ 字段所在分区（1-based，0 表示无）。
    - record_lv_offset_bytes[r]：LV$ 字段在分区内的字节偏移。
    - record_lv_addrs[r]：LV$ 字段在当前进程的“可直接解引用地址”（= base + offset）。
      只有当对应分区已成功 mmap 时，该地址才有效；否则为 0。
    */
    std::vector<std::string> record_ids;
    std::vector<int> record_max_dim;
    // 物理容量（用于越界缓冲/“保险丝”）：
    // - record_max_dim：逻辑上限（DAT DIM，业务语义）
    // - record_physical_dim：物理分配容量（默认=3*record_max_dim；也可配置为 1 表示关闭）
    // 说明：字段偏移/分区大小按 record_physical_dim 计算，从而在 record_max_dim 之后留出缓冲区。
    std::vector<int> record_physical_dim;
    std::vector<long> record_lv_addrs;
    std::vector<short> record_lv_partition_1based;
    std::vector<long> record_lv_offset_bytes;
    std::vector<short> record_table_prefix;
    std::vector<short> record_size_bytes;

    /*
    字段表（全局）：
    - field_ids.size() == field_count
    - field_partition_1based / field_record_1based 采用 1-based：
      0 表示“全局字段（非记录字段）”
      >0 表示绑定到对应记录编号（1-based）。
    - field_offset_bytes：字段在分区内的起始偏移（字节）
    - field_type：I/R/D/S/K/C 等类型码
    - field_item_bytes：单元素字节数（注意：记录字段物理占用 = item_bytes * record_physical_dim[record-1]）
    */
    std::vector<std::string> field_ids;
    std::vector<short> field_partition_1based;
    std::vector<short> field_record_1based;
    std::vector<long> field_offset_bytes;
    std::vector<char> field_type;
    std::vector<std::uint8_t> field_item_bytes;

    /*
    记录-字段映射表（table）：
    - table_field_1based.size() == table_count
    - 采用 1-based：table_field_1based[t] ∈ [1, field_count]
    - 对某条记录 r（0-based），其字段列表范围为：
      [record_table_prefix[r], record_table_prefix[r+1])
      并对该范围内每个 t 取 fieldIdx = table_field_1based[t] - 1。
    */
    std::vector<short> table_field_1based;

    void clear()
    {
        db_count = partition_count = record_count = field_count = table_count = 0;
        partition_base_addrs.clear();
        db_ids.clear();
        db_partition_prefix.clear();
        db_record_prefix.clear();
        partition_ids.clear();
        partition_bytes.clear();
        partition_db_1based.clear();
        partition_field_prefix.clear();
        record_ids.clear();
        record_max_dim.clear();
        record_physical_dim.clear();
        record_lv_addrs.clear();
        record_lv_partition_1based.clear();
        record_lv_offset_bytes.clear();
        record_table_prefix.clear();
        record_size_bytes.clear();
        field_ids.clear();
        field_partition_1based.clear();
        field_record_1based.clear();
        field_offset_bytes.clear();
        field_type.clear();
        field_item_bytes.clear();
        table_field_1based.clear();
    }

    // 按给定计数一次性分配/初始化所有数组（便于 loadRuntime 后直接填充）
    void resizeByCounts(int ndb, int nprt, int nrec, int nfld, int ntbl)
    {
        db_count = ndb;
        partition_count = nprt;
        record_count = nrec;
        field_count = nfld;
        table_count = ntbl;

        partition_base_addrs.assign((size_t)nprt, 0);
        db_ids.assign((size_t)ndb, std::string {});
        db_partition_prefix.assign((size_t)ndb + 1, 0);
        db_record_prefix.assign((size_t)ndb + 1, 0);

        partition_ids.assign((size_t)nprt, std::string {});
        partition_bytes.assign((size_t)nprt, 0);
        partition_db_1based.assign((size_t)nprt, 0);
        partition_field_prefix.assign((size_t)nprt + 1, 0);

        record_ids.assign((size_t)nrec, std::string {});
        record_max_dim.assign((size_t)nrec, 0);
        record_physical_dim.assign((size_t)nrec, 0);
        record_lv_addrs.assign((size_t)nrec, 0);
        record_lv_partition_1based.assign((size_t)nrec, 0);
        record_lv_offset_bytes.assign((size_t)nrec, 0);
        record_table_prefix.assign((size_t)nrec + 1, 0);
        record_size_bytes.assign((size_t)nrec, 0);

        field_ids.assign((size_t)nfld, std::string {});
        field_partition_1based.assign((size_t)nfld, 0);
        field_record_1based.assign((size_t)nfld, 0);
        field_offset_bytes.assign((size_t)nfld, 0);
        field_type.assign((size_t)nfld, 0);
        field_item_bytes.assign((size_t)nfld, 0);

        table_field_1based.assign((size_t)ntbl, 0);
    }
};

// 运行期全局 meta（由 MmdbManager::setContext()/loadMeta() 装载与维护）
extern RuntimeAppConfig g_runtime_app;

/* -----------------------------
 * StaticModelConfig（静态建模 meta）
 * ----------------------------- */

struct StaticModelConfig {
    /*
    app_id：应用名（与 /var/ZRTDB/<APP>/ 目录名一致）
    db_count：DB 数量
    */
    std::string app_id;
    int db_count = 0;

    /*
    DB 前缀索引（prefix array 形式，与 RuntimeAppConfig 的语义一致，但元素类型为 int）：
    - db_partition_prefix.size() == db_count + 1
    - db_record_prefix.size() == db_count + 1
    */
    std::vector<std::string> db_ids;
    std::vector<int> db_partition_prefix;
    std::vector<int> db_record_prefix;

    // 记录表（静态）：
    // - record_max_dim：逻辑上限（DAT DIM）
    // - record_physical_dim：物理分配容量（用于越界缓冲/保险丝；默认=3*record_max_dim）
    std::vector<std::string> record_ids;
    std::vector<int> record_max_dim;
    std::vector<int> record_physical_dim;
    std::vector<int> record_first_addr;

    // 分区表（静态）：分区名、字段前缀索引、分区静态尺寸（字节）
    std::vector<std::string> partition_ids;
    std::vector<int> partition_field_prefix;
    std::vector<int> partition_size_bytes;

    // 字段表（静态）：字段名、类型、绑定记录、字节数、有效性与掩码索引等
    std::vector<std::string> field_ids;
    std::vector<char> field_type;
    std::vector<int> field_record_1based;
    std::vector<int> field_bytes;
    std::vector<char> field_valid;
    std::vector<int> field_mask_index;

    void clear()
    {
        app_id.clear();
        db_count = 0;
        db_ids.clear();
        db_partition_prefix.clear();
        db_record_prefix.clear();
        record_ids.clear();
        record_max_dim.clear();
        record_physical_dim.clear();
        record_first_addr.clear();
        partition_ids.clear();
        partition_field_prefix.clear();
        partition_size_bytes.clear();
        field_ids.clear();
        field_type.clear();
        field_record_1based.clear();
        field_bytes.clear();
        field_valid.clear();
        field_mask_index.clear();
    }
};

// 静态建模 meta（由 MmdbManager::loadMeta() 读取 clone 文件填充）
extern StaticModelConfig g_static_model;

#endif
