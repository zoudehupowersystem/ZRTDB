// ZRTDB-backed NETMOM database adapter
// SPDX-License-Identifier: Apache-2.0
#include "netmom/netmom.hpp"

#include "zrtdb.h"
#include "zrtdb_const.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace netmom {
namespace {

std::string upper(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string trim(std::string s)
{
    auto ns = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    auto b = std::find_if(s.begin(), s.end(), ns);
    auto e = std::find_if(s.rbegin(), s.rend(), ns).base();
    return b < e ? std::string(b, e) : std::string {};
}

std::int64_t as_i64(const DbValue& v)
{
    if (const auto* p = std::get_if<std::int64_t>(&v)) return *p;
    if (const auto* p = std::get_if<double>(&v)) return static_cast<std::int64_t>(*p);
    if (const auto* p = std::get_if<std::string>(&v)) {
        try { return std::stoll(*p); } catch (...) { return 0; }
    }
    return 0;
}

double as_double(const DbValue& v)
{
    if (const auto* p = std::get_if<double>(&v)) return *p;
    if (const auto* p = std::get_if<std::int64_t>(&v)) return static_cast<double>(*p);
    if (const auto* p = std::get_if<std::string>(&v)) {
        try { return std::stod(*p); } catch (...) { return 0.0; }
    }
    return 0.0;
}

std::string as_string(const DbValue& v)
{
    if (const auto* p = std::get_if<std::string>(&v)) return *p;
    if (const auto* p = std::get_if<std::int64_t>(&v)) return std::to_string(*p);
    std::ostringstream os;
    os << std::get<double>(v);
    return os.str();
}

class WriteLock {
public:
    WriteLock() : locked_(SnapshotReadLock_() > 0) {}
    ~WriteLock() { if (locked_) SnapshotReadUnlock_(); }
    bool ok() const { return locked_; }
private:
    bool locked_;
};

const char* const kRecords[] = {
    "SYS", "CO", "DV", "ST", "KV", "ND", "CBTYP", "CB", "ZBR",
    "LN", "XF", "LD", "UN", "CP", "TOPO", "CALCBUS", "ISLAND"
};

} // namespace

Database::~Database()
{
    close();
}

int Database::record_index(std::string_view record) const
{
    if (db_index_ < 0 || db_index_ >= g_runtime_app.db_count) return -1;
    const std::string want = upper(record);
    const int begin = g_runtime_app.db_record_prefix[static_cast<std::size_t>(db_index_)];
    const int end = g_runtime_app.db_record_prefix[static_cast<std::size_t>(db_index_ + 1)];
    for (int r = begin; r < end; ++r) {
        if (upper(trim(g_runtime_app.record_ids[static_cast<std::size_t>(r)])) == want) return r;
    }
    return -1;
}

int Database::field_index(int record_index0, std::string_view field_name) const
{
    const std::string want = upper(field_name);
    for (int f = 0; f < g_runtime_app.field_count; ++f) {
        if (g_runtime_app.field_record_1based[static_cast<std::size_t>(f)] != record_index0 + 1) continue;
        if (upper(trim(g_runtime_app.field_ids[static_cast<std::size_t>(f)])) == want) return f;
    }
    return -1;
}

char* Database::field_address(int field_index0, int row1) const
{
    if (field_index0 < 0 || field_index0 >= g_runtime_app.field_count) return nullptr;
    const int p = g_runtime_app.field_partition_1based[static_cast<std::size_t>(field_index0)] - 1;
    if (p < 0 || p >= g_runtime_app.partition_count) return nullptr;
    const long base = g_runtime_app.partition_base_addrs[static_cast<std::size_t>(p)];
    if (base == 0) return nullptr;
    long offset = g_runtime_app.field_offset_bytes[static_cast<std::size_t>(field_index0)];
    if (g_runtime_app.field_record_1based[static_cast<std::size_t>(field_index0)] > 0) {
        const int bytes = static_cast<unsigned char>(
            g_runtime_app.field_item_bytes[static_cast<std::size_t>(field_index0)]);
        offset += static_cast<long>(row1 - 1) * bytes;
    }
    return reinterpret_cast<char*>(base + offset);
}

char* Database::lv_address(int record_index0) const
{
    if (record_index0 < 0 || record_index0 >= g_runtime_app.record_count) return nullptr;
    const long direct = g_runtime_app.record_lv_addrs[static_cast<std::size_t>(record_index0)];
    if (direct != 0) return reinterpret_cast<char*>(direct);
    const int p = g_runtime_app.record_lv_partition_1based[static_cast<std::size_t>(record_index0)] - 1;
    if (p < 0 || p >= g_runtime_app.partition_count) return nullptr;
    const long base = g_runtime_app.partition_base_addrs[static_cast<std::size_t>(p)];
    if (base == 0) return nullptr;
    return reinterpret_cast<char*>(
        base + g_runtime_app.record_lv_offset_bytes[static_cast<std::size_t>(record_index0)]);
}

bool Database::open(std::string app, std::string db, std::string* error)
{
    close();
    app_ = upper(app);
    db_ = upper(db);
    if (RegisterApp_(app_.c_str()) < 0) {
        if (error) *error = "RegisterApp_ failed for " + app_;
        return false;
    }

    for (int i = 0; i < g_runtime_app.db_count; ++i) {
        if (upper(trim(g_runtime_app.db_ids[static_cast<std::size_t>(i)])) == db_) {
            db_index_ = i;
            break;
        }
    }
    if (db_index_ < 0) {
        if (error) *error = "DB " + db_ + " is not attached to APP " + app_;
        free_MapMemory_();
        return false;
    }

    for (int p = 0; p < g_runtime_app.partition_count; ++p) {
        if (g_runtime_app.partition_db_1based[static_cast<std::size_t>(p)] != db_index_ + 1) continue;
        const std::string part = trim(g_runtime_app.partition_ids[static_cast<std::size_t>(p)]);
        const std::string spec = part + "/" + db_;
        char* addr = nullptr;
        if (MapMemory_(spec.c_str(), &addr) < 0 || addr == nullptr) {
            if (error) *error = "MapMemory_ failed for " + spec;
            free_MapMemory_();
            db_index_ = -1;
            return false;
        }
    }

    open_ = true;
    return true;
}

void Database::close()
{
    if (open_ || db_index_ >= 0) free_MapMemory_();
    open_ = false;
    db_index_ = -1;
    app_.clear();
    db_.clear();
}

int Database::row_count(std::string_view record) const
{
    const int r = record_index(record);
    char* p = lv_address(r);
    if (!p) return -1;
    int lv = 0;
    std::memcpy(&lv, p, sizeof(lv));
    const int cap = g_runtime_app.record_max_dim[static_cast<std::size_t>(r)];
    return std::clamp(lv, 0, cap);
}

int Database::capacity(std::string_view record) const
{
    const int r = record_index(record);
    if (r < 0) return -1;
    return g_runtime_app.record_max_dim[static_cast<std::size_t>(r)];
}

bool Database::clear_record(std::string_view record)
{
    const int r = record_index(record);
    char* p = lv_address(r);
    if (!p) return false;
    const int zero = 0;
    std::memcpy(p, &zero, sizeof(zero));
    return true;
}

bool Database::clear_all()
{
    bool ok = true;
    for (const char* rec : kRecords) {
        if (record_index(rec) >= 0) ok = clear_record(rec) && ok;
    }
    return ok;
}

bool Database::set(std::string_view record, int row1, std::string_view field_name,
                   const DbValue& value, std::string* error)
{
    const int r = record_index(record);
    if (r < 0) {
        if (error) *error = "record not found: " + std::string(record);
        return false;
    }
    if (row1 <= 0 || row1 > g_runtime_app.record_max_dim[static_cast<std::size_t>(r)]) {
        if (error) *error = "row outside record capacity";
        return false;
    }
    const int f = field_index(r, field_name);
    if (f < 0) {
        if (error) *error = "field not found: " + std::string(field_name);
        return false;
    }
    char* p = field_address(f, row1);
    if (!p) {
        if (error) *error = "field partition is not mapped";
        return false;
    }

    const char type = g_runtime_app.field_type[static_cast<std::size_t>(f)];
    const int bytes = static_cast<unsigned char>(
        g_runtime_app.field_item_bytes[static_cast<std::size_t>(f)]);
    try {
        switch (type) {
        case 'I': {
            const int x = static_cast<int>(as_i64(value));
            std::memcpy(p, &x, sizeof(x));
            return true;
        }
        case 'K': {
            const long x = static_cast<long>(as_i64(value));
            std::memcpy(p, &x, sizeof(x));
            return true;
        }
        case 'R': {
            const float x = static_cast<float>(as_double(value));
            std::memcpy(p, &x, sizeof(x));
            return true;
        }
        case 'D': {
            const double x = as_double(value);
            std::memcpy(p, &x, sizeof(x));
            return true;
        }
        case 'S':
        case 'C': {
            const std::string x = as_string(value);
            std::memset(p, 0, static_cast<std::size_t>(bytes));
            if (bytes > 0) {
                const auto n = std::min<std::size_t>(x.size(), static_cast<std::size_t>(bytes - 1));
                std::memcpy(p, x.data(), n);
            }
            return true;
        }
        default:
            if (error) *error = "unsupported ZRTDB field type";
            return false;
        }
    } catch (...) {
        if (error) *error = "value conversion failed";
        return false;
    }
}

DbValue Database::get(std::string_view record, int row1, std::string_view field_name,
                      std::string* error) const
{
    const int r = record_index(record);
    if (r < 0 || row1 <= 0 || row1 > row_count(record)) {
        if (error) *error = "record/row not found";
        return std::int64_t {0};
    }
    const int f = field_index(r, field_name);
    if (f < 0) {
        if (error) *error = "field not found: " + std::string(field_name);
        return std::int64_t {0};
    }
    char* p = field_address(f, row1);
    if (!p) {
        if (error) *error = "field partition is not mapped";
        return std::int64_t {0};
    }
    const char type = g_runtime_app.field_type[static_cast<std::size_t>(f)];
    const int bytes = static_cast<unsigned char>(
        g_runtime_app.field_item_bytes[static_cast<std::size_t>(f)]);
    switch (type) {
    case 'I': {
        int x = 0; std::memcpy(&x, p, sizeof(x)); return static_cast<std::int64_t>(x);
    }
    case 'K': {
        long x = 0; std::memcpy(&x, p, sizeof(x)); return static_cast<std::int64_t>(x);
    }
    case 'R': {
        float x = 0; std::memcpy(&x, p, sizeof(x)); return static_cast<double>(x);
    }
    case 'D': {
        double x = 0; std::memcpy(&x, p, sizeof(x)); return x;
    }
    case 'S':
    case 'C': {
        std::string x(p, p + bytes);
        const auto z = std::find(x.begin(), x.end(), '\0');
        x.erase(z, x.end());
        return x;
    }
    default:
        if (error) *error = "unsupported ZRTDB field type";
        return std::int64_t {0};
    }
}

int Database::append(std::string_view record,
                     const std::vector<std::pair<std::string, DbValue>>& fields,
                     std::string* error)
{
    const int r = record_index(record);
    if (r < 0) {
        if (error) *error = "record not found: " + std::string(record);
        return -1;
    }
    const int current = row_count(record);
    const int cap = capacity(record);
    if (current < 0 || current >= cap) {
        if (error) *error = "record capacity exceeded: " + std::string(record);
        return -1;
    }
    const int row = current + 1;
    for (const auto& [name, value] : fields) {
        if (!set(record, row, name, value, error)) return -1;
    }
    char* p = lv_address(r);
    if (!p) {
        if (error) *error = "LV address unavailable";
        return -1;
    }
    std::memcpy(p, &row, sizeof(row));
    return row;
}

int add_switch(Database& db, const SwitchDevice& device, std::string* error)
{
    const int type_row = device.kind == SwitchKind::Disconnector ? 2 : 1;
    const int id = device.id > 0 ? device.id : db.row_count("CB") + 1;
    return db.append("CB", {
        {"ID_CB", std::int64_t{id}},
        {"NAME_CB", device.name},
        {"CBTYP_IDX_CB", std::int64_t{type_row}},
        {"ND1_IDX_CB", std::int64_t{device.node1}},
        {"ND2_IDX_CB", std::int64_t{device.node2}},
        {"CLOSED_CB", std::int64_t{device.closed ? 1 : 0}},
        {"NORMAL_CLOSED_CB", std::int64_t{device.normal_closed ? 1 : 0}},
        {"IN_SERVICE_CB", std::int64_t{device.in_service ? 1 : 0}}
    }, error);
}

int add_zero_branch(Database& db, const ZeroBranch& branch, std::string* error)
{
    const int id = branch.id > 0 ? branch.id : db.row_count("ZBR") + 1;
    return db.append("ZBR", {
        {"ID_ZBR", std::int64_t{id}},
        {"NAME_ZBR", branch.name},
        {"ND1_IDX_ZBR", std::int64_t{branch.node1}},
        {"ND2_IDX_ZBR", std::int64_t{branch.node2}},
        {"SOURCE_ZBR", branch.source_type},
        {"CLOSED_ZBR", std::int64_t{branch.closed ? 1 : 0}},
        {"IN_SERVICE_ZBR", std::int64_t{branch.in_service ? 1 : 0}}
    }, error);
}

bool write_bpa_case(Database& db, const BpaCase& input,
                    double zero_impedance_eps, std::string* error)
{
    if (!db.is_open()) {
        if (error) *error = "NETMOM database is not open";
        return false;
    }
    WriteLock lock;
    if (!lock.ok()) {
        if (error) *error = "failed to acquire ZRTDB snapshot/write lock";
        return false;
    }
    if (!db.clear_all()) {
        if (error) *error = "failed to reset NETMOM records";
        return false;
    }

    auto add = [&](std::string_view rec,
                   std::initializer_list<std::pair<std::string, DbValue>> fields) -> int {
        return db.append(rec, std::vector<std::pair<std::string, DbValue>>(fields), error);
    };

    if (add("SYS", {{"ID_SYS", std::int64_t{1}},
                    {"NAME_SYS", input.case_id},
                    {"MVA_BASE_SYS", input.base_mva}}) < 0) return false;

    if (add("CO", {{"ID_CO", std::int64_t{1}}, {"NAME_CO", std::string("BPA_IMPORT")}}) < 0) return false;
    if (add("DV", {{"ID_DV", std::int64_t{1}}, {"NAME_DV", std::string("BPA_IMPORT")},
                   {"CO_IDX_DV", std::int64_t{1}}}) < 0) return false;
    if (add("ST", {{"ID_ST", std::int64_t{1}}, {"NAME_ST", std::string("BPA_SYSTEM")},
                   {"DV_IDX_ST", std::int64_t{1}}}) < 0) return false;

    if (add("CBTYP", {{"ID_CBTYP", std::int64_t{1}}, {"NAME_CBTYP", std::string("BREAKER")},
                      {"KIND_CBTYP", std::int64_t{static_cast<int>(SwitchKind::Breaker)}}}) < 0) return false;
    if (add("CBTYP", {{"ID_CBTYP", std::int64_t{2}}, {"NAME_CBTYP", std::string("DISCONNECTOR")},
                      {"KIND_CBTYP", std::int64_t{static_cast<int>(SwitchKind::Disconnector)}}}) < 0) return false;

    std::unordered_map<long long, int> kv_to_row;
    auto ensure_kv = [&](double kv) -> int {
        const long long key = std::llround(kv * 1000.0);
        auto it = kv_to_row.find(key);
        if (it != kv_to_row.end()) return it->second;
        std::ostringstream name;
        name << std::fixed << std::setprecision(3) << kv << "kV";
        const int row = add("KV", {{"ID_KV", std::int64_t{static_cast<std::int64_t>(kv_to_row.size() + 1)}},
                                   {"NAME_KV", name.str()},
                                   {"ST_IDX_KV", std::int64_t{1}},
                                   {"BASEKV_KV", kv}});
        if (row > 0) kv_to_row.emplace(key, row);
        return row;
    };

    std::unordered_map<std::string, int> node_by_key;
    int load_id = 0, gen_id = 0, shunt_id = 0;
    for (const auto& b : input.buses) {
        const int kv_row = ensure_kv(b.base_kv);
        if (kv_row <= 0) return false;
        const int node_id = db.row_count("ND") + 1;
        const int row = add("ND", {{"ID_ND", std::int64_t{node_id}},
                                   {"NAME_ND", b.name},
                                   {"KV_IDX_ND", std::int64_t{kv_row}},
                                   {"BASEKV_ND", b.base_kv},
                                   {"ZONE_ND", b.zone},
                                   {"BUS_TYPE_ND", std::int64_t{static_cast<int>(b.bus_type)}},
                                   {"IN_SERVICE_ND", std::int64_t{1}},
                                   {"SOURCE_ND", std::string("BPA")}});
        if (row <= 0) return false;
        node_by_key[bus_key(b.name, b.base_kv)] = row;

        if (std::abs(b.load_p) > 1e-12 || std::abs(b.load_q) > 1e-12) {
            ++load_id;
            if (add("LD", {{"ID_LD", std::int64_t{load_id}},
                           {"NAME_LD", b.name + "_LOAD"},
                           {"ND_IDX_LD", std::int64_t{row}},
                           {"P_MW_LD", b.load_p},
                           {"Q_MVAR_LD", b.load_q},
                           {"IN_SERVICE_LD", std::int64_t{1}}}) < 0) return false;
        }
        if (std::abs(b.gen_p) > 1e-12 || std::abs(b.gen_q) > 1e-12 ||
            b.bus_type == BusType::PV || b.bus_type == BusType::Slack) {
            ++gen_id;
            if (add("UN", {{"ID_UN", std::int64_t{gen_id}},
                           {"NAME_UN", b.name + "_GEN"},
                           {"ND_IDX_UN", std::int64_t{row}},
                           {"P_MW_UN", b.gen_p},
                           {"Q_MVAR_UN", b.gen_q},
                           {"QMIN_MVAR_UN", b.q_min},
                           {"QMAX_MVAR_UN", b.q_max},
                           {"VSET_PU_UN", b.v_set},
                           {"BUS_TYPE_UN", std::int64_t{static_cast<int>(b.bus_type)}},
                           {"IN_SERVICE_UN", std::int64_t{1}}}) < 0) return false;
        }
        if (std::abs(b.shunt_g) > 1e-12 || std::abs(b.shunt_b) > 1e-12) {
            ++shunt_id;
            if (add("CP", {{"ID_CP", std::int64_t{shunt_id}},
                           {"NAME_CP", b.name + "_SHUNT"},
                           {"ND_IDX_CP", std::int64_t{row}},
                           {"G_PU_CP", b.shunt_g},
                           {"B_PU_CP", b.shunt_b},
                           {"IN_SERVICE_CP", std::int64_t{1}}}) < 0) return false;
        }
    }

    auto endpoint = [&](std::string_view name, double kv) -> int {
        auto it = node_by_key.find(bus_key(name, kv));
        return it == node_by_key.end() ? 0 : it->second;
    };

    int line_id = 0, zbr_id = 0;
    for (const auto& l : input.lines) {
        const int n1 = endpoint(l.bus1, l.kv1);
        const int n2 = endpoint(l.bus2, l.kv2);
        if (n1 <= 0 || n2 <= 0) {
            if (error) *error = "BPA branch endpoint not found: " + l.bus1 + " -> " + l.bus2;
            return false;
        }
        const bool same_kv = std::abs(l.kv1 - l.kv2) <= 1e-3;
        const bool zero = same_kv && std::abs(l.r) <= zero_impedance_eps &&
                          std::abs(l.x) <= zero_impedance_eps;
        std::ostringstream nm;
        nm << l.bus1 << "-" << l.bus2;
        if (!l.circuit.empty()) nm << "-" << l.circuit;
        if (zero) {
            ++zbr_id;
            if (add("ZBR", {{"ID_ZBR", std::int64_t{zbr_id}},
                            {"NAME_ZBR", nm.str()},
                            {"ND1_IDX_ZBR", std::int64_t{n1}},
                            {"ND2_IDX_ZBR", std::int64_t{n2}},
                            {"SOURCE_ZBR", l.type},
                            {"CLOSED_ZBR", std::int64_t{1}},
                            {"IN_SERVICE_ZBR", std::int64_t{1}}}) < 0) return false;
        } else {
            ++line_id;
            if (add("LN", {{"ID_LN", std::int64_t{line_id}},
                           {"NAME_LN", nm.str()},
                           {"SOURCE_LN", l.type},
                           {"ND1_IDX_LN", std::int64_t{n1}},
                           {"ND2_IDX_LN", std::int64_t{n2}},
                           {"CIRCUIT_LN", l.circuit},
                           {"R_PU_LN", l.r}, {"X_PU_LN", l.x},
                           {"G1_PU_LN", l.g1}, {"B1_PU_LN", l.b1},
                           {"G2_PU_LN", l.g2}, {"B2_PU_LN", l.b2},
                           {"RATING_LN", l.rating},
                           {"IN_SERVICE_LN", std::int64_t{1}}}) < 0) return false;
        }
    }

    int xf_id = 0;
    for (const auto& t : input.transformers) {
        const int n1 = endpoint(t.bus1, t.kv1);
        const int n2 = endpoint(t.bus2, t.kv2);
        if (n1 <= 0 || n2 <= 0) {
            if (error) *error = "BPA transformer endpoint not found: " + t.bus1 + " -> " + t.bus2;
            return false;
        }
        ++xf_id;
        std::ostringstream nm;
        nm << t.bus1 << "-" << t.bus2;
        if (!t.circuit.empty()) nm << "-" << t.circuit;
        const double angle = t.phase_shifter ? t.tap1 : 0.0;
        if (add("XF", {{"ID_XF", std::int64_t{xf_id}},
                       {"NAME_XF", nm.str()},
                       {"ND1_IDX_XF", std::int64_t{n1}},
                       {"ND2_IDX_XF", std::int64_t{n2}},
                       {"CIRCUIT_XF", t.circuit},
                       {"R_PU_XF", t.r}, {"X_PU_XF", t.x},
                       {"G_PU_XF", t.g}, {"B_PU_XF", t.b},
                       {"TAP1_XF", t.tap1}, {"TAP2_XF", t.tap2},
                       {"ANGLE_DEG_XF", angle},
                       {"RATING_MVA_XF", t.rating_mva},
                       {"PHASE_SHIFTER_XF", std::int64_t{t.phase_shifter ? 1 : 0}},
                       {"IN_SERVICE_XF", std::int64_t{1}}}) < 0) return false;
    }

    return true;
}

NetworkModel load_network(Database& db, std::string* error)
{
    NetworkModel m;
    auto i = [&](std::string_view r, int row, std::string_view f) {
        return static_cast<int>(as_i64(db.get(r, row, f, error)));
    };
    auto d = [&](std::string_view r, int row, std::string_view f) {
        return as_double(db.get(r, row, f, error));
    };
    auto s = [&](std::string_view r, int row, std::string_view f) {
        return as_string(db.get(r, row, f, error));
    };

    for (int row = 1, n = db.row_count("ND"); row <= n; ++row) {
        Node x;
        x.id = i("ND", row, "ID_ND");
        x.name = s("ND", row, "NAME_ND");
        x.base_kv = d("ND", row, "BASEKV_ND");
        x.zone = s("ND", row, "ZONE_ND");
        x.bus_type = static_cast<BusType>(i("ND", row, "BUS_TYPE_ND"));
        x.in_service = i("ND", row, "IN_SERVICE_ND") != 0;
        m.nodes.push_back(std::move(x));
    }
    std::unordered_map<int, SwitchKind> switch_kind_by_type;
    for (int row = 1, n = db.row_count("CBTYP"); row <= n; ++row) {
        const int kind = i("CBTYP", row, "KIND_CBTYP");
        switch_kind_by_type[row] = kind == static_cast<int>(SwitchKind::Disconnector)
            ? SwitchKind::Disconnector : SwitchKind::Breaker;
    }
    for (int row = 1, n = db.row_count("CB"); row <= n; ++row) {
        SwitchDevice x;
        x.id = i("CB", row, "ID_CB");
        x.name = s("CB", row, "NAME_CB");
        const int typ = i("CB", row, "CBTYP_IDX_CB");
        const auto kt = switch_kind_by_type.find(typ);
        x.kind = kt == switch_kind_by_type.end() ? SwitchKind::Breaker : kt->second;
        x.node1 = i("CB", row, "ND1_IDX_CB");
        x.node2 = i("CB", row, "ND2_IDX_CB");
        x.closed = i("CB", row, "CLOSED_CB") != 0;
        x.normal_closed = i("CB", row, "NORMAL_CLOSED_CB") != 0;
        x.in_service = i("CB", row, "IN_SERVICE_CB") != 0;
        m.switches.push_back(std::move(x));
    }
    for (int row = 1, n = db.row_count("ZBR"); row <= n; ++row) {
        ZeroBranch x;
        x.id = i("ZBR", row, "ID_ZBR");
        x.name = s("ZBR", row, "NAME_ZBR");
        x.node1 = i("ZBR", row, "ND1_IDX_ZBR");
        x.node2 = i("ZBR", row, "ND2_IDX_ZBR");
        x.source_type = s("ZBR", row, "SOURCE_ZBR");
        x.closed = i("ZBR", row, "CLOSED_ZBR") != 0;
        x.in_service = i("ZBR", row, "IN_SERVICE_ZBR") != 0;
        m.zero_branches.push_back(std::move(x));
    }
    for (int row = 1, n = db.row_count("LN"); row <= n; ++row) {
        Branch x;
        x.id = i("LN", row, "ID_LN");
        x.name = s("LN", row, "NAME_LN");
        x.source_type = s("LN", row, "SOURCE_LN");
        x.node1 = i("LN", row, "ND1_IDX_LN");
        x.node2 = i("LN", row, "ND2_IDX_LN");
        x.circuit = s("LN", row, "CIRCUIT_LN");
        x.r = d("LN", row, "R_PU_LN"); x.x = d("LN", row, "X_PU_LN");
        x.g1 = d("LN", row, "G1_PU_LN"); x.b1 = d("LN", row, "B1_PU_LN");
        x.g2 = d("LN", row, "G2_PU_LN"); x.b2 = d("LN", row, "B2_PU_LN");
        x.rating = d("LN", row, "RATING_LN");
        x.in_service = i("LN", row, "IN_SERVICE_LN") != 0;
        m.lines.push_back(std::move(x));
    }
    for (int row = 1, n = db.row_count("XF"); row <= n; ++row) {
        Transformer x;
        x.id = i("XF", row, "ID_XF");
        x.name = s("XF", row, "NAME_XF");
        x.node1 = i("XF", row, "ND1_IDX_XF");
        x.node2 = i("XF", row, "ND2_IDX_XF");
        x.circuit = s("XF", row, "CIRCUIT_XF");
        x.r = d("XF", row, "R_PU_XF"); x.x = d("XF", row, "X_PU_XF");
        x.g = d("XF", row, "G_PU_XF"); x.b = d("XF", row, "B_PU_XF");
        x.tap1 = d("XF", row, "TAP1_XF"); x.tap2 = d("XF", row, "TAP2_XF");
        x.angle_deg = d("XF", row, "ANGLE_DEG_XF");
        x.rating_mva = d("XF", row, "RATING_MVA_XF");
        x.phase_shifter = i("XF", row, "PHASE_SHIFTER_XF") != 0;
        x.in_service = i("XF", row, "IN_SERVICE_XF") != 0;
        m.transformers.push_back(std::move(x));
    }
    for (int row = 1, n = db.row_count("LD"); row <= n; ++row) {
        Load x;
        x.id = i("LD", row, "ID_LD"); x.name = s("LD", row, "NAME_LD");
        x.node = i("LD", row, "ND_IDX_LD");
        x.p_mw = d("LD", row, "P_MW_LD"); x.q_mvar = d("LD", row, "Q_MVAR_LD");
        x.in_service = i("LD", row, "IN_SERVICE_LD") != 0;
        m.loads.push_back(std::move(x));
    }
    for (int row = 1, n = db.row_count("UN"); row <= n; ++row) {
        Generator x;
        x.id = i("UN", row, "ID_UN"); x.name = s("UN", row, "NAME_UN");
        x.node = i("UN", row, "ND_IDX_UN");
        x.p_mw = d("UN", row, "P_MW_UN"); x.q_mvar = d("UN", row, "Q_MVAR_UN");
        x.q_min_mvar = d("UN", row, "QMIN_MVAR_UN");
        x.q_max_mvar = d("UN", row, "QMAX_MVAR_UN");
        x.v_set_pu = d("UN", row, "VSET_PU_UN");
        x.bus_type = static_cast<BusType>(i("UN", row, "BUS_TYPE_UN"));
        x.in_service = i("UN", row, "IN_SERVICE_UN") != 0;
        m.generators.push_back(std::move(x));
    }
    for (int row = 1, n = db.row_count("CP"); row <= n; ++row) {
        Shunt x;
        x.id = i("CP", row, "ID_CP"); x.name = s("CP", row, "NAME_CP");
        x.node = i("CP", row, "ND_IDX_CP");
        x.g_pu = d("CP", row, "G_PU_CP"); x.b_pu = d("CP", row, "B_PU_CP");
        x.in_service = i("CP", row, "IN_SERVICE_CP") != 0;
        m.shunts.push_back(std::move(x));
    }
    return m;
}

bool store_topology(Database& db, const TopologyResult& result, std::string* error)
{
    WriteLock lock;
    if (!lock.ok()) {
        if (error) *error = "failed to acquire ZRTDB snapshot/write lock";
        return false;
    }
    if (!db.clear_record("TOPO") || !db.clear_record("CALCBUS") || !db.clear_record("ISLAND")) {
        if (error) *error = "failed to clear topology result records";
        return false;
    }

    for (std::size_t node = 1; node < result.node_to_calc_bus.size(); ++node) {
        if (result.node_to_calc_bus[node] <= 0) continue;
        if (db.append("TOPO", {
                {"ND_IDX_TOPO", static_cast<std::int64_t>(node)},
                {"CALCBUS_ID_TOPO", static_cast<std::int64_t>(result.node_to_calc_bus[node])},
                {"ISLAND_ID_TOPO", static_cast<std::int64_t>(result.node_to_island[node])}
            }, error) < 0) return false;
    }
    for (const auto& cb : result.calc_buses) {
        if (db.append("CALCBUS", {
                {"ID_CALCBUS", std::int64_t{cb.id}},
                {"REP_ND_IDX_CALCBUS", std::int64_t{cb.representative_node}},
                {"ISLAND_ID_CALCBUS", std::int64_t{cb.island_id}},
                {"BASEKV_CALCBUS", cb.base_kv},
                {"NODE_COUNT_CALCBUS", std::int64_t{cb.node_count}}
            }, error) < 0) return false;
    }
    for (const auto& isl : result.islands) {
        if (db.append("ISLAND", {
                {"ID_ISLAND", std::int64_t{isl.id}},
                {"CALCBUS_COUNT_ISLAND", std::int64_t{isl.calc_bus_count}},
                {"NODE_COUNT_ISLAND", std::int64_t{isl.node_count}},
                {"GEN_COUNT_ISLAND", std::int64_t{isl.generator_count}},
                {"LOAD_COUNT_ISLAND", std::int64_t{isl.load_count}},
                {"ENERGIZED_ISLAND", std::int64_t{isl.energized ? 1 : 0}}
            }, error) < 0) return false;
    }
    return true;
}

} // namespace netmom
