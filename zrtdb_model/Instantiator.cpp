// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "Instantiator.h"

#include "DefIO.hpp"
#include "MetaIO.hpp"
#include "StringUtils.h"
#include "zrtdb_fingerprint.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;

namespace zrtdb::init {

static std::string normalize_id(std::string s)
{
    // remove embedded NUL (legacy safety)
    auto z = s.find('\0');
    if (z != std::string::npos)
        s.erase(z);
    return mmdb::utils::trim(s);
}

Instantiator::Instantiator() { }

fs::path Instantiator::home() const
{
    // Static root (DAT/meta/header). Legacy env MMDB_HOME is still accepted.
    const char* env = std::getenv("MMDB_HOME");
    if (env && *env)
        return fs::path(env);

    env = std::getenv("MMDB_STATIC_ROOT");
    if (env && *env)
        return fs::path(env);

    return fs::path("/usr/local/ZRTDB");
}

fs::path Instantiator::runtimeBaseRoot() const
{
    const char* env = std::getenv("MMDB_RUNTIME_ROOT");
    if (env && *env)
        return fs::path(env);
    return fs::path("/var/ZRTDB");
}

fs::path Instantiator::runtimeRoot() const
{
    // Ensure we never create shared top-level zrtdb_data/dbbin for multiple apps.
    // Each app owns its own folder: <runtimeBaseRoot>/<APP>.
    if (appUpper_.empty()) {
        return runtimeBaseRoot();
    }
    return runtimeBaseRoot() / appUpper_;
}

fs::path Instantiator::defPath() const { return home() / "header" / ".datdef"; }
fs::path Instantiator::metaAppPath() const { return runtimeRoot() / "meta" / "apps"; }
fs::path Instantiator::secsPath() const { return runtimeRoot() / "zrtdb_data"; }

bool Instantiator::loadAppDef(const std::string& appUpper, app_strc_dat& out)
{
    auto file = defPath() / (appUpper + ".APPDEF");
    if (!zrtdb::def::loadAppDef(file, out)) {
        std::cerr << "Failed to load APPDEF: " << file << std::endl;
        return false;
    }
    return true;
}

bool Instantiator::loadDbDef(const std::string& dbUpper, db_strc_dat& out)
{
    auto file = defPath() / (dbUpper + ".DBDEF");
    if (!zrtdb::def::loadDbDef(file, out)) {
        std::cerr << "Failed to load DBDEF: " << file << std::endl;
        return false;
    }
    return true;
}

bool Instantiator::mergeDbDefs(const app_strc_dat& appDef, StaticModelConfig& outClone,
    std::vector<std::pair<std::string, db_strc_dat>>& loaded)
{
    outClone.clear();

    // app id
    outClone.app_id = mmdb::utils::toUpper(normalize_id(appDef.app_id));
    outClone.db_count = appDef.db_count;

    const int ndb = appDef.db_count;
    outClone.db_ids.assign((size_t)ndb, std::string {});
    outClone.db_partition_prefix.assign((size_t)ndb + 1, 0);
    outClone.db_record_prefix.assign((size_t)ndb + 1, 0);

    // partition_field_prefix is prefix array: size = (#partitions + 1). We build incrementally.
    outClone.partition_field_prefix.clear();
    outClone.partition_field_prefix.push_back(0);

    int curPrt = 0;
    int curRec = 0;
    int curFld = 0;

    for (int dbi = 0; dbi < ndb; ++dbi) {
        std::string db = mmdb::utils::toUpper(normalize_id(appDef.db_ids[(size_t)dbi]));
        outClone.db_ids[(size_t)dbi] = db;

        // record/partition prefix start for this DB
        outClone.db_partition_prefix[(size_t)dbi] = curPrt;
        outClone.db_record_prefix[(size_t)dbi] = curRec;

        if (db.empty()) {
            outClone.db_partition_prefix[(size_t)dbi + 1] = curPrt;
            outClone.db_record_prefix[(size_t)dbi + 1] = curRec;
            continue;
        }

        db_strc_dat dbDef {};
        if (!loadDbDef(db, dbDef))
            return false;
        loaded.emplace_back(db, dbDef);

        // Records
        for (int r = 0; r < dbDef.record_count; ++r) {
            outClone.record_ids.push_back(mmdb::utils::toUpper(normalize_id(dbDef.record_ids[(size_t)r])));
            outClone.record_max_dim.push_back(dbDef.record_max_dim[(size_t)r]);
            // 物理容量缓冲（DBDEF v2 兼容：若未提供则默认=record_max_dim）
            if ((size_t)r < dbDef.record_physical_dim.size() && dbDef.record_physical_dim[(size_t)r] > 0) {
                outClone.record_physical_dim.push_back(dbDef.record_physical_dim[(size_t)r]);
            } else {
                outClone.record_physical_dim.push_back(dbDef.record_max_dim[(size_t)r]);
            }
            outClone.record_first_addr.push_back(dbDef.record_first_addr[(size_t)r]);
            ++curRec;
        }

        // Partitions + fields
        for (int p = 0; p < dbDef.partition_count; ++p) {
            outClone.partition_ids.push_back(mmdb::utils::toUpper(normalize_id(dbDef.partition_ids[(size_t)p])));
            outClone.partition_size_bytes.push_back(dbDef.partition_size_bytes[(size_t)p]);

            int f0 = dbDef.partition_field_prefix[p];
            int f1 = dbDef.partition_field_prefix[p + 1];
            for (int f = f0; f < f1; ++f) {
                outClone.field_ids.push_back(mmdb::utils::trim(normalize_id(dbDef.field_ids[(size_t)f])));
                outClone.field_type.push_back(dbDef.field_type[(size_t)f]);
                outClone.field_bytes.push_back(dbDef.field_bytes[(size_t)f]);
                outClone.field_valid.push_back(dbDef.field_valid[(size_t)f]);

                // field_record_1based needs record index shift (global record numbering is 1-based)
                if (dbDef.field_record_1based[(size_t)f] > 0) {
                    outClone.field_record_1based.push_back(dbDef.field_record_1based[(size_t)f] + outClone.db_record_prefix[(size_t)dbi]);
                } else {
                    outClone.field_record_1based.push_back(0);
                }
                ++curFld;
            }

            // next partition's field start
            outClone.partition_field_prefix.push_back(curFld);
            ++curPrt;
        }

        // Next db start indices
        outClone.db_partition_prefix[(size_t)dbi + 1] = curPrt;
        outClone.db_record_prefix[(size_t)dbi + 1] = curRec;
    }

    // masks are currently unused in the C++20 refactor; keep a correctly-sized placeholder.
    outClone.field_mask_index.assign((size_t)curFld + 1, 0);

    zrtdb::fingerprint::Fnv64 fh;
    fh.addString(outClone.app_id);
    for (const auto& s : outClone.db_ids) fh.addString(s);
    for (const auto& s : outClone.record_ids) fh.addString(s);
    for (int v : outClone.record_max_dim) fh.addPodLE<std::int32_t>(v);
    for (int v : outClone.record_physical_dim) fh.addPodLE<std::int32_t>(v);
    for (const auto& s : outClone.partition_ids) fh.addString(s);
    for (int v : outClone.partition_size_bytes) fh.addPodLE<std::int32_t>(v);
    for (const auto& s : outClone.field_ids) fh.addString(s);
    for (char c : outClone.field_type) fh.addByte((std::uint8_t)c);
    outClone.layout_fingerprint = fh.hex();

    return true;
}

bool Instantiator::ensurePhysicalFiles(const std::vector<std::pair<std::string, db_strc_dat>>& loaded)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        page = 4096;

    zrtdb::fingerprint::Fnv64 appHash;
    appHash.addString("APP");
    appHash.addString(appUpper_);
    for (const auto& [dbUpper0, dbDef0] : loaded) {
        appHash.addString(dbUpper0);
        appHash.addString(dbDef0.layout_fingerprint);
    }
    const std::string appFp = appHash.hex();

    for (const auto& [dbUpper, dbDef] : loaded) {
        auto dir = secsPath() / dbUpper;
        try {
            fs::create_directories(dir);
        } catch (...) {
        }

        for (int p = 0; p < dbDef.partition_count; ++p) {
            std::string prt = mmdb::utils::toUpper(normalize_id(dbDef.partition_ids[(size_t)p]));
            if (prt.empty())
                continue;

            long raw = dbDef.partition_size_bytes[(size_t)p];
            long pages = (raw + page - 1) / page;
            long fsz = pages * page;

            auto secFile = dir / (prt + ".sec");
            bool needCreate = true;
            if (fs::exists(secFile)) {
                try {
                    auto sz = fs::file_size(secFile);
                    if ((long)sz >= fsz)
                        needCreate = false;
                } catch (...) {
                }
            }

            if (needCreate) {
                std::ofstream ofs(secFile, std::ios::binary | std::ios::trunc);
                if (!ofs) {
                    std::cerr << "Failed to create sec: " << secFile << std::endl;
                    return false;
                }
                ofs.seekp(fsz - 1);
                ofs.write("", 1);
            }
            {
                auto man = secFile;
                man += ".manifest";
                std::ofstream mo(man, std::ios::binary | std::ios::trunc);
                if (mo) {
                    mo << "{\"db\":\"" << dbUpper << "\",\"partition\":\"" << prt << "\",\"bytes\":" << raw
                       << ",\"layout_fingerprint\":\"" << appFp << "\"}";
                }
            }
        }
    }
    return true;
}

void Instantiator::buildRuntime(const app_strc_dat& appDef, const StaticModelConfig& clone, RuntimeAppConfig& out)
{
    out.clear();

    // ----------------------
    // DB list (from APPDEF)
    // ----------------------
    out.db_count = appDef.db_count;
    out.db_ids.reserve((size_t)out.db_count);
    for (int i = 0; i < appDef.db_count; ++i) {
        std::string db = mmdb::utils::toUpper(normalize_id(appDef.db_ids[(size_t)i]));
        out.db_ids.push_back(db);
    }

    // ---------------------------------
    // Records (global list is clone order)
    // ---------------------------------
    out.record_count = (int)clone.record_ids.size();
    out.record_ids.reserve((size_t)out.record_count);
    out.record_max_dim.reserve((size_t)out.record_count);
    out.record_physical_dim.reserve((size_t)out.record_count);
    out.record_lv_addrs.assign((size_t)out.record_count, 0);
    out.record_lv_partition_1based.assign((size_t)out.record_count, 0);
    out.record_lv_offset_bytes.assign((size_t)out.record_count, 0);
    out.record_size_bytes.assign((size_t)out.record_count, 0);

    for (int r = 0; r < out.record_count; ++r) {
        out.record_ids.push_back(mmdb::utils::toUpper(normalize_id(clone.record_ids[(size_t)r])));
        out.record_max_dim.push_back(clone.record_max_dim[(size_t)r]);
        // 物理容量缓冲（clone meta v2 兼容：若未提供则默认=record_max_dim）
        if ((size_t)r < clone.record_physical_dim.size() && clone.record_physical_dim[(size_t)r] > 0) {
            out.record_physical_dim.push_back(clone.record_physical_dim[(size_t)r]);
        } else {
            out.record_physical_dim.push_back(clone.record_max_dim[(size_t)r]);
        }
    }

    // Record name -> global record no (1-based)
    std::unordered_map<std::string, int> recByName;
    recByName.reserve((size_t)out.record_count);
    for (int r = 0; r < out.record_count; ++r) {
        recByName[out.record_ids[(size_t)r]] = r + 1;
    }

    // -----------------------------
    // DB->partition/record prefixes
    // -----------------------------
    const int ndb = clone.db_count;
    out.db_partition_prefix.assign((size_t)ndb + 1, 0);
    out.db_record_prefix.assign((size_t)ndb + 1, 0);

    // Partition list prefix array (filled incrementally)
    out.partition_field_prefix.clear();
    // We'll push one entry per emitted partition and append the final end after all partitions.

    // ----------------------
    // Partitions + fields
    // ----------------------
    out.partition_count = 0;
    out.field_count = 0;

    for (int dbi = 0; dbi < ndb; ++dbi) {
        out.db_partition_prefix[(size_t)dbi] = (short)out.partition_count;
        out.db_record_prefix[(size_t)dbi] = (short)clone.db_record_prefix[(size_t)dbi];

        int prtStart = clone.db_partition_prefix[(size_t)dbi];
        int prtEnd = clone.db_partition_prefix[(size_t)dbi + 1];

        for (int p = prtStart; p < prtEnd; ++p) {
            std::string prtName = mmdb::utils::toUpper(normalize_id(clone.partition_ids[(size_t)p]));
            if (prtName.empty())
                continue;

            const int prtIdx0 = out.partition_count;
            out.partition_ids.push_back(prtName);
            out.partition_bytes.push_back(clone.partition_size_bytes[(size_t)p]);
            out.partition_db_1based.push_back((short)(dbi + 1));
            out.partition_field_prefix.push_back((short)out.field_count);

            long off = 0;
            int f0 = clone.partition_field_prefix[(size_t)p];
            int f1 = clone.partition_field_prefix[(size_t)p + 1];
            for (int f = f0; f < f1; ++f) {
                if (clone.field_valid[(size_t)f] == 0)
                    continue;

                std::string fn = mmdb::utils::trim(normalize_id(clone.field_ids[(size_t)f]));
                if (fn.empty())
                    continue;

                int recNo = clone.field_record_1based[(size_t)f];
                int bytes = clone.field_bytes[(size_t)f];

                // field emit
                out.field_ids.push_back(fn);
                out.field_partition_1based.push_back((short)(prtIdx0 + 1));
                out.field_record_1based.push_back((short)recNo);
                out.field_offset_bytes.push_back(off);
                out.field_type.push_back(clone.field_type[(size_t)f]);
                out.field_item_bytes.push_back((std::uint8_t)bytes);

                // item size for offset accumulation
                long itemSize = bytes;
                if (recNo > 0) {
                    int recIdx0 = recNo - 1;
                    // 注意：字段布局按“物理容量（CAP）”计算，以便在 MX 之后预留缓冲区。
                    int dim = (recIdx0 >= 0 && recIdx0 < out.record_count) ? out.record_physical_dim[(size_t)recIdx0] : 0;
                    itemSize = (long)bytes * (long)dim;
                }

                // LV$<REC> mapping: global field that points to record live counter
                if (recNo == 0) {
                    std::string up = mmdb::utils::toUpper(fn);
                    if (up.rfind("LV$", 0) == 0) {
                        std::string recName = up.substr(3);
                        auto it = recByName.find(recName);
                        if (it != recByName.end()) {
                            int ridx1 = it->second; // 1-based record number
                            out.record_lv_partition_1based[(size_t)ridx1 - 1] = (short)(prtIdx0 + 1);
                            out.record_lv_offset_bytes[(size_t)ridx1 - 1] = off;
                        }
                    }
                }

                off += itemSize;
                ++out.field_count;
            }

            ++out.partition_count;
        }

        out.db_partition_prefix[(size_t)dbi + 1] = (short)out.partition_count;
        out.db_record_prefix[(size_t)dbi + 1] = (short)clone.db_record_prefix[(size_t)dbi + 1];
    }

    // finalize partition->field prefix end marker
    out.partition_field_prefix.push_back((short)out.field_count);

    // Partition base addresses initialized to zero; filled by MmdbManager::mapPartition()
    out.partition_base_addrs.assign((size_t)out.partition_count, 0);

    // ----------------------
    // Per-record field tables
    // ----------------------
    out.record_table_prefix.assign((size_t)out.record_count + 1, 0);

    std::vector<int> cnt((size_t)out.record_count + 1, 0);
    for (int f = 0; f < out.field_count; ++f) {
        int r = out.field_record_1based[(size_t)f];
        if (r > 0 && r <= out.record_count)
            cnt[(size_t)r]++;
    }

    int total = 0;
    out.record_table_prefix[0] = 0;
    for (int r = 1; r <= out.record_count; ++r) {
        total += cnt[(size_t)r];
        out.record_table_prefix[(size_t)r] = (short)total;
    }

    out.table_field_1based.assign((size_t)total, 0);
    std::vector<int> cursor((size_t)out.record_count + 1, 0);
    for (int r = 1; r <= out.record_count; ++r) {
        cursor[(size_t)r] = out.record_table_prefix[(size_t)r - 1];
    }

    for (int f = 0; f < out.field_count; ++f) {
        int r = out.field_record_1based[(size_t)f];
        if (r <= 0 || r > out.record_count)
            continue;
        int& pos = cursor[(size_t)r];
        out.table_field_1based[(size_t)pos] = (short)(f + 1); // 1-based field index
        ++pos;
    }

    out.table_count = total;
    out.layout_fingerprint = clone.layout_fingerprint;

    // Approx record size (sum bytes of record fields; legacy semantics ignores dim)
    for (int r = 1; r <= out.record_count; ++r) {
        int rs = 0;
        int b = out.record_table_prefix[(size_t)r - 1];
        int e = out.record_table_prefix[(size_t)r];
        for (int i = b; i < e; ++i) {
            int fIdx0 = out.table_field_1based[(size_t)i] - 1;
            if (fIdx0 >= 0 && fIdx0 < out.field_count)
                rs += (int)out.field_item_bytes[(size_t)fIdx0];
        }
        out.record_size_bytes[(size_t)r - 1] = (short)rs;
    }
}

bool Instantiator::saveMeta(const std::string& appUpper, const StaticModelConfig& clone, const RuntimeAppConfig& runtime)
{
    auto dir = metaAppPath();
    try {
        fs::create_directories(dir);
    } catch (...) {
    }

    auto clonFile = dir / (appUpper + ".sec");
    auto appFile = dir / (appUpper + "_NEW.sec");

    if (!mmdb::meta::saveClone(clonFile, clone))
        return false;
    if (!mmdb::meta::saveRuntime(appFile, runtime))
        return false;

    return true;
}

bool Instantiator::instantiate(const std::string& appUpper)
{
    // Bind current app context so runtimeRoot()/... paths are created under
    // /var/ZRTDB/<APP>/... rather than shared global directories.
    appUpper_ = appUpper;

    app_strc_dat appDef {};
    if (!loadAppDef(appUpper, appDef))
        return false;

    StaticModelConfig clone {};
    std::vector<std::pair<std::string, db_strc_dat>> loaded;
    if (!mergeDbDefs(appDef, clone, loaded))
        return false;
    if (!appDef.layout_fingerprint.empty())
        clone.layout_fingerprint = appDef.layout_fingerprint;

    if (!ensurePhysicalFiles(loaded))
        return false;

    RuntimeAppConfig runtime {};
    buildRuntime(appDef, clone, runtime);

    return saveMeta(appUpper, clone, runtime);
}

} // namespace zrtdb::init
