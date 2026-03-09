// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "zrtdb_const.h"

#include <string>
#include <vector>

// 这些结构仅用于 zrtdb_model（编译/实例化阶段），用于承载 DAT 解析结果与中间产物。
// 说明：
// 1) 旧版大量固定长度 C 数组已移除，避免栈帧过大与上限脆弱；
// 2) *.APPDEF / *.DBDEF 的持久化由 zrtdb::def(v2) 负责；
// 3) 这些结构不是运行期共享内存布局本身，运行期以 <APP>.sec / <APP>_NEW.sec + zrtdb_data/*.sec 为准。

struct app_strc_dat {
    // APP 名（通常等于 /var/ZRTDB/<APP>/ 目录名，建模时统一转大写）
    std::string app_id;
    int db_count = 0; // DB 数量
    std::vector<std::string> db_ids; // DB 名列表（与 DBDEF 文件一一对应）
    std::string layout_fingerprint; // APP 级布局指纹（聚合 DB 指纹 + 生成器版本）

    void clear()
    {
        app_id.clear();
        db_count = 0;
        db_ids.clear();
        layout_fingerprint.clear();
    }
};

constexpr int ID_MAX_LEN = 24;

struct db_strc_dat {
    // 单个 DB 的 DAT 解析结果（生成 DBDEF、分区布局、字段表等的基础输入）
    std::string db_id; // DB 名（通常与 DBDEF 文件名一致）
    int record_count = 0; // 记录数
    int partition_count = 0; // 分区数
    std::string layout_fingerprint; // DB 级布局指纹

    std::vector<std::string> record_ids;
    std::vector<int> record_first_addr;
    std::vector<int> record_max_dim;
    // 物理分配容量（用于越界缓冲/保险丝；默认=3*record_max_dim）
    std::vector<int> record_physical_dim;

    std::vector<std::string> partition_ids;
    std::vector<int> partition_field_prefix; // prefix, size = partition_count + 1
    std::vector<int> partition_size_bytes;

    std::vector<std::string> field_ids;
    std::vector<char> field_type; // I,R,D,S,K,C
    std::vector<int> field_record_1based; // 1-based record index; 0 => global
    std::vector<int> field_bytes;
    std::vector<char> field_valid;
    std::vector<int> field_mask_index;

    void clear()
    {
        db_id.clear();
        record_count = 0;
        partition_count = 0;
        layout_fingerprint.clear();

        record_ids.clear();
        record_first_addr.clear();
        record_max_dim.clear();
        record_physical_dim.clear();

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
