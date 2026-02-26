// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mmdb {
namespace watchdog {

    // 是否禁用 watchdog 交互：
    // - ZRTDB_WATCHDOG_DISABLE=1 -> 全部禁用（不拉起、不卡住、不写日志）
    bool isDisabled();

    // 确保 zrtdb_watchdog 已运行：
    // - 如果 socket 不存在/不可达，则尝试 fork+exec 拉起（需要 zrtdb_watchdog 在 PATH 或设置 ZRTDB_WATCHDOG_BIN）
    void ensureRunning();

    // 发送一条 JSON 事件（单行文本）。
    // - 失败将静默忽略（不影响实时应用主流程）。
    void sendEvent(const std::string& jsonLine);

    // 发送进程 attach/detach（用于 watchdog 追踪“映射该应用的进程集合”）。
    void sendAttach(const char* component);
    void sendDetach(const char* component);

    // API/TOOL 审计日志（不强制 JSON 完整性，但建议传入 {...} 结构）。
    void sendAudit(const char* component,
        const char* op,
        const std::string& argsJson,
        int rc,
        std::int64_t durUs);

} // namespace watchdog
} // namespace mmdb
