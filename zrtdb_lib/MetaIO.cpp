// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "MetaIO.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace mmdb::meta {

namespace {

    constexpr std::array<char, 8> kMagic = { 'Z', 'R', 'T', 'D', 'B', 'M', '2', '\0' };

    enum class Kind : std::uint32_t { Clone = 1,
        Runtime = 2 };

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
            throw std::runtime_error("meta writePod failed");
    }

    template <typename T>
    void readPod(std::ifstream& ifs, T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        ifs.read(reinterpret_cast<char*>(&v), sizeof(T));
        if (!ifs)
            throw std::runtime_error("meta readPod failed");
    }

    void writeString(std::ofstream& ofs, const std::string& s)
    {
        if (s.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("meta string too large");
        std::uint32_t n = static_cast<std::uint32_t>(s.size());
        writePod(ofs, n);
        if (n) {
            ofs.write(s.data(), n);
            if (!ofs)
                throw std::runtime_error("meta writeString failed");
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
                throw std::runtime_error("meta readString failed");
        }
    }

    template <typename T>
    void writeVector(std::ofstream& ofs, const std::vector<T>& v)
    {
        if (v.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("meta vector too large");
        std::uint32_t n = static_cast<std::uint32_t>(v.size());
        writePod(ofs, n);
        if (n) {
            ofs.write(reinterpret_cast<const char*>(v.data()), sizeof(T) * n);
            if (!ofs)
                throw std::runtime_error("meta writeVector failed");
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
                throw std::runtime_error("meta readVector failed");
        }
    }

    void writeVectorString(std::ofstream& ofs, const std::vector<std::string>& v)
    {
        if (v.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("meta vector<string> too large");
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
            throw std::runtime_error("meta magic mismatch");
        if (h.version != 2)
            throw std::runtime_error("meta version mismatch");
        if (h.kind != static_cast<std::uint32_t>(expected))
            throw std::runtime_error("meta kind mismatch");
    }

} // namespace

bool saveClone(const std::filesystem::path& file, const StaticModelConfig& clone)
{
    try {
        std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;

        Header h = makeHeader(Kind::Clone);
        writePod(ofs, h);

        writeString(ofs, clone.app_id);
        writePod(ofs, clone.db_count);

        writeVectorString(ofs, clone.db_ids);
        writeVector(ofs, clone.db_partition_prefix);
        writeVector(ofs, clone.db_record_prefix);

        writeVectorString(ofs, clone.record_ids);
        writeVector(ofs, clone.record_max_dim);
        writeVector(ofs, clone.record_first_addr);

        writeVectorString(ofs, clone.partition_ids);
        writeVector(ofs, clone.partition_field_prefix);
        writeVector(ofs, clone.partition_size_bytes);

        writeVectorString(ofs, clone.field_ids);
        writeVector(ofs, clone.field_type);
        writeVector(ofs, clone.field_record_1based);
        writeVector(ofs, clone.field_bytes);
        writeVector(ofs, clone.field_valid);
        writeVector(ofs, clone.field_mask_index);

        // v2 兼容策略：新字段一律追加在文件尾部，旧版 loadClone 不会读到这里。
        // record_physical_dim 用于“物理容量缓冲”，为空时建议按 record_max_dim 处理。
        writeVector(ofs, clone.record_physical_dim);
        writeString(ofs, clone.layout_fingerprint);
        return true;
    } catch (...) {
        return false;
    }
}

bool loadClone(const std::filesystem::path& file, StaticModelConfig& clone)
{
    try {
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs)
            return false;

        Header h;
        readPod(ifs, h);
        validateHeader(h, Kind::Clone);

        clone.clear();

        readString(ifs, clone.app_id);
        readPod(ifs, clone.db_count);

        readVectorString(ifs, clone.db_ids);
        readVector(ifs, clone.db_partition_prefix);
        readVector(ifs, clone.db_record_prefix);

        readVectorString(ifs, clone.record_ids);
        readVector(ifs, clone.record_max_dim);
        readVector(ifs, clone.record_first_addr);

        readVectorString(ifs, clone.partition_ids);
        readVector(ifs, clone.partition_field_prefix);
        readVector(ifs, clone.partition_size_bytes);

        readVectorString(ifs, clone.field_ids);
        readVector(ifs, clone.field_type);
        readVector(ifs, clone.field_record_1based);
        readVector(ifs, clone.field_bytes);
        readVector(ifs, clone.field_valid);
        readVector(ifs, clone.field_mask_index);

        // 兼容旧版本 clone meta：若文件尾部不存在 record_physical_dim，则默认=record_max_dim。
        try {
            readVector(ifs, clone.record_physical_dim);
        } catch (...) {
            clone.record_physical_dim = clone.record_max_dim;
        }
        try { readString(ifs, clone.layout_fingerprint); } catch (...) { clone.layout_fingerprint.clear(); }
        return true;
    } catch (...) {
        return false;
    }
}

bool saveRuntime(const std::filesystem::path& file, const RuntimeAppConfig& app)
{
    try {
        std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;

        Header h = makeHeader(Kind::Runtime);
        writePod(ofs, h);

        writePod(ofs, app.db_count);
        writePod(ofs, app.partition_count);
        writePod(ofs, app.record_count);
        writePod(ofs, app.field_count);
        writePod(ofs, app.table_count);

        writeVector(ofs, app.partition_base_addrs);

        writeVectorString(ofs, app.db_ids);
        writeVector(ofs, app.db_partition_prefix);
        writeVector(ofs, app.db_record_prefix);

        writeVectorString(ofs, app.partition_ids);
        writeVector(ofs, app.partition_bytes);
        writeVector(ofs, app.partition_db_1based);
        writeVector(ofs, app.partition_field_prefix);

        writeVectorString(ofs, app.record_ids);
        writeVector(ofs, app.record_max_dim);
        writeVector(ofs, app.record_lv_addrs);
        writeVector(ofs, app.record_lv_partition_1based);
        writeVector(ofs, app.record_lv_offset_bytes);
        writeVector(ofs, app.record_table_prefix);
        writeVector(ofs, app.record_size_bytes);

        writeVectorString(ofs, app.field_ids);
        writeVector(ofs, app.field_partition_1based);
        writeVector(ofs, app.field_record_1based);
        writeVector(ofs, app.field_offset_bytes);
        writeVector(ofs, app.field_type);
        writeVector(ofs, app.field_item_bytes);

        writeVector(ofs, app.table_field_1based);

        // v2 兼容策略：新字段追加在文件尾部。
        writeVector(ofs, app.record_physical_dim);
        writeString(ofs, app.layout_fingerprint);
        return true;
    } catch (...) {
        return false;
    }
}

bool loadRuntime(const std::filesystem::path& file, RuntimeAppConfig& app)
{
    try {
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs)
            return false;

        Header h;
        readPod(ifs, h);
        validateHeader(h, Kind::Runtime);

        app.clear();

        readPod(ifs, app.db_count);
        readPod(ifs, app.partition_count);
        readPod(ifs, app.record_count);
        readPod(ifs, app.field_count);
        readPod(ifs, app.table_count);

        readVector(ifs, app.partition_base_addrs);

        readVectorString(ifs, app.db_ids);
        readVector(ifs, app.db_partition_prefix);
        readVector(ifs, app.db_record_prefix);

        readVectorString(ifs, app.partition_ids);
        readVector(ifs, app.partition_bytes);
        readVector(ifs, app.partition_db_1based);
        readVector(ifs, app.partition_field_prefix);

        readVectorString(ifs, app.record_ids);
        readVector(ifs, app.record_max_dim);
        readVector(ifs, app.record_lv_addrs);
        readVector(ifs, app.record_lv_partition_1based);
        readVector(ifs, app.record_lv_offset_bytes);
        readVector(ifs, app.record_table_prefix);
        readVector(ifs, app.record_size_bytes);

        readVectorString(ifs, app.field_ids);
        readVector(ifs, app.field_partition_1based);
        readVector(ifs, app.field_record_1based);
        readVector(ifs, app.field_offset_bytes);
        readVector(ifs, app.field_type);
        readVector(ifs, app.field_item_bytes);

        readVector(ifs, app.table_field_1based);

        // 兼容旧版本 runtime meta：若不存在 record_physical_dim，则默认=record_max_dim。
        try {
            readVector(ifs, app.record_physical_dim);
        } catch (...) {
            app.record_physical_dim = app.record_max_dim;
        }
        try { readString(ifs, app.layout_fingerprint); } catch (...) { app.layout_fingerprint.clear(); }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace mmdb::meta
