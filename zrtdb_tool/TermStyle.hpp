// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdlib>
#include <cctype>
#include <string>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace mmdb::dbio {

// Minimal ANSI styling helper for terminal output.
//
// Enable rules:
// - If env NO_COLOR is set -> disabled.
// - Else, env ZRTDB_TOOL_COLOR can be: always|on|1, never|off|0, auto (default).
// - In auto, color is enabled only if stdout is a TTY.
class TermStyle {
public:
    enum class Mode { Auto, Always, Never };

    static TermStyle& instance()
    {
        static TermStyle inst;
        return inst;
    }

    bool enabled() const
    {
        if (noColor_)
            return false;
        if (mode_ == Mode::Never)
            return false;
        if (mode_ == Mode::Always)
            return true;
        return isTty_;
    }

    // Style helpers (use conservative palette; readable on dark/light backgrounds).
    std::string record(std::string_view s) const { return wrap(s, "\x1b[1;36m"); }  // bold cyan
    std::string field(std::string_view s) const { return wrap(s, "\x1b[1;33m"); }   // bold yellow
    std::string comment(std::string_view s) const { return wrap(s, "\x1b[2;32m"); } // dim green
    std::string idx(std::string_view s) const { return wrap(s, "\x1b[2m"); }         // dim
    std::string number(std::string_view s) const { return wrap(s, "\x1b[36m"); }     // cyan
    std::string str(std::string_view s) const { return wrap(s, "\x1b[35m"); }        // magenta
    std::string warn(std::string_view s) const { return wrap(s, "\x1b[33m"); }       // yellow
    std::string err(std::string_view s) const { return wrap(s, "\x1b[31m"); }        // red
    std::string critical(std::string_view s) const { return wrap(s, "\x1b[1;31m"); } // bold red
    std::string dim(std::string_view s) const { return wrap(s, "\x1b[2m"); }
    std::string prompt(std::string_view s) const { return wrap(s, "\x1b[1;34m"); }   // bold blue

private:
    Mode mode_ = Mode::Auto;
    bool isTty_ = false;
    bool noColor_ = false;

    TermStyle()
    {
        // Standard opt-out
        noColor_ = (std::getenv("NO_COLOR") != nullptr);

        // Custom switch
        if (const char* v = std::getenv("ZRTDB_TOOL_COLOR")) {
            std::string s(v);
            for (auto& ch : s)
                ch = (char)std::tolower((unsigned char)ch);
            if (s == "1" || s == "on" || s == "always")
                mode_ = Mode::Always;
            else if (s == "0" || s == "off" || s == "never")
                mode_ = Mode::Never;
            else
                mode_ = Mode::Auto;
        }

#if defined(__unix__) || defined(__APPLE__)
        isTty_ = (::isatty(STDOUT_FILENO) != 0) || (::isatty(STDERR_FILENO) != 0);
#else
        isTty_ = false;
#endif
    }

    std::string wrap(std::string_view s, const char* code) const
    {
        if (!enabled())
            return std::string(s);
        std::string out;
        out.reserve(s.size() + 16);
        out += code;
        out.append(s.data(), s.size());
        out += "\x1b[0m"; // reset
        return out;
    }
};

} // namespace mmdb::dbio
