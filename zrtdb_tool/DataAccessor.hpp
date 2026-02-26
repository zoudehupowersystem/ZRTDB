// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "zrtdb_const.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <variant>

namespace mmdb::dbio {

template <typename... Args>
static std::string string_format(const std::string& format, Args... args)
{
    int size_s = std::snprintf(nullptr, 0, format.c_str(), args...) + 1;
    if (size_s <= 0)
        return "";
    auto size = static_cast<size_t>(size_s);
    std::unique_ptr<char[]> buf(new char[size]);
    std::snprintf(buf.get(), size, format.c_str(), args...);
    return std::string(buf.get(), buf.get() + size - 1);
}

using DbValue = std::variant<int, float, double, long, std::string>;

class DataAccessor {
public:
    static DbValue getValue(int fldIdx, int recSlot)
    {
        char* p = getAddress(fldIdx, recSlot);
        if (!p)
            return 0;

        char t = g_runtime_app.field_type[fldIdx];
        int bytes = static_cast<unsigned char>(g_runtime_app.field_item_bytes[fldIdx]);

        switch (t) {
        case 'I':
            return *reinterpret_cast<int*>(p);
        case 'K':
            return *reinterpret_cast<long*>(p);
        case 'R':
            return *reinterpret_cast<float*>(p);
        case 'D':
            return *reinterpret_cast<double*>(p);
        case 'S':
        case 'C': {
            std::string s(p, p + bytes);
            auto z = std::find(s.begin(), s.end(), '\0');
            s.erase(z, s.end());
            return s;
        }
        default:
            return 0;
        }
    }

    static bool setValue(int fldIdx, int recSlot, const std::string& inputVal)
    {
        char* p = getAddress(fldIdx, recSlot);
        if (!p)
            return false;

        char t = g_runtime_app.field_type[fldIdx];
        int bytes = static_cast<unsigned char>(g_runtime_app.field_item_bytes[fldIdx]);

        try {
            switch (t) {
            case 'I':
                *reinterpret_cast<int*>(p) = std::stoi(inputVal);
                break;
            case 'K':
                *reinterpret_cast<long*>(p) = std::stol(inputVal);
                break;
            case 'R':
                *reinterpret_cast<float*>(p) = std::stof(inputVal);
                break;
            case 'D':
                *reinterpret_cast<double*>(p) = std::stod(inputVal);
                break;
            case 'S':
            case 'C': {
                // overwrite string up to 120 bytes
                int cap = std::min(bytes, 120);
                std::memset(p, 0, bytes);
                std::memcpy(p, inputVal.data(), std::min<int>(cap, (int)inputVal.size()));
                break;
            }
            default:
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::string formatValue(const DbValue& v)
    {
        return std::visit([](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return string_format("'%s'", arg.c_str());
            } else if constexpr (std::is_same_v<T, float>) {
                return string_format("%.6f", arg);
            } else if constexpr (std::is_same_v<T, double>) {
                return string_format("%.10g", arg);
            } else if constexpr (std::is_same_v<T, long>) {
                return string_format("%ld", arg);
            } else {
                return string_format("%d", arg);
            }
        },
            v);
    }

    static char* getAddress(int fldIdx, int recSlot)
    {
        int prtIdx0 = g_runtime_app.field_partition_1based[fldIdx] - 1;
        if (prtIdx0 < 0)
            return nullptr;
        long base = g_runtime_app.partition_base_addrs[prtIdx0];
        if (base == 0)
            return nullptr;

        long off = g_runtime_app.field_offset_bytes[fldIdx];
        int bytes = static_cast<unsigned char>(g_runtime_app.field_item_bytes[fldIdx]);

        // if record field: recSlot indexes element (1-based)
        if (g_runtime_app.field_record_1based[fldIdx] > 0) {
            long slotOff = static_cast<long>(recSlot - 1) * bytes;
            return reinterpret_cast<char*>(base + off + slotOff);
        }
        // global scalar/string: ignore recSlot
        return reinterpret_cast<char*>(base + off);
    }
};

} // namespace mmdb::dbio
