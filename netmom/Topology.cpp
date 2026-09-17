// ZRTDB NETMOM topology engine
// SPDX-License-Identifier: Apache-2.0
#include "netmom/netmom.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace netmom {
namespace {

class Dsu {
public:
    explicit Dsu(int n) : parent_(static_cast<std::size_t>(n + 1)),
                          rank_(static_cast<std::size_t>(n + 1), 0)
    {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    int find(int x)
    {
        if (parent_[static_cast<std::size_t>(x)] != x) {
            parent_[static_cast<std::size_t>(x)] = find(parent_[static_cast<std::size_t>(x)]);
        }
        return parent_[static_cast<std::size_t>(x)];
    }

    void unite(int a, int b)
    {
        if (a <= 0 || b <= 0) return;
        a = find(a);
        b = find(b);
        if (a == b) return;
        auto& ra = rank_[static_cast<std::size_t>(a)];
        auto& rb = rank_[static_cast<std::size_t>(b)];
        if (ra < rb) std::swap(a, b);
        parent_[static_cast<std::size_t>(b)] = a;
        if (ra == rb) ++rank_[static_cast<std::size_t>(a)];
    }

private:
    std::vector<int> parent_;
    std::vector<unsigned char> rank_;
};

} // namespace

TopologyResult TopologyEngine::calculate(const NetworkModel& model,
                                         const TopologyOptions& options)
{
    TopologyResult out;

    int max_node = 0;
    for (const auto& n : model.nodes) max_node = std::max(max_node, n.id);
    out.node_to_calc_bus.assign(static_cast<std::size_t>(max_node + 1), 0);
    out.node_to_island.assign(static_cast<std::size_t>(max_node + 1), 0);
    if (max_node == 0) return out;

    std::vector<const Node*> nodes(static_cast<std::size_t>(max_node + 1), nullptr);
    for (const auto& n : model.nodes) {
        if (n.id > 0 && n.id <= max_node) nodes[static_cast<std::size_t>(n.id)] = &n;
    }

    auto active_node = [&](int id) {
        return id > 0 && id <= max_node &&
               nodes[static_cast<std::size_t>(id)] != nullptr &&
               nodes[static_cast<std::size_t>(id)]->in_service;
    };

    Dsu physical(max_node);

    // A calculation bus is a connected component of physical nodes joined by
    // closed switching devices and explicit zero-impedance branches.
    for (const auto& sw : model.switches) {
        if (sw.in_service && sw.closed && active_node(sw.node1) && active_node(sw.node2)) {
            physical.unite(sw.node1, sw.node2);
        }
    }
    for (const auto& z : model.zero_branches) {
        if (z.in_service && z.closed && active_node(z.node1) && active_node(z.node2)) {
            physical.unite(z.node1, z.node2);
        }
    }

    // BPA/IPF often uses a tiny/zero L branch for bus ties. The importer
    // normally promotes exact-zero branches to ZBR, but this optional rule
    // makes the topology engine robust to directly-created NETMOM data.
    if (options.merge_zero_impedance_lines) {
        for (const auto& ln : model.lines) {
            if (!ln.in_service || !active_node(ln.node1) || !active_node(ln.node2)) continue;
            if (std::abs(ln.r) > options.zero_impedance_eps ||
                std::abs(ln.x) > options.zero_impedance_eps) {
                continue;
            }
            const double kv1 = nodes[static_cast<std::size_t>(ln.node1)]->base_kv;
            const double kv2 = nodes[static_cast<std::size_t>(ln.node2)]->base_kv;
            if (std::abs(kv1 - kv2) <= options.same_voltage_eps_kv) {
                physical.unite(ln.node1, ln.node2);
            }
        }
    }

    std::unordered_map<int, int> root_to_bus;
    for (int id = 1; id <= max_node; ++id) {
        if (!active_node(id)) continue;
        const int root_id = physical.find(id);
        auto [it, inserted] = root_to_bus.emplace(root_id,
                                                  static_cast<int>(root_to_bus.size()) + 1);
        const int cb = it->second;
        out.node_to_calc_bus[static_cast<std::size_t>(id)] = cb;
        if (inserted) {
            CalcBus item;
            item.id = cb;
            item.representative_node = id;
            item.base_kv = nodes[static_cast<std::size_t>(id)]->base_kv;
            item.node_count = 1;
            out.calc_buses.push_back(item);
        } else {
            auto& item = out.calc_buses[static_cast<std::size_t>(cb - 1)];
            ++item.node_count;
            item.representative_node = std::min(item.representative_node, id);
        }
    }

    Dsu electrical(static_cast<int>(out.calc_buses.size()));
    auto connect = [&](int n1, int n2) {
        if (!active_node(n1) || !active_node(n2)) return;
        const int a = out.node_to_calc_bus[static_cast<std::size_t>(n1)];
        const int b = out.node_to_calc_bus[static_cast<std::size_t>(n2)];
        if (a > 0 && b > 0 && a != b) electrical.unite(a, b);
    };

    for (const auto& ln : model.lines) {
        if (!ln.in_service) continue;
        // Zero-impedance lines already merged above if appropriate. Calling
        // unite here as well is harmless and preserves connectivity if they
        // were deliberately not merged.
        connect(ln.node1, ln.node2);
    }
    for (const auto& xf : model.transformers) {
        if (xf.in_service) connect(xf.node1, xf.node2);
    }

    std::unordered_map<int, int> island_root_to_id;
    for (auto& cb : out.calc_buses) {
        const int root_id = electrical.find(cb.id);
        auto [it, inserted] = island_root_to_id.emplace(
            root_id, static_cast<int>(island_root_to_id.size()) + 1);
        cb.island_id = it->second;
        if (inserted) {
            Island isl;
            isl.id = it->second;
            out.islands.push_back(isl);
        }
        auto& isl = out.islands[static_cast<std::size_t>(cb.island_id - 1)];
        ++isl.calc_bus_count;
        isl.node_count += cb.node_count;
    }

    for (int id = 1; id <= max_node; ++id) {
        const int cb = out.node_to_calc_bus[static_cast<std::size_t>(id)];
        if (cb > 0) {
            out.node_to_island[static_cast<std::size_t>(id)] =
                out.calc_buses[static_cast<std::size_t>(cb - 1)].island_id;
        }
    }

    for (const auto& gen : model.generators) {
        if (!gen.in_service || gen.node <= 0 || gen.node > max_node) continue;
        const int island = out.node_to_island[static_cast<std::size_t>(gen.node)];
        if (island <= 0) continue;
        auto& isl = out.islands[static_cast<std::size_t>(island - 1)];
        ++isl.generator_count;
        isl.energized = true;
    }
    for (const auto& load : model.loads) {
        if (!load.in_service || load.node <= 0 || load.node > max_node) continue;
        const int island = out.node_to_island[static_cast<std::size_t>(load.node)];
        if (island > 0) {
            ++out.islands[static_cast<std::size_t>(island - 1)].load_count;
        }
    }

    return out;
}

} // namespace netmom
