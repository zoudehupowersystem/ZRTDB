// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "MmdbManager.hpp"
#include "MetaIO.hpp"
#include "StringUtils.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

// 运行期全局元数据（单进程内有效）：
// - g_static_model：静态建模 meta（来自 <APP>.sec）
// - g_runtime_app：运行期实例 meta（来自 <APP>_NEW.sec，包含映射地址等运行期信息）
RuntimeAppConfig g_runtime_app;
StaticModelConfig g_static_model;
// GLOBAL_TABLE_STRUCT GLBTBL;

namespace mmdb {

MmdbManager::MmdbManager() { }

MmdbManager& MmdbManager::instance()
{
    static MmdbManager inst;
    return inst;
}

std::filesystem::path MmdbManager::getStaticRootPath() const
{
    // Default install-time static root (DAT/meta/header/bins). Can be overridden for legacy setups.
    // Preference order:
    //   1) MMDB_HOME (legacy)
    //   2) MMDB_STATIC_ROOT (optional)
    //   3) /usr/local/ZRTDB (default)
    if (const char* env = std::getenv("MMDB_HOME"); env && *env) {
        return std::filesystem::path(env);
    }
    if (const char* env = std::getenv("MMDB_STATIC_ROOT"); env && *env) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path("/usr/local/ZRTDB");
}

std::filesystem::path MmdbManager::getRuntimeRootPath() const
{
    // Default run-time root (zrtdb_data/logs/snapshots etc.). Prefer /var for FHS compliance.
    // Can be overridden if the system uses a different layout.
    if (const char* env = std::getenv("MMDB_RUNTIME_ROOT"); env && *env) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path("/var/ZRTDB");
}

std::filesystem::path MmdbManager::getAppRootPath() const
{
    // App-scoped runtime root.
    // If no app context is set yet, fall back to the runtime root.
    auto base = getRuntimeRootPath();
    if (currentAppUpper_.empty()) {
        return base;
    }
    return base / currentAppUpper_;
}

std::filesystem::path MmdbManager::getHomePath() const
{
    // 向后兼容：历史上 home==static root。
    return getStaticRootPath();
}

std::filesystem::path MmdbManager::getSecsPath() const { return getAppRootPath() / "zrtdb_data"; }

std::filesystem::path MmdbManager::getMetaPath() const { return getAppRootPath() / "meta" / "apps"; }

// 设置当前 APP 上下文，并加载其 meta。
// 典型调用：应用/工具启动后先 setContext(APP)，再按需 mapPartition。
bool MmdbManager::setContext(std::string_view app)
{
    std::string appUpper = mmdb::utils::toUpper(app);
    if (currentAppUpper_ == appUpper) {
        return true;
    }

    freeContext();
    currentAppUpper_ = appUpper;

    // std::memset(GLBTBL.currentapp, 0, sizeof(GLBTBL.currentapp));
    // std::strncpy(GLBTBL.currentapp, currentAppUpper_.c_str(), APP_MAX_LEN);

    return loadMetaData();
}

// 从 /var/ZRTDB/<APP>/meta/apps/ 读取两个 meta：
// - <APP>.sec     ：静态建模 meta（clone）
// - <APP>_NEW.sec ：运行期实例 meta（runtime）
// 注意：此处只加载“描述信息”，并不做 .sec 分区文件 mmap。
bool MmdbManager::loadMetaData()
{
    auto metaDir = getMetaPath();
    auto cloneFile = metaDir / (currentAppUpper_ + ".sec");
    auto appFile = metaDir / (currentAppUpper_ + "_NEW.sec");

    if (!std::filesystem::exists(cloneFile) || !std::filesystem::exists(appFile)) {
        std::cerr << "Meta file not found for APP=" << currentAppUpper_ << "\n"
                  << "  " << cloneFile << "\n"
                  << "  " << appFile << std::endl;
        return false;
    }

    if (!mmdb::meta::loadClone(cloneFile, g_static_model)) {
        std::cerr << "Failed to read clone meta: " << cloneFile << std::endl;
        return false;
    }
    if (!mmdb::meta::loadRuntime(appFile, g_runtime_app)) {
        std::cerr << "Failed to read runtime meta: " << appFile << std::endl;
        return false;
    }

    if (!g_static_model.layout_fingerprint.empty() && !g_runtime_app.layout_fingerprint.empty()
        && g_static_model.layout_fingerprint != g_runtime_app.layout_fingerprint) {
        std::cerr << "Layout fingerprint mismatch between static/runtime meta: "
                  << g_static_model.layout_fingerprint << " vs " << g_runtime_app.layout_fingerprint << std::endl;
        return false;
    }

    // 清空运行期映射地址（随后由 mapPartition() 在当前进程内填充）。
    std::fill(g_runtime_app.partition_base_addrs.begin(), g_runtime_app.partition_base_addrs.end(), 0);
    std::fill(g_runtime_app.record_lv_addrs.begin(), g_runtime_app.record_lv_addrs.end(), 0);

    return true;
}

std::filesystem::path MmdbManager::getSecFilePath(const std::string& dbUpper, const std::string& partUpper) const
{
    // 分区文件路径：<runtime_root>/<APP>/zrtdb_data/<DB>/<PART>.sec
    return getSecsPath() / dbUpper / (partUpper + ".sec");
}

int MmdbManager::mapPartition(std::string_view partName, char** partAddr, bool readOnly)
{
    if (currentAppUpper_.empty()) {
        std::cerr << "Context not set!" << std::endl;
        return -1;
    }
    if (!partAddr) {
        return -1;
    }

    std::string raw = mmdb::utils::trim(std::string(partName));
    if (raw.empty()) {
        return -1;
    }

    // 兼容两种输入：
    // - "PART/DB"：显式指定 DB 与分区名
    // - "PART"   ：若未指定 DB，则默认 DB==PART（兼容旧工程命名习惯）
    std::string partUpper;
    std::string dbUpper;

    auto slash = raw.find('/');
    if (slash != std::string::npos) {
        partUpper = mmdb::utils::toUpper(mmdb::utils::trim(raw.substr(0, slash)));
        dbUpper = mmdb::utils::toUpper(mmdb::utils::trim(raw.substr(slash + 1)));
    } else {
        partUpper = mmdb::utils::toUpper(raw);
        dbUpper = partUpper;
    }

    std::string key = dbUpper + ":" + partUpper;
    auto it = sections_.find(key);
    if (it != sections_.end()) {
        *partAddr = static_cast<char*>(it->second->data());
        return 1;
    }

    // 在运行期 meta 中定位分区索引，并得到建模期定义的最小期望尺寸（字节）。
    long expectedMin = 0;
    int prtIdx = -1;
    for (int i = 0; i < g_runtime_app.partition_count; ++i) {
        std::string pid = mmdb::utils::toUpper(mmdb::utils::trim(g_runtime_app.partition_ids[(size_t)i]));
        if (pid == partUpper) {
            prtIdx = i;
            expectedMin = g_runtime_app.partition_bytes[(size_t)i];
            break;
        }
    }

    auto filePath = getSecFilePath(dbUpper, partUpper);
    if (!std::filesystem::exists(filePath)) {
        std::cerr << "Sec file not found: " << filePath << std::endl;
        return -1;
    }

    try {

    {
        auto man = filePath;
        man += ".manifest";
        std::ifstream mif(man);
        if (mif && !g_runtime_app.layout_fingerprint.empty()) {
            std::string body((std::istreambuf_iterator<char>(mif)), std::istreambuf_iterator<char>());
            if (body.find(g_runtime_app.layout_fingerprint) == std::string::npos) {
                std::cerr << "Layout fingerprint mismatch for sec manifest: " << man << std::endl;
                return -1;
            }
        }
    }

        auto mapped = std::make_unique<MappedFile>(filePath, readOnly);        if (expectedMin > 0 && static_cast<long>(mapped->size()) < expectedMin) {
            throw std::runtime_error("sec size too small: " + filePath.string());
        }
        *partAddr = static_cast<char*>(mapped->data());
        sections_[key] = std::move(mapped);

        if (prtIdx >= 0) {
            g_runtime_app.partition_base_addrs[(size_t)prtIdx] = reinterpret_cast<long>(*partAddr);

            // 补齐 record_lv_addrs：若某条记录的 LV$ 落在当前分区，则其地址 = 分区基址 + 偏移。
            const long base = g_runtime_app.partition_base_addrs[(size_t)prtIdx];
            const short prtNo = static_cast<short>(prtIdx + 1);
            for (int r = 0; r < g_runtime_app.record_count; ++r) {
                if (g_runtime_app.record_lv_partition_1based[(size_t)r] == prtNo) {
                    g_runtime_app.record_lv_addrs[(size_t)r] = base + g_runtime_app.record_lv_offset_bytes[(size_t)r];
                }
            }
        }

        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Map failed: " << e.what() << std::endl;
        return -1;
    }
}

void MmdbManager::freeContext()
{
    sections_.clear();
    currentAppUpper_.clear();
}

} // namespace mmdb
