// BPA/IPF fixed-column parser for ZRTDB NETMOM
// SPDX-License-Identifier: Apache-2.0
#include "netmom/netmom.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace netmom {
namespace {

std::string trim(std::string s)
{
    auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    auto b = std::find_if(s.begin(), s.end(), not_space);
    auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
    if (b >= e) return {};
    return std::string(b, e);
}

std::string field(const std::string& line, std::size_t pos, std::size_t len)
{
    if (pos >= line.size()) return std::string(len, ' ');
    std::string out = line.substr(pos, std::min(len, line.size() - pos));
    if (out.size() < len) out.append(len - out.size(), ' ');
    return out;
}

double fixed_real(std::string s, int implied_decimals)
{
    s = trim(std::move(s));
    if (s.empty()) return 0.0;
    for (char& c : s) {
        if (c == 'd' || c == 'D') c = 'E';
    }
    try {
        if (s.find('.') != std::string::npos ||
            s.find('e') != std::string::npos ||
            s.find('E') != std::string::npos) {
            return std::stod(s);
        }
        double value = std::stod(s);
        for (int i = 0; i < implied_decimals; ++i) value /= 10.0;
        return value;
    } catch (...) {
        throw std::runtime_error("invalid numeric field '" + s + "'");
    }
}

int fixed_int(const std::string& s)
{
    const auto t = trim(s);
    if (t.empty()) return 0;
    try {
        return std::stoi(t);
    } catch (...) {
        throw std::runtime_error("invalid integer field '" + t + "'");
    }
}

BusType bpa_bus_type(char subtype)
{
    switch (subtype) {
    case 'S':
        return BusType::Slack;
    case 'Q':
    case 'E':
    case 'G':
        return BusType::PV;
    default:
        return BusType::PQ;
    }
}


} // namespace

std::string bus_key(std::string_view name, double kv)
{
    std::ostringstream os;
    os << trim(std::string(name)) << '@' << std::fixed << std::setprecision(3) << kv;
    return os.str();
}

BpaCase BpaParser::parse_file(const std::filesystem::path& path) const
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) throw std::runtime_error("cannot open BPA file: " + path.string());
    std::ostringstream buffer;
    buffer << ifs.rdbuf();
    return parse_text(buffer.str());
}

BpaCase BpaParser::parse_text(std::string_view text) const
{
    BpaCase out;
    std::istringstream in{std::string(text)};
    std::string line;
    int line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const char c0 = line[0];

        if (c0 == '/') {
            const auto key = line.find("MVA_BASE");
            if (key != std::string::npos) {
                const auto eq = line.find('=', key);
                if (eq != std::string::npos) {
                    try {
                        out.base_mva = std::stod(trim(line.substr(eq + 1)));
                    } catch (...) {
                        out.warnings.push_back("line " + std::to_string(line_no) +
                                               ": invalid /MVA_BASE value");
                    }
                }
            }
            continue;
        }
        if (c0 == '(') {
            const auto key = line.find("CASEID=");
            if (key != std::string::npos) {
                const auto start = key + 7;
                auto end = line.find_first_of(",)", start);
                out.case_id = trim(line.substr(start, end == std::string::npos
                                                       ? std::string::npos : end - start));
                if (out.case_id.empty()) out.case_id = "BPA_CASE";
            }
            continue;
        }
        if (c0 == '.' || c0 == ')' || c0 == '*') continue;

        try {
            if (c0 == 'B') {
                const char subtype = line.size() > 1 ? line[1] : ' ';
                if (subtype == 'D' || subtype == 'M') {
                    out.warnings.push_back("line " + std::to_string(line_no) +
                                           ": DC bus record is not imported in NETMOM v1");
                    continue;
                }
                const char change = line.size() > 2 ? line[2] : ' ';
                if (change != ' ') {
                    out.warnings.push_back("line " + std::to_string(line_no) +
                                           ": BPA change record skipped");
                    continue;
                }

                BpaBus b;
                b.name = trim(field(line, 6, 8));
                b.base_kv = fixed_real(field(line, 14, 4), 0);
                b.zone = trim(field(line, 18, 2));
                b.bus_type = bpa_bus_type(subtype);
                b.load_p = fixed_real(field(line, 20, 5), 0);
                b.load_q = fixed_real(field(line, 25, 5), 0);
                b.shunt_g = fixed_real(field(line, 30, 4), 0);
                b.shunt_b = fixed_real(field(line, 34, 4), 0);
                b.gen_p = fixed_real(field(line, 42, 5), 0);
                b.gen_q = fixed_real(field(line, 47, 5), 0);
                b.q_min = fixed_real(field(line, 52, 5), 0);
                b.v_set = fixed_real(field(line, 57, 4), 3);
                if (b.v_set == 0.0) b.v_set = 1.0;

                // On BE/BG/BQ/BS records columns 48-52 are QMAX rather
                // than a scheduled Q. Keep the value as QMAX while using
                // zero as the initial Q where the record semantics require it.
                if (subtype == 'E' || subtype == 'G' || subtype == 'Q' || subtype == 'S') {
                    b.q_max = b.gen_q;
                    b.gen_q = 0.0;
                } else {
                    b.q_max = 0.0;
                }

                if (b.name.empty() || b.base_kv <= 0.0) {
                    out.warnings.push_back("line " + std::to_string(line_no) +
                                           ": invalid/empty AC bus skipped");
                    continue;
                }
                out.buses.push_back(std::move(b));
                continue;
            }

            if (c0 == 'L' || c0 == 'E') {
                const char change = line.size() > 2 ? line[2] : ' ';
                if (change != ' ') {
                    out.warnings.push_back("line " + std::to_string(line_no) +
                                           ": BPA change branch skipped");
                    continue;
                }
                BpaLine l;
                l.type.assign(1, c0);
                l.bus1 = trim(field(line, 6, 8));
                l.kv1 = fixed_real(field(line, 14, 4), 0);
                l.bus2 = trim(field(line, 19, 8));
                // BPA/IPF coding sheets place the second base-kV immediately
                // after the bus-2 field and before circuit ID in column 32.
                l.kv2 = fixed_real(field(line, 27, 4), 0);
                l.circuit = trim(field(line, 31, 1));
                l.section = fixed_int(field(line, 32, 1));
                l.rating = fixed_real(field(line, 33, 4), 0);
                l.r = fixed_real(field(line, 38, 6), 5);
                l.x = fixed_real(field(line, 44, 6), 5);
                l.g1 = fixed_real(field(line, 50, 6), 5);
                l.b1 = fixed_real(field(line, 56, 6), 5);
                if (c0 == 'E') {
                    l.g2 = fixed_real(field(line, 62, 6), 5);
                    l.b2 = fixed_real(field(line, 68, 6), 5);
                } else {
                    l.g2 = l.g1;
                    l.b2 = l.b1;
                }
                if (l.bus1.empty() || l.bus2.empty()) {
                    out.warnings.push_back("line " + std::to_string(line_no) +
                                           ": branch endpoint missing; skipped");
                    continue;
                }
                out.lines.push_back(std::move(l));
                continue;
            }

            if (c0 == 'T') {
                const char subtype = line.size() > 1 ? line[1] : ' ';
                if (!(subtype == ' ' || subtype == 'P')) continue;
                const char change = line.size() > 2 ? line[2] : ' ';
                if (change != ' ') {
                    out.warnings.push_back("line " + std::to_string(line_no) +
                                           ": BPA change transformer skipped");
                    continue;
                }

                BpaTransformer t;
                t.phase_shifter = (subtype == 'P');
                t.bus1 = trim(field(line, 6, 8));
                t.kv1 = fixed_real(field(line, 14, 4), 0);
                t.bus2 = trim(field(line, 19, 8));
                t.kv2 = fixed_real(field(line, 27, 4), 0);
                t.circuit = trim(field(line, 31, 1));
                t.section = fixed_int(field(line, 32, 1));
                t.rating_mva = fixed_real(field(line, 33, 4), 0);
                t.r = fixed_real(field(line, 38, 6), 5);
                t.x = fixed_real(field(line, 44, 6), 5);
                t.g = fixed_real(field(line, 50, 6), 5);
                t.b = -std::abs(fixed_real(field(line, 56, 6), 5));
                t.tap1 = fixed_real(field(line, 62, 5), 2);
                t.tap2 = fixed_real(field(line, 67, 5), 2);
                if (t.bus1.empty() || t.bus2.empty()) {
                    out.warnings.push_back("line " + std::to_string(line_no) +
                                           ": transformer endpoint missing; skipped");
                    continue;
                }
                out.transformers.push_back(std::move(t));
                continue;
            }

            if (c0 == '+') {
                out.warnings.push_back("line " + std::to_string(line_no) +
                                       ": continuation bus record is currently ignored");
            }
        } catch (const std::exception& e) {
            out.warnings.push_back("line " + std::to_string(line_no) + ": " + e.what());
        }
    }

    return out;
}

} // namespace netmom
