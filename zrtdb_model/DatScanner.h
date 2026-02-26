// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <filesystem>
#include <vector>

namespace zrtdb::init {

struct ScanResult {
    std::vector<std::filesystem::path> db_dats;
    std::vector<std::filesystem::path> app_dats;
};

ScanResult scan_dat_dir(const std::filesystem::path& dat_dir);

} // namespace zrtdb::init
