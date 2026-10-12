// ============================================================================
//  RE Parts Tracker  v2.0
//  A Bajaj RE Components and Inventory Management System in C++
//
//  - Standard C++17, no 3rd-party libraries. Console (CLI) app PLUS an optional
//    local web dashboard you can open in Chrome (plain sockets, no GUI toolkit).
//  - NEW: login / logout system (salted + iterated SHA-256 password hashes,
//    constant-time comparison, hidden password entry, attempt lockout).
//  - NEW: redesigned console UI (double-border banners, boxed tables, status
//    panels, colour when the terminal supports it).
//  - Parts are stored in parts.csv: loaded on start-up and saved
//    automatically after every create / update / delete.
//
//  Build (Windows, MSYS2/MinGW-w64 g++ 9 or newer):
//      g++ -std=c++17 -Wall -Wextra main.cpp -o re_parts_tracker.exe -lws2_32
//  Build (Linux / macOS):
//      g++ -std=c++17 -Wall -Wextra main.cpp -o re_parts_tracker
//  Console-only build (no sockets, no -lws2_32 needed):
//      g++ -std=c++17 -Wall -Wextra -DRE_NO_WEB main.cpp -o re_parts_tracker.exe
//
//  Run console : re_parts_tracker.exe          (menu option 11 starts the web mode)
//  Run in Chrome: re_parts_tracker.exe web     (opens http://localhost:8080)
//
//  Default login:  admin / password123   (see the "auth" section to change it)
//  Forgot password (web): answer the security question "What is the secret admin code?"
//      default answer RE-ADMIN-2026 (Authenticator::kRecoveryAnswer); new passwords are saved in users.csv.
//  The console and the web dashboard use the same accounts.
//  Tip: use a terminal at least 100 columns wide.
// ============================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Platform headers are used ONLY to (a) hide typed passwords, (b) enable
// UTF-8 box drawing / colour on the Windows console. Everything degrades
// gracefully if they are unavailable (e.g. output redirected to a file).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX  // MinGW's headers already define it
#define NOMINMAX
#endif
#ifndef RE_NO_WEB
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <termios.h>
#include <unistd.h>
#ifndef RE_NO_WEB
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif
#endif

// ----------------------------------------------------------------------------
//  Limits shared by the input prompts and the CSV loader
// ----------------------------------------------------------------------------
namespace limits {
constexpr int kMaxId = 999999;
constexpr int kMaxQty = 999999;
constexpr double kMaxPrice = 9999999.99;
}  // namespace limits

// ----------------------------------------------------------------------------
//  Small string helpers (UTF-8 aware so box-drawing text always lines up)
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

bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) { return toLower(a) == toLower(b); }

// Plain "1234.50" - used for the CSV file (no thousands separators!).
std::string money(double v) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << v;
    return os.str();
}

// Display form "1,234.50".
std::string moneyFmt(double v) {
    std::string s = money(v);
    size_t dot = s.find('.');
    std::string ip = s.substr(0, dot), frac = dot == std::string::npos ? "" : s.substr(dot);
    std::string out;
    int count = 0;
    for (size_t i = ip.size(); i-- > 0;) {
        out.insert(out.begin(), ip[i]);
        if (++count % 3 == 0 && i > 0 && std::isdigit(static_cast<unsigned char>(ip[i - 1]))) out.insert(out.begin(), ',');
    }
    return out + frac;
}

// Number of visible characters (counts UTF-8 code points, not bytes).
size_t displayWidth(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// First n code points of s.
std::string takeChars(const std::string& s, size_t n) {
    size_t i = 0, count = 0;
    while (i < s.size()) {
        size_t j = i + 1;
        while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) ++j;
        if (count == n) break;
        ++count;
        i = j;
    }
    return s.substr(0, i);
}

// Truncate with "..." so long text never breaks the table layout.
std::string clip(const std::string& s, size_t width) {
    if (displayWidth(s) <= width) return s;
    if (width <= 3) return takeChars(s, width);
    return takeChars(s, width - 3) + "...";
}

std::string rep(const std::string& unit, size_t n) {
    std::string out;
    out.reserve(unit.size() * n);
    for (size_t i = 0; i < n; ++i) out += unit;
    return out;
}

std::string padRight(const std::string& s, size_t w) {
    std::string t = clip(s, w);
    return t + std::string(w - displayWidth(t), ' ');
}

std::string padLeft(const std::string& s, size_t w) {
    std::string t = clip(s, w);
    return std::string(w - displayWidth(t), ' ') + t;
}

std::string center(const std::string& s, size_t w) {
    std::string t = clip(s, w);
    size_t total = w - displayWidth(t), left = total / 2;
    return std::string(left, ' ') + t + std::string(total - left, ' ');
}

// Greedy word-wrap to the given width.
std::vector<std::string> wrap(const std::string& text, size_t width) {
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word, cur;
    while (words >> word) {
        if (displayWidth(word) > width) word = clip(word, width);
        if (cur.empty()) cur = word;
        else if (displayWidth(cur) + 1 + displayWidth(word) <= width) cur += ' ' + word;
        else { lines.push_back(cur); cur = word; }
    }
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    return lines;
}

}  // namespace util

// ----------------------------------------------------------------------------
//  Authentication
//
//  Passwords are never stored in plain text. Each account stores a per-user
//  salt and the result of 20,000 rounds of salted SHA-256 (a simple key
//  stretching scheme, implemented here so no external library is needed).
//  Login compares hashes in constant time and does the same amount of work
//  for unknown usernames, so the response does not reveal which part was wrong.
//
//  To add / change an account: run   re_parts_tracker.exe --make-credential
//  and paste the line it prints into defaultUsers() below.
// ----------------------------------------------------------------------------
namespace auth {

constexpr int kHashRounds = 20000;

using Digest = std::array<std::uint8_t, 32>;

Digest sha256(const std::string& message) {
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::string data = message;
    const std::uint64_t bitLen = static_cast<std::uint64_t>(message.size()) * 8u;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));

    auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };

    for (size_t off = 0; off < data.size(); off += 64) {
        std::uint32_t w[64];
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = off + i * 4;
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[p])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(data[p + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(data[p + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(data[p + 3]));
        }
        for (size_t i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    Digest out{};
    for (size_t i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

std::string toHex(const Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : d) { s += digits[b >> 4]; s += digits[b & 0x0F]; }
    return s;
}

// digest0 = SHA256(salt + pw);  digest(n+1) = SHA256(digest(n) + salt + pw)
std::string hashPassword(const std::string& salt, const std::string& password) {
    Digest d = sha256(salt + password);
    for (int i = 0; i < kHashRounds; ++i) {
        std::string in(d.begin(), d.end());
        in += salt;
        in += password;
        d = sha256(in);
    }
    return toHex(d);
}

// Compares every character regardless of where the first difference is.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

struct Credential {
    std::string username;
    std::string salt;
    std::string hash;  // hex of hashPassword(salt, password)
};

// ---- Pre-configured accounts (default: admin / password123) ----
std::vector<Credential> defaultUsers() {
    return {
        {"admin", "RE-PT:admin:9f3a1c7e", "e9f105d04cdf49007ae23da0762c82875784a557ea121b4c46118158c1548ed7"},
    };
}

std::string randomSalt(const std::string& username);  // defined just below

class Authenticator {
public:
    // ---- "Forgot password" recovery (preset security question) ----
    // CHANGE kRecoveryAnswer before handing the app to anyone else.
    static constexpr const char* kRecoveryQuestion = "What is the secret admin code?";
    static constexpr const char* kRecoveryAnswer = "RE-ADMIN-2026";   // case-insensitive
    static constexpr const char* kUsersFile = "users.csv";            // saved password changes

    Authenticator() : users_(defaultUsers()) {
        recoverySalt_ = "RE-PT:recovery:5c1e84ab";
        recoveryHash_ = hashPassword(recoverySalt_, normAnswer(kRecoveryAnswer));
        loadSaved();
    }

    // Returns the canonical username on success.
    std::optional<std::string> login(const std::string& username, const std::string& password) const {
        const Credential* found = find(username);
        // Always perform the hash so timing does not reveal whether the user exists.
        const std::string salt = found ? found->salt : std::string("RE-PT:no-such-user");
        const std::string computed = hashPassword(salt, password);
        const std::string expected = found ? found->hash : std::string(64, '0');
        const bool ok = constantTimeEquals(computed, expected) && found != nullptr;
        if (ok) return found->username;
        return std::nullopt;
    }

    // Checks the recovery answer for an existing account (constant-time).
    std::optional<std::string> verifyRecovery(const std::string& username, const std::string& answer) const {
        const Credential* found = find(username);
        const bool ok = constantTimeEquals(hashPassword(recoverySalt_, normAnswer(answer)), recoveryHash_) && found != nullptr;
        if (ok) return found->username;
        return std::nullopt;
    }

    // Sets a new password (fresh salt) and saves it to users.csv. False = unknown user.
    bool setPassword(const std::string& username, const std::string& newPassword) {
        for (Credential& c : users_) {
            if (util::equalsIgnoreCase(c.username, username)) {
                c.salt = randomSalt(c.username);
                c.hash = hashPassword(c.salt, newPassword);
                saveOk_ = saveUsers();
                return true;
            }
        }
        return false;
    }
    bool lastSaveOk() const { return saveOk_; }

private:
    std::vector<Credential> users_;
    std::string recoverySalt_, recoveryHash_;
    bool saveOk_ = true;

    static std::string normAnswer(const std::string& s) { return util::toLower(util::trim(s)); }

    const Credential* find(const std::string& username) const {
        for (const Credential& c : users_)
            if (util::equalsIgnoreCase(c.username, username)) return &c;
        return nullptr;
    }

    bool saveUsers() const {
        std::ofstream out(kUsersFile, std::ios::trunc);
        if (!out) return false;
        for (const Credential& c : users_) out << c.username << ',' << c.salt << ',' << c.hash << '\n';
        out.flush();
        return static_cast<bool>(out);
    }

    // users.csv (username,salt,hash) overrides the built-in default password.
    void loadSaved() {
        std::ifstream in(kUsersFile);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t c1 = line.find(','), c2 = c1 == std::string::npos ? c1 : line.find(',', c1 + 1);
            if (c2 == std::string::npos) continue;
            const std::string name = line.substr(0, c1), salt = line.substr(c1 + 1, c2 - c1 - 1), hash = line.substr(c2 + 1);
            if (hash.size() != 64) continue;
            for (Credential& u : users_)
                if (util::equalsIgnoreCase(u.username, name)) { u.salt = salt; u.hash = hash; }
        }
    }
};

std::string randomSalt(const std::string& username) {
    std::random_device rd;
    static const char* digits = "0123456789abcdef";
    std::string s = "RE-PT:" + username + ":";
    for (int i = 0; i < 16; ++i) s += digits[rd() % 16];
    return s;
}

}  // namespace auth

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
    bool isOutOfStock() const { return isLowStock() && quantity_ == 0; }
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

    static bool isHeaderLine(const std::string& line) {
        auto f = splitCSV(line);
        if (f.empty()) return false;
        const std::string first = util::trim(f[0]);
        return util::equalsIgnoreCase(first, "PartID") || util::equalsIgnoreCase(first, "ID");
    }

    // Builds a Part from a CSV line. Returns nullopt for header / malformed /
    // out-of-range lines. Accepts the current 6-column format and the legacy
    // 4-column format (id,name,price,stock) used by the earliest version.
    static std::optional<Part> fromCSV(const std::string& line) {
        std::vector<std::string> f = splitCSV(line);
        try {
            int id = 0, qty = 0, mn = 5;
            double price = 0;
            std::string name, cat = "General";
            if (f.size() >= 6) {
                id = std::stoi(util::trim(f[0]));
                name = util::trim(f[1]);
                cat = util::trim(f[2]);
                price = std::stod(util::trim(f[3]));
                qty = std::stoi(util::trim(f[4]));
                mn = std::stoi(util::trim(f[5]));
            } else if (f.size() == 4) {
                id = std::stoi(util::trim(f[0]));
                name = util::trim(f[1]);
                price = std::stod(util::trim(f[2]));
                qty = std::stoi(util::trim(f[3]));
            } else {
                return std::nullopt;
            }
            if (name.empty() || cat.empty()) return std::nullopt;
            if (id < 1 || id > limits::kMaxId) return std::nullopt;
            if (!(price >= 0.0 && price <= limits::kMaxPrice)) return std::nullopt;  // also rejects NaN
            if (qty < 0 || qty > limits::kMaxQty || mn < 0 || mn > limits::kMaxQty) return std::nullopt;
            return Part(id, name, cat, price, qty, mn);
        } catch (const std::exception&) {
            return std::nullopt;  // header row or corrupt line
        }
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
    explicit InventoryManager(std::string filename = "parts.csv") : filename_(std::move(filename)) { load(); }

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
        auto it = std::remove_if(parts_.begin(), parts_.end(), [id](const Part& p) { return p.id() == id; });
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
    bool save() {
        lastSaveOk_ = false;
        if (saveBlocked_) {
            lastError_ = "'" + filename_ + "' exists but could not be read, so saving is disabled to protect it.";
            return false;
        }
        const std::string tmp = filename_ + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) { lastError_ = "Could not write '" + tmp + "'."; return false; }
            out << "PartID,Name,Category,Price,Quantity,MinStock\n";
            for (const Part& p : parts_) out << p.toCSV() << '\n';
            out.flush();
            if (!out) { lastError_ = "Write error while saving '" + tmp + "'."; return false; }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, filename_, ec);
        if (ec) {  // some platforms refuse to rename over an existing file
            std::filesystem::remove(filename_, ec);
            std::filesystem::rename(tmp, filename_, ec);
        }
        if (ec) { lastError_ = "Could not replace '" + filename_ + "': " + ec.message(); return false; }
        lastSaveOk_ = true;
        lastError_.clear();
        return true;
    }

    const std::string& filename() const { return filename_; }
    bool lastSaveOk() const { return lastSaveOk_; }
    const std::string& lastError() const { return lastError_; }
    int skippedLines() const { return skipped_; }            // bad/duplicate rows ignored at load
    bool createdSampleData() const { return seeded_; }       // true on first run
    const std::string& loadProblem() const { return loadProblem_; }

private:
    void load() {
        parts_.clear();
        std::error_code ec;
        std::ifstream in(filename_);
        if (!in) {
            if (std::filesystem::exists(filename_, ec)) {
                // File is there but unreadable: never overwrite it with sample data.
                loadProblem_ = "'" + filename_ + "' exists but could not be opened. Check its permissions.";
                saveBlocked_ = true;
            } else {
                seedSampleData();  // first run
            }
            return;
        }
        std::string line;
        bool firstLine = true;
        while (std::getline(in, line)) {
            if (firstLine) {  // strip a UTF-8 BOM (written by Excel / Notepad)
                firstLine = false;
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                    static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
                    line.erase(0, 3);
            }
            if (util::trim(line).empty()) continue;
            if (auto p = Part::fromCSV(line)) {
                if (findById(p->id())) ++skipped_;  // duplicate ID
                else parts_.push_back(*p);
            } else if (!Part::isHeaderLine(line)) {
                ++skipped_;  // malformed row
            }
        }
        if (skipped_ > 0) {  // keep an untouched copy before the next save rewrites the file
            std::filesystem::copy_file(filename_, filename_ + ".bak",
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    // First run (no data file yet): start with a few typical Bajaj RE parts.
    void seedSampleData() {
        parts_ = {
            Part(101, "Engine Oil 1L",       "Engine",       450.00, 20, 8),
            Part(102, "Brake Pad Set",       "Braking",      850.50, 12, 5),
            Part(103, "Spark Plug",          "Electrical",   180.00, 30, 10),
            Part(104, "Clutch Plate Set",    "Transmission", 1250.00, 6, 4),
            Part(105, "Front Fork Assembly", "Chassis",      3200.00, 3, 2),
            Part(106, "Headlight Bulb",      "Electrical",   120.00,  4, 10),
        };
        seeded_ = true;
        save();
    }

    std::string filename_;
    std::vector<Part> parts_;
    bool lastSaveOk_ = true;
    bool saveBlocked_ = false;
    bool seeded_ = false;
    int skipped_ = 0;
    std::string lastError_;
    std::string loadProblem_;
};

// ----------------------------------------------------------------------------
//  Console UI: frames, banners, panels, tables, prompts
// ----------------------------------------------------------------------------
namespace ui {

constexpr int kWidth = 96;           // total width of every frame
constexpr int kInner = kWidth - 2;   // space between the two vertical borders

inline bool gAnsi = false;           // terminal understands escape codes
inline bool gColor = false;
inline std::string gUser;            // shown in the header while logged in

const std::vector<std::string> kSystems = {"Engine", "Braking", "Electrical", "Chassis",
                                           "Transmission", "Body", "Other"};

// ANSI colour codes (SGR parameters)
constexpr const char* kRed = "31";
constexpr const char* kGreen = "32";
constexpr const char* kYellow = "33";
constexpr const char* kCyan = "36";
constexpr const char* kBoldCyan = "1;36";
constexpr const char* kBoldRed = "1;31";
constexpr const char* kBoldYellow = "1;33";
constexpr const char* kBoldGreen = "1;32";
constexpr const char* kDim = "2";

std::string paint(const std::string& s, const char* code) {
    if (!gColor || code == nullptr || s.empty()) return s;
    return std::string("\033[") + code + "m" + s + "\033[0m";
}

// Prepares the console (UTF-8 + colour) and restores it on exit.
class ConsoleSession {
public:
    ConsoleSession() {
#if defined(_WIN32)
        oldCp_ = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            gAnsi = SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
        gAnsi = isatty(STDOUT_FILENO) != 0;
#endif
        gColor = gAnsi && std::getenv("NO_COLOR") == nullptr;
    }
    ~ConsoleSession() {
#if defined(_WIN32)
        if (oldCp_ != 0) SetConsoleOutputCP(oldCp_);
#endif
        if (gColor) std::cout << "\033[0m";
    }
    ConsoleSession(const ConsoleSession&) = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;

private:
#if defined(_WIN32)
    UINT oldCp_ = 0;
#endif
};

void clear() {
    if (gAnsi) std::cout << "\033[2J\033[3J\033[H" << std::flush;
    else std::cout << "\n\n";
}

// ---- Frames ----
struct Frame { const char *tl, *tr, *bl, *br, *h, *v, *lt, *rt; };
constexpr Frame kDouble{"╔", "╗", "╚", "╝", "═", "║", "╠", "╣"};
constexpr Frame kSingle{"┌", "┐", "└", "┘", "─", "│", "├", "┤"};

enum class Align { Left, Center, Right };

void top(const Frame& f, const std::string& title = "", const char* border = nullptr) {
    std::string mid;
    if (title.empty()) {
        mid = util::rep(f.h, kInner);
    } else {
        std::string t = util::clip(title, kInner - 6);
        size_t used = 1 + 2 + util::displayWidth(t) + 2;
        mid = std::string(f.h) + "[ " + t + " ]" + util::rep(f.h, kInner - used);
    }
    std::cout << paint(std::string(f.tl) + mid + f.tr, border) << '\n';
}

void bottom(const Frame& f, const char* border = nullptr) {
    std::cout << paint(std::string(f.bl) + util::rep(f.h, kInner) + f.br, border) << '\n';
}

void divider(const Frame& f, const char* border = nullptr) {
    std::cout << paint(std::string(f.lt) + util::rep(f.h, kInner) + f.rt, border) << '\n';
}

// A labelled divider inside a double frame:  ╟─ LABEL ─────────────╢
void section(const std::string& label, const char* border = nullptr) {
    std::string t = "─ " + util::clip(label, kInner - 6) + " ";
    size_t used = 3 + util::displayWidth(label);  // "─ " + label + " "
    std::cout << paint("╟" + t + util::rep("─", kInner - used) + "╢", border) << '\n';
}

// One framed text row (1-space margin on each side).
void row(const Frame& f, const std::string& text, const char* border = nullptr,
         const char* textColor = nullptr, Align a = Align::Left) {
    const size_t w = kInner - 2;
    std::string cell = a == Align::Center ? util::center(text, w)
                     : a == Align::Right  ? util::padLeft(text, w)
                                          : util::padRight(text, w);
    std::cout << paint(f.v, border) << ' ' << paint(cell, textColor) << ' ' << paint(f.v, border) << '\n';
}

// Row with text on the left and on the right.
void rowLR(const Frame& f, const std::string& left, const std::string& right,
           const char* border = nullptr, const char* textColor = nullptr) {
    const size_t w = kInner - 2;
    std::string r = util::clip(right, w / 2);
    std::string l = util::clip(left, w - util::displayWidth(r) - (r.empty() ? 0 : 1));
    size_t gap = w - util::displayWidth(l) - util::displayWidth(r);
    std::cout << paint(f.v, border) << ' ' << paint(l + std::string(gap, ' ') + r, textColor) << ' '
              << paint(f.v, border) << '\n';
}

// Boxed block of (word-wrapped) lines with a title in the top border.
void panel(const std::string& title, const std::vector<std::string>& lines, const char* color = nullptr,
           bool colorText = true) {
    top(kSingle, title, color);
    for (const std::string& l : lines)
        for (const std::string& w : util::wrap(l, kInner - 2)) row(kSingle, w, color, colorText ? color : nullptr);
    bottom(kSingle, color);
}

// ---- Screen headers / banners ----
void logo() {
    static const char* art[] = {
        R"( ___ ___   ___  _   ___ _____ ___   _____ ___    _   ___ _  _____ ___ )",
        R"(| _ \ __| | _ \/_\ | _ \_   _/ __| |_   _| _ \  /_\ / __| |/ / __| _ \)",
        R"(|   / _|  |  _/ _ \|   / | | \__ \   | | |   / / _ \ (__| ' <| _||   /)",
        R"(|_|_\___| |_|/_/ \_\_|_\ |_| |___/   |_| |_|_\/_/ \_\___|_|\_\___|_|_\)",
    };
    top(kDouble, "", kCyan);
    row(kDouble, "", kCyan);
    for (const char* line : art) row(kDouble, line, kCyan, kBoldCyan, Align::Center);
    row(kDouble, "", kCyan);
    row(kDouble, "Bajaj RE Components & Inventory Management System   v2.0", kCyan, nullptr, Align::Center);
    bottom(kDouble, kCyan);
}

// Compact header for every screen after login.
void screen(const std::string& title) {
    clear();
    top(kDouble, "", kCyan);
    rowLR(kDouble, "RE PARTS TRACKER  >  " + title, gUser.empty() ? "" : "User: " + gUser, kCyan, kBoldCyan);
    bottom(kDouble, kCyan);
}

void sectionTitle(const std::string& text) {
    std::cout << '\n' << paint("  ■ " + text, kBoldCyan) << '\n';
}

// ---- Status banners ----
enum class Kind { Success, Error, Warning, Info };

void notify(Kind k, const std::string& message) {
    const char* color = kCyan;
    std::string label = "INFO";
    switch (k) {
        case Kind::Success: color = kBoldGreen; label = "SUCCESS"; break;
        case Kind::Error:   color = kBoldRed;   label = "ERROR";   break;
        case Kind::Warning: color = kBoldYellow; label = "WARNING"; break;
        case Kind::Info:    break;
    }
    std::cout << '\n';
    panel(label, {message}, color);
}

void hint(const std::string& text) { std::cout << paint("  " + text, kDim) << '\n'; }
void problem(const std::string& text) { std::cout << paint("  ! " + text, kRed) << '\n'; }

void pause() {
    std::cout << '\n' << paint("  Press Enter to continue...", kDim) << std::flush;
    std::string dummy;
    std::getline(std::cin, dummy);
}

// ---- Input helpers ----
// Reads a full line. Returns false on EOF so loops can exit cleanly.
bool readLine(const std::string& label, std::string& out) {
    std::cout << "  " << paint(">", kBoldCyan) << ' ' << label << std::flush;
    if (!std::getline(std::cin, out)) return false;
    out = util::trim(out);
    return true;
}

// Reads a password without echoing it (when the terminal allows).
bool readPassword(const std::string& label, std::string& out) {
    std::cout << "  " << paint(">", kBoldCyan) << ' ' << label << std::flush;
    bool masked = false;
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) masked = SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT) != 0;
    const bool ok = static_cast<bool>(std::getline(std::cin, out));
    if (masked) SetConsoleMode(h, mode);
#else
    termios oldTerm{};
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &oldTerm) == 0) {
        termios t = oldTerm;
        t.c_lflag &= static_cast<tcflag_t>(~ECHO);
        masked = tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
    }
    const bool ok = static_cast<bool>(std::getline(std::cin, out));
    if (masked) tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
#endif
    if (masked) std::cout << '\n';
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return ok;
}

// Re-prompts until a non-empty value is entered. Returns nullopt on EOF.
std::optional<std::string> askText(const std::string& label) {
    std::string s;
    while (true) {
        if (!readLine(label, s)) return std::nullopt;
        if (!s.empty()) return s;
        problem("Value cannot be empty.");
    }
}

// Integer within [lo, hi]. If allowBlank, an empty entry returns blank = true.
struct IntResult { bool ok = false; bool blank = false; int value = 0; };

IntResult askInt(const std::string& label, int lo, int hi, bool allowBlank = false) {
    std::string s;
    while (true) {
        if (!readLine(label, s)) return {};
        if (s.empty() && allowBlank) return {true, true, 0};
        try {
            size_t pos = 0;
            long long v = std::stoll(s, &pos);
            if (pos != s.size()) throw std::invalid_argument("trailing");
            if (v < lo || v > hi) {
                problem("Please enter a whole number between " + std::to_string(lo) + " and " + std::to_string(hi) + ".");
                continue;
            }
            return {true, false, static_cast<int>(v)};
        } catch (const std::exception&) {
            problem("That is not a valid whole number. Try again.");
        }
    }
}

struct DoubleResult { bool ok = false; bool blank = false; double value = 0; };

DoubleResult askDouble(const std::string& label, double lo, double hi, bool allowBlank = false) {
    std::string s;
    while (true) {
        if (!readLine(label, s)) return {};
        if (s.empty() && allowBlank) return {true, true, 0};
        try {
            size_t pos = 0;
            double v = std::stod(s, &pos);
            if (pos != s.size()) throw std::invalid_argument("trailing");
            if (!(v >= lo && v <= hi)) {
                problem("Please enter an amount between " + util::moneyFmt(lo) + " and " + util::moneyFmt(hi) + ".");
                continue;
            }
            return {true, false, v};
        } catch (const std::exception&) {
            problem("That is not a valid amount. Try again.");
        }
    }
}

bool confirm(const std::string& question) {
    std::string s;
    while (true) {
        if (!readLine(question + " (y/n): ", s)) return false;
        s = util::toLower(s);
        if (s == "y" || s == "yes") return true;
        if (s == "n" || s == "no") return false;
        problem("Please answer y or n.");
    }
}

// Lets the user pick a vehicle system from a list or type a custom one.
// If allowBlank, an empty entry means "keep current" and returns "".
std::optional<std::string> askSystem(bool allowBlank = false) {
    std::cout << '\n';
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < kSystems.size(); ++i) {
        cur += util::padRight(std::to_string(i + 1) + ". " + kSystems[i], 22);
        if ((i + 1) % 4 == 0 || i + 1 == kSystems.size()) { lines.push_back(cur); cur.clear(); }
    }
    top(kSingle, "VEHICLE SYSTEMS", kCyan);
    for (const auto& l : lines) row(kSingle, l, kCyan);
    bottom(kSingle, kCyan);
    std::string s;
    while (true) {
        if (!readLine(allowBlank ? "Choose number or type a name (Enter = keep): " : "Choose number or type a name: ", s))
            return std::nullopt;
        if (s.empty()) {
            if (allowBlank) return std::string();
            problem("Value cannot be empty.");
            continue;
        }
        bool numeric = std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        if (numeric) {
            size_t n = s.size() > 6 ? 0 : static_cast<size_t>(std::stoul(s));
            if (n >= 1 && n <= kSystems.size()) return kSystems[n - 1];
            problem("Pick 1-" + std::to_string(kSystems.size()) + " or type a name.");
            continue;
        }
        for (const auto& k : kSystems) if (util::equalsIgnoreCase(k, s)) return k;  // normalise capitalisation
        return s;
    }
}

// ---- Table / detail rendering ----
std::string statusText(const Part& p) { return p.isOutOfStock() ? "OUT OF STOCK" : p.isLowStock() ? "LOW STOCK" : "OK"; }
const char* statusColor(const Part& p) { return p.isOutOfStock() ? kBoldRed : p.isLowStock() ? kBoldYellow : kGreen; }

void printTable(const std::vector<const Part*>& rows) {
    if (rows.empty()) { notify(Kind::Info, "No parts to show."); return; }

    static const size_t w[7] = {6, 20, 12, 12, 6, 6, 12};
    static const char* heads[7] = {"ID", "Part Name", "System", "Price (PHP)", "Qty", "Min", "Status"};
    static const Align al[7] = {Align::Right, Align::Left, Align::Left, Align::Right, Align::Right, Align::Right, Align::Left};

    auto border = [&](const char* l, const char* m, const char* r) {
        std::string s = l;
        for (int i = 0; i < 7; ++i) s += util::rep("─", w[i] + 2) + (i < 6 ? m : r);
        std::cout << s << '\n';
    };
    auto cell = [&](int i, const std::string& text, const char* color) {
        std::string t = al[i] == Align::Right ? util::padLeft(text, w[i]) : util::padRight(text, w[i]);
        return " " + paint(t, color) + " ";
    };

    std::cout << '\n';
    border("┌", "┬", "┐");
    std::cout << "│";
    for (int i = 0; i < 7; ++i) std::cout << cell(i, heads[i], kBoldCyan) << "│";
    std::cout << '\n';
    border("├", "┼", "┤");

    int low = 0;
    for (const Part* p : rows) {
        const char* rowColor = p->isLowStock() ? kYellow : nullptr;
        std::cout << "│" << cell(0, std::to_string(p->id()), rowColor) << "│" << cell(1, p->name(), rowColor) << "│"
                  << cell(2, p->category(), rowColor) << "│" << cell(3, util::moneyFmt(p->price()), rowColor) << "│"
                  << cell(4, std::to_string(p->quantity()), rowColor) << "│"
                  << cell(5, std::to_string(p->minStock()), rowColor) << "│" << cell(6, statusText(*p), statusColor(*p))
                  << "│\n";
        if (p->isLowStock()) ++low;
    }
    border("└", "┴", "┘");
    std::cout << "  " << rows.size() << " part(s) shown  |  "
              << (low ? paint(std::to_string(low) + " low / out of stock", kBoldYellow) : std::string("0 low / out of stock"))
              << "  |  prices in PHP\n";
}

void printDetail(const Part& p) {
    std::vector<std::string> lines = {
        "Part ID            : " + std::to_string(p.id()),
        "Name               : " + p.name(),
        "Vehicle system     : " + p.category(),
        "Unit price (PHP)   : " + util::moneyFmt(p.price()),
        "Quantity in stock  : " + std::to_string(p.quantity()),
        "Minimum threshold  : " + std::to_string(p.minStock()),
        "Stock value (PHP)  : " + util::moneyFmt(p.stockValue()),
        std::string("Status             : ") +
            (p.isOutOfStock() ? "!! OUT OF STOCK - reorder now" : p.isLowStock() ? "!! LOW STOCK - reorder needed" : "OK"),
    };
    std::cout << '\n';
    top(kSingle, "PART #" + std::to_string(p.id()), p.isLowStock() ? kYellow : kCyan);
    for (const auto& l : lines) row(kSingle, l, p.isLowStock() ? kYellow : kCyan);
    bottom(kSingle, p.isLowStock() ? kYellow : kCyan);
}

// Banner shown after login and after anything that changes stock levels.
void showLowStockAlert(const InventoryManager& inv) {
    auto low = inv.lowStockParts();
    if (low.empty()) return;
    std::vector<std::string> lines;
    for (const Part* p : low) {
        lines.push_back("[" + std::to_string(p->id()) + "] " + util::clip(p->name(), 30) + "  -  " +
                        std::to_string(p->quantity()) + " in stock (min " + std::to_string(p->minStock()) + ")" +
                        (p->isOutOfStock() ? "   << OUT OF STOCK" : ""));
    }
    std::cout << '\n';
    panel("LOW STOCK ALERT - " + std::to_string(low.size()) + " part(s) below minimum", lines, kBoldYellow);
}

}  // namespace ui

#ifndef RE_NO_WEB
// ----------------------------------------------------------------------------
//  Web dashboard: a tiny built-in HTTP server so the inventory can be used from
//  Chrome at http://localhost:8080  (start with:  re_parts_tracker.exe web).
//
//  - Pure C++ (Winsock on Windows, BSD sockets elsewhere). HTML is generated by
//    C++; there is no JavaScript.
//  - Listens on 127.0.0.1 only, so it is NOT reachable from other computers,
//    and rejects requests whose Host header is not localhost (DNS-rebinding).
//  - Same accounts as the console login. After signing in the browser gets a
//    random session cookie (HttpOnly, SameSite=Strict) that expires after 30
//    minutes of inactivity or when you press "Log out". Repeated failed logins
//    trigger a 30-second lock.
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
        else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
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

struct Request {
    std::string method, path;
    Params query, form, headers;  // header names are lower-case
};

struct Response {
    int status = 200;
    std::string body;
    std::string location;   // non-empty => 303 redirect
    std::string setCookie;  // non-empty => Set-Cookie header
};

bool toInt(const std::string& s, int lo, int hi, int& out) {
    try {
        const std::string t = util::trim(s);
        size_t pos = 0;
        long long v = std::stoll(t, &pos);
        if (pos != t.size() || v < lo || v > hi) return false;
        out = static_cast<int>(v);
        return true;
    } catch (const std::exception&) { return false; }
}

bool toDouble(const std::string& s, double lo, double hi, double& out) {
    try {
        const std::string t = util::trim(s);
        size_t pos = 0;
        double v = std::stod(t, &pos);
        if (pos != t.size() || !(v >= lo && v <= hi)) return false;
        out = v;
        return true;
    } catch (const std::exception&) { return false; }
}

std::string get(const Params& p, const std::string& k) {
    auto it = p.find(k);
    return it == p.end() ? std::string() : it->second;
}

// Random session tokens. std::random_device is mixed with the clock and a
// counter through SHA-256, so tokens stay unpredictable even on toolchains
// where random_device is weak.
std::string newToken() {
    static std::random_device rd;
    static std::uint64_t counter = 0;
    std::string seed;
    for (int i = 0; i < 8; ++i) seed += std::to_string(rd()) + ":";
    seed += std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ":" +
            std::to_string(++counter) + ":" + std::to_string(reinterpret_cast<std::uintptr_t>(&seed));
    return auth::toHex(auth::sha256(seed));
}

class SessionStore {
public:
    using Clock = std::chrono::steady_clock;

    std::string create(const std::string& user) {
        purge();
        std::string t = newToken();
        sessions_[t] = {user, Clock::now() + kIdle};
        return t;
    }

    // Returns the username for a valid token and extends its idle timeout.
    std::optional<std::string> lookup(const std::string& token) {
        auto it = sessions_.find(token);
        if (token.empty() || it == sessions_.end()) return std::nullopt;
        if (Clock::now() > it->second.expires) { sessions_.erase(it); return std::nullopt; }
        it->second.expires = Clock::now() + kIdle;
        return it->second.user;
    }

    void destroy(const std::string& token) { sessions_.erase(token); }

private:
    struct Entry { std::string user; Clock::time_point expires; };
    static constexpr std::chrono::minutes kIdle{30};

    void purge() {
        const auto now = Clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();)
            it = now > it->second.expires ? sessions_.erase(it) : std::next(it);
    }

    std::map<std::string, Entry> sessions_;
};

class Server {
    // Socket type shims (must come first: used in member declarations below).
#ifdef _WIN32
    using sock_t = SOCKET;
    static constexpr sock_t kBad = INVALID_SOCKET;
    static void closeSock(sock_t s) { closesocket(s); }
#else
    using sock_t = int;
    static constexpr sock_t kBad = -1;
    static void closeSock(sock_t s) { close(s); }
#endif
#ifdef MSG_NOSIGNAL
    static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    static constexpr int kSendFlags = 0;
#endif

public:
    Server(auth::Authenticator& auth, InventoryManager& inv) : auth_(auth), inv_(inv) {}

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
        port_ = bound;

        const std::string url = "http://localhost:" + std::to_string(bound);
        std::cout << "\n  RE Parts Tracker web dashboard is running at " << url
                  << "\n  Sign in with your normal account (default: admin / password123)."
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
    auth::Authenticator& auth_;
    InventoryManager& inv_;
    SessionStore sessions_;
    int port_ = 8080;
    std::string curUser_;  // user of the request being handled ("" = not signed in)
    int failedLogins_ = 0;
    SessionStore::Clock::time_point lockedUntil_{};
    struct ResetEntry { std::string user; SessionStore::Clock::time_point expires; };
    std::map<std::string, ResetEntry> resets_;  // short-lived password-reset tokens
    int failedRecovery_ = 0;
    SessionStore::Clock::time_point recoveryLockedUntil_{};

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
        const size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false;
        std::istringstream in(raw.substr(0, headerEnd));
        std::string first;
        std::getline(in, first);
        std::istringstream firstLine(first);
        std::string target;
        firstLine >> r.method >> target;
        if (r.method.empty() || target.empty()) return false;
        size_t q = target.find('?');
        r.path = target.substr(0, q);
        if (q != std::string::npos) r.query = parseParams(target.substr(q + 1));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            r.headers[util::toLower(util::trim(line.substr(0, colon)))] = util::trim(line.substr(colon + 1));
        }
        if (r.method == "POST") r.form = parseParams(raw.substr(headerEnd + 4));
        return true;
    }

    static void sendResponse(sock_t c, const Response& res) {
        std::ostringstream h;
        const char* common =
            "Cache-Control: no-store\r\nX-Frame-Options: DENY\r\nX-Content-Type-Options: nosniff\r\n"
            "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; form-action 'self'; "
            "frame-ancestors 'none'; base-uri 'none'\r\nConnection: close\r\n";
        if (!res.location.empty()) {
            h << "HTTP/1.1 303 See Other\r\nLocation: " << res.location << "\r\nContent-Length: 0\r\n";
        } else {
            const char* text = res.status == 200 ? "OK" : res.status == 404 ? "Not Found" : "Bad Request";
            h << "HTTP/1.1 " << res.status << ' ' << text << "\r\nContent-Type: text/html; charset=utf-8\r\n"
              << "Content-Length: " << res.body.size() << "\r\n";
        }
        if (!res.setCookie.empty()) h << "Set-Cookie: " << res.setCookie << "\r\n";
        h << common << "\r\n";
        if (res.location.empty()) h << res.body;
        const std::string out = h.str();
        size_t sent = 0;
        while (sent < out.size()) {
            int n = send(c, out.data() + sent, static_cast<int>(out.size() - sent), kSendFlags);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    // ---- Response helpers ----
    static Response htmlPage(int status, std::string body) {
        Response r;
        r.status = status;
        r.body = std::move(body);
        return r;
    }
    static Response redirectTo(const std::string& location) {
        Response r;
        r.location = location;
        return r;
    }
    static Response redirectMsg(const std::string& msg, const std::string& kind = "ok") {
        return redirectTo("/?msg=" + urlEncode(msg) + "&kind=" + kind);
    }
    Response bad(const std::string& msg) const {
        return htmlPage(400, layout("Error", "<div class='card' style='max-width:560px'><h2>Could not complete that</h2>" + flashBox(msg, "err") +
                                                 "<a class='btn' href='/'>Back to inventory</a></div>"));
    }

    static std::string cookieValue(const Request& r, const std::string& name) {
        auto it = r.headers.find("cookie");
        if (it == r.headers.end()) return "";
        std::istringstream in(it->second);
        std::string part;
        while (std::getline(in, part, ';')) {
            part = util::trim(part);
            if (part.compare(0, name.size() + 1, name + "=") == 0) return part.substr(name.size() + 1);
        }
        return "";
    }

    static constexpr const char* kCookieAttrs = "; Path=/; HttpOnly; SameSite=Strict";

    // ---- Routing ----
    Response handle(const Request& r) {
        // Only answer requests addressed to localhost / 127.0.0.1 (blocks DNS-rebinding tricks).
        const std::string host = util::toLower(get(r.headers, "host"));
        const std::string p = std::to_string(port_);
        if (host != "localhost:" + p && host != "127.0.0.1:" + p && host != "localhost" && host != "127.0.0.1")
            return htmlPage(400, "Bad host.");

        const std::string token = cookieValue(r, "sid");
        auto user = sessions_.lookup(token);

        if (r.path == "/login") {
            if (r.method == "POST") return doLogin(r);
            if (user) return redirectTo("/");
            curUser_.clear();
            return htmlPage(200, loginPage("", get(r.query, "msg")));
        }
        if (r.path == "/forgot-password") {
            if (r.method == "POST") return doForgot(r);
            curUser_.clear();
            return htmlPage(200, forgotPage(""));
        }
        if (r.path == "/reset-password") return r.method == "POST" ? doReset(r) : redirectTo("/forgot-password");
        if (r.path == "/favicon.ico") return htmlPage(404, "");

        if (!user) return redirectTo("/login");  // everything else needs a session
        curUser_ = *user;

        if (r.method == "GET") {
            if (r.path == "/") return htmlPage(200, dashboard(r));
            if (r.path == "/add") return htmlPage(200, addForm(""));
            if (r.path == "/edit") return htmlPage(200, editForm(r));
            if (r.path == "/delete") return htmlPage(200, deleteConfirm(r));
        } else if (r.method == "POST") {
            if (r.path == "/logout") {
                sessions_.destroy(token);
                Response res = redirectTo("/login?msg=" + urlEncode("You have been logged out safely."));
                res.setCookie = std::string("sid=") + kCookieAttrs + "; Max-Age=0";
                return res;
            }
            if (r.path == "/add") return doAdd(r);
            if (r.path == "/edit") return doEdit(r);
            if (r.path == "/adjust") return doAdjust(r);
            if (r.path == "/delete") return doDelete(r);
        }
        return htmlPage(404, layout("Not found", "<div class='card'><h2>Page not found</h2><p class='sub'>Nothing lives at this address.</p><a class='btn' href='/'>Back to inventory</a></div>"));
    }

    // ---- Login ----
    Response doLogin(const Request& r) {
        curUser_.clear();
        const auto now = SessionStore::Clock::now();
        if (now < lockedUntil_) {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(lockedUntil_ - now).count() + 1;
            return htmlPage(200, loginPage("Too many failed attempts. Try again in " + std::to_string(left) + " seconds.", ""));
        }
        const std::string username = util::trim(get(r.form, "username"));
        if (auto name = auth_.login(username, get(r.form, "password"))) {
            failedLogins_ = 0;
            Response res = redirectMsg("Welcome, " + *name + "!");
            res.setCookie = "sid=" + sessions_.create(*name) + kCookieAttrs;
            return res;
        }
        if (++failedLogins_ >= 5) {
            failedLogins_ = 0;
            lockedUntil_ = now + std::chrono::seconds(30);
        }
        return htmlPage(200, loginPage("Invalid username or password.", ""));
    }

    // ======================================================================
    //  EMBEDDED WEB UI  -  all HTML / CSS / JS lives in C++ raw string literals
    // ======================================================================
    static constexpr const char* kCss = R"html(
:root{--bg:#0f172a;--ink:#0f172a;--mut:#64748b;--line:#e2e8f0;--blue:#2563eb;--teal:#0d9488;--amber:#f59e0b;--red:#dc2626}
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;min-height:100vh;font-family:Inter,"Segoe UI",system-ui,-apple-system,Roboto,Arial,sans-serif;color:#e2e8f0;
background:radial-gradient(1100px 600px at 85% -10%,rgba(37,99,235,.28),transparent 60%),radial-gradient(900px 500px at -10% 110%,rgba(13,148,136,.22),transparent 60%),var(--bg);background-attachment:fixed}
a{color:#2563eb}.ic{flex:none}
header.top{position:sticky;top:0;z-index:20;background:rgba(15,23,42,.72);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);border-bottom:1px solid rgba(255,255,255,.08)}
.top-in{max-width:1200px;margin:0 auto;padding:12px 20px;display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}
.brand{display:flex;align-items:center;gap:12px;color:#fff;text-decoration:none}
.lg{width:40px;height:40px;border-radius:11px;display:grid;place-items:center;color:#fff;background:linear-gradient(135deg,#2563eb,#0d9488);box-shadow:0 6px 18px rgba(37,99,235,.45)}
.brand b{display:block;font-size:1.05rem}.brand small{color:#94a3b8;font-size:.76rem}
.who{display:flex;align-items:center;gap:10px;margin:0}
.chip{display:flex;align-items:center;gap:8px;padding:5px 12px 5px 5px;border-radius:99px;background:rgba(255,255,255,.07);font-size:.85rem}
.av{width:26px;height:26px;border-radius:50%;display:grid;place-items:center;background:var(--teal);color:#fff;font-weight:700;font-size:.75rem}
main{max-width:1200px;margin:24px auto 48px;padding:0 20px}
.page-h{margin-bottom:18px;color:#fff}.page-h h2{margin:0;font-size:1.5rem}.page-h p{margin:4px 0 0;color:#94a3b8;font-size:.9rem}
.card{background:#fff;color:var(--ink);border-radius:12px;padding:22px;margin-bottom:18px;box-shadow:0 10px 30px rgba(2,6,23,.35)}
.card h2{margin:0 0 8px}.sub{color:var(--mut);margin:0 0 18px}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-bottom:18px}
.stat{display:flex;align-items:center;gap:14px;background:#fff;color:var(--ink);border-radius:12px;padding:16px 18px;box-shadow:0 10px 30px rgba(2,6,23,.3);transition:transform .2s,box-shadow .2s}
.stat:hover{transform:translateY(-3px);box-shadow:0 16px 36px rgba(2,6,23,.45)}
.stat span{display:block;font-size:.74rem;color:var(--mut);font-weight:700;text-transform:uppercase;letter-spacing:.05em}
.stat b{display:block;font-size:1.45rem;margin-top:2px;line-height:1.1}
.ib{width:46px;height:46px;border-radius:12px;display:grid;place-items:center;flex:none}
.ib.blue{background:#dbeafe;color:#2563eb}.ib.teal{background:#ccfbf1;color:#0d9488}.ib.violet{background:#ede9fe;color:#7c3aed}.ib.amber{background:#fef3c7;color:#d97706}.ib.red{background:#fee2e2;color:#dc2626}
.stat.warn{outline:2px solid rgba(245,158,11,.75)}.stat.warn b{color:#b45309}
.alert{display:flex;gap:14px;align-items:flex-start;background:linear-gradient(90deg,#fff7ed,#fff);border-left:5px solid var(--amber);color:#7c2d12;border-radius:12px;padding:14px 16px;margin-bottom:18px;box-shadow:0 10px 30px rgba(2,6,23,.3);transition:opacity .25s,transform .25s}
.alert>div{flex:1}.alert ul{margin:6px 0 0;padding-left:18px}.alert li{margin:2px 0;font-size:.9rem}
.toolbar{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:14px}.toolbar .sp{flex:1}
.search{position:relative;flex:1;min-width:230px}
.search>.ic{position:absolute;left:12px;top:50%;transform:translateY(-50%);color:#94a3b8;pointer-events:none}
.search input{padding-left:38px;padding-right:38px}
.search .x{position:absolute;right:7px;top:50%;transform:translateY(-50%);width:24px;height:24px;border-radius:50%;border:0;background:#e2e8f0;color:#475569;cursor:pointer;display:grid;place-items:center;padding:0}
.search .x:hover{background:#cbd5e1}
input,select{width:100%;padding:10px 12px;border:1px solid #cbd5e1;border-radius:9px;font:inherit;font-size:.93rem;background:#fff;color:var(--ink);transition:border-color .15s,box-shadow .15s}
input:focus,select:focus{outline:0;border-color:var(--blue);box-shadow:0 0 0 4px rgba(37,99,235,.15)}
.toolbar select{width:auto;min-width:190px}
label{display:block;margin:14px 0 6px;font-weight:600;font-size:.84rem;color:#334155}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:7px;padding:10px 16px;border:0;border-radius:9px;background:linear-gradient(180deg,#3b82f6,#2563eb);color:#fff;font:inherit;font-size:.9rem;font-weight:600;cursor:pointer;text-decoration:none;box-shadow:0 4px 12px rgba(37,99,235,.35);transition:transform .15s,filter .15s}
.btn:hover{transform:translateY(-1px);filter:brightness(1.08)}.btn:active{transform:none}
.btn.teal{background:linear-gradient(180deg,#14b8a6,#0d9488);box-shadow:0 4px 12px rgba(13,148,136,.35)}
.btn.ghost{background:#eef2f7;color:#334155;box-shadow:none}
.btn.danger{background:linear-gradient(180deg,#ef4444,#dc2626);box-shadow:0 4px 12px rgba(220,38,38,.3)}
.btn.out{background:rgba(255,255,255,.08);color:#e2e8f0;box-shadow:none}
.btn.sm{padding:6px 10px;font-size:.8rem;border-radius:8px}.btn.block{width:100%;padding:12px}
.btn.tog[aria-pressed=true]{background:linear-gradient(180deg,#fbbf24,#f59e0b);color:#451a03;box-shadow:0 4px 12px rgba(245,158,11,.4)}
.tw{max-height:64vh;overflow:auto;border-radius:10px;border:1px solid var(--line)}
table{width:100%;border-collapse:separate;border-spacing:0;font-size:.9rem}
th,td{padding:11px 12px;text-align:left;white-space:nowrap}
thead th{position:sticky;top:0;z-index:2;background:#0f172a;color:#cbd5e1;font-size:.72rem;text-transform:uppercase;letter-spacing:.07em}
td{border-bottom:1px solid #eef2f7}tbody tr:nth-child(even) td{background:#f8fafc}tbody tr:hover td{background:#eff6ff}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}td.nm{font-weight:600;white-space:normal;min-width:180px}
tr.low td:first-child{box-shadow:inset 4px 0 0 var(--amber)}tr.out td:first-child{box-shadow:inset 4px 0 0 var(--red)}
.sysb{display:inline-block;padding:2px 9px;border-radius:7px;background:#e0e7ff;color:#3730a3;font-size:.78rem;font-weight:600}
.badge{display:inline-flex;align-items:center;gap:6px;padding:3px 11px;border-radius:99px;font-size:.7rem;font-weight:700;letter-spacing:.05em}
.badge::before{content:"";width:7px;height:7px;border-radius:50%;background:currentColor}
.b-ok{background:#dcfce7;color:#15803d}.b-low{background:#fee2e2;color:#b91c1c;animation:glow 1.8s infinite}.b-out{background:#b91c1c;color:#fff;animation:glow 1.8s infinite}
@keyframes glow{0%{box-shadow:0 0 0 0 rgba(220,38,38,.55)}70%{box-shadow:0 0 0 9px rgba(220,38,38,0)}100%{box-shadow:0 0 0 0 rgba(220,38,38,0)}}
.adj{display:inline-flex;gap:4px;margin:0}.adj input{width:60px;padding:6px 8px}
.sq{width:30px;height:33px;border:0;border-radius:8px;font-weight:700;font-size:1rem;cursor:pointer;color:#fff;transition:transform .15s,filter .15s}
.sq:hover{transform:translateY(-1px);filter:brightness(1.1)}.sq.plus{background:var(--teal)}.sq.minus{background:var(--amber)}
.acts{display:flex;gap:6px}.empty{text-align:center;color:var(--mut);padding:34px 10px;white-space:normal}.meta{color:var(--mut);font-size:.82rem;margin-top:10px}
.flash{display:flex;align-items:center;gap:10px;padding:12px 14px;border-radius:10px;margin-bottom:16px;font-size:.9rem;font-weight:500;animation:slide .3s ease;transition:opacity .25s,transform .25s}
.flash.ok{background:#dcfce7;color:#166534;border:1px solid #86efac}.flash.err{background:#fee2e2;color:#991b1b;border:1px solid #fca5a5}
.flash>span{flex:1}.fx{border:0;background:transparent;color:inherit;cursor:pointer;opacity:.6;padding:3px;display:grid;place-items:center;border-radius:6px}.fx:hover{opacity:1;background:rgba(0,0,0,.08)}
.flash.gone,.alert.gone{opacity:0;transform:translateY(-6px)}@keyframes slide{from{opacity:0;transform:translateY(-8px)}}
.fg{display:grid;grid-template-columns:1fr 1fr;gap:0 16px}.fg .full{grid-column:1/-1}.fa{display:flex;gap:10px;margin-top:22px;flex-wrap:wrap}
@media(max-width:640px){.fg{grid-template-columns:1fr}}
body.auth{display:grid;place-items:center;padding:24px}
.auth-card{width:100%;max-width:430px;padding:34px 32px;margin:0;animation:slide .4s ease}
.auth-card .lg{width:60px;height:60px;border-radius:16px;margin:0 auto 14px}
.auth-card h1{margin:0;text-align:center;font-size:1.45rem}.auth-card .sub{text-align:center;margin:6px 0 22px}
.field{position:relative}.field>.ic{position:absolute;left:13px;top:50%;transform:translateY(-50%);color:#94a3b8;pointer-events:none}
.field input{padding:12px 42px 12px 40px}
.eye{position:absolute;right:6px;top:50%;transform:translateY(-50%);border:0;background:transparent;color:#64748b;cursor:pointer;padding:6px;border-radius:8px;display:grid;place-items:center}.eye:hover{background:#f1f5f9}
.row-end{display:flex;justify-content:flex-end;margin:10px 0 18px}.row-end a,.back{font-size:.85rem;font-weight:600;text-decoration:none;display:inline-flex;gap:6px;align-items:center}
.row-end a:hover,.back:hover{text-decoration:underline}.back{margin-top:16px;justify-content:center;width:100%}
.qbox{background:#f1f5f9;border:1px dashed #94a3b8;border-radius:10px;padding:12px 14px;font-weight:600;display:flex;gap:10px;align-items:center}
.auth-foot{text-align:center;color:#94a3b8;font-size:.78rem;margin:18px 0 0}
.modal-back{position:fixed;inset:0;z-index:50;background:rgba(2,6,23,.62);backdrop-filter:blur(4px);display:grid;place-items:center;padding:20px;animation:fade .2s}
.modal{background:#fff;color:var(--ink);border-radius:14px;padding:26px;width:100%;max-width:420px;text-align:center;box-shadow:0 30px 80px rgba(0,0,0,.5);animation:pop .22s ease}
.m-ic{width:56px;height:56px;margin:0 auto 12px;border-radius:50%;display:grid;place-items:center;background:#fee2e2;color:#dc2626}
.modal h3{margin:0 0 8px;font-size:1.2rem}.modal p{margin:6px 0;color:#475569;font-size:.92rem}.m-actions{display:flex;gap:10px;justify-content:center;margin-top:20px}
@keyframes fade{from{opacity:0}}@keyframes pop{from{opacity:0;transform:scale(.93) translateY(8px)}}
)html";

    static constexpr const char* kJs = R"html(
(function(){
  var $=function(i){return document.getElementById(i)};
  function dismiss(el){el.classList.add('gone');setTimeout(function(){el.remove()},260)}
  document.querySelectorAll('.flash[data-auto]').forEach(function(f){setTimeout(function(){if(f.isConnected)dismiss(f)},6000)});
  if(/[?&]msg=/.test(location.search)&&history.replaceState)history.replaceState(null,'',location.pathname);
  var dm=$('dm');
  function closeDel(){if(dm)dm.hidden=true}
  document.addEventListener('click',function(e){
    var t=e.target.closest('[data-dismiss]');
    if(t){dismiss(t.closest('.flash,.alert'));return}
    t=e.target.closest('[data-toggle-pw]');
    if(t){var i=t.parentNode.querySelector('input');i.type=i.type==='password'?'text':'password';return}
    t=e.target.closest('[data-del]');
    if(t&&dm){e.preventDefault();$('dmId').value=t.dataset.id;$('dmText').textContent='['+t.dataset.id+'] '+t.dataset.name;dm.hidden=false;return}
    if(dm&&(e.target===dm||e.target.closest('[data-close]')))closeDel();
  });
  document.addEventListener('keydown',function(e){
    if(e.key==='Escape')closeDel();
    var q=$('q');
    if(q&&e.key==='/'&&!/INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName)){e.preventDefault();q.focus()}
  });
  var q=$('q');
  if(q){
    var sys=$('sys'),low=$('lowOnly'),clr=$('clr'),none=$('none'),cnt=$('count');
    var rows=[].slice.call(document.querySelectorAll('tbody tr[data-s]'));
    var pre=new URLSearchParams(location.search).get('q');if(pre)q.value=pre;
    var apply=function(){
      var v=q.value.trim().toLowerCase(),s=sys.value.toLowerCase(),lo=low.getAttribute('aria-pressed')==='true',n=0;
      rows.forEach(function(r){
        var ok=(!v||r.dataset.s.indexOf(v)>-1)&&(!s||r.dataset.sys===s)&&(!lo||r.dataset.low==='1');
        r.hidden=!ok;if(ok)n++;
      });
      cnt.textContent=n;none.hidden=n>0;clr.hidden=!q.value;
    };
    q.addEventListener('input',apply);sys.addEventListener('change',apply);
    low.addEventListener('click',function(){low.setAttribute('aria-pressed',low.getAttribute('aria-pressed')==='true'?'false':'true');apply()});
    clr.addEventListener('click',function(){q.value='';apply();q.focus()});
    $('reset').addEventListener('click',function(){q.value='';sys.value='';low.setAttribute('aria-pressed','false');apply()});
    apply();
  }
})();
)html";

    // ---- inline SVG icons (stroke style) ----
    static constexpr const char* kCar = "<path d='M3 13l2-6a2 2 0 0 1 2-1.4h10a2 2 0 0 1 2 1.4l2 6v4a1 1 0 0 1-1 1h-1a1 1 0 0 1-1-1v-1H6v1a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z'/><path d='M5 11h14'/><circle cx='7.5' cy='13.5' r='.6'/><circle cx='16.5' cy='13.5' r='.6'/>";
    static constexpr const char* kWrench = "<path d='M14.7 6.3a4 4 0 0 0-5.4 5.4L3 18l3 3 6.3-6.3a4 4 0 0 0 5.4-5.4l-2.6 2.6-2.4-.6-.6-2.4z'/>";
    static constexpr const char* kBox = "<path d='M21 8l-9-5-9 5v8l9 5 9-5z'/><path d='M3 8l9 5 9-5M12 13v8'/>";
    static constexpr const char* kLayers = "<path d='M12 2l10 5-10 5L2 7z'/><path d='M2 12l10 5 10-5M2 17l10 5 10-5'/>";
    static constexpr const char* kCoin = "<circle cx='12' cy='12' r='9'/><path d='M14.8 9.2C14.3 8.4 13.3 8 12 8c-1.7 0-3 .8-3 2s1.3 1.7 3 2 3 .8 3 2-1.3 2-3 2c-1.3 0-2.3-.4-2.8-1.2M12 6v2m0 8v2'/>";
    static constexpr const char* kWarn = "<path d='M10.3 3.9L1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0zM12 9v4m0 4h.01'/>";
    static constexpr const char* kSearch = "<circle cx='11' cy='11' r='7'/><path d='M21 21l-4.3-4.3'/>";
    static constexpr const char* kX = "<path d='M18 6L6 18M6 6l12 12'/>";
    static constexpr const char* kUser = "<path d='M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2'/><circle cx='12' cy='7' r='4'/>";
    static constexpr const char* kLock = "<rect x='3' y='11' width='18' height='11' rx='2'/><path d='M7 11V7a5 5 0 0 1 10 0v4'/>";
    static constexpr const char* kEye = "<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z'/><circle cx='12' cy='12' r='3'/>";
    static constexpr const char* kTrash = "<path d='M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2m3 0v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6'/>";
    static constexpr const char* kEdit = "<path d='M12 20h9'/><path d='M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z'/>";
    static constexpr const char* kPlus = "<path d='M12 5v14M5 12h14'/>";
    static constexpr const char* kCheck = "<path d='M20 6L9 17l-5-5'/>";
    static constexpr const char* kLogout = "<path d='M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9'/>";
    static constexpr const char* kKey = "<circle cx='7.5' cy='15.5' r='4.5'/><path d='M10.7 12.3L21 2m-3 3l3 3m-6 0l2 2'/>";
    static constexpr const char* kShield = "<path d='M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z'/>";
    static constexpr const char* kArrowL = "<path d='M19 12H5m7-7l-7 7 7 7'/>";

    static std::string ic(const char* paths, int size = 18) {
        const std::string s = std::to_string(size);
        return "<svg class='ic' xmlns='http://www.w3.org/2000/svg' width='" + s + "' height='" + s +
               "' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' "
               "stroke-linejoin='round' aria-hidden='true'>" + paths + "</svg>";
    }

    static std::string docHead(const std::string& title, const std::string& cls) {
        return "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'><title>" + htmlEscape(title) +
               " - RE Parts Tracker</title><style>" + kCss + "</style></head><body class='" + cls + "'>";
    }
    static std::string docFoot() { return std::string("<script>") + kJs + "</script></body></html>"; }

    // Dismissible alert banner (success banners also fade out by themselves).
    static std::string flashBox(const std::string& msg, const std::string& kind) {
        const bool e = (kind == "err");
        return std::string("<div class='flash ") + (e ? "err" : "ok") + "' role='alert'" + (e ? "" : " data-auto") + ">" +
               ic(e ? kWarn : kCheck) + "<span>" + htmlEscape(msg) + "</span>"
               "<button class='fx' type='button' data-dismiss aria-label='Dismiss'>" + ic(kX, 16) + "</button></div>";
    }

    static std::string authPage(const std::string& title, const std::string& inner) {
        return docHead(title, "auth") + "<div class='card auth-card'>" + inner + "</div>" + docFoot();
    }

    static std::string pwField(const std::string& id, const std::string& name, const std::string& ph) {
        return "<div class='field'>" + ic(kLock) + "<input id='" + id + "' type='password' name='" + name + "' placeholder='" + ph +
               "' required><button class='eye' type='button' data-toggle-pw aria-label='Show or hide password'>" + ic(kEye) + "</button></div>";
    }

    // ---- Login ----
    std::string loginPage(const std::string& error, const std::string& info) const {
        std::string h = "<div class='lg'>" + ic(kCar, 30) + "</div><h1>RE Parts Tracker</h1>"
                        "<p class='sub'>Sign in to manage Bajaj RE components and stock levels.</p>";
        if (!error.empty()) h += flashBox(error, "err");
        if (!info.empty()) h += flashBox(info, "ok");
        h += "<form method='post' action='/login' autocomplete='off'>"
             "<label for='u'>Username</label><div class='field'>" + ic(kUser) +
             "<input id='u' type='text' name='username' placeholder='Enter your username' required autofocus></div>"
             "<label for='p'>Password</label>" + pwField("p", "password", "Enter your password") +
             "<div class='row-end'><a href='/forgot-password'>Forgot Password?</a></div>"
             "<button class='btn block' type='submit'>Sign in</button></form>"
             "<p class='auth-foot'>Secure local session &middot; auto sign-out after 30 min idle</p>";
        return authPage("Sign in", h);
    }

    // ---- Forgot password: security question -> reset ----
    std::string forgotPage(const std::string& error, const std::string& user = "") const {
        std::string h = "<div class='lg'>" + ic(kKey, 28) + "</div><h1>Reset your password</h1>"
                        "<p class='sub'>Answer the recovery question to regain access.</p>";
        if (!error.empty()) h += flashBox(error, "err");
        h += "<div class='qbox'>" + ic(kShield, 20) + "<span>" + htmlEscape(auth::Authenticator::kRecoveryQuestion) + "</span></div>"
             "<form method='post' action='/forgot-password' autocomplete='off'>"
             "<label for='u'>Username</label><div class='field'>" + ic(kUser) +
             "<input id='u' type='text' name='username' placeholder='Your username' required value='" + htmlEscape(user) + "'></div>"
             "<label for='a'>Recovery answer</label>" + pwField("a", "answer", "Enter the secret code") +
             "<div style='height:18px'></div><button class='btn teal block' type='submit'>Verify answer</button></form>"
             "<a class='back' href='/login'>" + ic(kArrowL, 15) + "Back to sign in</a>";
        return authPage("Forgot password", h);
    }

    std::string resetPage(const std::string& token, const std::string& error) const {
        std::string h = "<div class='lg'>" + ic(kShield, 28) + "</div><h1>Choose a new password</h1>"
                        "<p class='sub'>Identity verified. Use at least 8 characters.</p>";
        if (!error.empty()) h += flashBox(error, "err");
        h += "<form method='post' action='/reset-password' autocomplete='off'><input type='hidden' name='token' value='" + htmlEscape(token) + "'>"
             "<label for='n1'>New password</label>" + pwField("n1", "password", "New password") +
             "<label for='n2'>Confirm password</label>" + pwField("n2", "confirm", "Repeat new password") +
             "<div style='height:18px'></div><button class='btn teal block' type='submit'>Update password</button></form>"
             "<a class='back' href='/login'>" + ic(kArrowL, 15) + "Cancel</a>";
        return authPage("New password", h);
    }

    Response doForgot(const Request& r) {
        curUser_.clear();
        const auto now = SessionStore::Clock::now();
        const std::string user = util::trim(get(r.form, "username"));
        if (now < recoveryLockedUntil_) {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(recoveryLockedUntil_ - now).count() + 1;
            return htmlPage(200, forgotPage("Too many attempts. Try again in " + std::to_string(left) + " seconds.", user));
        }
        if (auto name = auth_.verifyRecovery(user, get(r.form, "answer"))) {
            failedRecovery_ = 0;
            for (auto it = resets_.begin(); it != resets_.end();)
                it = now > it->second.expires ? resets_.erase(it) : std::next(it);
            const std::string token = newToken();
            resets_[token] = {*name, now + std::chrono::minutes(10)};
            return htmlPage(200, resetPage(token, ""));
        }
        if (++failedRecovery_ >= 5) {
            failedRecovery_ = 0;
            recoveryLockedUntil_ = now + std::chrono::seconds(60);
        }
        return htmlPage(200, forgotPage("Username or recovery answer is incorrect.", user));
    }

    Response doReset(const Request& r) {
        curUser_.clear();
        const std::string token = get(r.form, "token");
        auto it = resets_.find(token);
        if (token.empty() || it == resets_.end() || SessionStore::Clock::now() > it->second.expires) {
            if (it != resets_.end()) resets_.erase(it);
            return htmlPage(200, forgotPage("Your reset session expired. Please verify again."));
        }
        const std::string pw = get(r.form, "password");
        if (pw.size() < 8 || pw.size() > 128) return htmlPage(200, resetPage(token, "Password must be 8 to 128 characters."));
        if (pw != get(r.form, "confirm")) return htmlPage(200, resetPage(token, "The two passwords do not match."));
        if (!auth_.setPassword(it->second.user, pw)) return htmlPage(200, forgotPage("Account not found."));
        const bool saved = auth_.lastSaveOk();
        resets_.erase(it);
        return redirectTo("/login?msg=" + urlEncode(saved ? "Password updated. You can sign in now."
                                                           : "Password updated for this session, but it could not be saved to users.csv."));
    }

    // ---- Pages ----
    std::string layout(const std::string& title, const std::string& body, const std::string& flash = "",
                       const std::string& kind = "ok") const {
        std::string h = docHead(title, "app");
        h += "<header class='top'><div class='top-in'><a class='brand' href='/'><span class='lg'>" + ic(kWrench, 22) +
             "</span><span><b>RE Parts Tracker</b><small>Bajaj RE Components &amp; Inventory</small></span></a>";
        if (!curUser_.empty()) {
            const std::string initial(1, static_cast<char>(std::toupper(static_cast<unsigned char>(curUser_[0]))));
            h += "<form class='who' method='post' action='/logout'><span class='chip'><span class='av'>" + htmlEscape(initial) + "</span>" +
                 htmlEscape(curUser_) + "</span><button class='btn out sm' type='submit'>" + ic(kLogout, 15) + "Log out</button></form>";
        }
        h += "</div></header><main>";
        if (!flash.empty()) h += flashBox(flash, kind);
        h += body + "</main>" + docFoot();
        return h;
    }

    static std::string statCard(const char* color, const char* icon, const std::string& label, const std::string& value, bool warn = false) {
        return std::string("<div class='stat") + (warn ? " warn" : "") + "'><div class='ib " + color + "'>" + ic(icon, 22) + "</div><div><span>" +
               label + "</span><b>" + value + "</b></div></div>";
    }

    std::string dashboard(const Request& r) {
        double value = 0;
        long units = 0;
        for (const Part& p : inv_.all()) { value += p.stockValue(); units += p.quantity(); }
        auto low = inv_.lowStockParts();
        std::vector<const Part*> rows;
        for (const Part& p : inv_.all()) rows.push_back(&p);
        std::sort(rows.begin(), rows.end(), [](const Part* a, const Part* b) { return a->id() < b->id(); });

        std::string h = "<div class='page-h'><h2>Inventory dashboard</h2><p>Live overview of spare parts, stock levels and value.</p></div>";
        h += "<div class='stats'>" + statCard("blue", kBox, "Total parts", std::to_string(inv_.all().size())) +
             statCard("teal", kLayers, "Total units", std::to_string(units)) +
             statCard("violet", kCoin, "Inventory value", "PHP " + util::moneyFmt(value)) +
             statCard(low.empty() ? "teal" : "amber", kWarn, "Low stock", std::to_string(low.size()), !low.empty()) + "</div>";

        if (!low.empty()) {
            h += "<div class='alert' role='alert'>" + ic(kWarn, 22) + "<div><b>Low stock alert</b> - " + std::to_string(low.size()) + " part(s) below minimum<ul>";
            for (const Part* p : low)
                h += "<li>[" + std::to_string(p->id()) + "] " + htmlEscape(p->name()) + ": " + std::to_string(p->quantity()) +
                     " in stock (min " + std::to_string(p->minStock()) + ")" + (p->isOutOfStock() ? " <b>- OUT OF STOCK</b>" : "") + "</li>";
            h += "</ul></div><button class='fx' type='button' data-dismiss aria-label='Dismiss'>" + ic(kX, 16) + "</button></div>";
        }

        h += "<div class='card'><div class='toolbar'><div class='search'>" + ic(kSearch) +
             "<input id='q' type='text' placeholder='Search by ID, name or system  ( press / )' autocomplete='off' value='" + htmlEscape(util::trim(get(r.query, "q"))) + "'>"
             "<button id='clr' class='x' type='button' aria-label='Clear search' hidden>" + ic(kX, 14) + "</button></div>"
             "<select id='sys' aria-label='Vehicle system'><option value=''>All vehicle systems</option>";
        for (const auto& c : inv_.categories()) h += "<option value='" + htmlEscape(c) + "'>" + htmlEscape(c) + "</option>";
        h += "</select><button id='lowOnly' class='btn ghost tog' type='button' aria-pressed='false'>" + ic(kWarn, 16) + "Low stock only</button>"
             "<button id='reset' class='btn ghost' type='button'>Reset</button><span class='sp'></span>"
             "<a class='btn teal' href='/add'>" + ic(kPlus, 16) + "Add new part</a></div>"
             "<div class='tw'><table><thead><tr><th>ID</th><th>Part name</th><th>System</th><th class='num'>Price (PHP)</th><th class='num'>Qty</th>"
             "<th class='num'>Min</th><th>Status</th><th>Adjust stock</th><th>Actions</th></tr></thead><tbody>";
        for (const Part* p : rows) {
            const std::string id = std::to_string(p->id());
            const bool out = p->isOutOfStock(), lowp = p->isLowStock();
            const std::string search = util::toLower(id + " " + p->name() + " " + p->category());
            h += "<tr class='" + std::string(out ? "out" : lowp ? "low" : "") + "' data-s='" + htmlEscape(search) + "' data-sys='" +
                 htmlEscape(util::toLower(p->category())) + "' data-low='" + (lowp ? "1" : "0") + "'><td>" + id + "</td><td class='nm'>" +
                 htmlEscape(p->name()) + "</td><td><span class='sysb'>" + htmlEscape(p->category()) + "</span></td><td class='num'>" +
                 util::moneyFmt(p->price()) + "</td><td class='num'>" + std::to_string(p->quantity()) + "</td><td class='num'>" +
                 std::to_string(p->minStock()) + "</td><td>" +
                 (out ? "<span class='badge b-out'>OUT OF STOCK</span>" : lowp ? "<span class='badge b-low'>LOW STOCK</span>" : "<span class='badge b-ok'>OK</span>") +
                 "</td><td><form class='adj' method='post' action='/adjust'><input type='hidden' name='id' value='" + id +
                 "'><input type='number' name='qty' value='1' min='1' aria-label='Quantity'><button class='sq plus' name='op' value='add' title='Add stock'>+</button>"
                 "<button class='sq minus' name='op' value='remove' title='Remove stock'>&minus;</button></form></td>"
                 "<td><div class='acts'><a class='btn sm ghost' href='/edit?id=" + id + "'>" + ic(kEdit, 14) + "Edit</a>"
                 "<a class='btn sm danger' href='/delete?id=" + id + "' data-del data-id='" + id + "' data-name='" + htmlEscape(p->name()) + "'>" +
                 ic(kTrash, 14) + "Delete</a></div></td></tr>";
        }
        h += "</tbody></table><div id='none' class='empty'" + std::string(rows.empty() ? "" : " hidden") + ">No parts match your search.</div></div>"
             "<div class='meta'>Showing <b id='count'>" + std::to_string(rows.size()) + "</b> of " + std::to_string(rows.size()) + " parts</div></div>";

        h += "<div class='modal-back' id='dm' hidden><div class='modal' role='dialog' aria-modal='true' aria-labelledby='dmT'>"
             "<div class='m-ic'>" + ic(kTrash, 26) + "</div><h3 id='dmT'>Delete this part?</h3><p><b id='dmText'></b></p>"
             "<p>This permanently removes the record and cannot be undone.</p><form method='post' action='/delete'>"
             "<input type='hidden' name='id' id='dmId'><div class='m-actions'><button type='button' class='btn ghost' data-close>Cancel</button>"
             "<button class='btn danger' type='submit'>Yes, delete</button></div></form></div></div>";
        return layout("Inventory", h, get(r.query, "msg"), get(r.query, "kind"));
    }

    std::string systemsDatalist() const {
        std::string d = "<datalist id='systems'>";
        std::vector<std::string> all = ui::kSystems;
        for (const auto& c : inv_.categories())
            if (std::none_of(all.begin(), all.end(), [&](const std::string& s) { return util::equalsIgnoreCase(s, c); })) all.push_back(c);
        for (const auto& s : all) d += "<option value='" + htmlEscape(s) + "'>";
        return d + "</datalist>";
    }

    std::string partForm(const std::string& title, const std::string& action, const Part* p, const std::string& error) const {
        const bool edit = p != nullptr;
        std::string h = "<div class='card' style='max-width:640px'><h2>" + htmlEscape(title) + "</h2><p class='sub'>" +
                        (edit ? "Update the details below and save." : "Fill in the details of the new spare part.") + "</p>";
        if (!error.empty()) h += flashBox(error, "err");
        h += "<form method='post' action='" + action + "'><div class='fg'>";
        if (edit) h += "<input type='hidden' name='id' value='" + std::to_string(p->id()) + "'>";
        else h += "<div><label>Part ID</label><input type='number' name='id' min='1' max='" + std::to_string(limits::kMaxId) + "' required></div>";
        h += "<div><label>Vehicle system / category</label><input type='text' name='category' list='systems' required value='" +
             (edit ? htmlEscape(p->category()) : "") + "'>" + systemsDatalist() + "</div>"
             "<div class='full'><label>Part name</label><input type='text' name='name' required value='" + (edit ? htmlEscape(p->name()) : "") + "'></div>"
             "<div><label>Unit price (PHP)</label><input type='number' name='price' step='0.01' min='0' required value='" + (edit ? util::money(p->price()) : "") + "'></div>"
             "<div><label>Quantity in stock</label><input type='number' name='qty' min='0' required value='" + (edit ? std::to_string(p->quantity()) : "") + "'></div>"
             "<div><label>Minimum stock threshold</label><input type='number' name='min' min='0' required value='" + (edit ? std::to_string(p->minStock()) : "") + "'></div></div>"
             "<div class='fa'><button class='btn teal' type='submit'>" + ic(kCheck, 16) + "Save part</button><a class='btn ghost' href='/'>Cancel</a></div></form></div>";
        return layout(title, h);
    }

    std::string addForm(const std::string& error) const { return partForm("Add new part", "/add", nullptr, error); }

    std::string notFoundPage() const {
        return layout("Not found", "<div class='card'><h2>Part not found</h2><p class='sub'>That record does not exist.</p><a class='btn' href='/'>Back to inventory</a></div>");
    }

    std::string editForm(const Request& r) {
        int id = 0;
        Part* p = toInt(get(r.query, "id"), 1, limits::kMaxId, id) ? inv_.findById(id) : nullptr;
        if (!p) return notFoundPage();
        return partForm("Update part " + std::to_string(id), "/edit", p, "");
    }

    // Fallback confirmation page (used only when JavaScript is unavailable).
    std::string deleteConfirm(const Request& r) {
        int id = 0;
        Part* p = toInt(get(r.query, "id"), 1, limits::kMaxId, id) ? inv_.findById(id) : nullptr;
        if (!p) return notFoundPage();
        return layout("Delete part",
                      "<div class='card' style='max-width:520px'><h2>Delete this part?</h2><p class='sub'><b>[" + std::to_string(id) + "] " + htmlEscape(p->name()) +
                      "</b><br>" + htmlEscape(p->category()) + " &middot; " + std::to_string(p->quantity()) + " in stock</p>" +
                      flashBox("This permanently removes the record.", "err") + "<form method='post' action='/delete'><input type='hidden' name='id' value='" +
                      std::to_string(id) + "'><div class='fa'><button class='btn danger' type='submit'>Yes, delete</button><a class='btn ghost' href='/'>Cancel</a></div></form></div>");
    }

    // ---- Form handlers ----
    // Shared validation for add/edit; fills 'out' or returns an error message.
    static std::string readPart(const Params& f, Part& out, bool needId) {
        int id = 0, qty = 0, mn = 0;
        double price = 0;
        if (needId && !toInt(get(f, "id"), 1, limits::kMaxId, id))
            return "Part ID must be a whole number from 1 to " + std::to_string(limits::kMaxId) + ".";
        std::string name = util::trim(get(f, "name")), cat = util::trim(get(f, "category"));
        if (name.empty()) return "Part name cannot be empty.";
        if (cat.empty()) return "Vehicle system cannot be empty.";
        for (const auto& k : ui::kSystems) if (util::equalsIgnoreCase(k, cat)) cat = k;
        if (!toDouble(get(f, "price"), 0, limits::kMaxPrice, price)) return "Price must be a number from 0 to " + util::moneyFmt(limits::kMaxPrice) + ".";
        if (!toInt(get(f, "qty"), 0, limits::kMaxQty, qty)) return "Quantity must be a whole number from 0 to " + std::to_string(limits::kMaxQty) + ".";
        if (!toInt(get(f, "min"), 0, limits::kMaxQty, mn)) return "Minimum threshold must be a whole number from 0 to " + std::to_string(limits::kMaxQty) + ".";
        out = Part(id, name, cat, price, qty, mn);
        return "";
    }

    Response afterSave(const std::string& okMsg) {
        if (!inv_.lastSaveOk()) return redirectMsg("Changed in memory but NOT saved to disk: " + inv_.lastError(), "err");
        return redirectMsg(okMsg);
    }

    Response doAdd(const Request& r) {
        Part p;
        std::string err = readPart(r.form, p, true);
        if (!err.empty()) return bad(err);
        if (!inv_.addPart(p)) return bad("A part with ID " + std::to_string(p.id()) + " already exists.");
        return afterSave("Part " + std::to_string(p.id()) + " added and saved.");
    }

    Response doEdit(const Request& r) {
        int id = 0;
        Part* cur = toInt(get(r.form, "id"), 1, limits::kMaxId, id) ? inv_.findById(id) : nullptr;
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
        return afterSave("Part " + std::to_string(id) + " updated and saved.");
    }

    Response doAdjust(const Request& r) {
        int id = 0, qty = 0;
        Part* p = toInt(get(r.form, "id"), 1, limits::kMaxId, id) ? inv_.findById(id) : nullptr;
        if (!p) return bad("Part not found.");
        if (!toInt(get(r.form, "qty"), 1, limits::kMaxQty, qty)) return bad("Enter a whole number of 1 or more.");
        long long n = static_cast<long long>(p->quantity()) + (get(r.form, "op") == "remove" ? -qty : qty);
        if (n < 0) return redirectMsg("Cannot remove " + std::to_string(qty) + ": only " + std::to_string(p->quantity()) + " in stock.", "err");
        if (n > limits::kMaxQty) return redirectMsg("Stock cannot exceed " + std::to_string(limits::kMaxQty) + " units.", "err");
        p->setQuantity(static_cast<int>(n));
        inv_.save();
        return afterSave("Stock of part " + std::to_string(id) + " is now " + std::to_string(n) + ".");
    }

    Response doDelete(const Request& r) {
        int id = 0;
        if (!toInt(get(r.form, "id"), 1, limits::kMaxId, id) || !inv_.removePart(id)) return bad("Part not found.");
        return afterSave("Part " + std::to_string(id) + " deleted.");
    }
};

}  // namespace web
#endif  // RE_NO_WEB

// ----------------------------------------------------------------------------
//  Application: login, menu and actions
// ----------------------------------------------------------------------------
class App {
public:
    App() : inv_("parts.csv") {}

    void run() {
        while (true) {
            if (!loginScreen()) break;          // user chose Exit (or input closed)
            const bool exitApp = sessionLoop(); // true = Exit, false = Logout
            ui::gUser.clear();
            if (exitApp) break;
        }
        goodbye();
    }

    // Starts only the Chrome dashboard (run the program with the argument "web").
    // Sign-in happens in the browser.
    void runWeb() {
#ifndef RE_NO_WEB
        ui::showLowStockAlert(inv_);
        if (!web::Server(auth_, inv_).run(8080, true)) std::cerr << "  Could not start the web dashboard.\n";
#else
        std::cerr << "  This build was compiled with -DRE_NO_WEB (console only).\n";
#endif
    }

private:
    static constexpr int kMaxAttempts = 3;
    static constexpr int kLockoutSeconds = 5;

    auth::Authenticator auth_;
    InventoryManager inv_;

    // ======================= Authentication screens =======================
    // Returns true when logged in, false when the user wants to quit.
    bool loginScreen() {
        while (true) {
            ui::clear();
            ui::logo();
            std::cout << '\n';
            ui::top(ui::kDouble);
            ui::row(ui::kDouble, "WELCOME - PLEASE SIGN IN", nullptr, ui::kBoldCyan, ui::Align::Center);
            ui::section("MENU");
            ui::row(ui::kDouble, "[1]  Log in");
            ui::row(ui::kDouble, "[2]  Exit application");
            ui::bottom(ui::kDouble);
            std::cout << '\n';
            auto c = ui::askInt("Select an option [1-2]: ", 1, 2);
            if (!c.ok || c.value == 2) return false;

            for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
                std::cout << '\n' << ui::paint("  ── SECURE LOGIN ── (attempt " + std::to_string(attempt) + " of " +
                                                  std::to_string(kMaxAttempts) + ")", ui::kBoldCyan) << '\n';
                std::string user, pass;
                if (!ui::readLine("Username: ", user)) return false;
                if (!ui::readPassword("Password: ", pass)) return false;

                if (auto name = auth_.login(user, pass)) {
                    ui::gUser = *name;
                    return true;
                }
                const int left = kMaxAttempts - attempt;
                ui::notify(ui::Kind::Error, left > 0 ? "Invalid username or password. " + std::to_string(left) +
                                                           " attempt(s) remaining."
                                                     : "Invalid username or password.");
            }
            // Too many failures: short cool-down, then back to the welcome screen.
            std::cout << '\n';
            for (int s = kLockoutSeconds; s > 0; --s) {
                std::cout << "\r" << ui::paint("  Too many failed attempts. Please wait " + std::to_string(s) + "s ...   ", ui::kBoldRed)
                          << std::flush;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            std::cout << '\n';
        }
    }

    // Shown once right after a successful login.
    void startSession() {
        ui::screen("SESSION STARTED");
        std::time_t now = std::time(nullptr);
        char buf[64] = "";
        std::strftime(buf, sizeof buf, "%A, %d %B %Y  %H:%M", std::localtime(&now));
        ui::notify(ui::Kind::Success, "Login successful. Welcome, " + ui::gUser + "!  (" + buf + ")");
        std::cout << '\n';
        ui::panel("DATA FILE", {inv_.filename() + "  -  " + std::to_string(inv_.all().size()) + " part(s) loaded"}, ui::kCyan, false);
        if (inv_.createdSampleData())
            ui::notify(ui::Kind::Info, "First run: '" + inv_.filename() + "' was created with sample parts.");
        if (!inv_.loadProblem().empty()) ui::notify(ui::Kind::Error, inv_.loadProblem());
        if (inv_.skippedLines() > 0)
            ui::notify(ui::Kind::Warning, std::to_string(inv_.skippedLines()) +
                                              " row(s) in the data file were invalid or duplicated and were ignored. "
                                              "A backup of the original was saved as '" + inv_.filename() + ".bak'.");
        ui::showLowStockAlert(inv_);  // automatic check at login
        ui::pause();
    }

    void goodbye() {
        ui::clear();
        ui::top(ui::kDouble, "", ui::kCyan);
        ui::row(ui::kDouble, "", ui::kCyan);
        ui::row(ui::kDouble, "Thank you for using RE PARTS TRACKER.  All changes are saved.  Goodbye!", ui::kCyan,
                ui::kBoldCyan, ui::Align::Center);
        ui::row(ui::kDouble, "", ui::kCyan);
        ui::bottom(ui::kDouble, ui::kCyan);
        std::cout << '\n';
    }

    // ========================== Web dashboard =============================
    void startWebFromMenu() {
#ifndef RE_NO_WEB
        ui::screen("WEB DASHBOARD");
        ui::notify(ui::Kind::Info, "Starting the web dashboard. Sign in again in the browser. Press Ctrl+C in this window to stop it.");
        if (!web::Server(auth_, inv_).run(8080, true))
            ui::notify(ui::Kind::Error, "The web dashboard could not start (port 8080-8089 busy?).");
#else
        ui::notify(ui::Kind::Error, "This build was compiled with -DRE_NO_WEB, so the web dashboard is not available.");
#endif
    }

    // ============================ Main menu ===============================
    // Returns true to quit the program, false after a logout.
    bool sessionLoop() {
        startSession();
        while (true) {
            printMenu();
            auto c = ui::askInt("Select an option [0-11]: ", 0, 11);
            if (!c.ok) return true;  // input closed
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
                case 11: startWebFromMenu(); break;
                case 10:
                    if (logout()) return false;
                    continue;
                case 0:
                    if (confirmExit()) return true;
                    continue;
                default: break;
            }
            ui::pause();
        }
    }

    bool logout() {
        std::cout << '\n';
        if (!ui::confirm("Log out of account '" + ui::gUser + "'?")) return false;
        ui::screen("LOGOUT");
        ui::notify(ui::Kind::Success, "You have been logged out safely. All changes were already saved.");
        ui::pause();
        return true;
    }

    bool confirmExit() {
        std::cout << '\n';
        return ui::confirm("Exit the application?");
    }

    void printMenu() const {
        ui::screen("MAIN MENU");

        // ---- Inventory snapshot ----
        double value = 0;
        long units = 0;
        for (const Part& p : inv_.all()) { value += p.stockValue(); units += p.quantity(); }
        const size_t low = inv_.lowStockParts().size();
        std::cout << '\n';
        ui::top(ui::kSingle, "INVENTORY SNAPSHOT", ui::kCyan);
        ui::row(ui::kSingle,
                "Parts: " + std::to_string(inv_.all().size()) + "     Units in stock: " + std::to_string(units) +
                    "     Stock value: PHP " + util::moneyFmt(value),
                ui::kCyan);
        ui::bottom(ui::kSingle, ui::kCyan);
        if (low > 0) {
            ui::top(ui::kSingle, "", ui::kBoldYellow);
            ui::row(ui::kSingle, "!! " + std::to_string(low) + " part(s) below minimum stock - choose [9] for the report",
                    ui::kBoldYellow, ui::kBoldYellow);
            ui::bottom(ui::kSingle, ui::kBoldYellow);
        }

        // ---- Menu ----
        auto item = [](int n, const std::string& label, const std::string& desc) {
            ui::row(ui::kDouble, "[" + util::padLeft(std::to_string(n), 2) + "]  " + util::padRight(label, 26) + desc);
        };
        std::cout << '\n';
        ui::top(ui::kDouble, "MAIN MENU");
        ui::section("INVENTORY");
        item(1, "Add new part", "Register a new spare part");
        item(2, "View all parts", "Full inventory table");
        item(3, "View part details", "Look up one part by ID");
        item(4, "Update part details", "Edit name, system, price, stock");
        item(5, "Adjust stock level", "Quickly add or remove units");
        item(6, "Delete part", "Permanently remove a record");
        ui::section("SEARCH & REPORTS");
        item(7, "Search", "By ID, name or vehicle system");
        item(8, "Browse by vehicle system", "Engine, Braking, Electrical, ...");
        item(9, "Low stock report", "Parts below minimum threshold");
        ui::section("WEB");
#ifndef RE_NO_WEB
        item(11, "Web dashboard (Chrome)", "Open this inventory in your browser");
#else
        item(11, "Web dashboard (disabled)", "Built with -DRE_NO_WEB");
#endif
        ui::section("SESSION");
        item(10, "Log out", "Return to the login screen");
        item(0, "Exit application", "Save and quit");
        ui::bottom(ui::kDouble);
        std::cout << '\n';
    }

    // After any change: tell the user if the file could not be written.
    bool saved() {
        if (inv_.lastSaveOk()) return true;
        ui::notify(ui::Kind::Error, "Change made in memory but NOT saved to disk: " + inv_.lastError());
        return false;
    }

    // ============================== Create ================================
    void addPart() {
        ui::screen("ADD NEW PART");
        ui::hint("Fill in the details below. The Part ID must be unique.");
        std::cout << '\n';
        auto id = ui::askInt("Part ID (1-" + std::to_string(limits::kMaxId) + "): ", 1, limits::kMaxId);
        if (!id.ok) return;
        if (inv_.findById(id.value)) {
            ui::notify(ui::Kind::Error, "A part with ID " + std::to_string(id.value) + " already exists. Use 'Update part details' to change it.");
            return;
        }
        auto name = ui::askText("Part name: ");
        if (!name) return;
        auto sys = ui::askSystem();
        if (!sys) return;
        auto price = ui::askDouble("Unit price (PHP): ", 0.0, limits::kMaxPrice);
        if (!price.ok) return;
        auto qty = ui::askInt("Quantity in stock: ", 0, limits::kMaxQty);
        if (!qty.ok) return;
        auto minq = ui::askInt("Minimum stock threshold: ", 0, limits::kMaxQty);
        if (!minq.ok) return;

        Part p(id.value, *name, *sys, price.value, qty.value, minq.value);
        if (inv_.addPart(p)) {
            if (saved()) ui::notify(ui::Kind::Success, "Part " + std::to_string(p.id()) + " (" + p.name() + ") added and saved.");
            if (p.isLowStock()) ui::notify(ui::Kind::Warning, "This part is already below its minimum threshold.");
        }
    }

    // =============================== Read =================================
    void viewAll() {
        ui::screen("ALL PARTS");
        std::vector<const Part*> rows;
        for (const Part& p : inv_.all()) rows.push_back(&p);
        std::sort(rows.begin(), rows.end(), [](const Part* a, const Part* b) { return a->id() < b->id(); });
        ui::printTable(rows);
    }

    void viewDetail() {
        ui::screen("PART DETAILS");
        std::cout << '\n';
        auto id = ui::askInt("Part ID: ", 1, limits::kMaxId);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { ui::notify(ui::Kind::Error, "No part with ID " + std::to_string(id.value) + "."); return; }
        ui::printDetail(*p);
    }

    // ============================== Update ================================
    void updatePart() {
        ui::screen("UPDATE PART");
        std::cout << '\n';
        auto id = ui::askInt("Part ID to update: ", 1, limits::kMaxId);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { ui::notify(ui::Kind::Error, "No part with ID " + std::to_string(id.value) + "."); return; }
        ui::printDetail(*p);
        std::cout << '\n';
        ui::hint("Press Enter on any field to keep its current value.");
        std::cout << '\n';

        std::string s;
        if (!ui::readLine("New name [" + p->name() + "]: ", s)) return;
        std::string newName = s.empty() ? p->name() : s;

        std::cout << "  Current system: " << p->category() << '\n';
        auto sys = ui::askSystem(true);
        if (!sys) return;
        std::string newSys = sys->empty() ? p->category() : *sys;

        auto price = ui::askDouble("New price [" + util::money(p->price()) + "]: ", 0.0, limits::kMaxPrice, true);
        if (!price.ok) return;
        auto qty = ui::askInt("New quantity [" + std::to_string(p->quantity()) + "]: ", 0, limits::kMaxQty, true);
        if (!qty.ok) return;
        auto minq = ui::askInt("New minimum threshold [" + std::to_string(p->minStock()) + "]: ", 0, limits::kMaxQty, true);
        if (!minq.ok) return;

        p->setName(newName);
        p->setCategory(newSys);
        if (!price.blank) p->setPrice(price.value);
        if (!qty.blank) p->setQuantity(qty.value);
        if (!minq.blank) p->setMinStock(minq.value);
        inv_.save();

        if (saved()) ui::notify(ui::Kind::Success, "Part " + std::to_string(p->id()) + " updated and saved.");
        ui::printDetail(*p);
        ui::showLowStockAlert(inv_);  // re-check after stock/threshold change
    }

    void adjustStock() {
        ui::screen("ADJUST STOCK LEVEL");
        std::cout << '\n';
        auto id = ui::askInt("Part ID: ", 1, limits::kMaxId);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { ui::notify(ui::Kind::Error, "No part with ID " + std::to_string(id.value) + "."); return; }
        std::cout << "\n  " << ui::paint(p->name(), ui::kBoldCyan) << " - current stock: " << p->quantity()
                  << " (min " << p->minStock() << ")\n";
        ui::hint("Enter a positive number to ADD stock (e.g. 10) or a negative one to REMOVE (e.g. -3).");
        auto delta = ui::askInt("Change: ", -limits::kMaxQty, limits::kMaxQty);
        if (!delta.ok) return;
        long long newQty = static_cast<long long>(p->quantity()) + delta.value;
        if (newQty < 0) {
            ui::notify(ui::Kind::Error, "Cannot remove " + std::to_string(-delta.value) + ": only " +
                                            std::to_string(p->quantity()) + " in stock.");
            return;
        }
        if (newQty > limits::kMaxQty) {
            ui::notify(ui::Kind::Error, "Stock cannot exceed " + std::to_string(limits::kMaxQty) + " units.");
            return;
        }
        p->setQuantity(static_cast<int>(newQty));
        inv_.save();
        if (saved()) ui::notify(ui::Kind::Success, "Stock updated and saved. New quantity: " + std::to_string(p->quantity()));
        ui::showLowStockAlert(inv_);
    }

    // ============================== Delete ================================
    void deletePart() {
        ui::screen("DELETE PART");
        std::cout << '\n';
        auto id = ui::askInt("Part ID to delete: ", 1, limits::kMaxId);
        if (!id.ok) return;
        Part* p = inv_.findById(id.value);
        if (!p) { ui::notify(ui::Kind::Error, "No part with ID " + std::to_string(id.value) + "."); return; }
        ui::printDetail(*p);
        std::cout << '\n';
        ui::notify(ui::Kind::Warning, "Deleting a part permanently removes its record.");
        std::cout << '\n';
        if (ui::confirm("Permanently delete this part?")) {
            inv_.removePart(id.value);
            if (saved()) ui::notify(ui::Kind::Success, "Part deleted and changes saved.");
        } else {
            ui::notify(ui::Kind::Info, "Deletion cancelled. Nothing was changed.");
        }
    }

    // ============================== Search ================================
    void search() {
        ui::screen("SEARCH PARTS");
        ui::hint("Searching is case-insensitive.");
        std::cout << '\n';
        ui::top(ui::kSingle, "SEARCH BY", ui::kCyan);
        ui::row(ui::kSingle, "1. Part ID      2. Part name      3. Vehicle system      4. Anywhere (ID, name, system)", ui::kCyan);
        ui::bottom(ui::kSingle, ui::kCyan);
        auto mode = ui::askInt("Search by [1-4]: ", 1, 4);
        if (!mode.ok) return;
        auto q = ui::askText("Search text: ");
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
            default: break;
        }
        std::sort(rows.begin(), rows.end(), [](const Part* a, const Part* b) { return a->id() < b->id(); });
        ui::sectionTitle("Results for \"" + *q + "\"");
        ui::printTable(rows);
    }

    void browseBySystem() {
        ui::screen("BROWSE BY VEHICLE SYSTEM");
        auto cats = inv_.categories();
        if (cats.empty()) { ui::notify(ui::Kind::Info, "No parts in inventory."); return; }
        for (const auto& c : cats) {
            auto rows = inv_.searchBySystem(c);
            // searchBySystem() is a substring match; keep exact matches only here.
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [&c](const Part* p) { return !util::equalsIgnoreCase(p->category(), c); }),
                       rows.end());
            std::sort(rows.begin(), rows.end(), [](const Part* a, const Part* b) { return a->id() < b->id(); });
            ui::sectionTitle(c);
            ui::printTable(rows);
        }
    }

    // ============================ Monitoring ==============================
    void lowStockReport() {
        ui::screen("LOW STOCK REPORT");
        auto low = inv_.lowStockParts();
        if (low.empty()) {
            ui::notify(ui::Kind::Success, "All parts are at or above their minimum threshold.");
            return;
        }
        std::sort(low.begin(), low.end(), [](const Part* a, const Part* b) { return a->id() < b->id(); });
        ui::showLowStockAlert(inv_);
        ui::printTable(low);
    }
};

// ----------------------------------------------------------------------------
//  Optional helper:  re_parts_tracker.exe --make-credential
//  Prints a ready-to-paste account line (salted hash) for defaultUsers().
// ----------------------------------------------------------------------------
int makeCredential() {
    std::string user, p1, p2;
    std::cout << "Create a credential line for auth::defaultUsers()\n";
    if (!ui::readLine("Username: ", user) || user.empty()) { std::cerr << "Username required.\n"; return 1; }
    if (!ui::readPassword("Password: ", p1) || p1.size() < 8) { std::cerr << "Use a password of at least 8 characters.\n"; return 1; }
    if (!ui::readPassword("Repeat password: ", p2) || p1 != p2) { std::cerr << "Passwords do not match.\n"; return 1; }
    const std::string salt = auth::randomSalt(user);
    std::cout << "\nPaste this into defaultUsers():\n\n    {\"" << user << "\", \"" << salt << "\", \""
              << auth::hashPassword(salt, p1) << "\"},\n\n";
    return 0;
}

// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    ui::ConsoleSession console;  // UTF-8 + colour set-up, restored on exit
    const std::string arg = argc > 1 ? util::toLower(argv[1]) : "";
    if (arg == "--make-credential") return makeCredential();
    App app;
    if (arg == "web" || arg == "--web") app.runWeb();
    else app.run();
    return 0;
}