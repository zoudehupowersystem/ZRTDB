// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "DbIoShell.hpp"

#include "CommentLoader.hpp"
#include "DataAccessor.hpp"
#include "FieldPrinter.hpp"
#include "MmdbManager.hpp"
#include "QueryEngine.hpp"
#include "Snapshot.hpp"
#include "TermStyle.hpp"
#include "TableViewRenderer.hpp"
#include "TextTable.hpp"
#include "ToolUtil.hpp"
#include "WatchdogClient.hpp"
#include "zrtdb_const.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace mmdb::dbio {

namespace tu = mmdb::dbio::toolutil;

namespace fs = std::filesystem;

// --- local helpers ---
static std::string normalize_comment(std::string_view cmt) { return tu::normalize_comment(cmt); }

static std::string read_small_file(const fs::path& p, size_t maxBytes = 4096)
{
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs)
        return "";
    std::string s;
    s.resize(maxBytes);
    ifs.read(s.data(), (std::streamsize)maxBytes);
    s.resize((size_t)ifs.gcount());
    return s;
}

static std::string read_proc_comm(int pid)
{
    auto s = read_small_file(fs::path("/proc") / std::to_string(pid) / "comm", 256);
    s = tu::trim(s);
    return s.empty() ? "?" : s;
}

static std::string read_proc_cmdline(int pid)
{
    auto s = read_small_file(fs::path("/proc") / std::to_string(pid) / "cmdline", 4096);
    for (auto& ch : s) {
        if (ch == '\0')
            ch = ' ';
    }
    s = tu::trim(s);
    return s;
}

// file_time_type -> local time string with ms
static std::string format_fs_time_ms(const fs::file_time_type& ft)
{
    using namespace std::chrono;

    // Convert file clock -> system_clock (portable trick)
    auto sys_tp = time_point_cast<milliseconds>(
        system_clock::now() + (ft - fs::file_time_type::clock::now()));

    auto ms = duration_cast<milliseconds>(sys_tp.time_since_epoch()) % 1000;
    std::time_t tt = system_clock::to_time_t(time_point_cast<system_clock::duration>(sys_tp));
    std::tm tm {};
    localtime_r(&tt, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T") << "."
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

static std::string format_bytes(uintmax_t n)
{
    // 简单人类可读，不引入 fmt
    const char* unit[] = { "B", "KB", "MB", "GB", "TB" };
    double x = (double)n;
    int k = 0;
    while (x >= 1024.0 && k < 4) {
        x /= 1024.0;
        ++k;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision((k == 0) ? 0 : 2) << x << unit[k];
    return oss.str();
}

static inline bool safeCompare(std::string_view fixed, std::string_view up) { return tu::safeCompareFixedUpper(fixed, up); }
static inline std::string escape_json(std::string_view s) { return tu::escape_json(s); }

static inline std::string trim(std::string_view s) { return tu::trim(s); }
static inline std::string upper(std::string_view s) { return tu::upper(s); }

static std::vector<int> collect_global_fields_in_db(int dbIdx)
{
    std::vector<int> flds;
    if (dbIdx < 0)
        return flds;
    int p0 = g_runtime_app.db_partition_prefix[dbIdx];
    int p1 = g_runtime_app.db_partition_prefix[dbIdx + 1];
    int prtMin = p0 + 1;
    int prtMax = p1;
    for (int f = 0; f < g_runtime_app.field_count; ++f) {
        if (g_runtime_app.field_record_1based[f] != 0)
            continue;
        int prtNo = g_runtime_app.field_partition_1based[f];
        if (prtNo < prtMin || prtNo > prtMax)
            continue;
        flds.push_back(f);
    }
    return flds;
}

static std::vector<int> collect_record_fields(int recGlobalIdx)
{
    std::vector<int> flds;
    if (recGlobalIdx < 0)
        return flds;
    int startTbl = g_runtime_app.record_table_prefix[recGlobalIdx];
    int endTbl = g_runtime_app.record_table_prefix[recGlobalIdx + 1];
    flds.reserve((size_t)std::max(0, endTbl - startTbl));
    for (int t = startTbl; t < endTbl; ++t)
        flds.push_back(g_runtime_app.table_field_1based[t] - 1);
    return flds;
}

DbIoShell::DbIoShell(const std::string& appUpper, const std::string& startDbUpper)
    : appUpper_(appUpper)
{
    initCommands();
    // load comments from DAT
    CommentLoader::instance().load((mmdb::MmdbManager::instance().getStaticRootPath() / "DAT").string());

    // context already set by main
    if (!switchToDb(startDbUpper)) {
        // fallback to first db
        if (g_runtime_app.db_count > 0) {
            std::string db0 = g_runtime_app.db_ids[0];
            db0 = upper(trim(db0));
            switchToDb(db0);
        }
    }

    ctx_.isGlobal = true;
    ctx_.recIdx = -1;
    ctx_.recName = "ITEM";
    ctx_.currentSlot = 1;
    std::cout << "--- zrtdb_tool (APP=" << appUpper << ") ---\n";
    cmdListGlobals("");
}

void DbIoShell::initCommands()
{
    commands_["H"] = [this](auto s) { cmdHelp(s); };
    commands_["Q"] = [this](auto s) { cmdQuit(s); };
    commands_["EX"] = [this](auto s) { cmdQuit(s); };
    commands_["LI"] = [this](auto s) { cmdListRecords(s); };
    commands_["LIG"] = [this](auto s) { cmdListGlobals(s); };
    commands_["P/"] = [this](auto s) { cmdPosition(s); };
    commands_["SH"] = [this](auto s) { cmdShowCurrent(s); };
    commands_["R"] = [this](auto s) { cmdShowCurrent(s); };

    // 新增：快照命令（不影响旧命令）
    commands_["SNAP"] = [this](auto s) { cmdSnap(s); };
    commands_["LSNAP"] = [this](auto s) { cmdListSnap(s); };
    commands_["LOADSNAP"] = [this](auto s) { cmdLoadSnap(s); };

    // NEW
    commands_["STATUS"] = [this](auto s) { cmdStatus(s); };
    commands_["ST"] = [this](auto s) { cmdStatus(s); }; // 可选别名

    // Query / Locate / Sort
    commands_["SEL"] = [this](auto s) { cmdSelect(s); };
    commands_["FIND"] = [this](auto s) { cmdFind(s); };
    commands_["NEXT"] = [this](auto s) { cmdNext(s); };
    commands_["N"] = [this](auto s) { cmdNext(s); };
    commands_["PREV"] = [this](auto s) { cmdPrev(s); };
    commands_["B"] = [this](auto s) { cmdPrev(s); };
}

void DbIoShell::run()
{
    std::string line;
    while (running_) {
        std::cout << ctx_.dbName << "  RH>";
        if (!std::getline(std::cin, line))
            break;
        if (line.empty())
            continue;
        try {
            processLine(line);
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        } catch (...) {
            std::cout << "Error: unknown exception\n";
        }
    }
}

bool DbIoShell::handleViewKey(const std::string& line)
{
    if (!view_.active)
        return false;

    std::string cmd = trim(line);
    if (cmd == "li" || cmd == "LI") {
        view_ = TableViewState {};
        ctx_.isGlobal = true;
        ctx_.recIdx = -1;
        ctx_.recName = "ITEM";
        cmdListGlobals("");
        return true;
    }
    if (cmd == "^") {
        view_.row0 = std::max(1, view_.row0 - kPageRows);
        if (view_.kind == TableViewState::RECORD)
            renderRecordTable();
        else
            renderGlobalsTable();
        return true;
    }
    if (cmd == "v" || cmd == "V") {
        view_.row0 += kPageRows;
        if (view_.kind == TableViewState::RECORD)
            renderRecordTable();
        else
            renderGlobalsTable();
        return true;
    }
    if (cmd == "<") {
        view_.col0 = std::max(0, view_.col0 - kPageCols);
        if (view_.kind == TableViewState::RECORD)
            renderRecordTable();
        else
            renderGlobalsTable();
        return true;
    }
    if (cmd == ">") {
        view_.col0 += kPageCols;
        if (view_.kind == TableViewState::RECORD)
            renderRecordTable();
        else
            renderGlobalsTable();
        return true;
    }
    return false;
}

void DbIoShell::processLine(const std::string& lineRaw)
{
    // -----------------------------
    // 审计日志：记录每一次 tool 输入（含翻页按键等）。
    // 说明：该日志为 best-effort，不允许影响工具的交互与实时性。
    // -----------------------------
    struct AuditScope {
        std::string appUpper;
        std::string dbUpper;
        std::string recUpper;
        std::string input;
        bool enabled = true;
        std::chrono::steady_clock::time_point t0;
        int rc = 0;

        explicit AuditScope(const DbIoShell& self, const std::string& in)
            : appUpper(self.appUpper_)
            , dbUpper(self.ctx_.dbName)
            , recUpper(self.ctx_.recName)
            , input(in)
            , t0(std::chrono::steady_clock::now())
        {
            if (input.empty()) {
                enabled = false;
                return;
            }
            // 限制单条日志长度，避免用户误输入超长内容导致日志膨胀。
            if (input.size() > 256)
                input.resize(256);
        }

        ~AuditScope()
        {
            if (!enabled)
                return;
            using namespace std::chrono;
            const auto us = duration_cast<microseconds>(steady_clock::now() - t0).count();
            std::string args;
            args.reserve(128 + input.size());
            args += "{\"app\":\"" + escape_json(appUpper) + "\"";
            args += ",\"db\":\"" + escape_json(dbUpper) + "\"";
            args += ",\"rec\":\"" + escape_json(recUpper) + "\"";
            args += ",\"input\":\"" + escape_json(input) + "\"}";
            mmdb::watchdog::sendAudit("tool", "CmdLine", args, rc, us);
        }
    } audit(*this, trim(lineRaw));

    // view keys take precedence
    if (handleViewKey(lineRaw))
        return;

    std::string line = trim(lineRaw);
    if (line.empty())
        return;

    // show/<name>
    if (line.rfind("show/", 0) == 0 || line.rfind("SHOW/", 0) == 0) {
        cmdShow(line.substr(5));
        return;
    }

    // /field(...) or /field=...
    if (!line.empty() && line[0] == '/') {
        cmdInspectField(line.substr(1));
        return;
    }

    // +n/-n pointer move
    if (!line.empty() && (line[0] == '+' || line[0] == '-')) {
        int sign = (line[0] == '+') ? 1 : -1;
        std::string num = trim(line.substr(1));
        int off = 1;
        if (!num.empty()) {
            if (!tu::parse_int(num, off) || off < 0) {
                std::cout << "Bad offset: " << num << "\n";
                return;
            }
        }
        ctx_.currentSlot = std::max(1, ctx_.currentSlot + sign * off);
        std::cout << "Position is " << ctx_.recName << " (" << ctx_.currentSlot << ")\n";
        return;
    }

    // assignment like LV$REC = n
    {
        std::string up = upper(line);
        if (tryAssignSimple(up))
            return;
    }

    // P/xxx
    if (line.rfind("P/", 0) == 0 || line.rfind("p/", 0) == 0) {
        cmdPosition(line.substr(2));
        return;
    }

    // command tokens
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    std::string args;
    std::getline(ss, args);
    args = trim(args);

    cmd = upper(cmd);
    if (commands_.count(cmd)) {
        commands_[cmd](args);
        return;
    }
    audit.rc = 1;
    std::cout << "Unknown command. Use H.\n";
}

void DbIoShell::cmdHelp(const std::string&)
{
    std::cout
        << "Commands:\n"
        << "  LI                 list records in current DB\n"
        << "  LIG / li           list globals (ITEM)\n"
        << "  P/RECNAME          switch record (P/item for globals)\n"
        << "  +n / -n            move row pointer\n"
        << "  /FIELD             show field at current row\n"
        << "  /FIELD(3:9)        show field rows 3..9\n"
        << "  /FIELD(3:)         show field rows 3..LV\n"
        << "  /FIELD(:)=220      set range\n"
        << "  LV$REC = 3         set record LV size\n"
        << "  show/RECNAME       table view (10x10)\n"
        << "  show/item          global table view\n"
        << "  status             runtime status (apps/sec/proc maps)\n"
        << "  SEL <expr> [COLS ...] [SORT ...] [LIMIT n|ALL] [OFFSET n] [GOTO first|k]\n"
        << "                     filter/query current scope (record or ITEM); output a table\n"
        << "  FIND <expr> [...]   like SEL, but default LIMIT 1 and GOTO FIRST\n"
        << "  NEXT/N              jump to next matched row (from last SEL/FIND)\n"
        << "  PREV/B              jump to previous matched row (from last SEL/FIND)\n"
        << "  In table view: ^ v < > li\n"
        << "  SNAP [note...]     save snapshot under /var/ZRTDB/<APP>/<APP>_YYYYMMDD-HHMMSS.mmm/\n"
        << "  LSNAP              list snapshots of current APP\n"
        << "  LOADSNAP <dir>     restore snapshot (relative name or absolute path)\n"
        << "  Q/EX               quit\n";
}

void DbIoShell::cmdStatus(const std::string&)
{
    const auto runtimeRoot = mmdb::MmdbManager::instance().getRuntimeRootPath();

    struct SecInfo {
        fs::path path;
        uintmax_t size = 0;
        std::string mtime;
        int mappedProcCount = 0;
    };

    struct AppInfo {
        std::string app;
        fs::path root;
        std::vector<SecInfo> secs;
        std::vector<SecInfo> meta; // 用同一个结构体存 meta 文件信息
        uintmax_t totalBytes = 0;
    };

    std::vector<AppInfo> apps;

    // 1) discover apps
    try {
        if (!fs::exists(runtimeRoot) || !fs::is_directory(runtimeRoot)) {
            std::cout << "[status] runtime root not found: " << runtimeRoot << "\n";
            return;
        }

        for (const auto& e : fs::directory_iterator(runtimeRoot)) {
            if (!e.is_directory())
                continue;

            auto appDir = e.path();
            auto appName = appDir.filename().string();

            auto metaDir = appDir / "meta" / "apps";
            auto zrtdb_dataDir = appDir / "zrtdb_data";

            bool looksLikeApp = false;
            if (fs::exists(metaDir) && fs::is_directory(metaDir)) {
                looksLikeApp = true;
            }
            if (fs::exists(zrtdb_dataDir) && fs::is_directory(zrtdb_dataDir)) {
                // 有任何 .sec 也算
                for (const auto& ee : fs::recursive_directory_iterator(zrtdb_dataDir)) {
                    if (ee.is_regular_file() && ee.path().extension() == ".sec") {
                        looksLikeApp = true;
                        break;
                    }
                }
            }
            if (!looksLikeApp)
                continue;

            AppInfo ai;
            ai.app = appName;
            ai.root = appDir;

            // meta files
            {
                const fs::path clon = metaDir / (appName + ".sec");
                const fs::path appn = metaDir / (appName + "_NEW.sec");
                for (auto& mf : { clon, appn }) {
                    if (!fs::exists(mf) || !fs::is_regular_file(mf))
                        continue;
                    SecInfo si;
                    si.path = mf;
                    si.size = fs::file_size(mf);
                    si.mtime = format_fs_time_ms(fs::last_write_time(mf));
                    ai.meta.push_back(std::move(si));
                }
            }

            // sec files
            if (fs::exists(zrtdb_dataDir) && fs::is_directory(zrtdb_dataDir)) {
                for (const auto& sf : fs::recursive_directory_iterator(zrtdb_dataDir)) {
                    if (!sf.is_regular_file())
                        continue;
                    if (sf.path().extension() != ".sec")
                        continue;

                    SecInfo si;
                    si.path = sf.path();
                    si.size = fs::file_size(sf.path());
                    si.mtime = format_fs_time_ms(fs::last_write_time(sf.path()));
                    ai.totalBytes += si.size;
                    ai.secs.push_back(std::move(si));
                }
            }

            apps.push_back(std::move(ai));
        }
    } catch (const std::exception& e) {
        std::cout << "[status] scan apps failed: " << e.what() << "\n";
        return;
    }

    // 2) build sec path -> (app index, sec index)
    std::unordered_map<std::string, std::pair<int, int>> secIndex;
    secIndex.reserve(4096);
    for (int i = 0; i < (int)apps.size(); ++i) {
        for (int j = 0; j < (int)apps[i].secs.size(); ++j) {
            secIndex[apps[i].secs[j].path.string()] = { i, j };
        }
    }

    // 3) scan /proc/*/maps to find mapped processes
    // appPidMapCount[appIdx][pid] = mappedSecFilesCount
    std::vector<std::unordered_map<int, int>> appPidMapCount(apps.size());

    int procScanned = 0;
    int procReadable = 0;
    int hits = 0;

    try {
        for (const auto& pe : fs::directory_iterator("/proc")) {
            if (!pe.is_directory())
                continue;
            const std::string name = pe.path().filename().string();
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
                continue;

            ++procScanned;
            int pid = 0;
            if (!tu::parse_int(name, pid))
                continue;

            fs::path mapsPath = pe.path() / "maps";
            std::ifstream ifs(mapsPath);
            if (!ifs)
                continue;
            ++procReadable;

            std::string line;
            while (std::getline(ifs, line)) {
                // 快速过滤：只关心 .sec 且包含 runtimeRoot
                if (line.find(".sec") == std::string::npos)
                    continue;
                if (line.find(runtimeRoot.string()) == std::string::npos)
                    continue;

                // maps 行最后一列通常是 pathname（可能缺失）
                auto pos = line.rfind(' ');
                if (pos == std::string::npos)
                    continue;
                std::string path = trim(line.substr(pos + 1));
                if (path.empty() || path[0] != '/')
                    continue;

                auto it = secIndex.find(path);
                if (it == secIndex.end())
                    continue;

                const int appIdx = it->second.first;
                const int secIdx = it->second.second;

                apps[appIdx].secs[secIdx].mappedProcCount += 1; // 这里是“命中次数”，不是去重
                appPidMapCount[appIdx][pid] += 1;
                ++hits;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[status] scan /proc failed: " << e.what() << "\n";
    }

    // 4) 输出
    std::cout << "==== ZRTDB Runtime Status ====\n";
    std::cout << "RuntimeRoot: " << runtimeRoot << "\n";
    std::cout << "Apps: " << apps.size() << "\n";
    std::cout << "Proc scanned/readable: " << procScanned << "/" << procReadable
              << "  (hits=" << hits << ")\n";
    if (procReadable < procScanned) {
        std::cout << "Note: /proc/<pid>/maps may be unreadable without root; mapping list can be incomplete.\n";
    }

    for (int i = 0; i < (int)apps.size(); ++i) {
        auto& a = apps[i];
        std::cout << "\n-- APP=" << a.app << "  root=" << a.root << "\n";

        if (!a.meta.empty()) {
            std::cout << "  meta/apps:\n";
            for (auto& mf : a.meta) {
                std::cout << "    " << mf.path.filename().string()
                          << "  mtime=" << mf.mtime
                          << "  size=" << format_bytes(mf.size) << "\n";
            }
        } else {
            std::cout << "  meta/apps: (missing)\n";
        }

        std::cout << "  zrtdb_data: files=" << a.secs.size()
                  << "  total=" << format_bytes(a.totalBytes) << "\n";

        // 列出 sec 文件概览（按路径排序更利于人工查看）
        std::sort(a.secs.begin(), a.secs.end(), [](const SecInfo& x, const SecInfo& y) {
            return x.path.string() < y.path.string();
        });

        // 每个 .sec 给出 mtime/size/映射进程数（粗略）
        for (auto& s : a.secs) {
            // 映射进程数这里用“命中次数”并不严谨；为了不复杂化，这里只给粗略信息。
            // 更严格要对 pid 去重并在 sec 维度聚合；如果你强需求，我建议下一版再做。
            std::cout << "    " << fs::relative(s.path, a.root).string()
                      << "  mtime=" << s.mtime
                      << "  size=" << format_bytes(s.size) << "\n";
        }

        // 映射进程列表
        if (appPidMapCount[i].empty()) {
            std::cout << "  mapped-by: (none detected)\n";
        } else {
            std::cout << "  mapped-by: " << appPidMapCount[i].size() << " process(es)\n";

            // 为了稳定输出，按 pid 排序
            std::vector<int> pids;
            pids.reserve(appPidMapCount[i].size());
            for (auto& kv : appPidMapCount[i])
                pids.push_back(kv.first);
            std::sort(pids.begin(), pids.end());

            for (int pid : pids) {
                auto comm = read_proc_comm(pid);
                auto cmdl = read_proc_cmdline(pid);
                int cnt = appPidMapCount[i][pid];
                std::cout << "    pid=" << pid
                          << "  comm=" << comm
                          << "  mapped_entries=" << cnt;
                if (!cmdl.empty())
                    std::cout << "  cmdline=" << cmdl;
                std::cout << "\n";
            }
        }
    }
}

void DbIoShell::cmdSnap(const std::string& args)
{
    try {
        auto info = mmdb::snapshot::save(args);
        std::cout << "[snapshot] saved: " << info.dir.string() << "\n";
    } catch (const std::exception& e) {
        std::cout << "[snapshot] failed: " << e.what() << "\n";
    }
}

void DbIoShell::cmdListSnap(const std::string&)
{
    try {
        auto dirs = mmdb::snapshot::list();
        if (dirs.empty()) {
            std::cout << "[snapshot] none\n";
            return;
        }
        for (auto& d : dirs) {
            std::cout << "  " << d.filename().string() << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << "[snapshot] list failed: " << e.what() << "\n";
    }
}

void DbIoShell::cmdLoadSnap(const std::string& args)
{
    std::string s = trim(args);
    if (s.empty()) {
        std::cout << "Usage: LOADSNAP <snapshot_dir>\n";
        return;
    }
    try {
        mmdb::snapshot::load(s);
        std::cout << "[snapshot] loaded from: " << s << "\n";
    } catch (const std::exception& e) {
        std::cout << "[snapshot] load failed: " << e.what() << "\n";
    }
}

void DbIoShell::cmdQuit(const std::string&)
{
    running_ = false;
}

bool DbIoShell::switchToDb(const std::string& dbUpper)
{
    for (int i = 0; i < g_runtime_app.db_count; ++i) {
        if (safeCompare(g_runtime_app.db_ids[i], dbUpper)) {
            ctx_.dbIdx = i;
            ctx_.dbName = dbUpper;
            mapAllPartitionsForDb(i);

            // set default record as first record of this DB
            int rs = g_runtime_app.db_record_prefix[i];
            int re = g_runtime_app.db_record_prefix[i + 1];
            if (rs < re) {
                ctx_.recIdx = rs;
                ctx_.recName = upper(trim(std::string(g_runtime_app.record_ids[rs])));
            } else {
                ctx_.recIdx = -1;
                ctx_.recName = "ITEM";
                ctx_.isGlobal = true;
            }
            ctx_.currentSlot = 1;
            return true;
        }
    }
    return false;
}

void DbIoShell::mapPartitionByIndex(int prtIdx)
{
    if (prtIdx < 0 || prtIdx >= g_runtime_app.partition_count)
        return;
    std::string part = upper(trim(std::string(g_runtime_app.partition_ids[prtIdx])));

    // Determine owning DB
    int dbi1 = g_runtime_app.partition_db_1based[prtIdx]; // 1-based
    std::string db = part;
    if (dbi1 > 0 && dbi1 - 1 < g_runtime_app.db_count) {
        db = upper(trim(std::string(g_runtime_app.db_ids[dbi1 - 1])));
    }

    std::string spec = part + "/" + db;
    char* addr = nullptr;
    mmdb::MmdbManager::instance().mapPartition(spec, &addr, false);
}

void DbIoShell::mapAllPartitionsForDb(int dbIdx)
{
    int p0 = g_runtime_app.db_partition_prefix[dbIdx];
    int p1 = g_runtime_app.db_partition_prefix[dbIdx + 1];
    for (int p = p0; p < p1; ++p) {
        mapPartitionByIndex(p);
    }
}

void DbIoShell::cmdListGlobals(const std::string&)
{
    if (ctx_.dbIdx < 0)
        return;

    ctx_.isGlobal = true;
    ctx_.recIdx = -1;
    ctx_.recName = "ITEM";
    ctx_.currentSlot = 1;

    const auto flds = collect_global_fields_in_db(ctx_.dbIdx);
    const auto rows = collect_field_rows(flds, 1);
    FieldPrintOptions opt;
    auto& st = TermStyle::instance();
    opt.headerLine = "Globals in DB " + st.record(ctx_.dbName) + ":";
    opt.showSlot = false;
    print_field_rows(rows, opt);
}

void DbIoShell::cmdListRecords(const std::string&)
{
    if (ctx_.dbIdx < 0)
        return;

    int rs = g_runtime_app.db_record_prefix[ctx_.dbIdx];
    int re = g_runtime_app.db_record_prefix[ctx_.dbIdx + 1];

    auto& st = TermStyle::instance();

    // Pre-scan for alignment.
    int maxRecW = 0;
    for (int i = rs; i < re; ++i) {
        std::string rname = upper(trim(std::string(g_runtime_app.record_ids[i])));
        maxRecW = std::max(maxRecW, term_display_width(rname));
    }
    maxRecW = std::min(maxRecW, 16);

    for (int i = rs; i < re; ++i) {
        int lv = 0;
        if (g_runtime_app.record_lv_addrs[i] != 0) {
            lv = *reinterpret_cast<int*>(g_runtime_app.record_lv_addrs[i]);
        }
        int mx = g_runtime_app.record_max_dim[i];
        std::string rname = upper(trim(std::string(g_runtime_app.record_ids[i])));
        std::string cmt = CommentLoader::instance().getComment(rname);

        std::string recPad = term_pad_right(term_truncate(rname, maxRecW), maxRecW);
        std::cout << "  (" << term_pad_left(std::to_string(i - rs + 1), 3) << ") "
                  << st.dim("RECORD:") << " "
                  << st.record(recPad)
                  << "  "
                  << st.dim("LV=") << term_pad_left(std::to_string(lv), 6)
                  << "  "
                  << st.dim("MX=") << term_pad_left(std::to_string(mx), 6)
                  << "  "
                  << st.comment(cmt)
                  << "\n";
    }
}

bool DbIoShell::switchToRecord(const std::string& recUpper)
{
    if (ctx_.dbIdx < 0)
        return false;

    int rs = g_runtime_app.db_record_prefix[ctx_.dbIdx];
    int re = g_runtime_app.db_record_prefix[ctx_.dbIdx + 1];
    for (int i = rs; i < re; ++i) {
        if (safeCompare(g_runtime_app.record_ids[i], recUpper)) {
            ctx_.recIdx = i;
            ctx_.recName = recUpper;
            ctx_.isGlobal = false;
            ctx_.currentSlot = 1;
            return true;
        }
    }
    return false;
}

void DbIoShell::cmdPosition(const std::string& arg)
{
    std::string s = upper(trim(arg));
    if (s.empty())
        return;

    if (s == "ITEM") {
        cmdListGlobals("");
        return;
    }

    if (!switchToRecord(s)) {
        std::cout << "Record not found: " << s << "\n";
        return;
    }
    std::cout << "Position is " << ctx_.recName << " (" << ctx_.currentSlot << ")\n";
}

int DbIoShell::getLVForRecordGlobalIndex(int recGlobalIdx) const
{
    if (recGlobalIdx < 0 || recGlobalIdx >= g_runtime_app.record_count)
        return 0;
    if (g_runtime_app.record_lv_addrs[recGlobalIdx] == 0)
        return 0;
    return *reinterpret_cast<int*>(g_runtime_app.record_lv_addrs[recGlobalIdx]);
}

void DbIoShell::cmdShowCurrent(const std::string&)
{
    if (ctx_.dbIdx < 0)
        return;

    if (ctx_.isGlobal) {
        cmdListGlobals("");
        return;
    }

    if (ctx_.recIdx < 0)
        return;

    auto& st = TermStyle::instance();
    const auto flds = collect_record_fields(ctx_.recIdx);
    const auto rows = collect_field_rows(flds, ctx_.currentSlot);
    FieldPrintOptions opt;
    opt.headerLine = st.dim("Record ") + st.record(ctx_.recName) + st.dim(" (slot=")
        + st.idx(std::to_string(ctx_.currentSlot)) + st.dim(")");
    opt.showSlot = true;
    opt.slot = ctx_.currentSlot;
    print_field_rows(rows, opt);
}

int DbIoShell::findFieldIndexGlobalOrCurrent(const std::string& fieldUpper) const
{
    if (ctx_.dbIdx < 0)
        return -1;

    if (ctx_.isGlobal) {
        int p0 = g_runtime_app.db_partition_prefix[ctx_.dbIdx];
        int p1 = g_runtime_app.db_partition_prefix[ctx_.dbIdx + 1];
        int prtMin = p0 + 1;
        int prtMax = p1;
        for (int f = 0; f < g_runtime_app.field_count; ++f) {
            if (g_runtime_app.field_record_1based[f] > 0)
                continue;
            int prtNo = g_runtime_app.field_partition_1based[f];
            if (prtNo < prtMin || prtNo > prtMax)
                continue;
            if (safeCompare(g_runtime_app.field_ids[f], fieldUpper))
                return f;
        }
        return -1;
    }

    // current record fields
    int startTbl = g_runtime_app.record_table_prefix[ctx_.recIdx];
    int endTbl = g_runtime_app.record_table_prefix[ctx_.recIdx + 1];
    for (int t = startTbl; t < endTbl; ++t) {
        int fIdx = g_runtime_app.table_field_1based[t] - 1;
        if (safeCompare(g_runtime_app.field_ids[fIdx], fieldUpper))
            return fIdx;
    }

    // fallback: global
    for (int f = 0; f < g_runtime_app.field_count; ++f) {
        if (g_runtime_app.field_record_1based[f] == 0) {
            if (safeCompare(g_runtime_app.field_ids[f], fieldUpper))
                return f;
        }
    }

    return -1;
}

bool DbIoShell::tryAssignSimple(const std::string& lineUpper)
{
    auto pos = lineUpper.find('=');
    if (pos == std::string::npos)
        return false;

    std::string lhs = trim(lineUpper.substr(0, pos));
    std::string rhs = trim(lineUpper.substr(pos + 1));
    if (lhs.empty() || rhs.empty())
        return false;

    // only LV$<REC>
    if (lhs.rfind("LV$", 0) != 0)
        return false;

    int newLv = 0;
    if (!tu::parse_int(rhs, newLv)) {
        std::cout << "Bad number: " << rhs << "\n";
        return true;
    }

    // find field idx in globals
    bool prevGlobal = ctx_.isGlobal;
    ctx_.isGlobal = true;
    int fldIdx = findFieldIndexGlobalOrCurrent(lhs);
    ctx_.isGlobal = prevGlobal;

    if (fldIdx < 0) {
        std::cout << "No such field: " << lhs << "\n";
        return true;
    }

    // clamp by MX from record list if exists
    std::string rec = lhs.substr(3);
    int mx = 0;
    for (int i = 0; i < g_runtime_app.record_count; ++i) {
        if (safeCompare(g_runtime_app.record_ids[i], rec)) {
            mx = g_runtime_app.record_max_dim[i];
            break;
        }
    }
    if (newLv < 0)
        newLv = 0;
    if (mx > 0 && newLv > mx)
        newLv = mx;

    if (DataAccessor::setValue(fldIdx, 1, std::to_string(newLv))) {
        std::cout << lhs << " = " << newLv << "\n";
        // refresh view
        if (view_.active) {
            if (view_.kind == TableViewState::RECORD)
                renderRecordTable();
            else
                renderGlobalsTable();
        }
    } else {
        std::cout << "Failed to set " << lhs << "\n";
    }

    return true;
}

void DbIoShell::cmdInspectField(const std::string& exprRaw)
{
    std::string expr = trim(exprRaw);
    if (expr.empty())
        return;

    // parse assignment:  FIELD(1:3)=...
    auto [lhs, rhs] = tu::split1(expr, '=');
    const bool isAssign = !rhs.empty();
    const std::string valStr = rhs;

    // parse range: FIELD(3:9)
    std::string fld = lhs;
    int start = ctx_.currentSlot;
    int end = ctx_.currentSlot;
    bool hasRange = false;

    auto p1 = lhs.find('(');
    auto p2 = lhs.find(')');
    if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1) {
        fld = trim(lhs.substr(0, p1));
        const std::string inner = trim(lhs.substr(p1 + 1, p2 - p1 - 1));
        auto rg = tu::parse_range(inner);
        if (!rg.has && !inner.empty()) {
            std::cout << "Bad range: (" << inner << ")\n";
            return;
        }
        hasRange = rg.has;
        if (hasRange) {
            start = rg.start;
            end = rg.openEnd ? -1 : rg.end;
        }
    }

    std::string fldUpper = upper(fld);
    int fldIdx = findFieldIndexGlobalOrCurrent(fldUpper);
    if (fldIdx < 0) {
        std::cout << "Field not found: " << fldUpper << "\n";
        return;
    }

    // determine last index for record field
    int recNo = g_runtime_app.field_record_1based[fldIdx]; // 1-based record, 0 global
    int last = 1;
    if (recNo > 0) {
        int recGlobalIdx = recNo - 1;
        last = getLVForRecordGlobalIndex(recGlobalIdx);
        if (last <= 0)
            last = g_runtime_app.record_max_dim[recGlobalIdx];
        if (!hasRange) {
            start = end = ctx_.currentSlot;
        }
        if (end < 0)
            end = last;
        start = std::max(1, start);
        end = std::min(last, end);
    } else {
        // scalar/global field
        start = end = 1;
    }

    for (int i = start; i <= end; ++i) {
        if (isAssign) {
            if (!DataAccessor::setValue(fldIdx, i, valStr)) {
                std::cout << "  [" << i << "] Failed\n";
            }
        } else {
            auto v = DataAccessor::getValue(fldIdx, i);
            std::cout << "  [" << i << "] = " << DataAccessor::formatValue(v) << "\n";
        }
    }
}

void DbIoShell::cmdShow(const std::string& nameRaw)
{
    std::string name = upper(trim(nameRaw));
    if (name.empty())
        return;

    if (name == "ITEM" || name == "GLOBALS") {
        view_.active = true;
        view_.kind = TableViewState::GLOBALS;
        view_.row0 = 1;
        view_.col0 = 0;
        renderGlobalsTable();
        return;
    }

    // find record in current DB
    int rs = g_runtime_app.db_record_prefix[ctx_.dbIdx];
    int re = g_runtime_app.db_record_prefix[ctx_.dbIdx + 1];
    int recGlobal = -1;
    for (int i = rs; i < re; ++i) {
        if (safeCompare(g_runtime_app.record_ids[i], name)) {
            recGlobal = i;
            break;
        }
    }
    if (recGlobal < 0) {
        std::cout << "No such record: " << name << "\n";
        return;
    }

    view_.active = true;
    view_.kind = TableViewState::RECORD;
    view_.record_index = recGlobal;
    view_.row0 = 1;
    view_.col0 = 0;
    view_.field_indices = collect_record_fields(recGlobal);

    renderRecordTable();
}

// -----------------------------
// Query / Locate / Sort
// -----------------------------
static std::string joinTokensRange(const std::vector<std::string>& toks, size_t b, size_t e)
{
    std::string out;
    for (size_t i = b; i < e; ++i) {
        if (i > b)
            out.push_back(' ');
        out += toks[i];
    }
    return out;
}


static std::vector<std::string> splitCsvLoose(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        cur = trim(cur);
        if (!cur.empty())
            out.push_back(cur);
        cur.clear();
    };

    for (char ch : s) {
        if (ch == ',') {
            flush();
            continue;
        }
        cur.push_back(ch);
    }
    flush();

    // also split remaining parts by whitespace (for "a b c" style)
    std::vector<std::string> out2;
    for (auto& x : out) {
        std::istringstream iss(x);
        std::string w;
        while (iss >> w)
            out2.push_back(w);
    }
    return out2;
}

static bool tokIsKeyword(const std::string& up)
{
    return up == "COLS" || up == "SORT" || up == "LIMIT" || up == "OFFSET" || up == "GOTO";
}

void DbIoShell::cmdSelect(const std::string& argsRaw)
{
    if (ctx_.dbIdx < 0) {
        std::cout << "No DB selected.\n";
        return;
    }

    std::string args = trim(argsRaw);
    if (args.empty()) {
        std::cout << "Usage: SEL <expr> [COLS f1,f2,..|*] [SORT f[:asc|desc],..] [LIMIT n|ALL] [OFFSET n] [GOTO first|k]\n";
        return;
    }

    auto toks = QueryEngine::splitCommandTokens(args);

    // locate first keyword boundary
    size_t exprEnd = toks.size();
    for (size_t i = 0; i < toks.size(); ++i) {
        auto up = upper(toks[i]);
        if (tokIsKeyword(up)) {
            exprEnd = i;
            break;
        }
    }

    std::string expr = joinTokensRange(toks, 0, exprEnd);
    expr = trim(expr);
    if (expr.empty()) {
        std::cout << "SEL: empty expression.\n";
        return;
    }

    std::string colsSpec;
    std::string sortSpec;
    int limit = 100;
    bool limitAll = false;
    int offset = 0;
    bool hasGoto = false;
    int gotoK = 0;

    // parse sections
    size_t i = exprEnd;
    while (i < toks.size()) {
        std::string kw = upper(toks[i]);
        ++i;
        size_t j = i;
        while (j < toks.size() && !tokIsKeyword(upper(toks[j])))
            ++j;
        std::string body = joinTokensRange(toks, i, j);
        body = trim(body);

        if (kw == "COLS") {
            colsSpec = body;
        } else if (kw == "SORT") {
            sortSpec = body;
        } else if (kw == "LIMIT") {
            std::string upb = upper(body);
            if (upb == "ALL" || upb == "INF" || upb == "MAX") {
                limitAll = true;
                limit = -1;
            } else {
                if (!tu::parse_int(body, limit)) {
                    std::cout << "SEL: bad LIMIT: " << body << "\n";
                    return;
                }
                if (limit <= 0) {
                    limitAll = true;
                    limit = -1;
                }
            }
        } else if (kw == "OFFSET") {
            if (!tu::parse_int(body, offset)) {
                std::cout << "SEL: bad OFFSET: " << body << "\n";
                return;
            }
            if (offset < 0)
                offset = 0;
        } else if (kw == "GOTO") {
            std::string upb = upper(body);
            if (upb == "FIRST") {
                hasGoto = true;
                gotoK = 1;
            } else {
                if (!tu::parse_int(body, gotoK) || gotoK <= 0) {
                    std::cout << "SEL: bad GOTO: " << body << "\n";
                    return;
                }
                hasGoto = true;
            }
        }

        i = j;
    }

    // determine LV
    int lv = 1;
    int mx = 1;
    if (!ctx_.isGlobal) {
        if (ctx_.recIdx < 0) {
            std::cout << "SEL: no record selected.\n";
            return;
        }
        mx = g_runtime_app.record_max_dim[ctx_.recIdx];
        lv = getLVForRecordGlobalIndex(ctx_.recIdx);
        if (lv <= 0)
            lv = mx;
        if (mx > 0 && lv > mx)
            lv = mx;
        if (lv <= 0) {
            std::cout << "SEL: LV=0.\n";
            return;
        }
    }

    // compile expression
    QueryEngine::EvalLookup lk;
    lk.resolve = [this](const std::string& identUpper, int row, DbValue& out) -> bool {
        int fldIdx = findFieldIndexGlobalOrCurrent(identUpper);
        if (fldIdx < 0)
            return false;
        out = DataAccessor::getValue(fldIdx, row);
        return true;
    };

    QueryEngine::CompileResult cr;
    std::string err;
    if (!QueryEngine::compileBoolExpr(expr, lk, cr, err)) {
        std::cout << "SEL: bad expr: " << err << "\n";
        return;
    }

    // scan
    std::vector<int> rows;
    rows.reserve(256);

    int skip = offset;
    for (int r = 1; r <= lv; ++r) {
        if (!cr.eval || !cr.eval(r))
            continue;

        if (skip > 0) {
            --skip;
            continue;
        }
        rows.push_back(r);

        if (!limitAll && limit > 0 && (int)rows.size() >= limit)
            break;
    }

    // sorting
    struct SortKey {
        bool isIdx = false;
        int fldIdx = -1;
        bool desc = false;
        std::string nameUpper;
    };
    std::vector<SortKey> keys;

    if (!sortSpec.empty()) {
        auto parts = splitCsvLoose(sortSpec);
        for (auto& p : parts) {
            std::string s = trim(p);
            if (s.empty())
                continue;

            bool desc = false;
            if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
                desc = (s[0] == '-');
                s.erase(0, 1);
                s = trim(s);
            }

            // f:asc|desc
            std::string dir;
            auto colon = s.find(':');
            if (colon != std::string::npos) {
                dir = upper(trim(s.substr(colon + 1)));
                s = trim(s.substr(0, colon));
            }
            if (dir == "DESC")
                desc = true;
            else if (dir == "ASC")
                desc = false;

            std::string fup = upper(s);
            if (fup.empty())
                continue;

            SortKey k;
            k.desc = desc;
            k.nameUpper = fup;
            if (fup == "_IDX") {
                k.isIdx = true;
                k.fldIdx = -1;
                keys.push_back(std::move(k));
                continue;
            }
            int fIdx = findFieldIndexGlobalOrCurrent(fup);
            if (fIdx < 0) {
                std::cout << "SEL: SORT field not found: " << fup << " (ignored)\n";
                continue;
            }
            k.fldIdx = fIdx;
            keys.push_back(std::move(k));
        }
    }

    if (!keys.empty() && rows.size() >= 2) {
        struct SortVal {
            bool isNull = true;
            bool isNum = false;
            double num = 0.0;
            std::string str;
        };

        auto fetch = [&](int row, const SortKey& k) -> SortVal {
            SortVal v;
            if (k.isIdx) {
                v.isNull = false;
                v.isNum = true;
                v.num = (double)row;
                return v;
            }
            if (k.fldIdx < 0)
                return v;

            DbValue dv = DataAccessor::getValue(k.fldIdx, row);
            v.isNull = false;
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    v.isNum = false;
                    v.str = arg;
                    std::transform(v.str.begin(), v.str.end(), v.str.begin(),
                        [](unsigned char c) { return (char)std::tolower(c); });
                } else {
                    v.isNum = true;
                    v.num = (double)arg;
                }
            },
                dv);
            return v;
        };

        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
            for (const auto& k : keys) {
                SortVal va = fetch(a, k);
                SortVal vb = fetch(b, k);

                // NULL last
                if (va.isNull != vb.isNull)
                    return vb.isNull;

                if (va.isNum && vb.isNum) {
                    if (va.num == vb.num)
                        continue;
                    bool less = va.num < vb.num;
                    return k.desc ? !less : less;
                }

                if (!va.isNum && !vb.isNum) {
                    if (va.str == vb.str)
                        continue;
                    bool less = va.str < vb.str;
                    return k.desc ? !less : less;
                }

                // numeric before string
                if (va.isNum != vb.isNum) {
                    bool less = va.isNum; // num < str
                    return k.desc ? !less : less;
                }
            }
            return a < b;
        });
    }

    // columns: always show idx first; COLS defines additional fields.
    std::vector<int> colFields;
    std::vector<std::string> colNames;

    auto gatherAllFieldsForScope = [&]() -> std::vector<int> {
        std::vector<int> flds;
        if (ctx_.isGlobal) {
            int p0 = g_runtime_app.db_partition_prefix[ctx_.dbIdx];
            int p1 = g_runtime_app.db_partition_prefix[ctx_.dbIdx + 1];
            int prtMin = p0 + 1;
            int prtMax = p1;
            for (int f = 0; f < g_runtime_app.field_count; ++f) {
                if (g_runtime_app.field_record_1based[f] != 0)
                    continue;
                int prtNo = g_runtime_app.field_partition_1based[f];
                if (prtNo < prtMin || prtNo > prtMax)
                    continue;
                flds.push_back(f);
            }
        } else {
            int s0 = g_runtime_app.record_table_prefix[ctx_.recIdx];
            int s1 = g_runtime_app.record_table_prefix[ctx_.recIdx + 1];
            for (int t = s0; t < s1; ++t) {
                int fIdx = g_runtime_app.table_field_1based[t] - 1;
                flds.push_back(fIdx);
            }
        }
        return flds;
    };

    auto addField = [&](const std::string& fup) {
        if (fup.empty())
            return;
        if (fup == "_IDX")
            return; // idx already printed
        int fIdx = findFieldIndexGlobalOrCurrent(fup);
        if (fIdx < 0) {
            std::cout << "SEL: COLS field not found: " << fup << " (ignored)\n";
            return;
        }
        if (std::find(colFields.begin(), colFields.end(), fIdx) != colFields.end())
            return;
        colFields.push_back(fIdx);
        colNames.push_back(fup);
    };

    if (!colsSpec.empty()) {
        std::string upc = upper(trim(colsSpec));
        if (upc == "*") {
            colFields = gatherAllFieldsForScope();
            colNames.clear();
            colNames.reserve(colFields.size());
            for (int fIdx : colFields) {
                colNames.push_back(upper(trim(std::string(g_runtime_app.field_ids[fIdx]))));
            }
        } else {
            auto parts = splitCsvLoose(colsSpec);
            for (auto& p : parts) {
                addField(upper(trim(p)));
            }
        }
    } else {
        // default: identifiers + sort keys
        for (auto& id : cr.identifiers_upper) {
            addField(id);
        }
        for (auto& k : keys) {
            addField(k.nameUpper);
        }
        if (colFields.empty()) {
            // fallback: first 6 fields
            auto all = gatherAllFieldsForScope();
            for (size_t z = 0; z < all.size() && z < 6; ++z) {
                int fIdx = all[z];
                std::string fn = upper(trim(std::string(g_runtime_app.field_ids[fIdx])));
                addField(fn);
            }
        }
    }

    // output table
    const std::string scopeName = ctx_.isGlobal ? "ITEM" : ctx_.recName;
    if (!ctx_.isGlobal) {
        std::cout << "SEL/" << scopeName << "  LV=" << lv << "  MX=" << mx
                  << "  hits=" << rows.size();
    } else {
        std::cout << "SEL/" << scopeName << "  hits=" << rows.size();
    }
    if (!sortSpec.empty())
        std::cout << "  sort={" << sortSpec << "}";
    std::cout << "\n";

    if (rows.empty()) {
        // still update nav state
        qnav_.active = true;
        qnav_.dbIdx = ctx_.dbIdx;
        qnav_.recIdx = ctx_.recIdx;
        qnav_.isGlobal = ctx_.isGlobal;
        qnav_.rows.clear();
        qnav_.pos = -1;
        std::cout << "(no match)\n";
        return;
    }

    // header
    std::string header = "| idx |";
    std::string cmtrow = "| 注释 |";
    for (size_t ci = 0; ci < colFields.size(); ++ci) {
        header += " " + colNames[ci] + " |";
        std::string cmt = CommentLoader::instance().getComment(colNames[ci]);
        cmt = normalize_comment(cmt);
        cmtrow += " " + cmt + " |";
    }
    std::cout << header << "\n"
              << cmtrow << "\n";

    for (int r : rows) {
        std::string row = "| " + std::to_string(r) + " |";
        for (size_t ci = 0; ci < colFields.size(); ++ci) {
            auto v = DataAccessor::getValue(colFields[ci], r);
            row += " " + DataAccessor::formatValue(v) + " |";
        }
        std::cout << row << "\n";
    }

    // update nav state
    qnav_.active = true;
    qnav_.dbIdx = ctx_.dbIdx;
    qnav_.recIdx = ctx_.recIdx;
    qnav_.isGlobal = ctx_.isGlobal;
    qnav_.rows = rows;
    qnav_.pos = -1;

    // optional goto
    if (hasGoto) {
        if (gotoK >= 1 && gotoK <= (int)rows.size()) {
            qnav_.pos = gotoK - 1;
            ctx_.currentSlot = rows[qnav_.pos];
            std::cout << "Position is " << ctx_.recName << " (" << ctx_.currentSlot << ")\n";
            cmdShowCurrent("");
        } else {
            std::cout << "SEL: GOTO out of range: " << gotoK << " (hits=" << rows.size() << ")\n";
        }
    }
}

void DbIoShell::cmdFind(const std::string& argsRaw)
{
    std::string args = trim(argsRaw);
    if (args.empty()) {
        std::cout << "Usage: FIND <expr> [...]\n";
        return;
    }

    auto toks = QueryEngine::splitCommandTokens(args);
    bool hasLimit = false;
    bool hasGoto = false;
    for (auto& t : toks) {
        auto up = upper(t);
        if (up == "LIMIT")
            hasLimit = true;
        if (up == "GOTO")
            hasGoto = true;
    }

    if (!hasLimit)
        args += " LIMIT 1";
    if (!hasGoto)
        args += " GOTO FIRST";

    cmdSelect(args);
}

void DbIoShell::cmdNext(const std::string&)
{
    if (!qnav_.active || qnav_.dbIdx != ctx_.dbIdx || qnav_.recIdx != ctx_.recIdx || qnav_.isGlobal != ctx_.isGlobal) {
        std::cout << "NEXT: no active query result in current scope.\n";
        return;
    }
    if (qnav_.rows.empty()) {
        std::cout << "NEXT: empty result.\n";
        return;
    }

    if (qnav_.pos < 0)
        qnav_.pos = 0;
    else if (qnav_.pos + 1 < (int)qnav_.rows.size())
        ++qnav_.pos;

    ctx_.currentSlot = qnav_.rows[qnav_.pos];
    std::cout << "Position is " << ctx_.recName << " (" << ctx_.currentSlot << ")\n";
    cmdShowCurrent("");
}

void DbIoShell::cmdPrev(const std::string&)
{
    if (!qnav_.active || qnav_.dbIdx != ctx_.dbIdx || qnav_.recIdx != ctx_.recIdx || qnav_.isGlobal != ctx_.isGlobal) {
        std::cout << "PREV: no active query result in current scope.\n";
        return;
    }
    if (qnav_.rows.empty()) {
        std::cout << "PREV: empty result.\n";
        return;
    }

    if (qnav_.pos < 0)
        qnav_.pos = (int)qnav_.rows.size() - 1;
    else if (qnav_.pos - 1 >= 0)
        --qnav_.pos;

    ctx_.currentSlot = qnav_.rows[qnav_.pos];
    std::cout << "Position is " << ctx_.recName << " (" << ctx_.currentSlot << ")\n";
    cmdShowCurrent("");
}

void DbIoShell::renderGlobalsTable()
{
    const auto flds = collect_global_fields_in_db(ctx_.dbIdx);
    const int totalCols = static_cast<int>(flds.size());
    const int c0 = std::min(view_.col0, std::max(0, totalCols - kPageCols));
    const int c1 = std::min(totalCols, c0 + kPageCols);
    view_.col0 = c0;

    auto& st = TermStyle::instance();
    BoxTable tb;
    tb.banner = st.dim("show/") + st.record("item")
        + st.dim("  cols[") + st.idx(std::to_string(c0 + 1)) + st.dim(":") + st.idx(std::to_string(c1))
        + st.dim("]  (keys: ^ v < > li)");
    tb.rows = { 1 };

    for (int ci = c0; ci < c1; ++ci) {
        int fIdx = flds[ci];
        BoxTable::Col c;
        c.fieldIdx = fIdx;
        c.nameUpper = upper(trim(std::string(g_runtime_app.field_ids[fIdx])));
        c.comment = normalize_comment(CommentLoader::instance().getComment(c.nameUpper));
        char t = g_runtime_app.field_type[fIdx];
        c.isString = (t == 'S' || t == 'C');
        tb.cols.push_back(std::move(c));
    }
    tb.render();
}

void DbIoShell::renderRecordTable()
{
    const int rec = view_.record_index;
    int lv = getLVForRecordGlobalIndex(rec);
    if (lv <= 0)
        lv = g_runtime_app.record_max_dim[rec];
    const int mx = g_runtime_app.record_max_dim[rec];
    if (lv > mx)
        lv = mx;

    int r0 = view_.row0;
    if (r0 > lv)
        r0 = std::max(1, lv - kPageRows + 1);
    const int r1 = std::min(lv, r0 + kPageRows - 1);
    view_.row0 = r0;

    const int totalCols = static_cast<int>(view_.field_indices.size());
    const int c0 = std::min(view_.col0, std::max(0, totalCols - kPageCols));
    const int c1 = std::min(totalCols, c0 + kPageCols);
    view_.col0 = c0;

    const std::string recName = upper(trim(std::string(g_runtime_app.record_ids[rec])));
    auto& st = TermStyle::instance();

    BoxTable tb;
    tb.banner = st.dim("show/") + st.record(recName)
        + st.dim("  LV=") + st.idx(std::to_string(lv))
        + st.dim("  MX=") + st.idx(std::to_string(mx))
        + st.dim("  rows[") + st.idx(std::to_string(r0)) + st.dim(":") + st.idx(std::to_string(r1))
        + st.dim("]  cols[") + st.idx(std::to_string(c0 + 1)) + st.dim(":") + st.idx(std::to_string(c1))
        + st.dim("]  (keys: ^ v < > li)");
    tb.rows.reserve((size_t)std::max(0, r1 - r0 + 1));
    for (int r = r0; r <= r1; ++r)
        tb.rows.push_back(r);

    for (int ci = c0; ci < c1; ++ci) {
        int fIdx = view_.field_indices[ci];
        BoxTable::Col c;
        c.fieldIdx = fIdx;
        c.nameUpper = upper(trim(std::string(g_runtime_app.field_ids[fIdx])));
        c.comment = normalize_comment(CommentLoader::instance().getComment(c.nameUpper));
        char t = g_runtime_app.field_type[fIdx];
        c.isString = (t == 'S' || t == 'C');
        tb.cols.push_back(std::move(c));
    }
    tb.render();
}

} // namespace mmdb::dbio
