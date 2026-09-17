// ZRTDB NETMOM extension
// Copyright (c) 2026 Dehu Zou (邹德虎)
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace netmom {

enum class BusType : int {
    PQ = 1,
    PV = 2,
    Slack = 3
};

enum class SwitchKind : int {
    Breaker = 1,
    Disconnector = 2
};

struct Node {
    int id = 0;
    std::string name;
    double base_kv = 0.0;
    std::string zone;
    BusType bus_type = BusType::PQ;
    bool in_service = true;
};

struct SwitchDevice {
    int id = 0;
    std::string name;
    SwitchKind kind = SwitchKind::Breaker;
    int node1 = 0;
    int node2 = 0;
    bool closed = true;
    bool normal_closed = true;
    bool in_service = true;
};

struct Branch {
    int id = 0;
    std::string name;
    std::string source_type = "L";
    int node1 = 0;
    int node2 = 0;
    std::string circuit;
    double r = 0.0;
    double x = 0.0;
    double g1 = 0.0;
    double b1 = 0.0;
    double g2 = 0.0;
    double b2 = 0.0;
    double rating = 0.0;
    bool in_service = true;
};

struct ZeroBranch {
    int id = 0;
    std::string name;
    int node1 = 0;
    int node2 = 0;
    std::string source_type;
    bool closed = true;
    bool in_service = true;
};

struct Transformer {
    int id = 0;
    std::string name;
    int node1 = 0;
    int node2 = 0;
    std::string circuit;
    double r = 0.0;
    double x = 0.0;
    double g = 0.0;
    double b = 0.0;
    double tap1 = 0.0;
    double tap2 = 0.0;
    double angle_deg = 0.0;
    double rating_mva = 0.0;
    bool phase_shifter = false;
    bool in_service = true;
};

struct Load {
    int id = 0;
    std::string name;
    int node = 0;
    double p_mw = 0.0;
    double q_mvar = 0.0;
    bool in_service = true;
};

struct Generator {
    int id = 0;
    std::string name;
    int node = 0;
    double p_mw = 0.0;
    double q_mvar = 0.0;
    double q_min_mvar = 0.0;
    double q_max_mvar = 0.0;
    double v_set_pu = 1.0;
    BusType bus_type = BusType::PV;
    bool in_service = true;
};

struct Shunt {
    int id = 0;
    std::string name;
    int node = 0;
    double g_pu = 0.0;
    double b_pu = 0.0;
    bool in_service = true;
};

struct NetworkModel {
    std::vector<Node> nodes;
    std::vector<SwitchDevice> switches;
    std::vector<ZeroBranch> zero_branches;
    std::vector<Branch> lines;
    std::vector<Transformer> transformers;
    std::vector<Load> loads;
    std::vector<Generator> generators;
    std::vector<Shunt> shunts;
};

struct CalcBus {
    int id = 0;
    int representative_node = 0;
    int island_id = 0;
    double base_kv = 0.0;
    int node_count = 0;
};

struct Island {
    int id = 0;
    int calc_bus_count = 0;
    int node_count = 0;
    int generator_count = 0;
    int load_count = 0;
    bool energized = false;
};

struct TopologyResult {
    // 1-based node id -> calculation bus id / island id. Element 0 is unused.
    std::vector<int> node_to_calc_bus;
    std::vector<int> node_to_island;
    std::vector<CalcBus> calc_buses;
    std::vector<Island> islands;
};

struct TopologyOptions {
    double zero_impedance_eps = 1e-8;
    double same_voltage_eps_kv = 1e-3;
    bool merge_zero_impedance_lines = true;
};

class TopologyEngine {
public:
    static TopologyResult calculate(const NetworkModel& model,
                                    const TopologyOptions& options = {});
};

// Parsed BPA/IPF fixed-column model.
struct BpaBus {
    std::string name;
    double base_kv = 0.0;
    std::string zone;
    BusType bus_type = BusType::PQ;
    double load_p = 0.0;
    double load_q = 0.0;
    double shunt_g = 0.0;
    double shunt_b = 0.0;
    double gen_p = 0.0;
    double gen_q = 0.0;
    double q_min = 0.0;
    double q_max = 0.0;
    double v_set = 1.0;
};

struct BpaLine {
    std::string type = "L";
    std::string bus1;
    double kv1 = 0.0;
    std::string bus2;
    double kv2 = 0.0;
    std::string circuit;
    int section = 0;
    double rating = 0.0;
    double r = 0.0;
    double x = 0.0;
    double g1 = 0.0;
    double b1 = 0.0;
    double g2 = 0.0;
    double b2 = 0.0;
};

struct BpaTransformer {
    bool phase_shifter = false;
    std::string bus1;
    double kv1 = 0.0;
    std::string bus2;
    double kv2 = 0.0;
    std::string circuit;
    int section = 0;
    double rating_mva = 0.0;
    double r = 0.0;
    double x = 0.0;
    double g = 0.0;
    double b = 0.0;
    double tap1 = 0.0;
    double tap2 = 0.0;
};

struct BpaCase {
    std::string case_id = "BPA_CASE";
    double base_mva = 100.0;
    std::vector<BpaBus> buses;
    std::vector<BpaLine> lines;
    std::vector<BpaTransformer> transformers;
    std::vector<std::string> warnings;
};

class BpaParser {
public:
    BpaCase parse_file(const std::filesystem::path& path) const;
    BpaCase parse_text(std::string_view text) const;
};

using DbValue = std::variant<std::int64_t, double, std::string>;

class Database {
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(std::string app = "NETMOM", std::string db = "NETMOM", std::string* error = nullptr);
    void close();
    bool is_open() const { return open_; }

    int row_count(std::string_view record) const;
    int capacity(std::string_view record) const;
    bool clear_record(std::string_view record);
    bool clear_all();

    int append(std::string_view record,
               const std::vector<std::pair<std::string, DbValue>>& fields,
               std::string* error = nullptr);

    bool set(std::string_view record, int row1, std::string_view field,
             const DbValue& value, std::string* error = nullptr);

    DbValue get(std::string_view record, int row1, std::string_view field,
                std::string* error = nullptr) const;

private:
    int db_index_ = -1;
    bool open_ = false;
    std::string app_;
    std::string db_;

    int record_index(std::string_view record) const;
    int field_index(int record_index0, std::string_view field) const;
    char* field_address(int field_index0, int row1) const;
    char* lv_address(int record_index0) const;
};

int add_switch(Database& db, const SwitchDevice& device,
               std::string* error = nullptr);

int add_zero_branch(Database& db, const ZeroBranch& branch,
                    std::string* error = nullptr);

bool write_bpa_case(Database& db, const BpaCase& input,
                    double zero_impedance_eps = 1e-8,
                    std::string* error = nullptr);

NetworkModel load_network(Database& db, std::string* error = nullptr);

bool store_topology(Database& db, const TopologyResult& result,
                    std::string* error = nullptr);

std::string bus_key(std::string_view name, double kv);

} // namespace netmom
