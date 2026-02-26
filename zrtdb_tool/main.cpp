// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "DbIoShell.hpp"
#include "TermStyle.hpp"
#include "MmdbManager.hpp"
#include "StringUtils.h"
#include "WatchdogClient.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

// 根据全局 record 索引（0-based）推断所属 DB（用于定位 DAT 中的定义位置）。
static std::string findDbForRecordIdx0(int ridx0)
{
    if (g_runtime_app.db_count <= 0)
        return {};
    for (int dbi = 0; dbi < g_runtime_app.db_count; ++dbi) {
        int r0 = g_runtime_app.db_record_prefix[(size_t)dbi];
        int r1 = g_runtime_app.db_record_prefix[(size_t)dbi + 1];
        if (ridx0 >= r0 && ridx0 < r1) {
            return g_runtime_app.db_ids[(size_t)dbi];
        }
    }
    // fallback: unknown
    return {};
}

// 将包含 LV$ 字段的分区全部映射（只读即可），确保 record_lv_addrs 可读。
static void mapAllLvPartitionsReadOnly()
{
    auto& mgr = mmdb::MmdbManager::instance();
    std::vector<int> prtList;
    prtList.reserve((size_t)g_runtime_app.record_count);
    for (int r = 0; r < g_runtime_app.record_count; ++r) {
        int prt1 = g_runtime_app.record_lv_partition_1based[(size_t)r];
        if (prt1 <= 0)
            continue;
        prtList.push_back(prt1 - 1);
    }
    std::sort(prtList.begin(), prtList.end());
    prtList.erase(std::unique(prtList.begin(), prtList.end()), prtList.end());

    for (int prtIdx0 : prtList) {
        if (prtIdx0 < 0 || prtIdx0 >= g_runtime_app.partition_count)
            continue;
        std::string part = g_runtime_app.partition_ids[(size_t)prtIdx0];
        int dbIdx0 = g_runtime_app.partition_db_1based[(size_t)prtIdx0] - 1;
        std::string db = (dbIdx0 >= 0 && dbIdx0 < g_runtime_app.db_count) ? g_runtime_app.db_ids[(size_t)dbIdx0] : part;
        std::string spec = part + "/" + db;
        char* dummy = nullptr;
        // 工具本身具备“修改”能力，因此这里不要用只读映射。
        // 否则后续对同一分区内的字段赋值会触发 SIGSEGV（写只读页）。
        mgr.mapPartition(spec.c_str(), &dummy, false);
    }
}

static void printCapacityWarnings(const std::string& appUpper)
{
    // 先映射 LV 所在分区，保证 record_lv_addrs 已填充。
    mapAllLvPartitionsReadOnly();

    struct Item {
        std::string rec;
        std::string db;
        int lv;
        int mx;
        int cap;
    };
    std::vector<Item> warn;

    for (int r = 0; r < g_runtime_app.record_count; ++r) {
        int mx = g_runtime_app.record_max_dim[(size_t)r];
        int cap = mx;
        if (!g_runtime_app.record_physical_dim.empty() && (size_t)r < g_runtime_app.record_physical_dim.size() && g_runtime_app.record_physical_dim[(size_t)r] > 0) {
            cap = g_runtime_app.record_physical_dim[(size_t)r];
        }
        if (mx <= 0)
            continue;
        std::uintptr_t p = g_runtime_app.record_lv_addrs[(size_t)r];
        if (p == 0)
            continue;
        int lv = *(reinterpret_cast<volatile int*>(p));
        if (lv >= mx) {
            warn.push_back({ g_runtime_app.record_ids[(size_t)r], findDbForRecordIdx0(r), lv, mx, cap });
        }
    }

    if (warn.empty())
        return;

    std::sort(warn.begin(), warn.end(), [](const Item& a, const Item& b) {
        // 优先展示“逼近/超过 CAP(physical)”的记录：按 LV/CAP 降序排列。
        const long long aCap = (a.cap > 0) ? a.cap : 1;
        const long long bCap = (b.cap > 0) ? b.cap : 1;
        const long long lhs = (long long)a.lv * bCap;
        const long long rhs = (long long)b.lv * aCap;
        if (lhs != rhs)
            return lhs > rhs;
        // 次级：按 LV/MX
        const long long aMx = (a.mx > 0) ? a.mx : 1;
        const long long bMx = (b.mx > 0) ? b.mx : 1;
        const long long lhs2 = (long long)a.lv * bMx;
        const long long rhs2 = (long long)b.lv * aMx;
        if (lhs2 != rhs2)
            return lhs2 > rhs2;
        // 再次：按 LV
        if (a.lv != b.lv)
            return a.lv > b.lv;
        return a.rec < b.rec;
    });

    auto& st = mmdb::dbio::TermStyle::instance();
    const bool useColor = st.enabled();
    if (useColor) std::cerr << "\x1b[1;31m"; // BEGIN_CRIT_COLOR

    std::cerr << "\n==================== 严重告警 / CRITICAL WARNING ====================\n";
    std::cerr << "APP: " << appUpper << "\n";

    std::cerr << "检测到至少一个记录的 LV >= MX（逻辑上限/建模契约上限）。这属于【建模契约被破坏】的高危状态。\n";
    std::cerr << "可能后果（非常严重）：\n";
    std::cerr << "  1) 越界写导致共享内存数据破坏（覆盖相邻字段/相邻记录/相邻分区），出现“记录串扰”、不可解释的随机故障；\n";
    std::cerr << "  2) Snapshot/下装一致性失效，回滚与复现不可可信；\n";
    std::cerr << "  3) 业务逻辑可能在错误数据上作出控制/仿真决策，风险远大于进程崩溃；\n";
    std::cerr << "  4) 若 LV 超过 CAP(physical)=3*MX，zrtdb_watchdog 将执行强制隔离：杀掉映射该 APP 的相关进程。\n";
    std::cerr << "\n";
    std::cerr << "This tool detected LV >= MX (modeled logical capacity). This is a CRITICAL contract violation.\n";
    std::cerr << "Severe consequences may include: memory corruption (out-of-bounds writes), cross-record contamination, invalid snapshots,\n";
    std::cerr << "wrong control/simulation decisions, and enforced termination by zrtdb_watchdog when LV exceeds CAP(physical)=3*MX.\n\n";
    std::cerr << "建议 / Recommendation:\n";
    std::cerr << "- 立即扩容：在 DAT 中找到对应 RECORD/STRUCT 的 DIM(MX) 定义，将其扩容到 >= 2 * LV（建议留更多裕度）。\n";
    std::cerr << "- 然后重新实例化（zrtdb_model）并受控切换/重启相关进程，确保所有进程重新映射新实例。\n";
    std::cerr << "- Enlarge DIM(MX) to >= 2 * LV (leave sufficient headroom), then re-instantiate (zrtdb_model) and restart/remap processes.\n\n";
    std::cerr << "当前异常记录列表（全部列出，共 " << warn.size() << " 项） / Abnormal records (ALL, total " << warn.size() << "):\n";
    std::cerr << "  REC_NAME\tDB\tLV\tMX\tSUGGEST_MX(>=2*LV)\n";
    for (const auto& it : warn) {
        const long long suggestMx = 2LL * (long long)it.lv;
        std::cerr << "  " << it.rec
                  << "\t" << (it.db.empty() ? "-" : it.db)
                  << "\t" << it.lv
                  << "\t" << it.mx
                  //<< "\t" << it.cap
                  << "\t\t" << suggestMx
                  << "\n";
    }
    std::cerr << "======================================================================\n\n";

    if (useColor) std::cerr << "\x1b[0m"; // END_CRIT_COLOR

    // 同步写一条 audit 事件给 watchdog 便于运维留痕。
    mmdb::watchdog::sendAudit("tool", "CapacityWarning", std::string("{\"app\":\"") + appUpper + "\",\"count\":" + std::to_string((int)warn.size()) + "}", 1, 0);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: zrtdb_tool <APP> [DB]\n";
        return 1;
    }

    std::string app = argv[1];
    std::string db = (argc >= 3) ? argv[2] : "";

    if (!mmdb::MmdbManager::instance().setContext(app)) {
        std::cerr << "MMDB context set failed for APP=" << app << "\n";
        return 2;
    }

    const std::string appUpper = mmdb::utils::toUpper(app);
    // 自动拉起 watchdog 并登记工具进程（best-effort，不影响工具功能）。
    mmdb::watchdog::sendAttach("tool");

    // Intro (CN/EN) + Copyright
    std::cout << "ZRTDB 运维交互工具：用于在线巡检并读写 ZRTDB 运行期共享内存字段，支持翻页与快照。\n";
    std::cout << "ZRTDB maintenance tool: interactively inspect/modify runtime shared-memory fields, with paging and snapshots.\n";
    std::cout << "Copyright (c) 2026 邹德虎 (Zou Dehu). SPDX-License-Identifier: Apache-2.0.\n\n";

    // default start DB: first declared in app runtime
    if (db.empty() && g_runtime_app.db_count > 0) {
        db = g_runtime_app.db_ids[0];
    }

    // tool 打开第一屏强制做容量告警（LV>=MX），并给出 DAT 扩容建议。
    printCapacityWarnings(appUpper);

    try {
        mmdb::dbio::DbIoShell shell(appUpper, mmdb::utils::toUpper(db));
        shell.run();
    } catch (const std::exception& e) {
        std::cerr << "Runtime error: " << e.what() << "\n";
        return 3;
    }

    mmdb::watchdog::sendDetach("tool");

    return 0;
}
