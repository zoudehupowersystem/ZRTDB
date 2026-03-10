// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace zrtdb::fingerprint {

class Fnv64 {
public:
    void addByte(std::uint8_t b)
    {
        h_ ^= b;
        h_ *= 1099511628211ull;
    }

    void addBytes(const void* p, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i)
            addByte(b[i]);
    }

    void addString(std::string_view s)
    {
        // Length-prefix each string to avoid stream ambiguity when payload contains separator bytes.
        addPodLE<std::uint32_t>(static_cast<std::uint32_t>(s.size()));
        addBytes(s.data(), s.size());
    }

    template <typename T>
    void addPodLE(T v)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            addByte(static_cast<std::uint8_t>((static_cast<std::uint64_t>(v) >> (i * 8)) & 0xff));
    }

    std::string hex() const
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(16) << h_;
        return oss.str();
    }

private:
    std::uint64_t h_ = 1469598103934665603ull;
};

} // namespace zrtdb::fingerprint
