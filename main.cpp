// ============================================================================
//  RE Parts Tracker
//  A Bajaj RE Components and Inventory Management System in C++
//
//  - Pure standard C++17, console (CLI) only, no external libraries.
//  - Parts are stored in a local CSV file (parts.csv), loaded on startup and
//    saved automatically after every create / update / delete.
//
//  Build:  g++ -std=c++17 -Wall -Wextra main.cpp -o re_parts_tracker.exe
// ============================================================================

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
//  Small string helpers
// ----------------------------------------------------------------------------
namespace util {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Case-insensitive "haystack contains needle".
bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    return toLower(a) == toLower(b);
}

// Truncate with "..." so long text never breaks the table layout.
std::string clip(const std::string& s, size_t width) {
    if (s.size() <= width) return s;
    if (width <= 3) return s.substr(0, width);
    return s.substr(0, width - 3) + "...";
}

std::string money(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v;
    return os.str();
}

}  // namespace util

// ----------------------------------------------------------------------------
//  Part: one spare-part record
// ----------------------------------------------------------------------------
class Part {
public:
    Part() = default;
    Part(int id, std::string name, std::string category, double price, int quantity, int minStock)
        : id_(id), name_(std::move(name)), category_(std::move(category)),
          price_(price), quantity_(quantity), minStock_(minStock) {}

    int id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& category() const { return category_; }
    double price() const { return price_; }
    int quantity() const { return quantity_; }
    int minStock() const { return minStock_; }

    void setName(const std::string& v) { name_ = v; }
    void setCategory(const std::string& v) { category_ = v; }
    void setPrice(double v) { price_ = v; }
    void setQuantity(int v) { quantity_ = v; }
    void setMinStock(int v) { minStock_ = v; }

    // A part is "low" when stock has fallen below its minimum threshold.
    bool isLowStock() const { return quantity_ < minStock_; }

    double stockValue() const { return price_ * quantity_; }

    // ---- CSV serialisation (RFC-4180 style quoting) ----
    static std::string csvEscape(const std::string& field) {
        if (field.find_first_of(",\"\n\r") == std::string::npos) return field;
        std::string out = "\"";
        for (char c : field) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += '"';
        return out;
    }

    std::string toCSV() const {
        std::ostringstream os;
        os << id_ << ',' << csvEscape(name_) << ',' << csvEscape(category_) << ','
           << util::money(price_) << ',' << quantity_ << ',' << minStock_;
        return os.str();
    }

    // Splits one CSV line into fields, honouring quoted fields.
    static std::vector<std::string> splitCSV(const std::string& line) {
        std::vector<std::string> fields;
        std::string cur;
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                    else inQuotes = false;
                } else cur += c;
            } else if (c == '"') inQuotes = true;
            else if (c == ',') { fields.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        fields.push_back(cur);
        return fields;
    }

    // Builds a Part from a CSV line. Returns nullopt for header / malformed lines.
    // Accepts the current 6-column format and the legacy 4-column format
    // (id,name,price,stock) used by the earlier version of this project.
    static std::optional<Part> fromCSV(const std::string& line) {
        std::vector<std::string> f = splitCSV(line);
        try {
            if (f.size() >= 6) {
                return Part(std::stoi(util::trim(f[0])), util::trim(f[1]), util::trim(f[2]),
                            std::stod(util::trim(f[3])), std::stoi(util::trim(f[4])),
                            std::stoi(util::trim(f[5])));
            }
            if (f.size() == 4) {
                return Part(std::stoi(util::trim(f[0])), util::trim(f[1]), "General",
                            std::stod(util::trim(f[2])), std::stoi(util::trim(f[3])), 5);
            }
        } catch (const std::exception&) {
            // header row or corrupt line: skip
        }
        return std::nullopt;
    }

private:
    int id_ = 0;
    std::string name_;
    std::string category_;
    double price_ = 0.0;
    int quantity_ = 0;
    int minStock_ = 0;
};

// ----------------------------------------------------------------------------
//  InventoryManager: owns the collection, CRUD, search, alerts, persistence
// ----------------------------------------------------------------------------
class InventoryManager {
public:
    explicit InventoryManager(std::string filename = "parts.csv") : filename_(std::move(filename)) {
        load();
    }

    // ---------- Create ----------
    bool addPart(const Part& p) {
        if (findById(p.id())) return false;  // IDs must be unique
        parts_.push_back(p);
        save();
        return true;
    }

    // ---------- Read ----------
    const std::vector<Part>& all() const { return parts_; }

    Part* findById(int id) {
        for (Part& p : parts_) if (p.id() == id) return &p;
        return nullptr;
    }

    // ---------- Delete ----------
    bool removePart(int id) {
        auto it = std::remove_if(parts_.begin(), parts_.end(),
                                 [id](const Part& p) { return p.id() == id; });
        if (it == parts_.end()) return false;
        parts_.erase(it, parts_.end());
        save();
        return true;
    }

    // ---------- Search (all case-insensitive) ----------
    std::vector<const Part*> searchByName(const std::string& q) const {
        std::vector<const Part*> out;
        for (const Part& p : parts_) if (util::containsIgnoreCase(p.name(), q)) out.push_back(&p);
        return out;
    }

    std::vector<const Part*> searchBySystem(const std::string& q) const {
        std::vector<const Part*> out;
        for (const Part& p : parts_) if (util::containsIgnoreCase(p.category(), q)) out.push_back(&p);
        return out;
    }

    std::vector<const Part*> searchByIdText(const std::string& q) const {
        std::vector<const Part*> out;
        for (const Part& p : parts_) if (std::to_string(p.id()).find(q) != std::string::npos) out.push_back(&p);
        return out;
    }

    // ---------- Monitoring ----------
    std::vector<const Part*> lowStockParts() const {
        std::vector<const Part*> out;
        for (const Part& p : parts_) if (p.isLowStock()) out.push_back(&p);
        return out;
    }

    // Distinct vehicle systems currently in the data, sorted.
    std::vector<std::string> categories() const {
        std::vector<std::string> out;
        for (const Part& p : parts_) {
            bool seen = false;
            for (const auto& c : out) if (util::equalsIgnoreCase(c, p.category())) { seen = true; break; }
            if (!seen) out.push_back(p.category());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // ---------- Persistence ----------
    // Called after every change. Writes to a temp file first, then swaps it in,
    // so a crash mid-write cannot destroy the existing data file.
    bool save() const {
        const std::string tmp = filename_ + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) {
                std::cerr << "  [!] Could not write '" << tmp << "'. Data NOT saved.\n";
                return false;
            }
            out << "PartID,Name,Category,Price,Quantity,MinStock\n";
            for (const Part& p : parts_) out << p.toCSV() << '\n';
            out.flush();
            if (!out) {
                std::cerr << "  [!] Write error. Data NOT saved.\n";
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, filename_, ec);
        if (ec) {
            std::cerr << "  [!] Could not replace '" << filename_ << "': " << ec.message() << "\n";
            return false;
        }
        return true;
    }

    const std::string& filename() const { return filename_; }
    bool loadedFromFile() const { return loadedFromFile_; }

private:
    void load() {
        parts_.clear();
        std::ifstream in(filename_);
        if (in) {
            loadedFromFile_ = true;
            std::string line;
            while (std::getline(in, line)) {
                if (util::trim(line).empty()) continue;
                if (auto p = Part::fromCSV(line)) {
                    if (!findById(p->id())) parts_.push_back(*p);  // skip duplicate IDs
                }
            }
        }
        if (parts_.empty()) seedSampleData();
    }

    // First run (or empty file): start with a few typical Bajaj RE parts.
    void seedSampleData() {
        parts_ = {
            Part(101, "Engine Oil 1L",          "Engine",       450.00, 20, 8),
            Part(102, "Brake Pad Set",          "Braking",      850.50, 12, 5),
            Part(103, "Spark Plug",             "Electrical",   180.00, 30, 10),
            Part(104, "Clutch Plate Set",       "Transmission", 1250.00, 6, 4),
            Part(105, "Front Fork Assembly",    "Chassis",      3200.00, 3, 2),
            Part(106, "Headlight Bulb",         "Electrical",   120.00,  4, 10),
        };
        save();
    }

    std::string filename_;
    std::vector<Part> parts_;
    bool loadedFromFile_ = false;
};

// ----------------------------------------------------------------------------
//  Console UI helpers
// ----------------------------------------------------------------------------
namespace ui {

const std::vector<std::string> kSystems = {"Engine", "Braking", "Electrical", "Chassis", "Transmission", "Body", "Other"};

void line(char c = '=', int n = 92) { std::cout << std::string(n, c) << '\n'; }

void title(const std::string& t) {
    std::cout << '\n';
    line();
    std::cout << "  " << t << '\n';
    line();
}

void pause() {
    std::cout << "\nPress Enter to continue...";
    std::string dummy;
    std::getline(std::cin, dummy);
}

// Reads a full line. Returns false on EOF so loops can exit cleanly.
bool readLine(const std::string& prompt, std::string& out) {
    std::cout << prompt;
    if (!std::getline(std::cin, out)) return false;
    out = util::trim(out);
    return true;
}

// Re-prompts until a non-empty value is entered. Returns nullopt on EOF.
std::optional<std::string> askText(const std::string& prompt) {
    std::string s;
    while (true) {
        if (!readLine(prompt, s)) return std::nullopt;
        if (!s.empty()) return s;
        std::cout << "  Value cannot be empty.\n";
    }
}

// Integer within [lo, hi]. If allowBlank, an empty entry returns the sentinel "blank = true".
struct IntResult { bool ok = false; bool blank = false; int value = 0; };

IntResult askInt(const std::string& prompt, int lo, int hi, bool allowBlank = false) {
    std::string s;
    while (true) {
        if (!readLine(prompt, s)) return {};
        if (s.empty() && allowBlank) return {true, true, 0};
        try {
            size_t pos = 0;
            long long v = std::stoll(s, &pos);
            if (pos != s.size()) throw std::invalid_argument("trailing");
            if (v < lo || v > hi) {
                std::cout << "  Please enter a whole number between " << lo << " and " << hi << ".\n";
                continue;
            }
            return {true, false, static_cast<int>(v)};
        } catch (const std::exception&) {
            std::cout << "  Invalid number. Try again.\n";
        }
    }
}

struct DoubleResult { bool ok = false; bool blank = false; double value = 0; };

DoubleResult askDouble(const std::string& prompt, double lo, double hi, bool allowBlank = false) {
    std::string s;
    while (true) {
        if (!readLine(prompt, s)) return {};
        if (s.empty() && allowBlank) return {true, true, 0};
        try {
            size_t pos = 0;
            double v = std::stod(s, &pos);
            if (pos != s.size()) throw std::invalid_argument("trailing");
            if (v < lo || v > hi) {
                std::cout << "  Please enter an amount between " << lo << " and " << hi << ".\n";
                continue;
            }
            return {true, false, v};
        } catch (const std::exception&) {
            std::cout << "  Invalid amount. Try again.\n";
        }
    }
}

bool confirm(const std::string& prompt) {
    std::string s;
    while (true) {
        if (!readLine(prompt + " (y/n): ", s)) return false;
        s = util::toLower(s);
        if (s == "y" || s == "yes") return true;
        if (s == "n" || s == "no") return false;
        std::cout << "  Please answer y or n.\n";
    }
}

// Lets the user pick a vehicle system from a list or type a custom one.
// If allowBlank, an empty entry means "keep current" and returns "".
std::optional<std::string> askSystem(bool allowBlank = false) {
    std::cout << "  Vehicle systems:\n";
    for (size_t i = 0; i < kSystems.size(); ++i) std::cout << "    " << (i + 1) << ". " << kSystems[i] << '\n';
    std::string s;
    while (true) {
        if (!readLine(allowBlank ? "  Choose number or type a name (Enter = keep): "
                                 : "  Choose number or type a name: ", s)) return std::nullopt;
        if (s.empty()) {
            if (allowBlank) return std::string();
            std::cout << "  Value cannot be empty.\n";
            continue;
        }
        bool numeric = std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
        if (numeric) {
            size_t n = std::stoul(s);
            if (n >= 1 && n <= kSystems.size()) return kSystems[n - 1];
            std::cout << "  Pick 1-" << kSystems.size() << " or type a name.\n";
            continue;
        }
        // Normalise capitalisation if it matches a known system.
        for (const auto& k : kSystems) if (util::equalsIgnoreCase(k, s)) return k;
        return s;
    }
}

// ---- Table / detail rendering ----
void printTable(const std::vector<const Part*>& rows) {
    if (rows.empty()) { std::cout << "\n  (no parts to show)\n"; return; }
    std::cout << '\n';
    std::cout << std::left << std::setw(7) << "ID" << std::setw(28) << "Part Name" << std::setw(14) << "System"
              << std::right << std::setw(11) << "Price" << std::setw(7) << "Qty" << std::setw(7) << "Min"
              << "  " << std::left << "Status" << '\n';
    line('-');
    int low = 0;
    for (const Part* p : rows) {
        std::cout << std::left << std::setw(7) << p->id() << std::setw(28) << util::clip(p->name(), 26)
                  << std::setw(14) << util::clip(p->category(), 12)
                  << std::right << std::setw(11) << util::money(p->price()) << std::setw(7) << p->quantity()
                  << std::setw(7) << p->minStock() << "  " << std::left
                  << (p->isLowStock() ? (p->quantity() == 0 ? "!! OUT OF STOCK" : "!! LOW STOCK") : "OK") << '\n';
        if (p->isLowStock()) ++low;
    }
    line('-');
    std::cout << "  " << rows.size() << " part(s) shown, " << low << " flagged low/out of stock.   Prices in PHP.\n";
}

void printDetail(const Part& p) {
    std::cout << "\n  Part ID           : " << p.id()
              << "\n  Name              : " << p.name()
              << "\n  Vehicle System    : " << p.category()
              << "\n  Unit Price (PHP)  : " << util::money(p.price())
              << "\n  Quantity in Stock : " << p.quantity()
              << "\n  Minimum Threshold : " << p.minStock()
              << "\n  Stock Value (PHP) : " << util::money(p.stockValue())
              << "\n  Status            : "
              << (p.isLowStock() ? (p.quantity() == 0 ? "!! OUT OF STOCK" : "!! LOW STOCK - reorder needed") : "OK")
              << '\n';
}

// Banner shown on launch and after anything that changes stock levels.
void showLowStockAlert(const InventoryManager& inv) {
    auto low = inv.lowStockParts();
    if (low.empty()) return;
    std::cout << '\n';
    line('*');
    std::cout << "  *** LOW STOCK ALERT: " << low.size() << " part(s) below minimum threshold ***\n";
    line('*');
    for (const Part* p : low) {
        std::cout << "   - [" << p->id() << "] " << util::clip(p->name(), 30) << " : " << p->quantity()
                  << " in stock (min " << p->minStock() << ")"
                  << (p->quantity() == 0 ? "  << OUT OF STOCK" : "") << '\n';
    }
    line('*');
}

}  // namespace ui

// ----------------------------------------------------------------------------
//  Menu actions
// ----------------------------------------------------------------------------
class App {
public:
    App() : inv_("parts.csv") {}

    void run() {
        std::cout << "\n  Welcome to RE Parts Tracker - Bajaj RE Inventory Management\n";
        std::cout << "  Data file: " << inv_.filename() << "  ("
                  << inv_.all().size() << " part(s) loaded)\n";
        ui::showLowStockAlert(inv_);  // automatic check on launch

        while (true) {
            printMenu();
            auto c = ui::askInt("Select an option (0-9): ", 0, 9);
            if (!c.ok) break;  // EOF
            switch (c.value) {
                case 1: addPart(); break;
                case 2: viewAll(); break;
                case 3: viewDetail(); break;
                case 4: updatePart(); break;
                case 5: adjustStock(); break;
                case 6: deletePart(); break;
                case 7: search(); break;
                case 8: browseBySystem(); break;
                case 9: lowStockReport(); break;
                case 0: std::cout << "\nAll changes are already saved. Goodbye!\n"; return;
            }
            ui::pause();
        }
    }

private:
    InventoryManager inv_;

    void printMenu() const {
        ui::title("RE PARTS TRACKER - MAIN MENU");
        std::cout << "  1. Add new part\n"
                     "  2. View all parts\n"
                     "  3. View part details\n"
                     "  4. Update part details\n"
                     "  5. Adjust stock level\n"
                     "  6. Delete part\n"
                     "  7. Search (ID / name / vehicle system)\n"
                     "  8. Browse by vehicle system\n"
                     "  9. Low stock report\n"
                     "  0. Exit\n";
        ui::line('-');
    }

    // ---- Create ----
    void addPart() {
        ui::title("ADD NEW PART");
        auto id = ui::askInt("  Part ID (number): ", 1, 999999999);
        if (!id.ok) return;
        if (inv_.findById(id.value)) {
            std::cout << "  A part with ID " << id.value << " already exists. Use 'Update' to change it.\n";
            return;
        }
        auto name = ui::askText("  Part name: ");
        if (!name) return;
        auto sys = ui::askSystem();
        if (!sys) return;
        auto price = ui::askDouble("  Unit price (PHP): ", 0.0, 100000000.0);
        if (!price.ok) return;
        auto qty = ui::askInt("  Quantity in stock: ", 0, 1000000);
        if (!qty.ok) return;
        auto minq = ui::askInt("  Minimum stock threshold: ", 0, 1000000);
        if (!minq.ok) return;

        Part p(id.value, *name, *sys, price.value, qty.value, minq.value);
        if (inv_.addPart(p)) {
            std::cout << "\n  Part added and saved.\n";
            if (p.isLowStock()) std::cout << "  NOTE: this part is already below its minimum threshold.\n";
        }
    }

    // ---- Read ----
    void viewAll() {
        ui::title("ALL PARTS");
        std::vector<const Part*> rows;
        for (const Part& p : inv_.all()) rows.push_back(&p);
        std::sort(rows.begin(), rows.end(), [](const Part* a, const Part* b) { return a->id() < b->id(); });
        ui::printTable(rows);
    }

    void viewDetail() {
        ui::title("PART DETAILS");
        auto id = ui::askInt("  Part ID: ", 1, 999999999);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { std::cout << "  No part with ID " << id.value << ".\n"; return; }
        ui::printDetail(*p);
    }

    // ---- Update ----
    void updatePart() {
        ui::title("UPDATE PART");
        auto id = ui::askInt("  Part ID to update: ", 1, 999999999);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { std::cout << "  No part with ID " << id.value << ".\n"; return; }
        ui::printDetail(*p);
        std::cout << "\n  Press Enter on any field to keep its current value.\n";

        std::string s;
        if (!ui::readLine("  New name [" + p->name() + "]: ", s)) return;
        std::string newName = s.empty() ? p->name() : s;

        std::cout << "  Current system: " << p->category() << '\n';
        auto sys = ui::askSystem(true);
        if (!sys) return;
        std::string newSys = sys->empty() ? p->category() : *sys;

        auto price = ui::askDouble("  New price [" + util::money(p->price()) + "]: ", 0.0, 100000000.0, true);
        if (!price.ok) return;
        auto qty = ui::askInt("  New quantity [" + std::to_string(p->quantity()) + "]: ", 0, 1000000, true);
        if (!qty.ok) return;
        auto minq = ui::askInt("  New minimum threshold [" + std::to_string(p->minStock()) + "]: ", 0, 1000000, true);
        if (!minq.ok) return;

        p->setName(newName);
        p->setCategory(newSys);
        if (!price.blank) p->setPrice(price.value);
        if (!qty.blank) p->setQuantity(qty.value);
        if (!minq.blank) p->setMinStock(minq.value);
        inv_.save();

        std::cout << "\n  Part updated and saved.\n";
        ui::printDetail(*p);
        ui::showLowStockAlert(inv_);  // re-check after stock/threshold change
    }

    void adjustStock() {
        ui::title("ADJUST STOCK LEVEL");
        auto id = ui::askInt("  Part ID: ", 1, 999999999);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { std::cout << "  No part with ID " << id.value << ".\n"; return; }
        std::cout << "  " << p->name() << " - current stock: " << p->quantity()
                  << " (min " << p->minStock() << ")\n";
        std::cout << "  Enter a positive number to ADD stock (e.g. 10) or a negative one to REMOVE (e.g. -3).\n";
        auto delta = ui::askInt("  Change: ", -1000000, 1000000);
        if (!delta.ok) return;
        long long newQty = static_cast<long long>(p->quantity()) + delta.value;
        if (newQty < 0) {
            std::cout << "  Cannot remove " << -delta.value << ": only " << p->quantity() << " in stock.\n";
            return;
        }
        p->setQuantity(static_cast<int>(newQty));
        inv_.save();
        std::cout << "  Stock updated and saved. New quantity: " << p->quantity() << '\n';
        ui::showLowStockAlert(inv_);
    }

    // ---- Delete ----
    void deletePart() {
        ui::title("DELETE PART");
        auto id = ui::askInt("  Part ID to delete: ", 1, 999999999);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { std::cout << "  No part with ID " << id.value << ".\n"; return; }
        ui::printDetail(*p);
        if (ui::confirm("\n  Permanently delete this part?")) {
            inv_.removePart(id.value);
            std::cout << "  Part deleted and changes saved.\n";
        } else {
            std::cout << "  Deletion cancelled.\n";
        }
    }

    // ---- Search ----
    void search() {
        ui::title("SEARCH PARTS (case-insensitive)");
        std::cout << "  1. By Part ID\n  2. By Part name\n  3. By Vehicle system\n  4. Anywhere (ID, name or system)\n";
        auto mode = ui::askInt("  Search by (1-4): ", 1, 4);
        if (!mode.ok) return;
        auto q = ui::askText("  Search text: ");
        if (!q) return;

        std::vector<const Part*> rows;
        auto addUnique = [&rows](const std::vector<const Part*>& v) {
            for (const Part* p : v)
                if (std::find(rows.begin(), rows.end(), p) == rows.end()) rows.push_back(p);
        };
        switch (mode.value) {
            case 1: addUnique(inv_.searchByIdText(*q)); break;
            case 2: addUnique(inv_.searchByName(*q)); break;
            case 3: addUnique(inv_.searchBySystem(*q)); break;
            case 4:
                addUnique(inv_.searchByIdText(*q));
                addUnique(inv_.searchByName(*q));
                addUnique(inv_.searchBySystem(*q));
                break;
        }
        ui::printTable(rows);
    }

    void browseBySystem() {
        ui::title("BROWSE BY VEHICLE SYSTEM");
        auto cats = inv_.categories();
        if (cats.empty()) { std::cout << "  No parts in inventory.\n"; return; }
        for (const auto& c : cats) {
            std::cout << "\n  >> " << c << '\n';
            ui::printTable(inv_.searchBySystem(c));
        }
    }

    // ---- Monitoring ----
    void lowStockReport() {
        ui::title("LOW STOCK REPORT");
        auto low = inv_.lowStockParts();
        if (low.empty()) { std::cout << "\n  All parts are at or above their minimum threshold.\n"; return; }
        ui::printTable(low);
    }
};

// ----------------------------------------------------------------------------
int main() {
    std::ios::sync_with_stdio(true);
    App app;
    app.run();
    return 0;
}