// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "zrtdb_model.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace zrtdb::init {

class DbCompiler {
public:
    DbCompiler();

    std::filesystem::path home() const;
    std::filesystem::path datPath() const;
    std::filesystem::path defPath() const;
    std::filesystem::path incPath() const;

    // Compile a single DB .DAT to DBDEF (and cache in-memory for later app header generation)
    bool compileDbFile(const std::filesystem::path& dbDatFile);

    // Compile application definitions.
    // Preferred format: DAT/APPDAT.json (single file defines multiple apps).
    // On success, outAppNamesUpper contains all compiled app names (upper-cased).
    bool compileAppConfig(const std::filesystem::path& appConfigFile, std::vector<std::string>& outAppNamesUpper);

    // Legacy: compile a single APP .APPDAT (line-based) to APPDEF.
    bool compileAppFile(const std::filesystem::path& appDatFile, std::string& outAppNameUpper);

private:
    bool parseDbDatNew(const std::filesystem::path& datFile, db_strc_dat& out);
    bool parseAppDatNew(const std::filesystem::path& datFile, app_strc_dat& out);

    // JSON APPDAT (APPDAT.json)
    bool parseAppDatJson(const std::filesystem::path& jsonFile, std::vector<app_strc_dat>& outApps);

    // Generate a single merged header for an APP: header/inc/<APP>.h
    void generateAppHeader(const std::string& appUpper, const app_strc_dat& app);

    bool getDbDefCachedOrLoad(const std::string& dbUpper, db_strc_dat& out);

    // helpers
    static std::string trim(const std::string& s);
    static std::string upper(std::string s);
    static std::string extract_after(const std::string& line, const std::string& tag);
    static bool starts_with(const std::string& s, const char* pfx);

private:
    std::unordered_map<std::string, db_strc_dat> db_cache_; // DB name upper -> parsed DB
};

} // namespace zrtdb::init
