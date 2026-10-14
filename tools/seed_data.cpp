// seed_data.cpp - imports the client's Bajaj RE supplier-receipt parts into parts.csv.
//
// Build:  g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic tools/seed_data.cpp -o seed_data
// Run:    ./seed_data [path/to/parts.csv]        (default: ./parts.csv)
//
// Schema written: id,part_name,vehicle_system,selling_price,quantity,min_stock,cost_price,compatible_models
//
// Behaviour
//  - Safe to run repeatedly: a receipt item already in the file (matched by its supplier part
//    number) is left untouched, so stock counted since the first import is never overwritten.
//  - Existing rows (e.g. the 6-column rows written by the app) are kept and padded to 8 columns.
//  - If the existing file has a row this tool cannot read, nothing is written.
//  - The old file is copied to parts.csv.bak before it is replaced.
//
// The app's part ID is a number (1..999999) but supplier numbers such as "36AA4028" are not, so each
// new part gets the next free internal ID (from 1001) and the supplier number is kept in the name
// as a "[36AA4028]" suffix. The app's search box therefore finds parts by supplier number too.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kHeader = "id,part_name,vehicle_system,selling_price,quantity,min_stock,cost_price,compatible_models";
constexpr size_t kColumns = 8;
constexpr int kFirstSeedId = 1001;
constexpr int kMaxId = 999999;          // limit enforced by the app
constexpr size_t kMaxNameLength = 80;   // limit enforced by the app
constexpr int kDefaultMinStock = 5;     // never higher than the quantity received
constexpr double kCostRatio = 0.80;     // estimate: cost = 80 % of SRP (edit once real costs are known)
constexpr const char* kModels = "Bajaj RE";

struct ReceiptItem {
    const char* partNo;
    const char* description;
    int quantity;
    double srp;
};

constexpr ReceiptItem kReceipt[] = {
    {"21160417", "PLATE - REINFORCEMENT", 5, 20.00},
    {"24151291", "CYLINDER ASSEMBLY", 3, 1700.00},
    {"24171055", "Rubber bush - cup", 30, 21.00},
    {"24171118", "Assembly rear axle", 10, 1502.00},
    {"24191105", "SCREW - SPECIAL", 10, 9.00},
    {"36AA4028", "Ignition lock (AA20)", 3, 735.00},
    {"36AZ0076", "BRAKE SHOE WITH", 2, 693.00},
    {"36AZ0078", "BRAKE SHOE WITH", 2, 693.00},
    {"36AZ0080", "BRAKE SHOE SPECIAL", 2, 609.00},
    {"36BA4039", "KIT PIVOT PIN WITH", 10, 452.00},
    {"36RA0002", "KIT FRICTION PLATE", 3, 890.00},
    {"AA101108", "SELECTOR - GEAR", 1, 207.00},
    {"AA101216", "Spring clutch", 10, 15.00},
    {"AA101722", "FLANGE ASSEMBLY", 1, 1026.00},
    {"AA101844", "SEAL HEAD COVER", 20, 116.00},
    {"AA102165", "SPRING CLUTCH", 10, 24.00},
    {"AA102166", "HUB CLUTCH", 3, 415.00},
    {"AA111082", "Armature assly.", 10, 1667.00},
    {"AA111086", "SPRING - PINION", 10, 38.00},
    {"AA191032", "LEVER - CLUTCH", 10, 134.00},
    {"AA191091", "CABLE ACCELERATOR", 2, 336.00},
    {"AA191092", "CABLE CLUTCH", 3, 614.00},
    {"AA191093", "CABLE GEAR BLACK", 5, 462.00},
    {"AA191094", "CABLE GEAR WHITE", 1, 420.00},
    {"AA191102", "SPEEDOMETER CABLE", 2, 462.00},
    {"AA191103", "CABLE CLUTCH", 6, 691.00},
    {"AA201185", "Fuse box assembly", 1, 557.00},
    {"AA201384", "BOX ASSEMBLY", 5, 588.00},
    {"AB101067", "CROSS COMMON", 1, 320.00},
    {"AB171044", "SHOCKABSORBER", 10, 893.00},
    {"AB201077", "Hazard switch", 2, 263.00},
    {"AF151089", "WHEEL RIM 3.00E", 2, 1632.00},
    {"AF191030", "CABLE ACCELERATOR", 3, 374.00},
    {"AL171081", "BUSH-LINK PIVOT", 10, 105.00},
    {"AL171095", "BEARING SLEEVE", 10, 109.00},
    {"AL241023", "WIPER ARM BLADE", 2, 418.00},
    {"AM121213", "Element Filter", 1, 679.00},
    {"AM201116", "Control switch RH", 2, 525.00},
    {"AN101135", "GASKET CYLINDER", 10, 47.00},
    {"AN101149", "GASKET HEAD C", 10, 189.00},
    {"AN101166", "Guide chain", 5, 86.00},
    {"AN101257", "Guide chain", 2, 172.00},
    {"AN191060", "Cable clutch", 2, 651.00},
    {"AN191080", "Cable Accelerator", 3, 368.00},
    {"AY141202", "Flange (wheel side)", 2, 838.00},
    {"AZ511201", "VALVE INTAKE", 2, 368.00},
    {"BH131803", "Cylinder comp.-TM", 1, 2520.00},
    {"DH101795", "CLUTCH LIFTER", 2, 221.00},
    {"DK101064", "CAM CHAIN 100L", 5, 495.00},
    {"JG511010", "O2 SENSOR FI", 2, 5154.00},
    {"JG611206", "MOTOR STEPPER", 5, 1532.00},
    {"JL351218", "IGNITION SPARK", 4, 264.00},
    {"KPBC6204", "BEARING - BALL", 5, 284.00},
    {"KPBC6301", "BALL - BEARING", 5, 224.00},
    {"22170434", "Bolt for axle", 40, 18.00},
    {"24151097", "Major kit", 10, 462.00},
    {"24151268", "BRAKE SHOE KIT", 30, 399.00},
    {"24191101", "Pulley gas control", 10, 30.00},
    {"24191184", "Choke cable comp", 10, 34.00},
    {"24201421", "REVERSE SWITCH", 5, 182.00},
    {"24231713", "Assly. Knob for rear", 20, 32.00},
    {"39029615", "Nut - hexagon :- M", 6, 6.00},
    {"39118215", "Nut", 20, 23.00},
    {"39193815", "Flange Nut", 5, 35.00},
    {"AA101472", "PLATE CLUTCH", 30, 53.00},
    {"AA102114", "CLIP NUT", 5, 39.00},
    {"AA191115", "CABLE REVERSE", 5, 546.00},
    {"AF201094", "Brake switch", 5, 284.00},
    {"AK201012", "Fuse blade type 10A", 100, 11.00},
    {"AL201012", "MAIN RELAY", 10, 153.00},
    {"AL201100", "Charger mobile", 5, 215.00},
    {"AN131007", "Oil plug", 10, 25.00},
    {"BA133062", "RUBBER BUFFER", 10, 36.00},
    {"JD401032", "LAMP 12V-5W", 30, 21.00},
    {"JY403604", "SENSOR ROLL OVER", 2, 461.00},
};

// ---------------------------------------------------------------- vehicle system
struct SystemRule {
    const char* system;
    std::vector<std::string_view> keywords;  // upper-case, matched anywhere in the description
};

// First matching rule wins, so the order matters (e.g. "Brake switch" is Electrical, not Braking).
const std::vector<SystemRule>& systemRules() {
    static const std::vector<SystemRule> rules = {
        {"Electrical", {"SWITCH", "FUSE", "RELAY", "LAMP", "SENSOR", "SPARK", "IGNITION", "ARMATURE", "STEPPER",
                        "CHARGER", "WIPER", "PINION", "BOX ASSEMBLY"}},
        {"Braking", {"BRAKE"}},
        {"Transmission", {"CLUTCH", "GEAR", "CABLE", "FRICTION PLATE", "REAR AXLE", "PULLEY"}},
        {"Engine", {"GASKET", "VALVE", "CHAIN", "FILTER", "HEAD COVER", "CYLINDER", "OIL PLUG"}},
    };
    return rules;
}
constexpr const char* kDefaultSystem = "Chassis";

// The description alone is ambiguous for these; the supplier part-number group decides.
struct SystemOverride {
    std::string_view partNo;
    const char* system;
};
constexpr SystemOverride kOverrides[] = {
    {"24151291", "Braking"},  // CYLINDER ASSEMBLY in the brake group (not an engine cylinder)
    {"24151097", "Braking"},  // Major kit, brake group
    {"AA101722", "Engine"},   // FLANGE ASSEMBLY, engine group
};

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string inferSystem(const ReceiptItem& item) {
    for (const SystemOverride& o : kOverrides)
        if (o.partNo == item.partNo) return o.system;
    const std::string name = toUpper(item.description);
    for (const SystemRule& rule : systemRules())
        for (std::string_view kw : rule.keywords)
            if (name.find(kw) != std::string::npos) return rule.system;
    return kDefaultSystem;
}

// ---------------------------------------------------------------- CSV helpers
using Row = std::vector<std::string>;

std::string csvEscape(const std::string& field) {
    if (field.find_first_of(",\"\r\n") == std::string::npos) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}

Row splitCsv(const std::string& line) {
    Row fields(1);
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { fields.back() += '"'; ++i; }
            else if (c == '"') quoted = false;
            else fields.back() += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') fields.emplace_back();
        else fields.back() += c;
    }
    return fields;
}

std::string joinCsv(const Row& row) {
    std::string out;
    for (size_t i = 0; i < row.size(); ++i) out += (i ? "," : "") + csvEscape(row[i]);
    return out;
}

std::string fixed2(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v;
    return os.str();
}

bool isId(const std::string& s) {
    return !s.empty() && s.size() <= 6 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// ---------------------------------------------------------------- existing parts.csv
// Returns false (and a message) if any data row is unreadable, so a hand-edited file is never damaged.
bool loadParts(const fs::path& file, std::vector<Row>& rows, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) { error = "cannot open " + file.string(); return false; }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        Row row = splitCsv(line);
        if (!isId(row[0])) {
            if (lineNo == 1) continue;  // header row
            error = "line " + std::to_string(lineNo) + ": unreadable part ID '" + row[0] + "'";
            return false;
        }
        if (row.size() < 6) { error = "line " + std::to_string(lineNo) + ": expected at least 6 columns"; return false; }
        row.resize(kColumns);
        if (row[6].empty()) row[6] = "0.00";  // cost_price of rows written by the 6-column app
        rows.push_back(std::move(row));
    }
    return true;
}

bool writeParts(const fs::path& file, const std::vector<Row>& rows) {
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << kHeader << '\n';
        for (const Row& r : rows) out << joinCsv(r) << '\n';
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {  // some platforms refuse to rename over an existing file
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
    }
    return !ec;
}

// "Fuse blade type 10A [AK201012]" -> "AK201012"
std::string supplierTag(const std::string& name) {
    const size_t open = name.rfind('[');
    const size_t close = name.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open) return "";
    return name.substr(open + 1, close - open - 1);
}

Row makeRow(int id, const ReceiptItem& item) {
    const double cost = std::round(item.srp * kCostRatio * 100.0) / 100.0;
    return {std::to_string(id),
            std::string(item.description) + " [" + item.partNo + "]",
            inferSystem(item),
            fixed2(item.srp),
            std::to_string(item.quantity),
            std::to_string(std::min(kDefaultMinStock, item.quantity)),
            fixed2(cost),
            kModels};
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path file = argc > 1 ? argv[1] : "parts.csv";

    std::vector<Row> rows;
    std::string error;
    const bool existed = fs::exists(file);
    if (existed && !loadParts(file, rows, error)) {
        std::cerr << "Nothing written - " << error << '\n';
        return 1;
    }

    std::set<std::string> known;
    int nextId = kFirstSeedId;
    for (const Row& r : rows) {
        known.insert(supplierTag(r[1]));
        nextId = std::max(nextId, std::stoi(r[0]) + 1);
    }

    int added = 0, skipped = 0;
    for (const ReceiptItem& item : kReceipt) {
        const Row row = makeRow(nextId, item);
        if (row[1].size() > kMaxNameLength || nextId > kMaxId) {
            std::cerr << "Skipped " << item.partNo << ": name too long or out of IDs\n";
            ++skipped;
        } else if (!known.insert(item.partNo).second) {
            ++skipped;  // already imported earlier
        } else {
            rows.push_back(row);
            ++nextId;
            ++added;
        }
    }

    if (added > 0) {
        std::error_code ec;
        if (existed) fs::copy_file(file, fs::path(file).concat(".bak"), fs::copy_options::overwrite_existing, ec);
        if (!writeParts(file, rows)) {
            std::cerr << "Could not write " << file << " - check permissions and free disk space.\n";
            return 1;
        }
    }

    std::cout << "parts.csv: " << file.string() << '\n'
              << "  added " << added << " part(s), skipped " << skipped << " (already present or invalid)\n"
              << "  total rows now: " << rows.size() << '\n';
    return 0;
}
