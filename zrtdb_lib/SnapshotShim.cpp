// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "Snapshot.hpp"
#include "SnapshotLock.hpp"
#include "WatchdogClient.hpp"

#include <chrono>
#include <cstring>
#include <iostream>

extern "C" {

int SnapshotReadLock_()
{
    auto t0 = std::chrono::steady_clock::now();
    if (!mmdb::SnapshotLock::instance().ensureOpenForCurrentApp()) {
        mmdb::watchdog::sendAudit("lib", "SnapshotReadLock_", "{}", -1, 0);
        return -1;
    }
    mmdb::SnapshotLock::instance().rdlock();
    auto t1 = std::chrono::steady_clock::now();
    const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    mmdb::watchdog::sendAudit("lib", "SnapshotReadLock_", "{}", 1, durUs);
    return 1;
}

int SnapshotReadUnlock_()
{
    auto t0 = std::chrono::steady_clock::now();
    mmdb::SnapshotLock::instance().unlock();
    auto t1 = std::chrono::steady_clock::now();
    const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    mmdb::watchdog::sendAudit("lib", "SnapshotReadUnlock_", "{}", 1, durUs);
    return 1;
}

int SaveSnapshot_(char* out_path, int out_len)
{
    auto t0 = std::chrono::steady_clock::now();
    try {
        auto info = mmdb::snapshot::save("");
        auto s = info.dir.string();
        if (out_path && out_len > 0) {
            std::strncpy(out_path, s.c_str(), (size_t)out_len - 1);
            out_path[out_len - 1] = '\0';
        }
        auto t1 = std::chrono::steady_clock::now();
        const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        mmdb::watchdog::sendAudit("lib", "SaveSnapshot_", std::string("{\"out_len\":") + std::to_string(out_len) + "}", 1, durUs);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[SaveSnapshot_] " << e.what() << "\n";
        auto t1 = std::chrono::steady_clock::now();
        const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        mmdb::watchdog::sendAudit("lib", "SaveSnapshot_", std::string("{\"out_len\":") + std::to_string(out_len) + "}", -1, durUs);
        return -1;
    }
}

int LoadSnapshot_(const char* snapshot_name_or_path)
{
    auto t0 = std::chrono::steady_clock::now();
    if (!snapshot_name_or_path || !*snapshot_name_or_path) {
        mmdb::watchdog::sendAudit("lib", "LoadSnapshot_", "{}", -1, 0);
        return -1;
    }

    try {
        mmdb::snapshot::load(snapshot_name_or_path);
        auto t1 = std::chrono::steady_clock::now();
        const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        mmdb::watchdog::sendAudit("lib", "LoadSnapshot_", std::string("{\"name\":\"") + snapshot_name_or_path + "\"}", 1, durUs);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[LoadSnapshot_] " << e.what() << "\n";
        auto t1 = std::chrono::steady_clock::now();
        const std::int64_t durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        mmdb::watchdog::sendAudit("lib", "LoadSnapshot_", std::string("{\"name\":\"") + snapshot_name_or_path + "\"}", -1, durUs);
        return -1;
    }
}

} // extern "C"
