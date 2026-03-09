// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "MmdbManager.hpp"
#include "StringUtils.h"
#include "zrtdb_const.h"
#include "zrtdb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

// -----------------------------
// 设计目标（工程约束）
// -----------------------------
// zrtdb_watchdog 是 ZRTDB 的“运行期守护进程”，用于：
// 1) 审计日志：记录 tool 与 lib API 的关键操作（通过 Unix Domain Datagram 上报）。
// 2) 容量监测：周期性检查每个 record 的 LV/MX。
// 3) 硬保险丝：当 LV > 3*MX（同时也是 record_physical_dim 的典型值）时，认为已经越过 mmap 物理上限，
//    为避免共享内存进一步被破坏，强制杀掉所有映射该 APP 的进程，并写入 FATAL 日志。
//
// 注意：本版本不引入外部配置文件；策略写死在代码中（interval 可通过参数传入）。

static constexpr int kDefaultIntervalMs = 200;
static constexpr int kKillRatio = 3; // LV > 3*MX 触发 kill
static constexpr int kSuggestGrowFactor = 2; // LV >= MX 时建议新 MX >= 2*LV

static std::atomic<bool> g_stop { false };

static void onSignal(int)
{
    g_stop.store(true, std::memory_order_relaxed);
}

static std::string nowLocalTimeMs()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm {};
    localtime_r(&tt, &tm);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        (long long)ms.count());
    return std::string(buf);
}

static std::string jsonEscape(std::string s)
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

// 极简 JSON 字段提取器：只支持本项目内部固定格式（kind/pid/exe/component/op）。
// 目的：避免引入第三方 JSON 库。
static bool jsonGetString(const std::string& j, const char* key, std::string& out)
{
    const std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    p = j.find('"', p);
    if (p == std::string::npos)
        return false;
    auto e = j.find('"', p + 1);
    if (e == std::string::npos)
        return false;
    out = j.substr(p + 1, e - (p + 1));
    return true;
}

static bool jsonGetInt(const std::string& j, const char* key, int& out)
{
    const std::string k = std::string("\"") + key + "\"";
    auto p = j.find(k);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    // skip spaces
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t'))
        ++p;
    bool neg = false;
    if (p < j.size() && j[p] == '-') {
        neg = true;
        ++p;
    }
    long long v = 0;
    bool any = false;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
        any = true;
        v = v * 10 + (j[p] - '0');
        ++p;
    }
    if (!any)
        return false;
    if (neg)
        v = -v;
    out = (int)v;
    return true;
}

struct ProcInfo {
    std::string component;
    std::string exe;
    std::string last_seen;
};

// record 水位状态，用于“只在状态变化时写日志”，避免刷屏。
enum class RecordLevel : int {
    OK = 0,
    CRIT = 1, // LV >= MX
    FATAL = 2, // LV > 3*MX
};

struct RecordState {
    RecordLevel level = RecordLevel::OK;
    int last_lv = 0;
};

static bool ensureDir(const fs::path& p)
{
    std::error_code ec;
    if (fs::exists(p, ec))
        return true;
    return fs::create_directories(p, ec);
}

static int acquireSingletonLock(const fs::path& lockFile)
{
    int fd = ::open(lockFile.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return -1;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1; // already running
    }
    return fd;
}

static int bindUnixDgram(const fs::path& sockPath)
{
    ::unlink(sockPath.c_str());

    int s = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string sp = sockPath.string();
    if (sp.size() >= sizeof(addr.sun_path)) {
        ::close(s);
        return -1;
    }
    std::strncpy(addr.sun_path, sp.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(s);
        return -1;
    }

    // 守护进程只在 APP 目录内通信，默认给用户读写权限即可。
    ::chmod(sockPath.c_str(), 0660);
    return s;
}

static void appendLogLine(std::ofstream& ofs, const std::string& line)
{
    // 单行 JSONL。为避免意外换行，做一次最小清理。
    std::string s = line;
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    ofs << s << "\n";
    ofs.flush();
}

static std::unordered_set<int> scanProcByMaps(const fs::path& appRoot)
{
    std::unordered_set<int> out;
    const std::string needle = appRoot.string();
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator("/proc", ec)) {
        if (ec)
            break;
        if (!ent.is_directory(ec))
            continue;
        const std::string name = ent.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit))
            continue;
        int pid = std::atoi(name.c_str());
        if (pid <= 1)
            continue;

        fs::path mapsPath = ent.path() / "maps";
        std::ifstream ifs(mapsPath);
        if (!ifs)
            continue;
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.find(needle) != std::string::npos) {
                out.insert(pid);
                break;
            }
        }
    }
    return out;
}

static void killProcesses(const std::unordered_set<int>& pids, std::ofstream& log, int selfPid,
    const std::string& appUpper, const std::string& reasonJson)
{
    std::vector<int> targets;
    targets.reserve(pids.size());
    for (int pid : pids) {
        if (pid <= 1 || pid == selfPid)
            continue;
        targets.push_back(pid);
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    // 先 SIGTERM
    for (int pid : targets) {
        ::kill(pid, SIGTERM);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 再 SIGKILL（仍存活的）
    std::vector<int> killed;
    killed.reserve(targets.size());
    for (int pid : targets) {
        if (::kill(pid, 0) == 0) {
            ::kill(pid, SIGKILL);
        }
        killed.push_back(pid);
    }

    // 记录一次 FATAL 执行动作
    std::ostringstream oss;
    oss << "{\"ts\":\"" << jsonEscape(nowLocalTimeMs()) << "\"";
    oss << ",\"app\":\"" << jsonEscape(appUpper) << "\"";
    oss << ",\"kind\":\"enforce_kill\"";
    oss << ",\"pids\":[";
    for (size_t i = 0; i < killed.size(); ++i) {
        if (i)
            oss << ",";
        oss << killed[i];
    }
    oss << "]";
    oss << ",\"reason\":" << reasonJson;
    oss << "}";
    appendLogLine(log, oss.str());
}

// 将包含 LV$ 的分区全部映射（只读），确保 g_runtime_app.record_lv_addrs 可用。
static void mapAllLvPartitionsReadOnly()
{
    auto& mgr = mmdb::MmdbManager::instance();

    std::vector<int> partitionIndexList;
    partitionIndexList.reserve((size_t)g_runtime_app.record_count);

    for (int r = 0; r < g_runtime_app.record_count; ++r) {
        int prt1 = g_runtime_app.record_lv_partition_1based[(size_t)r];
        if (prt1 > 0)
            partitionIndexList.push_back(prt1 - 1);
    }

    std::sort(partitionIndexList.begin(), partitionIndexList.end());
    partitionIndexList.erase(std::unique(partitionIndexList.begin(), partitionIndexList.end()), partitionIndexList.end());

    for (int prtIdx0 : partitionIndexList) {
        if (prtIdx0 < 0 || prtIdx0 >= g_runtime_app.partition_count)
            continue;
        std::string partUpper = g_runtime_app.partition_ids[(size_t)prtIdx0];
        int dbIdx0 = g_runtime_app.partition_db_1based[(size_t)prtIdx0] - 1;
        std::string dbUpper = (dbIdx0 >= 0 && dbIdx0 < g_runtime_app.db_count) ? g_runtime_app.db_ids[(size_t)dbIdx0] : partUpper;
        std::string spec = partUpper + "/" + dbUpper;
        char* dummy = nullptr;
        mgr.mapPartition(spec, &dummy, true);
    }
}

static int readLVInt32(std::uintptr_t addr)
{
    if (addr == 0)
        return 0;
    // LV$ 在现有模型中通常为 int32（bytes=4）。此处用 volatile 防止编译器优化为多次读取不一致。
    return *(reinterpret_cast<volatile int*>(addr));
}

struct Options {
    std::string appUpper;
    int intervalMs = kDefaultIntervalMs;
};

static bool parseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--app" && i + 1 < argc) {
            opt.appUpper = mmdb::utils::toUpper(argv[++i]);
        } else if (a == "--interval-ms" && i + 1 < argc) {
            opt.intervalMs = std::max(50, std::atoi(argv[++i]));
        } else if (a == "-h" || a == "--help") {
            return false;
        }
    }
    return !opt.appUpper.empty();
}

static void printHelp()
{
    std::cerr << "Usage: zrtdb_watchdog --app <APP> [--interval-ms <N>]\n";
}

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        printHelp();
        return 2;
    }

    // 处理退出信号（best-effort）
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    // 设定 APP 上下文（读取 meta）
    auto& mgr = mmdb::MmdbManager::instance();
    if (!mgr.setContext(opt.appUpper)) {
        // daemon 启动后 stdout/stderr 通常已被重定向到 /dev/null，这里返回码即可。
        return 3;
    }

    const fs::path appRoot = mgr.getAppRootPath();
    const fs::path metaRoot = appRoot / "meta";
    ensureDir(metaRoot);

    // 单实例锁（同一 APP 只能有一个 watchdog）
    const fs::path lockFile = metaRoot / "zrtdb_watchdog.lock";
    int lockFd = acquireSingletonLock(lockFile);
    if (lockFd < 0) {
        return 0; // already running
    }

    // 绑定 socket
    const fs::path sockPath = metaRoot / "zrtdb_watchdog.sock";
    int sockFd = bindUnixDgram(sockPath);
    if (sockFd < 0) {
        ::close(lockFd);
        return 4;
    }

    // 打开日志
    const fs::path logPath = metaRoot / "zrtdb_watchdog.log";
    std::ofstream log(logPath, std::ios::out | std::ios::app);
    if (!log) {
        ::close(sockFd);
        ::close(lockFd);
        return 5;
    }

    // 启动日志
    {
        std::ostringstream oss;
        oss << "{\"ts\":\"" << jsonEscape(nowLocalTimeMs()) << "\"";
        oss << ",\"app\":\"" << jsonEscape(opt.appUpper) << "\"";
        oss << ",\"kind\":\"startup\"";
        oss << ",\"interval_ms\":" << opt.intervalMs;
        oss << ",\"kill_ratio\":" << kKillRatio;
        oss << "}";
        appendLogLine(log, oss.str());
    }

    // 建议：启动时先把 LV 分区映射起来。
    mapAllLvPartitionsReadOnly();

    std::unordered_map<int, ProcInfo> attachedProcs;
    std::unordered_map<std::string, RecordState> recordStates; // key: record_id

    const int selfPid = (int)::getpid();

    auto logEventWrapped = [&](const std::string& raw) {
        std::ostringstream oss;
        oss << "{\"ts\":\"" << jsonEscape(nowLocalTimeMs()) << "\"";
        oss << ",\"app\":\"" << jsonEscape(opt.appUpper) << "\"";
        oss << ",\"kind\":\"event\"";
        oss << ",\"event\":" << raw;
        oss << "}";
        appendLogLine(log, oss.str());
    };

    auto enforceCapacity = [&]() {
        // 1) 清理已退出的进程注册
        for (auto it = attachedProcs.begin(); it != attachedProcs.end();) {
            if (it->first == selfPid) {
                ++it;
                continue;
            }
            if (::kill(it->first, 0) != 0) {
                it = attachedProcs.erase(it);
            } else {
                ++it;
            }
        }

        // 2) 扫描 record 水位
        bool needKill = false;
        std::string killReason;

        for (int r = 0; r < g_runtime_app.record_count; ++r) {
            int mx = g_runtime_app.record_max_dim[(size_t)r];
            if (mx <= 0)
                continue;

            int cap = mx * kKillRatio; // 默认物理映射=3*MX
            if (!g_runtime_app.record_physical_dim.empty() && (size_t)r < g_runtime_app.record_physical_dim.size()) {
                int pd = g_runtime_app.record_physical_dim[(size_t)r];
                if (pd > 0)
                    cap = pd;
            }

            std::uintptr_t addr = (std::uintptr_t)g_runtime_app.record_lv_addrs[(size_t)r];
            if (addr == 0)
                continue;

            int lv = readLVInt32(addr);
            const std::string rec = g_runtime_app.record_ids[(size_t)r];

            RecordState& st = recordStates[rec];
            st.last_lv = lv;

            RecordLevel level = RecordLevel::OK;
            if (lv > cap)
                level = RecordLevel::FATAL;
            else if (lv >= mx)
                level = RecordLevel::CRIT;

            if ((int)level > (int)st.level) {
                // 状态升级：写日志（一次即可）
                if (level == RecordLevel::CRIT) {
                    std::ostringstream oss;
                    oss << "{\"ts\":\"" << jsonEscape(nowLocalTimeMs()) << "\"";
                    oss << ",\"app\":\"" << jsonEscape(opt.appUpper) << "\"";
                    oss << ",\"kind\":\"capacity_crit\"";
                    oss << ",\"record\":\"" << jsonEscape(rec) << "\"";
                    oss << ",\"lv\":" << lv;
                    oss << ",\"mx\":" << mx;
                    oss << ",\"cap\":" << cap;
                    oss << ",\"suggest_mx\":" << (lv * kSuggestGrowFactor);
                    oss << ",\"msg_zh\":\"严重告警：LV>=MX（模型契约破坏）。请扩容 DAT 并重新实例化，否则可能越界写/数据破坏。\"";
                    oss << ",\"msg_en\":\"CRITICAL: LV>=MX (model contract violated). Enlarge DAT DIM and re-instantiate to avoid corruption.\"";
                    oss << "}";
                    appendLogLine(log, oss.str());
                } else if (level == RecordLevel::FATAL) {
                    std::ostringstream oss;
                    oss << "{\"record\":\"" << jsonEscape(rec) << "\"";
                    oss << ",\"lv\":" << lv;
                    oss << ",\"mx\":" << mx;
                    oss << ",\"cap\":" << cap;
                    oss << ",\"trigger\":\"lv_gt_cap\"";
                    oss << ",\"suggest_mx\":" << (lv * kSuggestGrowFactor);
                    oss << "}";
                    killReason = oss.str();
                    needKill = true;
                }
                st.level = level;
            }

            if (level == RecordLevel::FATAL) {
                // 只要存在一个 record 已进入 FATAL，本次扫描就触发 kill
                if (!needKill) {
                    std::ostringstream oss;
                    oss << "{\"record\":\"" << jsonEscape(rec) << "\"";
                    oss << ",\"lv\":" << lv;
                    oss << ",\"mx\":" << mx;
                    oss << ",\"cap\":" << cap;
                    oss << ",\"trigger\":\"lv_gt_cap\"";
                    oss << ",\"suggest_mx\":" << (lv * kSuggestGrowFactor);
                    oss << "}";
                    killReason = oss.str();
                    needKill = true;
                }
            }
        }

        if (!needKill)
            return;

        // 3) 组装需要 kill 的 PID 集合
        std::unordered_set<int> toKill;
        for (const auto& kv : attachedProcs)
            toKill.insert(kv.first);

        // 兜底：/proc/maps 扫描
        for (int pid : scanProcByMaps(appRoot))
            toKill.insert(pid);

        // 4) 紧急快照 + kill 并记录动作
        char snapName[256] = {0};
        if (SaveSnapshot_(snapName, sizeof(snapName)) < 0) {
            std::ostringstream oss;
            oss << "{\"ts\":\"" << jsonEscape(nowLocalTimeMs()) << "\",\"app\":\"" << jsonEscape(opt.appUpper)
                << "\",\"kind\":\"snapshot_failed_before_kill\",\"reason\":" << killReason << "}";
            appendLogLine(log, oss.str());
        } else {
            std::ostringstream oss;
            oss << "{\"ts\":\"" << jsonEscape(nowLocalTimeMs()) << "\",\"app\":\"" << jsonEscape(opt.appUpper)
                << "\",\"kind\":\"snapshot_before_kill\",\"name\":\"" << jsonEscape(snapName) << "\",\"reason\":" << killReason << "}";
            appendLogLine(log, oss.str());
        }
        killProcesses(toKill, log, selfPid, opt.appUpper, killReason);
    };

    // 主循环：poll socket + 周期性 enforce
    while (!g_stop.load(std::memory_order_relaxed)) {
        pollfd pfd { sockFd, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, opt.intervalMs);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            // drain
            for (;;) {
                char buf[8192];
                ssize_t n = ::recvfrom(sockFd, buf, sizeof(buf) - 1, MSG_DONTWAIT, nullptr, nullptr);
                if (n <= 0)
                    break;
                buf[n] = 0;
                std::string msg(buf, (size_t)n);

                // ping 不落盘
                std::string kind;
                if (jsonGetString(msg, "kind", kind) && kind == "ping")
                    continue;

                // event 留痕
                logEventWrapped(msg);

                if (kind == "attach") {
                    int pid = 0;
                    if (jsonGetInt(msg, "pid", pid) && pid > 0) {
                        ProcInfo pi;
                        jsonGetString(msg, "component", pi.component);
                        jsonGetString(msg, "exe", pi.exe);
                        pi.last_seen = nowLocalTimeMs();
                        attachedProcs[pid] = pi;
                    }
                } else if (kind == "detach") {
                    int pid = 0;
                    if (jsonGetInt(msg, "pid", pid) && pid > 0) {
                        attachedProcs.erase(pid);
                    }
                } else if (kind == "audit") {
                    // audit 事件本身已经写入 event log；此处可按需扩展统计。
                }
            }
        }

        // 周期扫描 + 强制动作
        enforceCapacity();
    }

    // 退出日志
    {
        std::ostringstream oss;
        oss << "{\"ts\":\"" << jsonEscape(nowLocalTimeMs()) << "\"";
        oss << ",\"app\":\"" << jsonEscape(opt.appUpper) << "\"";
        oss << ",\"kind\":\"shutdown\"";
        oss << "}";
        appendLogLine(log, oss.str());
    }

    ::close(sockFd);
    ::unlink(sockPath.c_str());
    ::close(lockFd);
    return 0;
}
