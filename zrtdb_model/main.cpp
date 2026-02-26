// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "DatScanner.h"
#include "DbCompiler.h"
#include "Instantiator.h"

#include <iostream>

int main()
{
    zrtdb::init::DbCompiler compiler;
    zrtdb::init::Instantiator inst;

    auto datDir = compiler.datPath();
    auto scan = zrtdb::init::scan_dat_dir(datDir);

    std::cout << "[zrtdb_model] DAT dir: " << datDir << "\n";
    std::cout << "[zrtdb_model] Found DB DAT: " << scan.db_dats.size()
              << ", APP config: " << scan.app_dats.size() << "\n";

    for (const auto& f : scan.db_dats) {
        std::cout << "[zrtdb_model] compile DB: " << f.filename() << "\n";
        if (!compiler.compileDbFile(f)) {
            std::cerr << "[zrtdb_model] DB compile failed: " << f << "\n";
            return 2;
        }
    }

    std::vector<std::string> apps;
    for (const auto& f : scan.app_dats) {
        std::vector<std::string> outApps;
        std::cout << "[zrtdb_model] compile APP config: " << f.filename() << "\n";
        if (!compiler.compileAppConfig(f, outApps)) {
            std::cerr << "[zrtdb_model] APP compile failed: " << f << "\n";
            return 3;
        }
        for (auto& a : outApps) {
            if (!a.empty())
                apps.push_back(a);
        }
    }

    for (const auto& app : apps) {
        std::cout << "[zrtdb_model] instantiate APP: " << app << "\n";
        if (!inst.instantiate(app)) {
            std::cerr << "[zrtdb_model] instantiate failed: " << app << "\n";
            return 4;
        }
    }

    std::cout << "[zrtdb_model] done.\n";
    return 0;
}
