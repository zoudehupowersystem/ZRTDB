// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "zrtdb_const.h"
#include <filesystem>

namespace mmdb::meta {

// Binary meta format v2 (NOT compatible with legacy POD dump).
// Files are still named as before:
//   <runtimeRoot>/<APP>/meta/apps/<APP>.sec       : StaticModelConfig
//   <runtimeRoot>/<APP>/meta/apps/<APP>_NEW.sec   : RuntimeAppConfig

bool saveClone(const std::filesystem::path& file, const StaticModelConfig& clone);
bool loadClone(const std::filesystem::path& file, StaticModelConfig& clone);

bool saveRuntime(const std::filesystem::path& file, const RuntimeAppConfig& app);
bool loadRuntime(const std::filesystem::path& file, RuntimeAppConfig& app);

} // namespace mmdb::meta
