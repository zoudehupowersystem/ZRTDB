// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string>
#include <string_view>

namespace mmdb::utils {

std::string toUpper(std::string_view s);
std::string toLower(std::string_view s);
std::string trim(std::string_view s);
bool extractBetween(std::string& out, const std::string& src,
    const std::string& startKey, const std::string& endKey);

} // namespace mmdb::utils

extern "C" {
char* upcase(char* des_str, char* src_str);
char* dwcase(char* des_str, char* src_str);
}

int get_string_new(std::string& des_str, const std::string& src_str, const char* prekey, const char* sufkey);
