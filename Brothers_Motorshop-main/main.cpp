// ============================================================================
//  RE Parts Tracker
//  A Bajaj RE Components and Inventory Management System in C++
//
//  - Pure standard C++17, console (CLI) only, no external libraries.
//  - Parts are stored in a local CSV file (parts.csv), loaded on startup and
//    saved automatically after every create / update / delete.
//
//  Build (Windows): g++ -std=c++17 -Wall -Wextra main.cpp -o re_parts_tracker.exe -lws2_32
//  Build (Linux)  : g++ -std=c++17 -Wall -Wextra main.cpp -o re_parts_tracker
//  Run console    : re_parts_tracker.exe        Run in Chrome: re_parts_tracker.exe web
// ============================================================================

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

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
//  Web dashboard (optional): a tiny built-in HTTP server so the inventory can
//  be used from Chrome at http://localhost:8080. Pure C++ (Winsock on Windows,
//  BSD sockets elsewhere). HTML is generated by C++; there is no JavaScript.
//  It listens on 127.0.0.1 only, so it is not reachable from other computers.
// ----------------------------------------------------------------------------
namespace web {

std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                 std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

using Params = std::map<std::string, std::string>;

Params parseParams(const std::string& s) {
    Params p;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t amp = s.find('&', pos);
        if (amp == std::string::npos) amp = s.size();
        std::string pair = s.substr(pos, amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) p[urlDecode(pair)] = "";
            else p[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return p;
}

struct Request { std::string method, path; Params query, form; };
struct Response { int status = 200; std::string body; std::string location; };

bool toInt(const std::string& s, int lo, int hi, int& out) {
    try {
        size_t pos = 0;
        long long v = std::stoll(util::trim(s), &pos);
        if (pos != util::trim(s).size() || v < lo || v > hi) return false;
        out = static_cast<int>(v);
        return true;
    } catch (const std::exception&) { return false; }
}

bool toDouble(const std::string& s, double lo, double hi, double& out) {
    try {
        size_t pos = 0;
        double v = std::stod(util::trim(s), &pos);
        if (pos != util::trim(s).size() || v < lo || v > hi) return false;
        out = v;
        return true;
    } catch (const std::exception&) { return false; }
}

std::string get(const Params& p, const std::string& k) {
    auto it = p.find(k);
    return it == p.end() ? std::string() : it->second;
}

class Server {
public:
    explicit Server(InventoryManager& inv) : inv_(inv) {}

    // Blocks until the console is closed / Ctrl+C. Returns false if it could not start.
    bool run(int port, bool openBrowser) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::cerr << "  [!] Winsock start-up failed.\n"; return false; }
#endif
        sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv == kBad) { std::cerr << "  [!] Could not create socket.\n"; return false; }
#ifndef _WIN32
        int yes = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
#endif
        int bound = 0;
        for (int p = port; p < port + 10 && !bound; ++p) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(static_cast<unsigned short>(p));
            if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) bound = p;
        }
        if (!bound || listen(srv, 16) != 0) {
            std::cerr << "  [!] Could not open a port (is another copy already running?).\n";
            closeSock(srv);
            return false;
        }

        const std::string url = "http://localhost:" + std::to_string(bound);
        std::cout << "\n  RE Parts Tracker web dashboard is running at " << url
                  << "\n  Keep this window open. Press Ctrl+C to stop.\n";
        if (openBrowser) launchBrowser(url);

        while (true) {
            sock_t c = accept(srv, nullptr, nullptr);
            if (c == kBad) continue;
            serve(c);
            closeSock(c);
        }
    }

private:
#ifdef _WIN32
    using sock_t = SOCKET;
    static constexpr sock_t kBad = INVALID_SOCKET;
    static void closeSock(sock_t s) { closesocket(s); }
#else
    using sock_t = int;
    static constexpr sock_t kBad = -1;
    static void closeSock(sock_t s) { close(s); }
#endif

    InventoryManager& inv_;

    static void launchBrowser(const std::string& url) {
#ifdef _WIN32
        // Try Chrome first; fall back to the default browser.
        if (std::system(("start \"\" chrome \"" + url + "\"").c_str()) != 0)
            std::system(("start \"\" \"" + url + "\"").c_str());
#else
        (void)url;
#endif
    }

    // ---- HTTP plumbing ----
    void serve(sock_t c) {
#ifdef _WIN32
        DWORD tv = 3000;  // ms: Chrome sometimes opens idle connections
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#else
        timeval tv{3, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        std::string raw;
        if (!readRequest(c, raw)) return;
        Request req;
        if (!parseRequest(raw, req)) return;
        Response res = handle(req);
        sendResponse(c, res);
    }

    static bool readRequest(sock_t c, std::string& raw) {
        char buf[4096];
        size_t headerEnd = std::string::npos, need = 0;
        while (raw.size() < (1u << 20)) {
            int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) return false;
            raw.append(buf, static_cast<size_t>(n));
            if (headerEnd == std::string::npos) {
                headerEnd = raw.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    std::string head = util::toLower(raw.substr(0, headerEnd));
                    size_t len = 0, cl = head.find("content-length:");
                    if (cl != std::string::npos) len = static_cast<size_t>(std::atoi(head.c_str() + cl + 15));
                    need = headerEnd + 4 + len;
                }
            }
            if (headerEnd != std::string::npos && raw.size() >= need) return true;
        }
        return false;
    }

    static bool parseRequest(const std::string& raw, Request& r) {
        size_t eol = raw.find("\r\n");
        if (eol == std::string::npos) return false;
        std::istringstream first(raw.substr(0, eol));
        std::string target;
        first >> r.method >> target;
        if (r.method.empty() || target.empty()) return false;
        size_t q = target.find('?');
        r.path = target.substr(0, q);
        if (q != std::string::npos) r.query = parseParams(target.substr(q + 1));
        if (r.method == "POST") {
            size_t he = raw.find("\r\n\r\n");
            if (he != std::string::npos) r.form = parseParams(raw.substr(he + 4));
        }
        return true;
    }

    static void sendResponse(sock_t c, const Response& res) {
        std::ostringstream h;
        if (!res.location.empty()) {
            h << "HTTP/1.1 303 See Other\r\nLocation: " << res.location << "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else {
            const char* text = res.status == 200 ? "OK" : res.status == 404 ? "Not Found" : "Bad Request";
            h << "HTTP/1.1 " << res.status << ' ' << text << "\r\nContent-Type: text/html; charset=utf-8\r\n"
              << "Content-Length: " << res.body.size() << "\r\nConnection: close\r\n\r\n" << res.body;
        }
        const std::string out = h.str();
        size_t sent = 0;
        while (sent < out.size()) {
            int n = send(c, out.data() + sent, static_cast<int>(out.size() - sent), 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    // ---- Routing ----
    Response handle(const Request& r) {
        if (r.method == "GET") {
            if (r.path == "/") return page(dashboard(r));
            if (r.path == "/add") return page(addForm(""));
            if (r.path == "/edit") return page(editForm(r));
            if (r.path == "/delete") return page(deleteConfirm(r));
            if (r.path == "/favicon.ico") return Response{404, "", ""};
        } else if (r.method == "POST") {
            if (r.path == "/add") return doAdd(r);
            if (r.path == "/edit") return doEdit(r);
            if (r.path == "/adjust") return doAdjust(r);
            if (r.path == "/delete") return doDelete(r);
        }
        return Response{404, layout("Not found", "<div class='card'><h2>Page not found</h2><a class='btn' href='/'>Back to inventory</a></div>"), ""};
    }

    static Response page(const std::string& html) { return Response{200, html, ""}; }
    static Response redirect(const std::string& msg, const std::string& kind = "ok") {
        return Response{200, "", "/?msg=" + urlEncode(msg) + "&kind=" + kind};
    }
    static Response bad(const std::string& msg) {
        return Response{400, layout("Error", "<div class='card'><h2>Could not save</h2><p class='err'>" + htmlEscape(msg) +
                                              "</p><a class='btn' href='/'>Back to inventory</a></div>"), ""};
    }

    // ---- Pages ----
    static std::string layout(const std::string& title, const std::string& body, const std::string& flash = "", const std::string& kind = "ok") {
        std::string h =
            "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>" + htmlEscape(title) + " - RE Parts Tracker</title><style>"
            "*{box-sizing:border-box}body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f1f4f8;color:#1b2430}"
            "header{background:#12304f;color:#fff;padding:16px 28px;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px}"
            "header h1{margin:0;font-size:1.3rem}header small{opacity:.75}main{max-width:1100px;margin:22px auto;padding:0 16px}"
            ".card{background:#fff;border-radius:10px;padding:20px;margin-bottom:18px;box-shadow:0 1px 4px rgba(0,0,0,.1)}"
            ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin-bottom:18px}"
            ".stat{background:#fff;border-radius:10px;padding:14px 18px;box-shadow:0 1px 4px rgba(0,0,0,.1)}"
            ".stat b{display:block;font-size:1.5rem;color:#12304f}.stat span{font-size:.85rem;color:#5b6775}"
            ".stat.warn b{color:#c0392b}"
            "table{width:100%;border-collapse:collapse}th,td{padding:9px 8px;text-align:left;border-bottom:1px solid #e3e8ee;vertical-align:middle}"
            "th{background:#12304f;color:#fff;font-weight:600;font-size:.9rem}td.num,th.num{text-align:right}"
            "tr.low td{background:#fff3f1}tr.out td{background:#ffe1dd}"
            ".badge{padding:2px 9px;border-radius:12px;font-size:.78rem;font-weight:600}.ok{background:#e3f6e8;color:#1e7a3c}"
            ".lowb{background:#ffe0dc;color:#b3261e}"
            "input,select{padding:8px;border:1px solid #c5ced8;border-radius:6px;font-size:.95rem}input[type=number]{width:80px}"
            "label{display:block;margin:10px 0 3px;font-weight:600;font-size:.9rem}form.grid input,form.grid select{width:100%}"
            ".btn,button{display:inline-block;padding:8px 14px;border:0;border-radius:6px;background:#1f6fb2;color:#fff;font-size:.9rem;cursor:pointer;text-decoration:none}"
            ".btn:hover,button:hover{background:#17588f}.btn.ghost{background:#e3e8ee;color:#1b2430}.btn.danger,button.danger{background:#c0392b}"
            "button.minus{background:#8a5a00}.inline{display:inline-flex;gap:4px;align-items:center}"
            ".alert{background:#fff3f1;border-left:5px solid #c0392b;padding:12px 16px;border-radius:6px;margin-bottom:18px}"
            ".flash{padding:10px 16px;border-radius:6px;margin-bottom:16px}.flash.ok{background:#e3f6e8;color:#1e7a3c}.flash.err{background:#ffe0dc;color:#b3261e}"
            ".err{color:#b3261e}.bar{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.bar .spacer{flex:1}"
            ".tablewrap{overflow-x:auto}"
            "</style></head><body><header><h1>RE Parts Tracker</h1><small>Bajaj RE Components &amp; Inventory Management</small></header><main>";
        if (!flash.empty()) h += "<div class='flash " + std::string(kind == "err" ? "err" : "ok") + "'>" + htmlEscape(flash) + "</div>";
        h += body + "</main></body></html>";
        return h;
    }

    std::string dashboard(const Request& r) {
        const std::string q = util::trim(get(r.query, "q")), sys = get(r.query, "sys");
        std::vector<const Part*> rows;
        for (const Part& p : inv_.all()) {
            bool hit = q.empty() || util::containsIgnoreCase(p.name(), q) || util::containsIgnoreCase(p.category(), q) ||
                       std::to_string(p.id()).find(q) != std::string::npos;
            bool inSys = sys.empty() || util::equalsIgnoreCase(sys, p.category());
            if (hit && inSys) rows.push_back(&p);
        }
        std::sort(rows.begin(), rows.end(), [](const Part* a, const Part* b) { return a->id() < b->id(); });

        double value = 0;
        long units = 0;
        for (const Part& p : inv_.all()) { value += p.stockValue(); units += p.quantity(); }
        auto low = inv_.lowStockParts();

        std::string h;
        h += "<div class='stats'><div class='stat'><b>" + std::to_string(inv_.all().size()) + "</b><span>Part records</span></div>"
             "<div class='stat'><b>" + std::to_string(units) + "</b><span>Units in stock</span></div>"
             "<div class='stat'><b>PHP " + util::money(value) + "</b><span>Total stock value</span></div>"
             "<div class='stat" + std::string(low.empty() ? "" : " warn") + "'><b>" + std::to_string(low.size()) + "</b><span>Low / out of stock</span></div></div>";

        if (!low.empty()) {
            h += "<div class='alert'><b>LOW STOCK ALERT</b> - " + std::to_string(low.size()) + " part(s) below minimum:<ul>";
            for (const Part* p : low)
                h += "<li>[" + std::to_string(p->id()) + "] " + htmlEscape(p->name()) + ": " + std::to_string(p->quantity()) +
                     " in stock (min " + std::to_string(p->minStock()) + ")" + (p->quantity() == 0 ? " <b>- OUT OF STOCK</b>" : "") + "</li>";
            h += "</ul></div>";
        }

        h += "<div class='card'><form class='bar' method='get' action='/'>"
             "<input type='text' name='q' placeholder='Search ID, name or system' value='" + htmlEscape(q) + "' style='min-width:240px'>"
             "<select name='sys'><option value=''>All vehicle systems</option>";
        for (const auto& c : inv_.categories())
            h += "<option value='" + htmlEscape(c) + "'" + (util::equalsIgnoreCase(c, sys) ? " selected" : "") + ">" + htmlEscape(c) + "</option>";
        h += "</select><button type='submit'>Search</button><a class='btn ghost' href='/'>Clear</a><span class='spacer'></span>"
             "<a class='btn' href='/add'>+ Add new part</a></form></div>";

        h += "<div class='card tablewrap'><table><tr><th>ID</th><th>Part name</th><th>System</th><th class='num'>Price (PHP)</th>"
             "<th class='num'>Qty</th><th class='num'>Min</th><th>Status</th><th>Adjust stock</th><th>Actions</th></tr>";
        if (rows.empty()) h += "<tr><td colspan='9'>No parts found.</td></tr>";
        for (const Part* p : rows) {
            const std::string id = std::to_string(p->id());
            const bool out = p->quantity() == 0 && p->isLowStock();
            h += "<tr class='" + std::string(out ? "out" : p->isLowStock() ? "low" : "") + "'><td>" + id + "</td><td>" + htmlEscape(p->name()) +
                 "</td><td>" + htmlEscape(p->category()) + "</td><td class='num'>" + util::money(p->price()) + "</td><td class='num'>" +
                 std::to_string(p->quantity()) + "</td><td class='num'>" + std::to_string(p->minStock()) + "</td><td>" +
                 (p->isLowStock() ? (out ? "<span class='badge lowb'>OUT OF STOCK</span>" : "<span class='badge lowb'>LOW STOCK</span>")
                                  : "<span class='badge ok'>OK</span>") +
                 "</td><td><form class='inline' method='post' action='/adjust'><input type='hidden' name='id' value='" + id +
                 "'><input type='number' name='qty' value='1' min='1'><button name='op' value='add'>+</button>"
                 "<button class='minus' name='op' value='remove'>&minus;</button></form></td>"
                 "<td><a class='btn ghost' href='/edit?id=" + id + "'>Edit</a> <a class='btn danger' href='/delete?id=" + id + "'>Delete</a></td></tr>";
        }
        h += "</table></div>";
        return layout("Inventory", h, get(r.query, "msg"), get(r.query, "kind"));
    }

    static std::string systemsDatalist(const InventoryManager& inv) {
        std::string d = "<datalist id='systems'>";
        std::vector<std::string> all = ui::kSystems;
        for (const auto& c : inv.categories())
            if (std::none_of(all.begin(), all.end(), [&](const std::string& s) { return util::equalsIgnoreCase(s, c); })) all.push_back(c);
        for (const auto& s : all) d += "<option value='" + htmlEscape(s) + "'>";
        return d + "</datalist>";
    }

    std::string partForm(const std::string& title, const std::string& action, const Part* p, const std::string& error) {
        const bool edit = p != nullptr;
        std::string h = "<div class='card' style='max-width:560px'><h2>" + htmlEscape(title) + "</h2>";
        if (!error.empty()) h += "<p class='err'>" + htmlEscape(error) + "</p>";
        h += "<form class='grid' method='post' action='" + action + "'>";
        if (edit) h += "<input type='hidden' name='id' value='" + std::to_string(p->id()) + "'>";
        else h += "<label>Part ID</label><input type='number' name='id' min='1' required>";
        h += "<label>Part name</label><input type='text' name='name' required value='" + (edit ? htmlEscape(p->name()) : "") + "'>"
             "<label>Vehicle system / category</label><input type='text' name='category' list='systems' required value='" +
             (edit ? htmlEscape(p->category()) : "") + "'>" + systemsDatalist(inv_) +
             "<label>Unit price (PHP)</label><input type='number' name='price' step='0.01' min='0' required value='" + (edit ? util::money(p->price()) : "") + "'>"
             "<label>Quantity in stock</label><input type='number' name='qty' min='0' required value='" + (edit ? std::to_string(p->quantity()) : "") + "'>"
             "<label>Minimum stock threshold</label><input type='number' name='min' min='0' required value='" + (edit ? std::to_string(p->minStock()) : "") + "'>"
             "<p><button type='submit'>Save</button> <a class='btn ghost' href='/'>Cancel</a></p></form></div>";
        return layout(title, h);
    }

    std::string addForm(const std::string& error) { return partForm("Add new part", "/add", nullptr, error); }

    std::string editForm(const Request& r) {
        int id = 0;
        Part* p = toInt(get(r.query, "id"), 1, 999999999, id) ? inv_.findById(id) : nullptr;
        if (!p) return layout("Not found", "<div class='card'><h2>Part not found</h2><a class='btn' href='/'>Back</a></div>");
        return partForm("Update part " + std::to_string(id), "/edit", p, "");
    }

    std::string deleteConfirm(const Request& r) {
        int id = 0;
        Part* p = toInt(get(r.query, "id"), 1, 999999999, id) ? inv_.findById(id) : nullptr;
        if (!p) return layout("Not found", "<div class='card'><h2>Part not found</h2><a class='btn' href='/'>Back</a></div>");
        return layout("Delete part",
                      "<div class='card' style='max-width:560px'><h2>Delete this part?</h2><p><b>[" + std::to_string(id) + "] " + htmlEscape(p->name()) +
                      "</b><br>" + htmlEscape(p->category()) + " &middot; " + std::to_string(p->quantity()) + " in stock</p>"
                      "<p class='err'>This permanently removes the record.</p><form method='post' action='/delete'>"
                      "<input type='hidden' name='id' value='" + std::to_string(id) + "'><button class='danger' type='submit'>Yes, delete</button> "
                      "<a class='btn ghost' href='/'>Cancel</a></form></div>");
    }

    // ---- Form handlers ----
    // Shared validation for add/edit; fills 'out' or returns an error message.
    static std::string readPart(const Params& f, Part& out, bool needId) {
        int id = 0, qty = 0, mn = 0;
        double price = 0;
        if (needId || !get(f, "id").empty()) {
            if (!toInt(get(f, "id"), 1, 999999999, id)) return "Part ID must be a whole number of 1 or more.";
        }
        std::string name = util::trim(get(f, "name")), cat = util::trim(get(f, "category"));
        if (name.empty()) return "Part name cannot be empty.";
        if (cat.empty()) return "Vehicle system cannot be empty.";
        for (const auto& k : ui::kSystems) if (util::equalsIgnoreCase(k, cat)) cat = k;
        if (!toDouble(get(f, "price"), 0, 100000000, price)) return "Price must be a number of 0 or more.";
        if (!toInt(get(f, "qty"), 0, 1000000, qty)) return "Quantity must be a whole number of 0 or more.";
        if (!toInt(get(f, "min"), 0, 1000000, mn)) return "Minimum threshold must be a whole number of 0 or more.";
        out = Part(id, name, cat, price, qty, mn);
        return "";
    }

    Response doAdd(const Request& r) {
        Part p;
        std::string err = readPart(r.form, p, true);
        if (!err.empty()) return bad(err);
        if (!inv_.addPart(p)) return bad("A part with ID " + std::to_string(p.id()) + " already exists.");
        return redirect("Part " + std::to_string(p.id()) + " added and saved.");
    }

    Response doEdit(const Request& r) {
        int id = 0;
        Part* cur = toInt(get(r.form, "id"), 1, 999999999, id) ? inv_.findById(id) : nullptr;
        if (!cur) return bad("Part not found.");
        Part p;
        std::string err = readPart(r.form, p, false);
        if (!err.empty()) return bad(err);
        cur->setName(p.name());
        cur->setCategory(p.category());
        cur->setPrice(p.price());
        cur->setQuantity(p.quantity());
        cur->setMinStock(p.minStock());
        inv_.save();
        return redirect("Part " + std::to_string(id) + " updated and saved.");
    }

    Response doAdjust(const Request& r) {
        int id = 0, qty = 0;
        Part* p = toInt(get(r.form, "id"), 1, 999999999, id) ? inv_.findById(id) : nullptr;
        if (!p) return bad("Part not found.");
        if (!toInt(get(r.form, "qty"), 1, 1000000, qty)) return bad("Enter a whole number of 1 or more.");
        long long n = static_cast<long long>(p->quantity()) + (get(r.form, "op") == "remove" ? -qty : qty);
        if (n < 0) return redirect("Cannot remove " + std::to_string(qty) + ": only " + std::to_string(p->quantity()) + " in stock.", "err");
        p->setQuantity(static_cast<int>(n));
        inv_.save();
        return redirect("Stock of part " + std::to_string(id) + " is now " + std::to_string(n) + ".");
    }

    Response doDelete(const Request& r) {
        int id = 0;
        if (!toInt(get(r.form, "id"), 1, 999999999, id) || !inv_.removePart(id)) return bad("Part not found.");
        return redirect("Part " + std::to_string(id) + " deleted.");
    }
};

}  // namespace web

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
            auto c = ui::askInt("Select an option (0-10): ", 0, 10);
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
                case 10: web::Server(inv_).run(8080, true); break;
                case 0: std::cout << "\nAll changes are already saved. Goodbye!\n"; return;
            }
            ui::pause();
        }
    }

    // Starts the Chrome dashboard directly (run the program with the argument "web").
    void runWeb() {
        ui::showLowStockAlert(inv_);
        web::Server(inv_).run(8080, true);
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
                     " 10. Open web dashboard in Chrome\n"
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
int main(int argc, char** argv) {
    std::ios::sync_with_stdio(true);
    App app;
    const std::string arg = argc > 1 ? util::toLower(argv[1]) : "";
    if (arg == "web" || arg == "--web") app.runWeb();
    else app.run();
    return 0;
}
