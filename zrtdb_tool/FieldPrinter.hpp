#pragma once
#include "CommentLoader.hpp"
#include "DataAccessor.hpp"
#include "TermStyle.hpp"
#include "TextTable.hpp"
#include "ToolUtil.hpp"
#include "zrtdb_const.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace mmdb::dbio {

struct FieldRow {
    int fldIdx = -1;
    char type = 0;
    int bytes = 0;
    std::string nameUpper;
    DbValue val;
    std::string comment;
};

struct FieldPrintOptions {
    int nameCap = 24;
    int valCapStr = 24;
    int valCapNum = 10; // non-string: narrower (≈60% shrink)
    int minValW = 8;
    bool showSlot = false;
    int slot = 1;
    std::string headerLine;
};

inline std::vector<FieldRow> collect_field_rows(const std::vector<int>& idx, int row)
{
    std::vector<FieldRow> out;
    out.reserve(idx.size());
    for (int f : idx) {
        FieldRow r;
        r.fldIdx = f;
        r.type = g_runtime_app.field_type[f];
        r.bytes = (int)g_runtime_app.field_item_bytes[f];
        r.nameUpper = toolutil::upper(toolutil::trim_view(g_runtime_app.field_ids[f]));
        r.val = DataAccessor::getValue(f, row);
        r.comment = CommentLoader::instance().getComment(r.nameUpper);
        out.push_back(std::move(r));
    }
    return out;
}

inline std::vector<std::string> field_wrap_lines(std::string_view s, int width)
{
    std::vector<std::string> out;
    if (width <= 0) {
        out.emplace_back("");
        return out;
    }
    if (s.empty()) {
        out.emplace_back("");
        return out;
    }

    size_t i = 0;
    while (i < s.size()) {
        std::string line;
        int w = 0;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80) {
                if (w + 1 > width)
                    break;
                line.push_back(static_cast<char>(c));
                ++i;
                ++w;
                continue;
            }
            if (w + 2 > width)
                break;
            size_t adv = 1;
            if ((c & 0xE0) == 0xC0)
                adv = 2;
            else if ((c & 0xF0) == 0xE0)
                adv = 3;
            else if ((c & 0xF8) == 0xF0)
                adv = 4;
            adv = std::min(adv, s.size() - i);
            line.append(s.substr(i, adv));
            i += adv;
            w += 2;
        }
        if (line.empty()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            size_t adv = 1;
            if ((c & 0xE0) == 0xC0)
                adv = 2;
            else if ((c & 0xF0) == 0xE0)
                adv = 3;
            else if ((c & 0xF8) == 0xF0)
                adv = 4;
            adv = std::min(adv, s.size() - i);
            line.append(s.substr(i, adv));
            i += adv;
        }
        out.emplace_back(std::move(line));
    }
    return out;
}

inline void print_field_rows(const std::vector<FieldRow>& rows, const FieldPrintOptions& opt)
{
    auto& st = TermStyle::instance();
    if (!opt.headerLine.empty())
        std::cout << opt.headerLine << "\n";

    int nameW = 0, vStrW = 0, vNumW = 0;
    for (const auto& r : rows) {
        nameW = std::max(nameW, term_display_width(r.nameUpper));
        const std::string fv = DataAccessor::formatValue(r.val);
        (std::holds_alternative<std::string>(r.val) ? vStrW : vNumW) =
            std::max(std::holds_alternative<std::string>(r.val) ? vStrW : vNumW, term_display_width(fv));
    }
    nameW = std::min(nameW, opt.nameCap);
    vStrW = std::min(vStrW, opt.valCapStr);
    vNumW = std::min(vNumW, opt.valCapNum);

    for (const auto& r : rows) {
        std::string typeInfo = term_pad_right(string_format("%c*%d", r.type, r.bytes), 6);
        std::string namePad = term_pad_right(term_truncate(r.nameUpper, nameW), nameW);
        const bool isStr = std::holds_alternative<std::string>(r.val);
        int vW = std::max(opt.minValW, isStr ? vStrW : vNumW);
        const std::string formatted = DataAccessor::formatValue(r.val);

        if (!isStr) {
            std::string vPlain = term_pad_right(term_truncate(formatted, vW), vW);
            const std::string vStyled = st.number(vPlain);

            std::cout << "  " << st.dim(typeInfo) << " " << st.field(namePad) << " = ";
            if (opt.showSlot)
                std::cout << "[" << st.idx(std::to_string(opt.slot)) << "] ";
            std::cout << vStyled << "  " << st.comment(r.comment) << "\n";
            continue;
        }

        auto lines = field_wrap_lines(formatted, vW);
        const int prefixW = 2 + term_display_width(typeInfo) + 1 + term_display_width(namePad) + 3
            + (opt.showSlot ? (2 + term_display_width(std::to_string(opt.slot)) + 2) : 0);

        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string padded = term_pad_right(lines[i], vW);
            if (i == 0) {
                std::cout << "  " << st.dim(typeInfo) << " " << st.field(namePad) << " = ";
                if (opt.showSlot)
                    std::cout << "[" << st.idx(std::to_string(opt.slot)) << "] ";
                std::cout << st.str(padded) << "  " << st.comment(r.comment) << "\n";
            } else {
                std::cout << term_repeat(' ', prefixW) << st.str(padded) << "\n";
            }
        }
    }
}


} // namespace mmdb::dbio
