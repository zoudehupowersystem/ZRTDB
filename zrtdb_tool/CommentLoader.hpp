// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace mmdb::dbio {

class CommentLoader {
public:
    static CommentLoader& instance()
    {
        static CommentLoader inst;
        return inst;
    }

    void load(const std::string& datPath)
    {
        namespace fs = std::filesystem;
        try {
            if (!fs::exists(datPath))
                return;
            for (const auto& entry : fs::directory_iterator(datPath)) {
                if (!entry.is_regular_file())
                    continue;
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::toupper(c); });

                // Only DAT carries field/record descriptions.
                if (ext == ".DAT") {
                    parseFile(entry.path());
                }
            }
        } catch (...) {
        }
    }

    std::string getComment(const std::string& name) const
    {
        std::string key = name;
        // trim right
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();

        // try exact and progressively remove suffix by '_'
        while (true) {
            auto it = comments_.find(key);
            if (it != comments_.end())
                return it->second;
            auto us = key.rfind('_');
            if (us == std::string::npos)
                break;
            key = key.substr(0, us);
        }
        return "";
    }

private:
    std::map<std::string, std::string> comments_;

    static std::string upper(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        return s;
    }

    static std::string trim(std::string s)
    {
        auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static std::string extractKey(const std::string& line, const std::string& tag)
    {
        auto start = line.find(tag);
        if (start == std::string::npos)
            return "";
        start += tag.size();
        auto end = line.find_first_of(" \t\r\n", start);
        if (end == std::string::npos)
            end = line.size();
        return upper(line.substr(start, end - start));
    }

    // Strip C/C++ style comments from src, preserving newlines.
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

    static size_t find_icase(const std::string& hay, const std::string& needle)
    {
        if (needle.empty() || hay.size() < needle.size())
            return std::string::npos;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < needle.size(); ++k) {
                if (std::toupper((unsigned char)hay[i + k]) != std::toupper((unsigned char)needle[k])) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return i;
        }
        return std::string::npos;
    }

    // Extract COMMENT:<text> in a line. Accepts either:
    // - COMMENT:xxx
    // - COMMENT:"xxx ..."  (quoted)
    // Stops at end-of-line.
    static std::string extractCommentValue(const std::string& line)
    {
        auto pos = find_icase(line, "COMMENT:");
        if (pos == std::string::npos)
            return "";
        pos += std::strlen("COMMENT:");
        if (pos >= line.size())
            return "";

        std::string tail = trim(line.substr(pos));
        if (tail.empty())
            return "";

        // quoted
        if (!tail.empty() && tail.front() == '"') {
            std::string out;
            bool esc = false;
            for (size_t i = 1; i < tail.size(); ++i) {
                char c = tail[i];
                if (esc) {
                    out.push_back(c);
                    esc = false;
                    continue;
                }
                if (c == '\\') {
                    esc = true;
                    continue;
                }
                if (c == '"')
                    break;
                out.push_back(c);
            }
            return trim(out);
        }

        return trim(tail);
    }

    void parseFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return;
        std::ostringstream oss;
        oss << file.rdbuf();
        const std::string text = strip_cpp_comments(oss.str());

        std::istringstream iss(text);
        std::string line;

        while (std::getline(iss, line)) {
            line = trim(line);
            if (line.empty())
                continue;

            std::string key;
            if (line.find("/RECORD:") != std::string::npos) {
                key = extractKey(line, "/RECORD:");
            } else if (line.find("/FIELD:") != std::string::npos) {
                key = extractKey(line, "/FIELD:");
            }

            if (key.empty())
                continue;

            std::string comment = extractCommentValue(line);
            if (comment.empty())
                continue;

            comments_[upper(key)] = comment;
        }
    }
};

} // namespace mmdb::dbio
