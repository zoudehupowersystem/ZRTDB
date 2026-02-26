// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "StringUtils.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace mmdb::utils {

std::string toUpper(std::string_view s)
{
    std::string ret(s);
    std::transform(ret.begin(), ret.end(), ret.begin(), [](unsigned char c) { return std::toupper(c); });
    return ret;
}

std::string toLower(std::string_view s)
{
    std::string ret(s);
    std::transform(ret.begin(), ret.end(), ret.begin(), [](unsigned char c) { return std::tolower(c); });
    return ret;
}

std::string trim(std::string_view s)
{
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos)
        return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(start, end - start + 1));
}

bool extractBetween(std::string& out, const std::string& src,
    const std::string& startKey, const std::string& endKey)
{
    size_t startPos = src.find(startKey);
    if (startPos == std::string::npos)
        return false;
    startPos += startKey.length();

    size_t endPos;
    if (endKey == "$") {
        endPos = src.length();
    } else {
        endPos = src.find(endKey, startPos);
        if (endPos == std::string::npos)
            return false;
    }

    out = src.substr(startPos, endPos - startPos);
    out.erase(std::remove_if(out.begin(), out.end(), [](unsigned char c) { return std::isspace(c); }), out.end());
    return !out.empty();
}

} // namespace mmdb::utils

extern "C" {

char* upcase(char* des_str, char* src_str)
{
    if (!des_str || !src_str)
        return nullptr;
    size_t len = std::strlen(src_str);
    for (size_t i = 0; i < len; ++i) {
        des_str[i] = std::toupper(static_cast<unsigned char>(src_str[i]));
    }
    des_str[len] = '\0';
    return des_str;
}

char* dwcase(char* des_str, char* src_str)
{
    if (!des_str || !src_str)
        return nullptr;
    size_t len = std::strlen(src_str);
    for (size_t i = 0; i < len; ++i) {
        des_str[i] = std::tolower(static_cast<unsigned char>(src_str[i]));
    }
    des_str[len] = '\0';
    return des_str;
}

} // extern "C"

int get_string_new(std::string& des_str, const std::string& src_str, const char* prekey, const char* sufkey)
{
    if (!prekey || !sufkey)
        return 0;
    return mmdb::utils::extractBetween(des_str, src_str, prekey, sufkey) ? 1 : 0;
}
