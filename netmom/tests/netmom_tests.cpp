// Pure parser/topology tests; no ZRTDB runtime is required.
// SPDX-License-Identifier: Apache-2.0
#include "netmom/netmom.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

namespace {

void put(std::string& s, std::size_t pos, std::string v)
{
    if (s.size() < pos + v.size()) s.resize(pos + v.size(), ' ');
    s.replace(pos, v.size(), v);
}

std::string bus(char subtype, const std::string& name, const std::string& kv,
                const std::string& lp, const std::string& lq,
                const std::string& pg)
{
    std::string s(80, ' ');
    s[0] = 'B';
    s[1] = subtype;
    put(s, 6, name);
    put(s, 14, kv);
    put(s, 18, "01");
    put(s, 20, lp);
    put(s, 25, lq);
    put(s, 42, pg);
    put(s, 57, "1020"); // F4.3 -> 1.020
    return s;
}

std::string line_card(char type, const std::string& b1, const std::string& kv1,
                      const std::string& b2, const std::string& kv2,
                      const std::string& r, const std::string& x)
{
    std::string s(88, ' ');
    s[0] = type;
    put(s, 6, b1);
    put(s, 14, kv1);
    put(s, 19, b2);
    put(s, 27, kv2);
    put(s, 31, "1");
    put(s, 38, r);
    put(s, 44, x);
    put(s, 56, "000100");
    return s;
}

std::string transformer_card(const std::string& b1, const std::string& kv1,
                             const std::string& b2, const std::string& kv2)
{
    std::string s(92, ' ');
    s[0] = 'T';
    put(s, 6, b1);
    put(s, 14, kv1);
    put(s, 19, b2);
    put(s, 27, kv2);
    put(s, 31, "1");
    put(s, 38, "000100");
    put(s, 44, "010000");
    put(s, 62, "22000"); // F5.2 -> 220.00 kV
    put(s, 67, "03500"); // F5.2 -> 35.00 kV
    return s;
}

void test_parser()
{
    std::string text;
    text += "(POWERFLOW,CASEID=MINI)\n";
    text += "/MVA_BASE=100\n";
    text += bus('S', "SLACK   ", "0500", "00000", "00000", "00100") + "\n";
    text += bus(' ', "LOAD    ", "0500", "00050", "00020", "00000") + "\n";
    text += line_card('L', "SLACK   ", "0500", "LOAD    ", "0500",
                      "000100", "001000") + "\n";
    text += transformer_card("SLACK   ", "0500", "LOAD    ", "0500") + "\n";

    netmom::BpaParser p;
    const auto c = p.parse_text(text);
    assert(c.case_id == "MINI");
    assert(std::abs(c.base_mva - 100.0) < 1e-12);
    assert(c.buses.size() == 2);
    assert(c.lines.size() == 1);
    assert(c.transformers.size() == 1);
    assert(std::abs(c.transformers[0].tap1 - 220.0) < 1e-12);
    assert(std::abs(c.transformers[0].tap2 - 35.0) < 1e-12);
    assert(c.buses[0].bus_type == netmom::BusType::Slack);
    assert(std::abs(c.buses[1].load_p - 50.0) < 1e-12);
    assert(std::abs(c.lines[0].r - 0.001) < 1e-12); // F6.5 implicit decimal
    assert(std::abs(c.lines[0].x - 0.01) < 1e-12);
}

void test_topology()
{
    netmom::NetworkModel m;
    for (int i = 1; i <= 6; ++i) {
        netmom::Node n;
        n.id = i;
        n.name = "N" + std::to_string(i);
        n.base_kv = 220.0;
        m.nodes.push_back(n);
    }

    netmom::SwitchDevice sw1;
    sw1.id = 1; sw1.node1 = 1; sw1.node2 = 2; sw1.closed = true;
    m.switches.push_back(sw1);
    netmom::SwitchDevice sw2;
    sw2.id = 2; sw2.node1 = 2; sw2.node2 = 3; sw2.closed = false;
    m.switches.push_back(sw2);

    netmom::ZeroBranch z;
    z.id = 1; z.node1 = 3; z.node2 = 4;
    m.zero_branches.push_back(z);

    netmom::Branch l1;
    l1.id = 1; l1.node1 = 2; l1.node2 = 3; l1.x = 0.1;
    m.lines.push_back(l1);
    netmom::Branch l2;
    l2.id = 2; l2.node1 = 4; l2.node2 = 5; l2.x = 0.2;
    m.lines.push_back(l2);

    netmom::Generator g;
    g.id = 1; g.node = 1;
    m.generators.push_back(g);
    netmom::Load ld;
    ld.id = 1; ld.node = 5;
    m.loads.push_back(ld);

    const auto r = netmom::TopologyEngine::calculate(m);
    assert(r.calc_buses.size() == 4); // {1,2}, {3,4}, {5}, {6}
    assert(r.islands.size() == 2);    // {1..5}, {6}
    assert(r.node_to_calc_bus[1] == r.node_to_calc_bus[2]);
    assert(r.node_to_calc_bus[3] == r.node_to_calc_bus[4]);
    assert(r.node_to_calc_bus[2] != r.node_to_calc_bus[3]);
    assert(r.node_to_island[1] == r.node_to_island[5]);
    assert(r.node_to_island[6] != r.node_to_island[1]);

    const int main_island = r.node_to_island[1];
    assert(r.islands[static_cast<std::size_t>(main_island - 1)].energized);
    assert(r.islands[static_cast<std::size_t>(main_island - 1)].generator_count == 1);
    assert(r.islands[static_cast<std::size_t>(main_island - 1)].load_count == 1);
}

} // namespace

int main()
{
    test_parser();
    test_topology();
    std::cout << "netmom tests passed\n";
    return 0;
}
