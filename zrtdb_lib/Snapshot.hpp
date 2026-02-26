// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace mmdb::snapshot {

struct FileItem {
    std::filesystem::path rel;
    std::uintmax_t size = 0;
    std::string method; // reflink/sparse/copy
};

struct SnapshotInfo {
    std::string appUpper;
    std::string createdLocal; // YYYY-MM-DD HH:MM:SS.mmm
    std::uint64_t epochMs = 0;
    std::filesystem::path dir;
    std::vector<FileItem> files;
};

// 保存当前 APP 的快照（APP context 必须已 setContext/RegisterApp_）。
// note 会写入 manifest（可空）。
SnapshotInfo save(std::string note = "");

// 下装：将 snapshotDir 下的 meta/apps 与 zrtdb_data 覆盖回运行态 APP 目录。
// snapshotNameOrPath：支持相对名（位于 /var/ZRTDB/<APP>/ 下）或绝对路径。
void load(const std::filesystem::path& snapshotNameOrPath);

// 列出当前 APP 目录下符合 <APP>_YYYYMMDD-... 的快照目录（按时间倒序）。
std::vector<std::filesystem::path> list();

} // namespace mmdb::snapshot
