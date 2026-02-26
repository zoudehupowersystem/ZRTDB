#include "TableViewRenderer.hpp"
#include "DataAccessor.hpp"
#include "TermStyle.hpp"
#include "TextTable.hpp"
#include <algorithm>
#include <iostream>

namespace mmdb::dbio {

namespace {
inline void print_sep(const std::vector<int>& w)
{
    std::cout << "+";
    for (int ww : w)
        std::cout << term_repeat('-', ww + 2) << "+";
    std::cout << "\n";
}

inline std::string cell_pad(std::string_view s, int w)
{
    return term_pad_right(term_truncate(s, w), w);
}

// Wrap UTF-8 text to a fixed display width (monospace approximation), without truncation.
// This is primarily used for the comment header row, which may be long.
inline std::vector<std::string> term_wrap_lines(std::string_view s, int width)
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
                ++w;
                ++i;
                continue;
            }
            // Non-ASCII, treat as width 2 (same convention as term_display_width)
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
            // If a single codepoint is wider than the column, still consume it to ensure progress.
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

inline int value_width(int fIdx, int row)
{
    auto v = DataAccessor::getValue(fIdx, row);
    return term_display_width(DataAccessor::formatValue(v));
}
}

void BoxTable::render() const
{
    auto& st = TermStyle::instance();
    if (!banner.empty())
        std::cout << banner << "\n";

    // idx column
    int maxRow = 1;
    for (int r : rows)
        maxRow = std::max(maxRow, r);

    const int colN = static_cast<int>(cols.size());
    std::vector<int> w(static_cast<size_t>(colN) + 1, 0);
    w[0] = std::max(term_display_width("idx"), term_display_width("注释"));
    w[0] = std::max(w[0], term_display_width(std::to_string(maxRow)));
    w[0] = std::min(w[0], idxWCap);

    for (int j = 0; j < colN; ++j) {
        int ww = 0;
        ww = std::max(ww, term_display_width(cols[j].nameUpper));
        ww = std::max(ww, term_display_width(cols[j].comment));
        for (int r : rows)
            ww = std::max(ww, value_width(cols[j].fieldIdx, r));
        const int cap = cols[j].isString ? maxColWStr : maxColWNum;
        w[static_cast<size_t>(j) + 1] = std::min(ww, cap);
    }

    print_sep(w);

    // header
    {
        std::cout << "| " << st.idx(cell_pad("idx", w[0])) << " |";
        for (int j = 0; j < colN; ++j) {
            std::cout << " " << st.field(cell_pad(cols[j].nameUpper, w[static_cast<size_t>(j) + 1])) << " |";
        }
        std::cout << "\n";
    }
    // comment row
    {
        std::vector<std::vector<std::string>> cLines(static_cast<size_t>(colN));
        int needLines = 1;
        for (int j = 0; j < colN; ++j) {
            const int cw = w[static_cast<size_t>(j) + 1];
            auto lines = term_wrap_lines(cols[j].comment, cw);
            needLines = std::max(needLines, static_cast<int>(lines.size()));
            cLines[static_cast<size_t>(j)] = std::move(lines);
        }

        for (int li = 0; li < needLines; ++li) {
            std::cout << "| " << st.comment(cell_pad(li == 0 ? "注释" : "", w[0])) << " |";
            for (int j = 0; j < colN; ++j) {
                const int cw = w[static_cast<size_t>(j) + 1];
                const auto& v = cLines[static_cast<size_t>(j)];
                const std::string_view seg = (li < static_cast<int>(v.size())) ? std::string_view(v[static_cast<size_t>(li)]) : std::string_view();
                std::cout << " " << st.comment(term_pad_right(seg, cw)) << " |";
            }
            std::cout << "\n";
        }
        print_sep(w);
    }

    // data
    for (int r : rows) {
        std::vector<std::vector<std::string>> cellLines(static_cast<size_t>(colN));
        int needLines = 1;
        for (int j = 0; j < colN; ++j) {
            auto v = DataAccessor::getValue(cols[j].fieldIdx, r);
            const int cw = w[static_cast<size_t>(j) + 1];
            if (std::holds_alternative<std::string>(v)) {
                auto lines = term_wrap_lines(DataAccessor::formatValue(v), cw);
                needLines = std::max(needLines, static_cast<int>(lines.size()));
                cellLines[static_cast<size_t>(j)] = std::move(lines);
            } else {
                cellLines[static_cast<size_t>(j)].push_back(term_pad_right(DataAccessor::formatValue(v), cw));
            }
        }

        for (int li = 0; li < needLines; ++li) {
            std::cout << "| " << st.idx(li == 0 ? term_pad_left(std::to_string(r), w[0]) : term_pad_right("", w[0])) << " |";
            for (int j = 0; j < colN; ++j) {
                const int cw = w[static_cast<size_t>(j) + 1];
                const auto& lines = cellLines[static_cast<size_t>(j)];
                const std::string_view seg = (li < static_cast<int>(lines.size())) ? std::string_view(lines[static_cast<size_t>(li)]) : std::string_view();
                const std::string padded = term_pad_right(seg, cw);
                const bool isStr = cols[j].isString;
                std::cout << " " << (isStr ? st.str(padded) : st.number(padded)) << " |";
            }
            std::cout << "\n";
        }
    }
    print_sep(w);
}

} // namespace mmdb::dbio
