// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "zrtdb_const.h"
#include "zrtdb_model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zrtdb::init {

class Instantiator {
public:
    Instantiator();

    std::filesystem::path home() const; // static root (legacy name)
    std::filesystem::path runtimeBaseRoot() const;
    std::filesystem::path runtimeRoot() const;
    std::filesystem::path defPath() const; // header/.datdef
    std::filesystem::path metaAppPath() const; // meta/apps
    std::filesystem::path secsPath() const; // runtime zrtdb_data root  // zrtdb_data

    bool instantiate(const std::string& appUpper);

private:
    std::string appUpper_;
    bool loadAppDef(const std::string& appUpper, app_strc_dat& out);
    bool loadDbDef(const std::string& dbUpper, db_strc_dat& out);

    bool mergeDbDefs(const app_strc_dat& appDef, StaticModelConfig& outClone,
        std::vector<std::pair<std::string, db_strc_dat>>& loaded);

    bool ensurePhysicalFiles(const std::vector<std::pair<std::string, db_strc_dat>>& loaded);

    void buildRuntime(const app_strc_dat& appDef, const StaticModelConfig& clone, RuntimeAppConfig& out);

    bool saveMeta(const std::string& appUpper, const StaticModelConfig& clone, const RuntimeAppConfig& runtime);
};

} // namespace zrtdb::init
