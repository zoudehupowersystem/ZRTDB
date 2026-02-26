// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "WatchdogClient.hpp"

#include "MmdbManager.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace mmdb::watchdog {

static std::string getSelfExePath()
{
    char buf[4096] = { 0 };
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0)
        return std::string(buf, (size_t)n);
    return {};
}

static std::filesystem::path getSocketPath()
{
    // 依赖 MmdbManager 上下文，因此必须在 setContext(...) 成功后调用。
    auto& mgr = MmdbManager::instance();
    if (mgr.getCurrentAppUpper().empty())
        return {};
    return mgr.getAppRootPath() / "meta" / "zrtdb_watchdog.sock";
}

bool isDisabled()
{
    const char* v = std::getenv("ZRTDB_WATCHDOG_DISABLE");
    return v && *v && std::atoi(v) != 0;
}

static void trySpawnWatchdog(const std::string& appUpper)
{
    // 1) env override
    const char* envBin = std::getenv("ZRTDB_WATCHDOG_BIN");

    std::string interval = "200";
    if (const char* e = std::getenv("ZRTDB_WATCHDOG_INTERVAL_MS")) {
        if (*e)
            interval = e;
    }

    // child: detach and exec
    pid_t pid = ::fork();
    if (pid != 0)
        return; // parent: best-effort

    ::setsid();

    // redirect stdio to /dev/null
    int fd = ::open("/dev/null", O_RDWR);
    if (fd >= 0) {
        ::dup2(fd, STDIN_FILENO);
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            ::close(fd);
    }
    ::chdir("/");

    auto execWith = [&](const char* path) -> void {
        char* const argv[] = {
            const_cast<char*>(path),
            const_cast<char*>("--app"),
            const_cast<char*>(appUpper.c_str()),
            const_cast<char*>("--interval-ms"),
            const_cast<char*>(interval.c_str()),
            nullptr,
        };
        ::execv(path, argv);
    };

    // a) explicit absolute path
    if (envBin && *envBin) {
        execWith(envBin);
    }

    // b) PATH
    {
        char* const argv[] = {
            const_cast<char*>("zrtdb_watchdog"),
            const_cast<char*>("--app"),
            const_cast<char*>(appUpper.c_str()),
            const_cast<char*>("--interval-ms"),
            const_cast<char*>(interval.c_str()),
            nullptr,
        };
        ::execvp("zrtdb_watchdog", argv);
    }

    // c) common fallbacks
    execWith("/bin/zrtdb_watchdog");
    execWith("/usr/local/bin/zrtdb_watchdog");

    // d) best-effort: static root sibling
    {
        auto& mgr = MmdbManager::instance();
        std::filesystem::path p = mgr.getStaticRootPath();
        p /= "bin";
        p /= "zrtdb_watchdog";
        execWith(p.c_str());
    }

    // e) last resort: sibling to current executable (适用于工具/示例程序与 watchdog 同目录的场景)
    {
        std::filesystem::path self = getSelfExePath();
        if (!self.empty()) {
            std::filesystem::path p = self.parent_path() / "zrtdb_watchdog";
            execWith(p.c_str());
        }
    }

    _exit(127);
}

void ensureRunning()
{
    if (isDisabled())
        return;

    auto& mgr = MmdbManager::instance();
    const std::string appUpper = mgr.getCurrentAppUpper();
    if (appUpper.empty())
        return;

    std::filesystem::path sockPath = getSocketPath();
    if (sockPath.empty())
        return;

    // ping: datagram sendto
    int s = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0)
        return;

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string sp = sockPath.string();
    if (sp.size() >= sizeof(addr.sun_path)) {
        ::close(s);
        return;
    }
    std::strncpy(addr.sun_path, sp.c_str(), sizeof(addr.sun_path) - 1);

    const char* ping = "{\"kind\":\"ping\"}";
    ssize_t n = ::sendto(s, ping, std::strlen(ping), MSG_NOSIGNAL, (sockaddr*)&addr, sizeof(addr));
    int err = errno;
    ::close(s);

    if (n >= 0)
        return; // already running

    // ENOENT: socket not present -> spawn
    if (err == ENOENT || err == ECONNREFUSED) {
        trySpawnWatchdog(appUpper);
    }
}

void sendEvent(const std::string& jsonLine)
{
    if (isDisabled())
        return;

    std::filesystem::path sockPath = getSocketPath();
    if (sockPath.empty())
        return;

    int s = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0)
        return;

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string sp = sockPath.string();
    if (sp.size() >= sizeof(addr.sun_path)) {
        ::close(s);
        return;
    }
    std::strncpy(addr.sun_path, sp.c_str(), sizeof(addr.sun_path) - 1);

    ::sendto(s, jsonLine.data(), jsonLine.size(), MSG_NOSIGNAL, (sockaddr*)&addr, sizeof(addr));
    ::close(s);
}

static std::string escapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

void sendAttach(const char* component)
{
    ensureRunning();
    const pid_t pid = ::getpid();
    const std::string exe = getSelfExePath();
    std::string json = std::string("{\"kind\":\"attach\",\"component\":\"") + escapeJson(component ? component : "?") + "\",\"pid\":" + std::to_string((int)pid) + ",\"exe\":\"" + escapeJson(exe) + "\"}";
    sendEvent(json);
}

void sendDetach(const char* component)
{
    const pid_t pid = ::getpid();
    std::string json = std::string("{\"kind\":\"detach\",\"component\":\"") + escapeJson(component ? component : "?") + "\",\"pid\":" + std::to_string((int)pid) + "}";
    sendEvent(json);
}

void sendAudit(const char* component,
    const char* op,
    const std::string& argsJson,
    int rc,
    std::int64_t durUs)
{
    ensureRunning();
    const pid_t pid = ::getpid();
    std::string json;
    json.reserve(256 + argsJson.size());
    json += "{\"kind\":\"audit\"";
    json += ",\"component\":\"";
    json += escapeJson(component ? component : "?");
    json += "\"";
    json += ",\"op\":\"";
    json += escapeJson(op ? op : "?");
    json += "\"";
    json += ",\"pid\":" + std::to_string((int)pid);
    json += ",\"rc\":" + std::to_string(rc);
    json += ",\"dur_us\":" + std::to_string((long long)durUs);
    json += ",\"args\":";
    if (argsJson.empty())
        json += "{}";
    else
        json += argsJson;
    json += "}";
    sendEvent(json);
}

} // namespace mmdb::watchdog
