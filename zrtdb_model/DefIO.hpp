// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "zrtdb_model.h"
#include <filesystem>

namespace zrtdb::def {

// Binary definition format v2.
//   <staticRoot>/header/.datdef/<APP>.APPDEF
//   <staticRoot>/header/.datdef/<DB>.DBDEF
// Not compatible with legacy POD-dump format.

bool saveAppDef(const std::filesystem::path& file, const app_strc_dat& app);
bool loadAppDef(const std::filesystem::path& file, app_strc_dat& app);

bool saveDbDef(const std::filesystem::path& file, const db_strc_dat& db);
bool loadDbDef(const std::filesystem::path& file, db_strc_dat& db);

} // namespace zrtdb::def
