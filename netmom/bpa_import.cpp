// BPA power-flow -> ZRTDB NETMOM importer
// SPDX-License-Identifier: Apache-2.0
#include "netmom/netmom.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage()
{
    std::cerr
        << "Usage: zrtdb_bpa_import <case.dat> [--app NETMOM] [--zero-eps value] [--topology]\n"
        << "  Imports BPA/IPF fixed-column B*/L/E/T/TP records into the NETMOM ZRTDB DB.\n"
        << "  Exact/small same-voltage L/E branches with |R| and |X| <= zero-eps become ZBR.\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }

    std::string file = argv[1];
    std::string app = "NETMOM";
    double zero_eps = 1e-8;
    bool run_topology = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app" && i + 1 < argc) {
            app = argv[++i];
        } else if (arg == "--zero-eps" && i + 1 < argc) {
            zero_eps = std::strtod(argv[++i], nullptr);
        } else if (arg == "--topology") {
            run_topology = true;
        } else {
            usage();
            return 2;
        }
    }

    try {
        netmom::BpaParser parser;
        const auto input = parser.parse_file(file);
        for (const auto& w : input.warnings) {
            std::cerr << "[BPA][WARN] " << w << '\n';
        }

        netmom::Database db;
        std::string error;
        if (!db.open(app, "NETMOM", &error)) {
            std::cerr << "[NETMOM][ERROR] " << error << '\n';
            std::cerr << "Run zrtdb_model after installing DAT/NETMOM.DAT and APPDAT.json.\n";
            return 3;
        }
        if (!netmom::write_bpa_case(db, input, zero_eps, &error)) {
            std::cerr << "[NETMOM][ERROR] import failed: " << error << '\n';
            return 4;
        }

        std::cout << "[NETMOM] BPA import complete\n"
                  << "  buses=" << input.buses.size()
                  << " lines/equivalents=" << input.lines.size()
                  << " transformers=" << input.transformers.size() << '\n'
                  << "  ND=" << db.row_count("ND")
                  << " LN=" << db.row_count("LN")
                  << " ZBR=" << db.row_count("ZBR")
                  << " XF=" << db.row_count("XF")
                  << " LD=" << db.row_count("LD")
                  << " UN=" << db.row_count("UN")
                  << " CP=" << db.row_count("CP") << '\n';

        if (run_topology) {
            auto model = netmom::load_network(db, &error);
            if (!error.empty()) {
                std::cerr << "[NETMOM][ERROR] load network: " << error << '\n';
                return 5;
            }
            netmom::TopologyOptions opt;
            opt.zero_impedance_eps = zero_eps;
            auto result = netmom::TopologyEngine::calculate(model, opt);
            if (!netmom::store_topology(db, result, &error)) {
                std::cerr << "[NETMOM][ERROR] store topology: " << error << '\n';
                return 6;
            }
            std::cout << "  calc_buses=" << result.calc_buses.size()
                      << " islands=" << result.islands.size() << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "[BPA][ERROR] " << e.what() << '\n';
        return 1;
    }

    return 0;
}
