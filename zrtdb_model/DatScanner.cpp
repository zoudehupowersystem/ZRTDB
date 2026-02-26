// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "DatScanner.h"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace zrtdb::init {

static std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

ScanResult scan_dat_dir(const std::filesystem::path& dat_dir)
{
    ScanResult r;
    if (!std::filesystem::exists(dat_dir)) {
        std::cerr << "DAT dir not found: " << dat_dir << std::endl;
        return r;
    }

    for (const auto& e : std::filesystem::directory_iterator(dat_dir)) {
        if (!e.is_regular_file())
            continue;

        const auto filenameUpper = upper(e.path().filename().string());
        const auto extUpper = upper(e.path().extension().string());

        if (extUpper == ".DAT") {
            r.db_dats.push_back(e.path());
            continue;
        }

        // Single JSON file declaring all apps: APPDAT.json
        if (filenameUpper == "APPDAT.JSON" && extUpper == ".JSON") {
            r.app_dats.push_back(e.path());
            continue;
        }

        // Legacy compatibility (kept for old deployments)
        if (extUpper == ".APPDAT") {
            r.app_dats.push_back(e.path());
            continue;
        }
    }

    std::sort(r.db_dats.begin(), r.db_dats.end());
    std::sort(r.app_dats.begin(), r.app_dats.end());
    return r;
}

} // namespace zrtdb::init
