// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace mmdb::dbio {

// Approximate monospace display width for UTF-8 strings.
// - ASCII: width 1
// - Non-ASCII: width 2 (works reasonably well for CJK comments in terminals)
inline int term_display_width(std::string_view s)
{
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++w;
            ++i;
            continue;
        }
        // Skip one UTF-8 codepoint.
        size_t adv = 1;
        if ((c & 0xE0) == 0xC0)
            adv = 2;
        else if ((c & 0xF0) == 0xE0)
            adv = 3;
        else if ((c & 0xF8) == 0xF0)
            adv = 4;
        i += std::min(adv, s.size() - i);
        w += 2;
    }
    return w;
}

inline std::string term_truncate(std::string_view s, int maxWidth)
{
    if (maxWidth <= 0)
        return "";
    if (term_display_width(s) <= maxWidth)
        return std::string(s);

    // Reserve 3 chars for "..." when possible.
    const int suffix = (maxWidth >= 4) ? 3 : 0;
    const int keep = maxWidth - suffix;

    std::string out;
    out.reserve(s.size());
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (w + 1 > keep)
                break;
            out.push_back(static_cast<char>(c));
            ++w;
            ++i;
            continue;
        }
        // Non-ASCII, treat as width 2
        if (w + 2 > keep)
            break;
        size_t adv = 1;
        if ((c & 0xE0) == 0xC0)
            adv = 2;
        else if ((c & 0xF0) == 0xE0)
            adv = 3;
        else if ((c & 0xF8) == 0xF0)
            adv = 4;
        adv = std::min(adv, s.size() - i);
        out.append(s.substr(i, adv));
        i += adv;
        w += 2;
    }
    if (suffix)
        out += "...";
    return out;
}

inline std::string term_pad_right(std::string_view s, int width)
{
    std::string out(s);
    int w = term_display_width(out);
    if (w < width)
        out.append(static_cast<size_t>(width - w), ' ');
    return out;
}

inline std::string term_pad_left(std::string_view s, int width)
{
    int w = term_display_width(s);
    if (w >= width)
        return std::string(s);
    std::string out;
    out.assign(static_cast<size_t>(width - w), ' ');
    out.append(s.data(), s.size());
    return out;
}

inline std::string term_repeat(char ch, int n)
{
    if (n <= 0)
        return "";
    return std::string(static_cast<size_t>(n), ch);
}

// Compute column widths given table cells (already truncated if you want caps).
inline std::vector<int> term_calc_col_widths(const std::vector<std::vector<std::string>>& rows)
{
    size_t cols = 0;
    for (auto& r : rows)
        cols = std::max(cols, r.size());
    std::vector<int> w(cols, 0);
    for (auto& r : rows) {
        for (size_t c = 0; c < r.size(); ++c)
            w[c] = std::max(w[c], term_display_width(r[c]));
    }
    return w;
}

} // namespace mmdb::dbio
