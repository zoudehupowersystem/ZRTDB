// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

// 应用程序的实现

#include "MmdbManager.hpp"
#include "WatchdogClient.hpp"
#include "zrtdb.h"

#include <chrono>

extern "C" {

int RegisterApp_(const char* app_name)
{
    auto t0 = std::chrono::steady_clock::now();
    if (!app_name)
        return -1;
    int rc = mmdb::MmdbManager::instance().setContext(app_name) ? 1 : -1;
    if (rc > 0) {
        mmdb::watchdog::sendAttach("lib");
    }
    auto t1 = std::chrono::steady_clock::now();
    const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    mmdb::watchdog::sendAudit("lib", "RegisterApp_", std::string("{\"app\":\"") + app_name + "\"}", rc, durUs);
    return rc;
}

int MapMemory_(const char* part_nm, char** part_addr)
{
    auto t0 = std::chrono::steady_clock::now();
    if (!part_nm || !part_addr)
        return -1;
    int rc = mmdb::MmdbManager::instance().mapPartition(part_nm, part_addr, false);
    auto t1 = std::chrono::steady_clock::now();
    const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    mmdb::watchdog::sendAudit("lib", "MapMemory_", std::string("{\"part\":\"") + part_nm + "\"}", rc, durUs);
    return rc;
}

int free_MapMemory_()
{
    auto t0 = std::chrono::steady_clock::now();
    // 先 detach（避免 freeContext 之后无法定位 socket 路径）
    mmdb::watchdog::sendDetach("lib");
    mmdb::MmdbManager::instance().freeContext();
    auto t1 = std::chrono::steady_clock::now();
    const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    mmdb::watchdog::sendAudit("lib", "free_MapMemory_", "{}", 1, durUs);
    return 1;
}

} // extern "C"
