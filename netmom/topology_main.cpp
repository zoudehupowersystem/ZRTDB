// ZRTDB NETMOM calculation-bus and electrical-island topology program
// SPDX-License-Identifier: Apache-2.0
#include "netmom/netmom.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage()
{
    std::cerr << "Usage: zrtdb_topology [--app NETMOM] [--zero-eps value]\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::string app = "NETMOM";
    double zero_eps = 1e-8;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app" && i + 1 < argc) {
            app = argv[++i];
        } else if (arg == "--zero-eps" && i + 1 < argc) {
            zero_eps = std::strtod(argv[++i], nullptr);
        } else {
            usage();
            return 2;
        }
    }

    netmom::Database db;
    std::string error;
    if (!db.open(app, "NETMOM", &error)) {
        std::cerr << "[TOPO][ERROR] " << error << '\n';
        return 3;
    }

    auto model = netmom::load_network(db, &error);
    if (!error.empty()) {
        std::cerr << "[TOPO][ERROR] " << error << '\n';
        return 4;
    }

    netmom::TopologyOptions options;
    options.zero_impedance_eps = zero_eps;
    auto result = netmom::TopologyEngine::calculate(model, options);

    if (!netmom::store_topology(db, result, &error)) {
        std::cerr << "[TOPO][ERROR] " << error << '\n';
        return 5;
    }

    std::cout << "[TOPO] physical_nodes=" << model.nodes.size()
              << " calculation_buses=" << result.calc_buses.size()
              << " islands=" << result.islands.size() << '\n';

    for (const auto& island : result.islands) {
        std::cout << "  island=" << island.id
                  << " calc_buses=" << island.calc_bus_count
                  << " nodes=" << island.node_count
                  << " generators=" << island.generator_count
                  << " loads=" << island.load_count
                  << " energized=" << (island.energized ? "yes" : "no")
                  << '\n';
    }

    return 0;
}
