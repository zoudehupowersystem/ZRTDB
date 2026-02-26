// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "DbCompiler.h"

#include "DefIO.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

namespace zrtdb::init {

static std::string read_all_text(const fs::path& p)
{
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs)
        return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// Strip C/C++ style comments from src. Newlines are preserved.
// This is used for DAT parsing so that "//" and "/* */" behave as true comments.
static std::string strip_cpp_comments(std::string_view src)
{
    std::string out;
    out.reserve(src.size());

    bool in_line = false;
    bool in_block = false;
    bool in_str = false;
    char quote = 0;

    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        char n = (i + 1 < src.size()) ? src[i + 1] : '\0';

        if (in_line) {
            if (c == '\n') {
                in_line = false;
                out.push_back('\n');
            }
            continue;
        }
        if (in_block) {
            if (c == '*' && n == '/') {
                in_block = false;
                ++i;
                continue;
            }
            if (c == '\n')
                out.push_back('\n');
            continue;
        }

        if (in_str) {
            out.push_back(c);
            if (c == '\\' && i + 1 < src.size()) {
                out.push_back(src[++i]);
                continue;
            }
            if (c == quote) {
                in_str = false;
                quote = 0;
            }
            continue;
        }

        if (c == '"' || c == '\'') {
            in_str = true;
            quote = c;
            out.push_back(c);
            continue;
        }

        if (c == '/' && n == '/') {
            in_line = true;
            ++i;
            continue;
        }
        if (c == '/' && n == '*') {
            in_block = true;
            ++i;
            continue;
        }

        out.push_back(c);
    }

    return out;
}

static std::unordered_map<std::string, std::string> parse_kv_tail(const std::string& tail)
{
    std::unordered_map<std::string, std::string> kv;
    std::istringstream iss(tail);
    std::string tok;
    while (iss >> tok) {
        auto p = tok.find(':');
        if (p == std::string::npos)
            continue;
        kv[tok.substr(0, p)] = tok.substr(p + 1);
    }
    return kv;
}

// 记录字段“物理容量缓冲”倍率：
// - 逻辑上限（MX）仍由 DAT DIM 决定；
// - 物理容量（CAP）= MX * factor，用于降低越界写导致的相邻字段破坏概率；
// - 默认 factor=3；设置为 1 表示关闭缓冲。
static int getRecordPhysicalFactor()
{
    const char* env = std::getenv("ZRTDB_ALLOC_FACTOR");
    if (!env || !*env)
        return 3;
    int f = std::atoi(env);
    if (f < 1)
        f = 1;
    if (f > 16)
        f = 16;
    return f;
}

// map new type string to legacy (type_code, bytes)
static bool map_type(const std::string& tIn, char& code, int& bytes)
{
    std::string t = tIn;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });

    // compatibility: allow old style like I*4, R*4, C*120, K*8
    if (t.size() >= 3 && (t[1] == '*')) {
        code = std::toupper(static_cast<unsigned char>(t[0]));
        bytes = std::stoi(t.substr(2));
        if (code == 'C') {
            // treat as string-like char array
            return true;
        }
        if (code == 'I' || code == 'R' || code == 'K')
            return true;
        if (code == 'D')
            return true;
    }

    auto parse_string_len = [&](int& outLen) {
        outLen = 120;
        auto star = t.find('*');
        auto lp = t.find('(');

        if (star != std::string::npos) {
            outLen = std::max(1, std::stoi(t.substr(star + 1)));
        } else if (lp != std::string::npos) {
            auto rp = t.find(')', lp + 1);
            if (rp != std::string::npos)
                outLen = std::max(1, std::stoi(t.substr(lp + 1, rp - lp - 1)));
        } else {
            // 支持 string64 / string120
            size_t i = 6; // strlen("string")
            if (t.size() > i) {
                bool allDigit = true;
                for (size_t k = i; k < t.size(); ++k)
                    allDigit &= std::isdigit((unsigned char)t[k]);
                if (allDigit)
                    outLen = std::max(1, std::stoi(t.substr(i)));
            }
        }
        if (outLen > 120)
            outLen = 120;
    };

    if (t == "int" || t == "i32" || t == "int32") {
        code = 'I';
        bytes = 4;
        return true;
    }
    if (t == "long" || t == "i64" || t == "int64") {
        code = 'K';
        bytes = 8;
        return true;
    }
    if (t == "float" || t == "f32") {
        code = 'R';
        bytes = 4;
        return true;
    }
    if (t == "double" || t == "f64") {
        code = 'D';
        bytes = 8;
        return true;
    }
    if (t.rfind("string", 0) == 0) {
        code = 'S';
        parse_string_len(bytes);
        return true;
    }
    return false;
}

DbCompiler::DbCompiler() { }

fs::path DbCompiler::home() const
{
    if (const char* env = std::getenv("MMDB_STATIC_ROOT"); env && *env)
        return fs::path(env);
    if (const char* env = std::getenv("MMDB_HOME"); env && *env)
        return fs::path(env); // legacy
    return fs::path("/usr/local/ZRTDB");
}

fs::path DbCompiler::datPath() const { return home() / "DAT"; }
fs::path DbCompiler::defPath() const { return home() / "header" / ".datdef"; }
fs::path DbCompiler::incPath() const { return home() / "header" / "inc"; }

std::string DbCompiler::trim(const std::string& s)
{
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string DbCompiler::upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

bool DbCompiler::starts_with(const std::string& s, const char* pfx)
{
    size_t n = std::strlen(pfx);
    return s.size() >= n && std::equal(pfx, pfx + n, s.begin());
}

std::string DbCompiler::extract_after(const std::string& line, const std::string& tag)
{
    auto pos = line.find(tag);
    if (pos == std::string::npos)
        return "";
    return trim(line.substr(pos + tag.size()));
}

bool DbCompiler::compileDbFile(const fs::path& dbDatFile)
{
    db_strc_dat db {};
    if (!parseDbDatNew(dbDatFile, db)) {
        return false;
    }

    std::string dbUpper = upper(db.db_id);
    fs::create_directories(defPath());

    auto defFile = defPath() / (dbUpper + ".DBDEF");
    if (!zrtdb::def::saveDbDef(defFile, db)) {
        std::cerr << "Failed to write DBDEF: " << defFile << std::endl;
        return false;
    }

    // Cache for APP header generation.
    db_cache_[dbUpper] = db;

    std::cout << "[DB] " << dbUpper << " -> " << defFile << std::endl;
    return true;
}

bool DbCompiler::getDbDefCachedOrLoad(const std::string& dbUpper, db_strc_dat& out)
{
    auto it = db_cache_.find(dbUpper);
    if (it != db_cache_.end()) {
        out = it->second;
        return true;
    }

    auto file = defPath() / (dbUpper + ".DBDEF");
    if (!zrtdb::def::loadDbDef(file, out)) {
        std::cerr << "Cannot load DBDEF: " << file << std::endl;
        return false;
    }
    db_cache_[dbUpper] = out;
    return true;
}

bool DbCompiler::compileAppFile(const fs::path& appDatFile, std::string& outAppNameUpper)
{
    app_strc_dat app {};
    if (!parseAppDatNew(appDatFile, app))
        return false;

    outAppNameUpper = upper(app.app_id);

    fs::create_directories(defPath());
    auto defFile = defPath() / (outAppNameUpper + ".APPDEF");

    if (!zrtdb::def::saveAppDef(defFile, app)) {
        std::cerr << "Failed to write APPDEF: " << defFile << std::endl;
        return false;
    }

    // Generate merged APP header.
    try {
        generateAppHeader(outAppNameUpper, app);
    } catch (const std::exception& e) {
        std::cerr << "Header generation failed for APP " << outAppNameUpper << ": " << e.what() << std::endl;
        return false;
    }

    std::cout << "[APP] " << outAppNameUpper << " -> " << defFile << std::endl;
    return true;
}

bool DbCompiler::compileAppConfig(const fs::path& appConfigFile, std::vector<std::string>& outAppNamesUpper)
{
    outAppNamesUpper.clear();

    const auto filenameUpper = upper(appConfigFile.filename().string());
    const auto extUpper = upper(appConfigFile.extension().string());

    if (filenameUpper == "APPDAT.JSON" && extUpper == ".JSON") {
        std::vector<app_strc_dat> apps;
        if (!parseAppDatJson(appConfigFile, apps))
            return false;

        fs::create_directories(defPath());

        for (auto& app : apps) {
            const std::string appUpper = upper(app.app_id);
            auto defFile = defPath() / (appUpper + ".APPDEF");
            if (!zrtdb::def::saveAppDef(defFile, app)) {
                std::cerr << "Failed to write APPDEF: " << defFile << std::endl;
                return false;
            }
            try {
                generateAppHeader(appUpper, app);
            } catch (const std::exception& e) {
                std::cerr << "Header generation failed for APP " << appUpper << ": " << e.what() << std::endl;
                return false;
            }
            std::cout << "[APP] " << appUpper << " -> " << defFile << std::endl;
            outAppNamesUpper.push_back(appUpper);
        }

        return true;
    }

    // Legacy: single .APPDAT file
    if (extUpper == ".APPDAT") {
        std::string appUpper;
        if (!compileAppFile(appConfigFile, appUpper))
            return false;
        if (!appUpper.empty())
            outAppNamesUpper.push_back(appUpper);
        return true;
    }

    std::cerr << "Unknown APP config file: " << appConfigFile << std::endl;
    return false;
}

bool DbCompiler::parseAppDatNew(const fs::path& datFile, app_strc_dat& out)
{
    const std::string raw = read_all_text(datFile);
    if (raw.empty()) {
        std::cerr << "Cannot open: " << datFile << std::endl;
        return false;
    }

    const std::string text = strip_cpp_comments(raw);
    std::istringstream iss(text);

    out.clear();
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty())
            continue;

        if (starts_with(line, "/APPNAME:")) {
            auto v = trim(line.substr(std::strlen("/APPNAME:")));
            v = upper(v);
            out.app_id = v;
        } else if (starts_with(line, "/DBNAME:")) {
            auto v = trim(line.substr(std::strlen("/DBNAME:")));
            v = upper(v);
            if (!v.empty() && (int)out.db_ids.size() < MXDBNUM) {
                out.db_ids.push_back(v);
            }
        } else if (starts_with(line, "/FAMNAME:")) {
            // ignored
        }
    }

    if (trim(out.app_id).empty()) {
        // fallback: filename stem
        auto stem = upper(datFile.stem().string());
        out.app_id = stem;
    }

    out.db_count = static_cast<int>(out.db_ids.size());
    if (out.db_count <= 0) {
        std::cerr << "APPDAT has no DBNAME: " << datFile << std::endl;
        return false;
    }
    return true;
}

bool DbCompiler::parseDbDatNew(const fs::path& datFile, db_strc_dat& out)
{
    const std::string raw = read_all_text(datFile);
    if (raw.empty()) {
        std::cerr << "Cannot open: " << datFile << std::endl;
        return false;
    }

    const std::string text = strip_cpp_comments(raw);
    std::istringstream iss(text);

    out.clear();

    // record name -> index (0-based)
    std::unordered_map<std::string, int> recIndex;

    bool mainPartitionCreated = false;
    int currentMskIdx = 0;

    auto current_field_count = [&]() -> int { return static_cast<int>(out.field_ids.size()); };

    auto touch_db_name = [&]() {
        if (!out.db_id.empty())
            return;
        out.db_id = upper(datFile.stem().string());
    };

    auto ensure_pfld_initialized = [&]() {
        if (out.partition_field_prefix.empty())
            out.partition_field_prefix.push_back(0);
    };

    auto add_field = [&](const std::string& fldUpper, char typeCode, int bytes, int rec1based,
                         char valid, int bytesToSize) {
        if ((int)out.field_ids.size() >= MXFLDNUM) {
            throw std::runtime_error("Too many fields in " + datFile.string());
        }
        out.field_ids.push_back(fldUpper);
        out.field_type.push_back(typeCode);
        out.field_bytes.push_back(bytes);
        out.field_record_1based.push_back(rec1based);
        out.field_valid.push_back(valid);
        out.field_mask_index.push_back(currentMskIdx);

        // attach to last partition
        if (!out.partition_size_bytes.empty()) {
            out.partition_size_bytes.back() += bytesToSize;
        }

        // update current partition end marker (partition_field_prefix.back())
        if (!out.partition_field_prefix.empty())
            out.partition_field_prefix.back() = current_field_count();
    };

    auto finalize_prev_partition_pad = [&]() {
        if (out.partition_ids.empty())
            return;
        int prevSize = out.partition_size_bytes.back();
        int rem = 4096 - (prevSize % 4096);
        if (rem == 4096)
            return;

        std::string padName = upper(out.partition_ids.back()) + "8888";
        add_field(padName, 'C', rem, 0, 'A', rem);
    };

    auto ensure_main_partition = [&]() {
        if (mainPartitionCreated)
            return;

        touch_db_name();
        ensure_pfld_initialized();

        if ((int)out.partition_ids.size() >= MXPRTNUM) {
            throw std::runtime_error("Too many partitions in " + datFile.string());
        }

        // main partition name is DBNAME
        out.partition_ids.push_back(upper(out.db_id));
        out.partition_size_bytes.push_back(0);

        // create end marker for partition 0 (partition_field_prefix[1])
        if (out.partition_field_prefix.size() == 1)
            out.partition_field_prefix.push_back(current_field_count());

        // DB0 sentinel
        add_field(upper(out.db_id) + "0", 'I', 4, 0, 'B', 4);

        // LV$<REC> for each record currently known
        for (const auto& recName : out.record_ids) {
            add_field("LV$" + upper(recName), 'I', 4, 0, 'T', 4);
        }

        // counts
        out.partition_count = static_cast<int>(out.partition_ids.size());
        mainPartitionCreated = true;
    };

    auto add_partition = [&](const std::string& prtNameUpper) {
        ensure_main_partition();

        finalize_prev_partition_pad();

        if ((int)out.partition_ids.size() >= MXPRTNUM) {
            throw std::runtime_error("Too many partitions in " + datFile.string());
        }

        // start new partition at current field count (partition_field_prefix[out.partition_ids.size()])
        out.partition_ids.push_back(prtNameUpper);
        out.partition_size_bytes.push_back(0);

        // create end marker for this new partition (partition_field_prefix.back())
        out.partition_field_prefix.push_back(current_field_count());

        // partition header field (legacy): prtName int32
        add_field(prtNameUpper, 'I', 4, 0, 'E', 4);

        out.partition_count = static_cast<int>(out.partition_ids.size());
    };

    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty())
            continue;

        if (starts_with(line, "/DBNAME:")) {
            std::string v = trim(line.substr(std::strlen("/DBNAME:")));
            v = upper(v);
            out.db_id = v;
            continue;
        }

        if (starts_with(line, "/RECORD:")) {
            std::string tail = line.substr(std::strlen("/RECORD:"));
            auto sp = tail.find(' ');
            std::string recName = (sp == std::string::npos) ? tail : tail.substr(0, sp);
            recName = upper(trim(recName));

            int dim = 0;
            if (sp != std::string::npos) {
                auto kv = parse_kv_tail(trim(tail.substr(sp + 1)));
                auto it = kv.find("DIM");
                if (it != kv.end())
                    dim = std::stoi(it->second);
            }
            if (dim <= 0) {
                std::cerr << "Bad RECORD DIM in: " << datFile << " line: " << line << std::endl;
                return false;
            }
            if ((int)out.record_ids.size() >= MXRECNUM)
                return false;

            out.record_ids.push_back(recName);
            out.record_max_dim.push_back(dim);
            // 物理容量：默认按 DIM * ZRTDB_ALLOC_FACTOR（默认 3）分配，用于降低越界破坏。
            out.record_physical_dim.push_back(dim * getRecordPhysicalFactor());
            out.record_first_addr.push_back(0);
            recIndex[recName] = static_cast<int>(out.record_ids.size()) - 1;
            out.record_count = static_cast<int>(out.record_ids.size());
            continue;
        }

        if (starts_with(line, "/PARTITION:")) {
            std::string prtName = trim(line.substr(std::strlen("/PARTITION:")));
            prtName = upper(prtName);
            try {
                add_partition(prtName);
            } catch (const std::exception& e) {
                std::cerr << e.what() << std::endl;
                return false;
            }
            continue;
        }

        if (starts_with(line, "/FIELD:")) {
            // allow fields before explicit partition => attach to main partition
            try {
                ensure_main_partition();
            } catch (const std::exception& e) {
                std::cerr << e.what() << std::endl;
                return false;
            }

            std::string tail = line.substr(std::strlen("/FIELD:"));
            auto sp = tail.find(' ');
            std::string fldName = (sp == std::string::npos) ? tail : tail.substr(0, sp);
            fldName = upper(trim(fldName));

            std::string tail2 = (sp == std::string::npos) ? "" : trim(tail.substr(sp + 1));
            auto kv = parse_kv_tail(tail2);

            auto itT = kv.find("TYPE");
            if (itT == kv.end()) {
                std::cerr << "FIELD missing TYPE: " << datFile << " line: " << line << std::endl;
                return false;
            }

            char tc = 0;
            int bytes = 0;
            if (!map_type(itT->second, tc, bytes)) {
                std::cerr << "Unknown TYPE: " << itT->second << " in " << datFile << std::endl;
                return false;
            }

            int recIdx = 0;
            // If name contains _REC and REC exists, bind to record
            auto us = fldName.rfind('_');
            if (us != std::string::npos) {
                std::string maybeRec = fldName.substr(us + 1);
                auto itR = recIndex.find(maybeRec);
                if (itR != recIndex.end()) {
                    recIdx = itR->second + 1; // 1-based
                }
            }

            int itemSize = bytes;
            if (recIdx > 0 && (recIdx - 1) < (int)out.record_physical_dim.size()) {
                // 记录字段的“物理占用”按 CAP 计算（CAP = DIM * factor），从而在布局上留出缓冲区。
                itemSize = bytes * out.record_physical_dim[recIdx - 1];
            }

            try {
                add_field(fldName, tc, bytes, recIdx, 'T', itemSize);
            } catch (const std::exception& e) {
                std::cerr << e.what() << std::endl;
                return false;
            }
            continue;
        }
    }

    // finalize last partition pad
    try {
        if (!mainPartitionCreated) {
            ensure_main_partition();
        }
        finalize_prev_partition_pad();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return false;
    }

    // derive counts
    out.record_count = static_cast<int>(out.record_ids.size());
    out.partition_count = static_cast<int>(out.partition_ids.size());

    // ensure partition_field_prefix size == partition_count + 1
    if ((int)out.partition_field_prefix.size() < out.partition_count + 1) {
        out.partition_field_prefix.resize((size_t)out.partition_count + 1, current_field_count());
    } else if ((int)out.partition_field_prefix.size() > out.partition_count + 1) {
        out.partition_field_prefix.resize((size_t)out.partition_count + 1);
    }

    return true;
}

// -------------------------
// APPDAT.json parsing (minimal strict JSON)
// -------------------------

namespace {

struct JVal {
    enum class Type {
        Null,
        String,
        Array,
        Object,
        Number,
        Bool
    };

    Type type = Type::Null;
    std::string s;
    std::vector<std::unique_ptr<JVal>> a;
    std::unordered_map<std::string, std::unique_ptr<JVal>> o; // keys are stored in lower-case

    JVal() = default;
    JVal(JVal&&) noexcept = default;
    JVal& operator=(JVal&&) noexcept = default;
    JVal(const JVal&) = delete;
    JVal& operator=(const JVal&) = delete;

    const JVal* get(std::string_view k) const
    {
        std::string key(k);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        auto it = o.find(key);
        return it == o.end() ? nullptr : it->second.get();
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src)
        : s_(src)
    {
    }

    bool parse(JVal& out, std::string& err)
    {
        try {
            i_ = 0;
            skip_ws();
            out = parse_value();
            skip_ws();
            if (i_ != s_.size())
                throw std::runtime_error("Trailing garbage after JSON root");
            return true;
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        }
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    void skip_ws()
    {
        while (i_ < s_.size()) {
            unsigned char c = (unsigned char)s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++i_;
            else
                break;
        }
    }

    char peek() const { return (i_ < s_.size()) ? s_[i_] : '\0'; }

    char get()
    {
        if (i_ >= s_.size())
            throw std::runtime_error("Unexpected EOF");
        return s_[i_++];
    }

    void expect(char c)
    {
        skip_ws();
        if (get() != c)
            throw std::runtime_error(std::string("Expected '") + c + "'");
    }

    bool consume(char c)
    {
        skip_ws();
        if (peek() == c) {
            ++i_;
            return true;
        }
        return false;
    }

    std::string parse_string()
    {
        skip_ws();
        if (get() != '"')
            throw std::runtime_error("Expected string");

        std::string out;
        while (i_ < s_.size()) {
            char c = get();
            if (c == '"')
                break;
            if (c == '\\') {
                char e = get();
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    // Minimal: keep \uXXXX verbatim if present (no UTF-16 decode).
                    // This project uses UTF-8 JSON normally; \u escapes are rare.
                    out += "\\u";
                    for (int k = 0; k < 4; ++k)
                        out.push_back(get());
                    break;
                default:
                    throw std::runtime_error("Bad escape in string");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    JVal parse_value()
    {
        skip_ws();
        char c = peek();

        if (c == '"') {
            JVal v;
            v.type = JVal::Type::String;
            v.s = parse_string();
            return v;
        }

        if (c == '{')
            return parse_object();
        if (c == '[')
            return parse_array();

        // literals
        if (s_.substr(i_, 4) == "true") {
            i_ += 4;
            JVal v;
            v.type = JVal::Type::Bool;
            v.s = "true";
            return v;
        }
        if (s_.substr(i_, 5) == "false") {
            i_ += 5;
            JVal v;
            v.type = JVal::Type::Bool;
            v.s = "false";
            return v;
        }
        if (s_.substr(i_, 4) == "null") {
            i_ += 4;
            JVal v;
            v.type = JVal::Type::Null;
            return v;
        }

        // number (we don't need its exact value; just consume)
        if (c == '-' || std::isdigit((unsigned char)c)) {
            size_t j = i_;
            if (peek() == '-')
                ++i_;
            while (std::isdigit((unsigned char)peek()))
                ++i_;
            if (peek() == '.') {
                ++i_;
                while (std::isdigit((unsigned char)peek()))
                    ++i_;
            }
            if (peek() == 'e' || peek() == 'E') {
                ++i_;
                if (peek() == '+' || peek() == '-')
                    ++i_;
                while (std::isdigit((unsigned char)peek()))
                    ++i_;
            }
            JVal v;
            v.type = JVal::Type::Number;
            v.s = std::string(s_.substr(j, i_ - j));
            return v;
        }

        throw std::runtime_error("Unexpected JSON token");
    }

    JVal parse_array()
    {
        expect('[');
        JVal v;
        v.type = JVal::Type::Array;
        skip_ws();
        if (consume(']'))
            return v;

        for (;;) {
            v.a.push_back(std::make_unique<JVal>(parse_value()));
            skip_ws();
            if (consume(']'))
                break;
            expect(',');
        }
        return v;
    }

    JVal parse_object()
    {
        expect('{');
        JVal v;
        v.type = JVal::Type::Object;
        skip_ws();
        if (consume('}'))
            return v;

        for (;;) {
            std::string key = parse_string();
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            expect(':');
            v.o.emplace(std::move(key), std::make_unique<JVal>(parse_value()));

            skip_ws();
            if (consume('}'))
                break;
            expect(',');
        }
        return v;
    }
};

} // namespace

bool DbCompiler::parseAppDatJson(const fs::path& jsonFile, std::vector<app_strc_dat>& outApps)
{
    const std::string raw = read_all_text(jsonFile);
    if (raw.empty()) {
        std::cerr << "Cannot open: " << jsonFile << std::endl;
        return false;
    }

    JsonParser p(raw);
    JVal root;
    std::string err;
    if (!p.parse(root, err)) {
        std::cerr << "Bad APPDAT.json: " << jsonFile << " error: " << err << std::endl;
        return false;
    }

    if (root.type != JVal::Type::Object) {
        std::cerr << "Bad APPDAT.json: root is not object: " << jsonFile << std::endl;
        return false;
    }

    const JVal* apps = root.get("apps");
    if (!apps || apps->type != JVal::Type::Array) {
        std::cerr << "Bad APPDAT.json: missing 'apps' array: " << jsonFile << std::endl;
        return false;
    }

    outApps.clear();
    for (const auto& itemPtr : apps->a) {
        if (!itemPtr)
            continue;
        const JVal& item = *itemPtr;
        if (item.type != JVal::Type::Object)
            continue;
        const JVal* appName = item.get("app");
        const JVal* dbs = item.get("dbs");
        if (!appName || appName->type != JVal::Type::String)
            continue;
        if (!dbs || dbs->type != JVal::Type::Array)
            continue;

        app_strc_dat app;
        app.app_id = upper(trim(appName->s));
        for (const auto& dPtr : dbs->a) {
            if (!dPtr)
                continue;
            const JVal& d = *dPtr;
            if (d.type != JVal::Type::String)
                continue;
            const std::string db = upper(trim(d.s));
            if (!db.empty() && (int)app.db_ids.size() < MXDBNUM)
                app.db_ids.push_back(db);
        }
        app.db_count = (int)app.db_ids.size();

        if (app.app_id.empty() || app.db_count <= 0)
            continue;

        outApps.push_back(std::move(app));
    }

    if (outApps.empty()) {
        std::cerr << "Bad APPDAT.json: no valid apps: " << jsonFile << std::endl;
        return false;
    }

    return true;
}

// -------------------------
// Merged per-APP header generator
// -------------------------

static std::string sanitize_ident(std::string s, bool toLower)
{
    for (char& c : s) {
        unsigned char uc = (unsigned char)c;
        if (std::isalnum(uc) || c == '_') {
            c = toLower ? (char)std::tolower(uc) : (char)std::toupper(uc);
        } else {
            c = '_';
        }
    }
    if (!s.empty() && std::isdigit((unsigned char)s[0]))
        s.insert(s.begin(), '_');
    if (s.empty())
        s = "_";
    return s;
}

static std::string ctype_for_stdint(char t, int bytes)
{
    switch (t) {
    case 'I':
        if (bytes == 4)
            return "int32_t";
        if (bytes == 2)
            return "int16_t";
        if (bytes == 1)
            return "int8_t";
        return "int32_t";
    case 'K':
        return "int64_t";
    case 'R':
        return "float";
    case 'D':
        return "double";
    case 'S':
    case 'C':
        return "char";
    default:
        return "int32_t";
    }
}

template <typename... Args>
static std::string string_format(const std::string& fmt, Args... args)
{
    int n = std::snprintf(nullptr, 0, fmt.c_str(), args...) + 1;
    if (n <= 0)
        return "";
    std::string s;
    s.resize(static_cast<size_t>(n));
    std::snprintf(s.data(), static_cast<size_t>(n), fmt.c_str(), args...);
    if (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

void DbCompiler::generateAppHeader(const std::string& appUpper, const app_strc_dat& app)
{
    fs::create_directories(incPath());

    const std::string appLower = sanitize_ident(appUpper, true);
    const std::string guard = "ZRTDB_APP_" + sanitize_ident(appUpper, false) + "_H";

    auto outFile = incPath() / (appUpper + ".h");
    std::ofstream os(outFile);
    if (!os)
        throw std::runtime_error("Cannot write header: " + outFile.string());

    os << "#ifndef " << guard << "\n#define " << guard << "\n\n";
    os << "#include <stdint.h>\n#include <stddef.h>\n\n";

    os << "#ifndef ZRTDB_NO_RUNTIME_API\n";
    os << "#include \"zrtdb.h\"\n";
    os << "#endif\n\n";

    os << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

    os << "// APP: " << appUpper << "\n";
    os << "static const char ZRTDB_APP_NAME_" << sanitize_ident(appUpper, false) << "[] = \"" << appUpper << "\";\n\n";

    os << "// Portable packed-struct helpers\n";
    os << "#if defined(_MSC_VER)\n";
    os << "#  pragma pack(push, 1)\n";
    os << "#  define ZRTDB_PACKED\n";
    os << "#else\n";
    os << "#  define ZRTDB_PACKED __attribute__((packed))\n";
    os << "#endif\n\n";

    // Load DBDEFs.
    std::vector<db_strc_dat> dbs;
    dbs.reserve(app.db_ids.size());
    for (const auto& dbId : app.db_ids) {
        const std::string dbUpper = upper(dbId);
        db_strc_dat db;
        if (!getDbDefCachedOrLoad(dbUpper, db)) {
            throw std::runtime_error("DBDEF missing for DB=" + dbUpper + " (did you compile its DAT first?)");
        }
        dbs.push_back(std::move(db));
    }

    // Partition names and record constants.
    for (const auto& db : dbs) {
        const std::string dbUpper = upper(db.db_id);
        const std::string dbUpperSan = sanitize_ident(dbUpper, false);

        os << "// ---- DB: " << dbUpper << " ----\n";

        for (int r = 0; r < db.record_count; ++r) {
            const std::string recUpper = upper(db.record_ids[r]);
            const std::string recSan = sanitize_ident(recUpper, false);
            const int mx = db.record_max_dim[r];
            int cap = mx;
            if ((size_t)r < db.record_physical_dim.size() && db.record_physical_dim[(size_t)r] > 0)
                cap = db.record_physical_dim[(size_t)r];

            os << "#define ZRTDB_" << dbUpperSan << "_MX_" << recSan << " " << mx << "\n";
            os << "#define ZRTDB_" << dbUpperSan << "_CAP_" << recSan << " " << cap << "\n";
        }

        for (int p = 0; p < db.partition_count; ++p) {
            const std::string partUpper = upper(db.partition_ids[p]);
            if (partUpper.empty())
                continue;
            const std::string partSan = sanitize_ident(partUpper, false);
            // Always emit PART/DB to avoid ambiguity.
            os << "static const char ZRTDB_PART_" << dbUpperSan << "_" << partSan << "[] = \"" << partUpper << "/" << dbUpper << "\";\n";
        }
        os << "\n";
    }

    // Per-partition structs.
    for (const auto& db : dbs) {
        const std::string dbUpper = upper(db.db_id);
        const std::string dbLower = sanitize_ident(dbUpper, true);
        const std::string dbUpperSan = sanitize_ident(dbUpper, false);

        for (int p = 0; p < db.partition_count; ++p) {
            const std::string partUpper = upper(db.partition_ids[p]);
            if (partUpper.empty())
                continue;

            const std::string partLower = sanitize_ident(partUpper, true);
            const std::string partUpperSan = sanitize_ident(partUpper, false);

            const std::string typeName = "zrtdb_" + dbLower + "_" + partLower + "_t";

            os << "// Partition: " << partUpper << " (DB=" << dbUpper << ")\n";
            os << "typedef struct " << typeName << " {\n";

            const int startFld = db.partition_field_prefix[p];
            const int endFld = db.partition_field_prefix[p + 1];

            int padCount = 0;
            for (int f = startFld; f < endFld; ++f) {
                const char vt = db.field_valid[f];
                const std::string fldUpper = upper(db.field_ids[f]);
                const std::string fldSan = sanitize_ident(fldUpper, false);

                char tc = db.field_type[f];
                int bytes = db.field_bytes[f];
                std::string ctype = ctype_for_stdint(tc, bytes);

                // Padding fields: emit as uint8_t[] to preserve layout but make intent clear.
                if (vt == 'A') {
                    os << "    uint8_t _pad" << padCount++ << "[" << bytes << "];\n";
                    continue;
                }

                // Partition header / sentinels: still emit but keep a readable name.
                // (Valid types: T=normal, E=partition header, B=db0 sentinel)

                if (db.field_record_1based[f] > 0) {
                    const int recIdx = db.field_record_1based[f] - 1;
                    const int mx = db.record_max_dim[recIdx];
                    int cap = mx;
                    if ((size_t)recIdx < db.record_physical_dim.size() && db.record_physical_dim[(size_t)recIdx] > 0)
                        cap = db.record_physical_dim[(size_t)recIdx];

                    if (tc == 'S' || tc == 'C') {
                        os << "    " << ctype << " " << fldSan << "[" << cap << "][" << bytes << "];\n";
                    } else {
                        os << "    " << ctype << " " << fldSan << "[" << cap << "];\n";
                    }
                } else {
                    if (tc == 'S' || tc == 'C') {
                        os << "    " << ctype << " " << fldSan << "[" << bytes << "];\n";
                    } else {
                        os << "    " << ctype << " " << fldSan << ";\n";
                    }
                }
            }

            os << "} ZRTDB_PACKED " << typeName << ";\n\n";

            // Size helper
            os << "#define ZRTDB_SIZE_" << dbUpperSan << "_" << partUpperSan << " ((size_t)sizeof(" << typeName << "))\n\n";
        }
    }

    // Restore packing scope for MSVC.
    os << "#if defined(_MSC_VER)\n";
    os << "#  pragma pack(pop)\n";
    os << "#endif\n\n";

    // APP context
    os << "// ---- APP context (pointers to mapped partitions) ----\n";
    os << "typedef struct zrtdb_app_" << appLower << "_ctx_t {\n";
    for (const auto& db : dbs) {
        const std::string dbUpper = upper(db.db_id);
        const std::string dbLower = sanitize_ident(dbUpper, true);
        const std::string dbUpperSan = sanitize_ident(dbUpper, false);

        for (int p = 0; p < db.partition_count; ++p) {
            const std::string partUpper = upper(db.partition_ids[p]);
            if (partUpper.empty())
                continue;
            const std::string partLower = sanitize_ident(partUpper, true);
            const std::string partUpperSan = sanitize_ident(partUpper, false);

            const std::string typeName = "zrtdb_" + dbLower + "_" + partLower + "_t";
            const std::string member = dbUpperSan + "_" + partUpperSan;
            os << "    " << typeName << "* " << member << ";\n";
        }
    }
    os << "} zrtdb_app_" << appLower << "_ctx_t;\n\n";

    os << "#ifndef ZRTDB_NO_RUNTIME_API\n";
    os << "static inline int zrtdb_app_" << appLower << "_init(zrtdb_app_" << appLower << "_ctx_t* ctx)\n{\n";
    os << "    if (!ctx) return -1;\n";
    os << "    if (RegisterApp_(ZRTDB_APP_NAME_" << sanitize_ident(appUpper, false) << ") < 0) return -1;\n";

    for (const auto& db : dbs) {
        const std::string dbUpper = upper(db.db_id);
        const std::string dbUpperSan = sanitize_ident(dbUpper, false);

        for (int p = 0; p < db.partition_count; ++p) {
            const std::string partUpper = upper(db.partition_ids[p]);
            if (partUpper.empty())
                continue;
            const std::string partUpperSan = sanitize_ident(partUpper, false);
            const std::string member = dbUpperSan + "_" + partUpperSan;
            os << "    if (MapMemory_(ZRTDB_PART_" << dbUpperSan << "_" << partUpperSan << ", (char**)&ctx->" << member << ") < 0) return -1;\n";
        }
    }

    os << "    return 1;\n}\n";
    os << "#endif\n\n";

    os << "#ifdef __cplusplus\n}\n#endif\n\n";
    os << "#endif\n";

    os.close();

    std::cout << "[HDR] " << appUpper << " -> " << outFile << std::endl;
}

} // namespace zrtdb::init
