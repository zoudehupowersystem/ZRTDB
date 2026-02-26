// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "MappedFile.hpp"
#include "zrtdb_const.h"
#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace mmdb {

class MmdbManager {
public:
    static MmdbManager& instance();

    bool setContext(std::string_view app);
    void freeContext();

    // Map partition. Accepts "PART/DB" or "PART".
    int mapPartition(std::string_view partName, char** partAddr, bool readOnly);

    std::filesystem::path getHomePath() const; // static root (backward-compatible)
    std::filesystem::path getStaticRootPath() const;
    std::filesystem::path getRuntimeRootPath() const;
    // App-scoped runtime root derived from getRuntimeRootPath() and the current app context.
    // Example: /var/ZRTDB/EMTP
    std::filesystem::path getAppRootPath() const;

    // App-scoped runtime zrtdb_data root: <appRoot>/zrtdb_data
    std::filesystem::path getSecsPath() const;

    // App-scoped meta/apps root: <appRoot>/meta/apps
    std::filesystem::path getMetaPath() const;

    // 当前 APP（大写）。用于工具/守护进程等做定位与日志。
    // 说明：返回内部引用，调用方不得长期持有跨越 setContext/freeContext 生命周期。
    const std::string& getCurrentAppUpper() const { return currentAppUpper_; }

private:
    MmdbManager();
    bool loadMetaData();
    std::filesystem::path getSecFilePath(const std::string& dbUpper, const std::string& partUpper) const;

    std::string currentAppUpper_;

    std::map<std::string, std::unique_ptr<MappedFile>> sections_;
};

} // namespace mmdb
