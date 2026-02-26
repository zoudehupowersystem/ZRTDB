// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "DefIO.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace zrtdb::def {

namespace {

    constexpr std::array<char, 8> kMagic = { 'Z', 'R', 'T', 'D', 'B', 'D', '2', '\0' };

    enum class Kind : std::uint32_t { AppDef = 1,
        DbDef = 2 };

    struct Header {
        std::array<char, 8> magic {};
        std::uint32_t version = 2;
        std::uint32_t kind = 0;
    };

    template <typename T>
    void writePod(std::ofstream& ofs, const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        ofs.write(reinterpret_cast<const char*>(&v), sizeof(T));
        if (!ofs)
            throw std::runtime_error("def writePod failed");
    }

    template <typename T>
    void readPod(std::ifstream& ifs, T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        ifs.read(reinterpret_cast<char*>(&v), sizeof(T));
        if (!ifs)
            throw std::runtime_error("def readPod failed");
    }

    void writeString(std::ofstream& ofs, const std::string& s)
    {
        if (s.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("def string too large");
        std::uint32_t n = static_cast<std::uint32_t>(s.size());
        writePod(ofs, n);
        if (n) {
            ofs.write(s.data(), n);
            if (!ofs)
                throw std::runtime_error("def writeString failed");
        }
    }

    void readString(std::ifstream& ifs, std::string& s)
    {
        std::uint32_t n = 0;
        readPod(ifs, n);
        s.assign(n, '\0');
        if (n) {
            ifs.read(s.data(), n);
            if (!ifs)
                throw std::runtime_error("def readString failed");
        }
    }

    template <typename T>
    void writeVector(std::ofstream& ofs, const std::vector<T>& v)
    {
        if (v.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("def vector too large");
        std::uint32_t n = static_cast<std::uint32_t>(v.size());
        writePod(ofs, n);
        if (n) {
            ofs.write(reinterpret_cast<const char*>(v.data()), sizeof(T) * n);
            if (!ofs)
                throw std::runtime_error("def writeVector failed");
        }
    }

    template <typename T>
    void readVector(std::ifstream& ifs, std::vector<T>& v)
    {
        std::uint32_t n = 0;
        readPod(ifs, n);
        v.resize(n);
        if (n) {
            ifs.read(reinterpret_cast<char*>(v.data()), sizeof(T) * n);
            if (!ifs)
                throw std::runtime_error("def readVector failed");
        }
    }

    void writeVectorString(std::ofstream& ofs, const std::vector<std::string>& v)
    {
        if (v.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("def vector<string> too large");
        std::uint32_t n = static_cast<std::uint32_t>(v.size());
        writePod(ofs, n);
        for (const auto& s : v)
            writeString(ofs, s);
    }

    void readVectorString(std::ifstream& ifs, std::vector<std::string>& v)
    {
        std::uint32_t n = 0;
        readPod(ifs, n);
        v.resize(n);
        for (std::uint32_t i = 0; i < n; ++i)
            readString(ifs, v[i]);
    }

    Header makeHeader(Kind kind)
    {
        Header h;
        h.magic = kMagic;
        h.version = 2;
        h.kind = static_cast<std::uint32_t>(kind);
        return h;
    }

    void validateHeader(const Header& h, Kind expected)
    {
        if (h.magic != kMagic)
            throw std::runtime_error("def magic mismatch");
        if (h.version != 2)
            throw std::runtime_error("def version mismatch");
        if (h.kind != static_cast<std::uint32_t>(expected))
            throw std::runtime_error("def kind mismatch");
    }

} // namespace

bool saveAppDef(const std::filesystem::path& file, const app_strc_dat& app)
{
    try {
        std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;

        Header h = makeHeader(Kind::AppDef);
        writePod(ofs, h);

        writeString(ofs, app.app_id);
        writePod(ofs, static_cast<std::int32_t>(app.db_count));
        writeVectorString(ofs, app.db_ids);
        return true;
    } catch (...) {
        return false;
    }
}

bool loadAppDef(const std::filesystem::path& file, app_strc_dat& app)
{
    try {
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs)
            return false;

        Header h;
        readPod(ifs, h);
        validateHeader(h, Kind::AppDef);

        app.clear();
        readString(ifs, app.app_id);

        std::int32_t pdb = 0;
        readPod(ifs, pdb);
        app.db_count = static_cast<int>(pdb);

        readVectorString(ifs, app.db_ids);
        if (app.db_count < 0)
            app.db_count = 0;
        if ((int)app.db_ids.size() < app.db_count)
            app.db_count = static_cast<int>(app.db_ids.size());
        return true;
    } catch (...) {
        return false;
    }
}

bool saveDbDef(const std::filesystem::path& file, const db_strc_dat& db)
{
    try {
        std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;

        Header h = makeHeader(Kind::DbDef);
        writePod(ofs, h);

        writeString(ofs, db.db_id);
        writePod(ofs, static_cast<std::int32_t>(db.record_count));
        writePod(ofs, static_cast<std::int32_t>(db.partition_count));

        writeVectorString(ofs, db.record_ids);
        writeVector(ofs, db.record_first_addr);
        writeVector(ofs, db.record_max_dim);

        writeVectorString(ofs, db.partition_ids);
        writeVector(ofs, db.partition_field_prefix);
        writeVector(ofs, db.partition_size_bytes);

        writeVectorString(ofs, db.field_ids);
        writeVector(ofs, db.field_type);
        writeVector(ofs, db.field_record_1based);
        writeVector(ofs, db.field_bytes);
        writeVector(ofs, db.field_valid);
        writeVector(ofs, db.field_mask_index);

        // v2 兼容：追加新字段到文件尾部（旧版 loader 会忽略）。
        writeVector(ofs, db.record_physical_dim);

        return true;
    } catch (...) {
        return false;
    }
}

bool loadDbDef(const std::filesystem::path& file, db_strc_dat& db)
{
    try {
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs)
            return false;

        Header h;
        readPod(ifs, h);
        validateHeader(h, Kind::DbDef);

        db.clear();

        readString(ifs, db.db_id);

        std::int32_t prec = 0;
        std::int32_t pprt = 0;
        readPod(ifs, prec);
        readPod(ifs, pprt);
        db.record_count = static_cast<int>(prec);
        db.partition_count = static_cast<int>(pprt);

        readVectorString(ifs, db.record_ids);
        readVector(ifs, db.record_first_addr);
        readVector(ifs, db.record_max_dim);

        readVectorString(ifs, db.partition_ids);
        readVector(ifs, db.partition_field_prefix);
        readVector(ifs, db.partition_size_bytes);

        readVectorString(ifs, db.field_ids);
        readVector(ifs, db.field_type);
        readVector(ifs, db.field_record_1based);
        readVector(ifs, db.field_bytes);
        readVector(ifs, db.field_valid);
        readVector(ifs, db.field_mask_index);

        // 兼容旧版本：若文件尾部没有 record_physical_dim，则默认=record_max_dim。
        try {
            readVector(ifs, db.record_physical_dim);
        } catch (...) {
            db.record_physical_dim = db.record_max_dim;
        }

        // sanity: derive counts from vectors if needed
        if (db.record_count < 0)
            db.record_count = 0;
        if (db.partition_count < 0)
            db.partition_count = 0;

        if ((int)db.record_ids.size() < db.record_count)
            db.record_count = static_cast<int>(db.record_ids.size());
        if ((int)db.partition_ids.size() < db.partition_count)
            db.partition_count = static_cast<int>(db.partition_ids.size());

        // ensure partition_field_prefix has db_partition_prefix+1
        if ((int)db.partition_field_prefix.size() < db.partition_count + 1) {
            // best-effort fix
            db.partition_field_prefix.resize((size_t)db.partition_count + 1, (int)db.field_ids.size());
        }

        return true;
    } catch (...) {
        return false;
    }
}

} // namespace zrtdb::def
