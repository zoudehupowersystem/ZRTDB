#pragma once

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>

namespace mmdb::dbio::toolutil {

inline std::string_view trim_view(std::string_view s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

inline std::string trim(std::string_view s) { return std::string(trim_view(s)); }

inline std::string upper(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return (char)std::toupper(c); });
    return out;
}

inline std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

// Legacy semantics: fixed-size char[] may include NUL and/or trailing spaces.
// Compare after (stop at NUL, trim trailing spaces, uppercase) against a pre-uppercased token.
inline bool safeCompareFixedUpper(std::string_view fixed, std::string_view tokenUpper)
{
    size_t n = 0;
    while (n < fixed.size() && fixed[n] != '\0')
        ++n;
    while (n > 0 && std::isspace((unsigned char)fixed[n - 1]))
        --n;
    if (n != tokenUpper.size())
        return false;
    for (size_t i = 0; i < n; ++i) {
        if ((char)std::toupper((unsigned char)fixed[i]) != tokenUpper[i])
            return false;
    }
    return true;
}

inline std::string normalize_comment(std::string_view cmt)
{
    std::string out = std::string(trim_view(cmt));
    constexpr std::string_view pfx = "注释:";
    if (out.rfind(pfx, 0) == 0)
        out = std::string(trim_view(std::string_view(out).substr(pfx.size())));
    return out;
}

inline std::string escape_json(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

inline bool parse_int(std::string_view s, int& out)
{
    s = trim_view(s);
    if (s.empty())
        return false;
    int sign = 1;
    size_t i = 0;
    if (s[0] == '+')
        i = 1;
    else if (s[0] == '-') {
        sign = -1;
        i = 1;
    }
    long long v = 0;
    for (; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!std::isdigit(c))
            return false;
        v = v * 10 + (c - '0');
        if (v > 0x7fffffffLL)
            break;
    }
    v *= sign;
    if (v < (long long)std::numeric_limits<int>::min() || v > (long long)std::numeric_limits<int>::max())
        return false;
    out = (int)v;
    return true;
}

struct Range {
    bool has = false;
    int start = 0;
    int end = 0;
    bool openEnd = false; // (a:) style
};

// Parse "a:b", "a:", ":b", or "k".
inline Range parse_range(std::string_view inner)
{
    Range r;
    inner = trim_view(inner);
    if (inner.empty())
        return r;

    auto col = inner.find(':');
    if (col == std::string_view::npos) {
        int k = 0;
        if (!parse_int(inner, k))
            return Range {};
        r.has = true;
        r.start = r.end = k;
        return r;
    }

    auto a = trim_view(inner.substr(0, col));
    auto b = trim_view(inner.substr(col + 1));
    r.has = true;

    if (!a.empty()) {
        if (!parse_int(a, r.start))
            return Range {};
    } else {
        r.start = 1;
    }

    if (!b.empty()) {
        if (!parse_int(b, r.end))
            return Range {};
    } else {
        r.openEnd = true;
        r.end = -1;
    }
    return r;
}

inline std::pair<std::string, std::string> split1(std::string_view s, char ch)
{
    auto p = s.find(ch);
    if (p == std::string_view::npos)
        return { trim(s), "" };
    return { trim(s.substr(0, p)), trim(s.substr(p + 1)) };
}

} // namespace mmdb::dbio::toolutil
