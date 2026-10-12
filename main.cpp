// ============================================================================
//  RE Parts Tracker  v2.1
//  A Bajaj RE Components and Inventory Management System in C++
//
//  - Standard C++17, no 3rd-party libraries. Console (CLI) app PLUS a local web
//    dashboard you can open in Chrome (plain sockets, no GUI toolkit, no JS framework).
//  - Parts are stored in parts.csv: loaded on start-up and saved after every change.
//
//  NEW in v2.1
//  - Stock history / audit log: every add, edit, delete and +/- adjustment is
//    appended to history.csv (timestamp, part, change, quantity, new stock level,
//    user, details) and shown in the dashboard ("Stock history") and console (menu 12).
//  - Sortable inventory table (click ID / Name / System / Price / Qty headers) and a
//    printable Reorder list of low-stock parts with the quantity needed (web /reorder,
//    console menu 13).
//  - "Download CSV Export" button (parts + history) and automatic backups: before
//    parts.csv is overwritten, the previous version is copied to
//    backups/parts_backup_YYYYMMDD_HHMMSS.csv (all kept for 7 days, then the newest
//    per day for 90 days).
//  - No hard-coded password or recovery answer. The first start asks you to choose
//    an admin password and a recovery question/answer; both can be changed later
//    (console menu 14 or the web "Security" page). Only salted, 20,000-round SHA-256
//    hashes are stored, in users.csv.
//
//  Build (Windows, MSYS2/MinGW-w64 g++ 9 or newer):
//      g++ -std=c++17 main.cpp -o main.exe -lws2_32
//  Build (Linux / macOS):
//      g++ -std=c++17 main.cpp -o main
//  Console-only build (no sockets, no -lws2_32 needed):
//      g++ -std=c++17 -DRE_NO_WEB main.cpp -o main.exe
//
//  Run console : main.exe          (menu option 11 starts the web mode)
//  Run in Chrome: main.exe web     (opens http://localhost:8080)
//
//  Lost both the password and the recovery answer?  Close the program, delete
//  users.csv and start it again: the first-time setup will run. (Part data,
//  history and backups are not touched.)
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


// ---- CSV helpers shared by parts.csv, history.csv and users.csv (RFC-4180 style) ----
std::string csvEscape(const std::string& field) {
    if (field.find_first_of(",\"\n\r") == std::string::npos) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

std::vector<std::string> splitCSV(const std::string& line) {
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

// Newlines would break the one-record-per-line CSV files, so flatten them.
std::string oneLine(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

// ---- Time helpers (std::chrono for "now", strftime only for text formatting) ----
inline std::tm localTime(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm out{};
    if (const std::tm* p = std::localtime(&t)) out = *p;  // the app is single-threaded, so this is safe
    return out;
}

inline std::string formatTime(std::chrono::system_clock::time_point tp, const char* fmt) {
    const std::tm tmv = localTime(tp);
    char buf[64] = "";
    std::strftime(buf, sizeof buf, fmt, &tmv);
    return buf;
}

inline std::string nowStamp() { return formatTime(std::chrono::system_clock::now(), "%Y-%m-%d %H:%M:%S"); }

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
//  The admin account and its recovery phrase are NOT in the source code. They
//  are created by the first-run setup prompt and stored (hashed) in users.csv.
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
    std::string hash;         // hex of hashPassword(salt, password)
    std::string recQuestion;  // shown on the "forgot password" page (not secret)
    std::string recSalt;
    std::string recHash;      // hex of hashPassword(recSalt, normalised answer); empty = not set up
};

// Rules enforced everywhere a password / recovery phrase is created or changed.
namespace policy {
constexpr size_t kMinPassword = 8, kMaxPassword = 128;
constexpr size_t kMinRecovery = 8, kMaxRecovery = 128;
constexpr size_t kMinQuestion = 5, kMaxQuestion = 120;
}  // namespace policy

std::string randomSalt(const std::string& username);  // defined below the class

// ----------------------------------------------------------------------------
//  There are NO built-in credentials and NO recovery answer in this source
//  file. The admin account is created by the first-run setup prompt and lives
//  in users.csv as:  username,salt,hash,recoverySalt,recoveryHash,question
//  (only salted + stretched hashes are stored; the recovery phrase is hashed
//  exactly like a password). Both can be changed later - from the console
//  menu or the web "Security" page - after proving the current password.
// ----------------------------------------------------------------------------
class Authenticator {
public:
    static constexpr const char* kUsersFile = "users.csv";
    static constexpr const char* kAdminName = "admin";

    Authenticator() { loadSaved(); }

    // False on a brand-new install: the first-run setup must create the admin account.
    bool isConfigured() const { return !users_.empty(); }
    bool hasRecovery(const std::string& username) const {
        const Credential* c = find(username);
        return c != nullptr && !c->recHash.empty();
    }
    // Question of the (single) admin account, for the "forgot password" page. "" = not set up.
    std::string recoveryQuestion() const {
        return users_.empty() || users_.front().recHash.empty() ? std::string() : users_.front().recQuestion;
    }
    std::string adminName() const { return users_.empty() ? std::string(kAdminName) : users_.front().username; }

    // ---- Validation (returns an error message, or "" when acceptable) ----
    static std::string checkPassword(const std::string& pw) {
        if (pw.size() < policy::kMinPassword || pw.size() > policy::kMaxPassword)
            return "Password must be " + std::to_string(policy::kMinPassword) + " to " + std::to_string(policy::kMaxPassword) + " characters.";
        return "";
    }
    static std::string checkRecovery(const std::string& question, const std::string& answer) {
        const std::string q = util::trim(question), a = normAnswer(answer);
        if (q.size() < policy::kMinQuestion || q.size() > policy::kMaxQuestion)
            return "The recovery question must be " + std::to_string(policy::kMinQuestion) + " to " + std::to_string(policy::kMaxQuestion) + " characters.";
        if (a.size() < policy::kMinRecovery || a.size() > policy::kMaxRecovery)
            return "The recovery answer must be " + std::to_string(policy::kMinRecovery) + " to " + std::to_string(policy::kMaxRecovery) + " characters.";
        return "";
    }

    // ---- First-run setup: creates the admin account. Returns "" on success. ----
    std::string initialSetup(const std::string& password, const std::string& question, const std::string& answer) {
        if (isConfigured()) return "An account already exists.";
        std::string err = checkPassword(password);
        if (err.empty()) err = checkRecovery(question, answer);
        if (!err.empty()) return err;
        if (normAnswer(answer) == util::toLower(util::trim(password))) return "The recovery answer must be different from the password.";
        Credential c;
        c.username = kAdminName;
        c.salt = randomSalt(c.username);
        c.hash = hashPassword(c.salt, password);
        fillRecovery(c, question, answer);
        users_.push_back(c);
        if (!saveUsers()) { users_.clear(); return std::string("Could not write '") + kUsersFile + "'. Check that the folder is writable."; }
        return "";
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
        const bool has = found != nullptr && !found->recHash.empty();
        const std::string salt = has ? found->recSalt : std::string("RE-PT:no-such-recovery");
        const std::string computed = hashPassword(salt, normAnswer(answer));
        const bool ok = constantTimeEquals(computed, has ? found->recHash : std::string(64, '0')) && has;
        if (ok) return found->username;
        return std::nullopt;
    }

    // Used by the "forgot password" flow (identity already proven by the recovery answer).
    bool setPassword(const std::string& username, const std::string& newPassword) {
        Credential* c = findMutable(username);
        if (!c) return false;
        c->salt = randomSalt(c->username);
        c->hash = hashPassword(c->salt, newPassword);
        saveOk_ = saveUsers();
        return true;
    }

    // Change password: needs the current password. Returns "" on success, else an error message.
    std::string changePassword(const std::string& username, const std::string& current, const std::string& newPassword) {
        if (!login(username, current)) return "The current password is incorrect.";
        std::string err = checkPassword(newPassword);
        if (!err.empty()) return err;
        const Credential* c = find(username);
        if (c && !c->recHash.empty() && constantTimeEquals(hashPassword(c->recSalt, normAnswer(newPassword)), c->recHash))
            return "The new password must be different from the recovery answer.";
        if (!setPassword(username, newPassword) || !saveOk_) return std::string("Password changed for this session, but '") + kUsersFile + "' could not be saved.";
        return "";
    }

    // Change recovery question + answer: needs the current password. Returns "" on success.
    std::string changeRecovery(const std::string& username, const std::string& currentPassword,
                               const std::string& question, const std::string& answer) {
        if (!login(username, currentPassword)) return "The current password is incorrect.";
        std::string err = checkRecovery(question, answer);
        if (!err.empty()) return err;
        if (normAnswer(answer) == util::toLower(util::trim(currentPassword))) return "The recovery answer must be different from the password.";
        Credential* c = findMutable(username);
        if (!c) return "Account not found.";
        fillRecovery(*c, question, answer);
        if (!saveUsers()) return std::string("Recovery phrase changed for this session, but '") + kUsersFile + "' could not be saved.";
        return "";
    }

    bool lastSaveOk() const { return saveOk_; }

private:
    std::vector<Credential> users_;
    bool saveOk_ = true;

    static std::string normAnswer(const std::string& s) { return util::toLower(util::trim(s)); }

    static void fillRecovery(Credential& c, const std::string& question, const std::string& answer) {
        c.recQuestion = util::oneLine(util::trim(question));
        c.recSalt = randomSalt(c.username + ":rec");
        c.recHash = hashPassword(c.recSalt, normAnswer(answer));
    }

    const Credential* find(const std::string& username) const {
        for (const Credential& c : users_)
            if (util::equalsIgnoreCase(c.username, username)) return &c;
        return nullptr;
    }
    Credential* findMutable(const std::string& username) {
        for (Credential& c : users_)
            if (util::equalsIgnoreCase(c.username, username)) return &c;
        return nullptr;
    }

    // Written to a temp file and swapped in, so a crash can never leave a half-written
    // users.csv (which would lock the admin out).
    bool saveUsers() {
        const std::string tmp = std::string(kUsersFile) + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return saveOk_ = false;
            for (const Credential& c : users_)
                out << util::csvEscape(c.username) << ',' << c.salt << ',' << c.hash << ',' << c.recSalt << ',' << c.recHash << ','
                    << util::csvEscape(c.recQuestion) << '\n';
            out.flush();
            if (!out) return saveOk_ = false;
        }
        std::error_code ec;
        // Best effort: owner-only access (honoured on Linux/macOS; mostly ignored on Windows).
        std::filesystem::permissions(tmp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        ec.clear();
        std::filesystem::rename(tmp, kUsersFile, ec);
        if (ec) {
            std::filesystem::remove(kUsersFile, ec);
            std::filesystem::rename(tmp, kUsersFile, ec);
        }
        return saveOk_ = !ec;
    }

    // users.csv: username,salt,hash[,recoverySalt,recoveryHash,question]
    // (the older 3-column format loads fine; such accounts just have no recovery phrase yet).
    void loadSaved() {
        std::ifstream in(kUsersFile);
        if (!in) return;
        std::string line;
        bool sawContent = false;
        while (std::getline(in, line)) {
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (util::trim(line).empty()) continue;
            sawContent = true;
            const std::vector<std::string> f = util::splitCSV(line);
            if (f.size() < 3 || f[0].empty() || f[2].size() != 64 || find(f[0])) continue;
            Credential c;
            c.username = f[0];
            c.salt = f[1];
            c.hash = f[2];
            if (f.size() >= 6 && f[4].size() == 64) { c.recSalt = f[3]; c.recHash = f[4]; c.recQuestion = f[5]; }
            users_.push_back(c);
        }
        if (sawContent && users_.empty()) {  // unreadable file: keep a copy before setup could replace it
            std::error_code ec;
            std::filesystem::copy_file(kUsersFile, std::string(kUsersFile) + ".corrupt",
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
};

std::string randomSalt(const std::string& username) {
    std::random_device rd;
    static const char* digits = "0123456789abcdef";
    std::string s = "RE-PT:" + username + ":";
    for (int i = 0; i < 16; ++i) s += digits[rd() % 16];
    // Mix in the clock so a weak random_device (old MinGW) still gives unique salts.
    s += digits[static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) % 16];
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
    static std::string csvEscape(const std::string& field) { return util::csvEscape(field); }

    std::string toCSV() const {
        std::ostringstream os;
        os << id_ << ',' << csvEscape(name_) << ',' << csvEscape(category_) << ','
           << util::money(price_) << ',' << quantity_ << ',' << minStock_;
        return os.str();
    }

    static std::vector<std::string> splitCSV(const std::string& line) { return util::splitCSV(line); }

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
//  HistoryEntry / HistoryLog: the stock-movement audit trail (history.csv)
//
//  Columns: Timestamp,PartID,PartName,Action,Change,QtyChanged,UpdatedStock,User,Details
//    Action     : Add Part | Edit Part | Delete Part | Adjust Stock
//    Change     : "+" stock went up, "-" stock went down, "=" stock unchanged (e.g. a price edit)
//    QtyChanged : how many units moved (always >= 0; the sign is in Change)
//  The file is append-only: every record is written the moment the change happens.
// ----------------------------------------------------------------------------
struct HistoryEntry {
    std::string timestamp;  // "YYYY-MM-DD HH:MM:SS" (local time)
    int partId = 0;
    std::string partName;
    std::string action;
    char change = '=';
    int qtyChanged = 0;
    int stockAfter = 0;
    std::string user;
    std::string details;
};

class HistoryLog {
public:
    explicit HistoryLog(std::string filename = "history.csv") : filename_(std::move(filename)) { load(); }

    // Appends one record to memory and to the file. Returns false if the file could not be written.
    bool record(HistoryEntry e) {
        e.partName = util::oneLine(e.partName);
        e.details = util::oneLine(e.details);
        e.user = util::oneLine(e.user);
        entries_.push_back(e);
        std::error_code ec;
        const bool needHeader = !std::filesystem::exists(filename_, ec) || std::filesystem::file_size(filename_, ec) == 0;
        std::ofstream out(filename_, std::ios::app);
        if (!out) { lastError_ = "Could not write '" + filename_ + "'."; return false; }
        if (needHeader) out << kHeader << '\n';
        out << toLine(e) << '\n';
        out.flush();
        if (!out) { lastError_ = "Write error while saving '" + filename_ + "'."; return false; }
        return true;
    }

    const std::vector<HistoryEntry>& entries() const { return entries_; }

    // Newest first, at most 'limit' rows (0 = all).
    std::vector<const HistoryEntry*> recent(size_t limit) const {
        std::vector<const HistoryEntry*> out;
        for (size_t i = entries_.size(); i-- > 0;) {
            if (limit != 0 && out.size() >= limit) break;
            out.push_back(&entries_[i]);
        }
        return out;
    }

    std::string toCsv() const {
        std::string out = std::string(kHeader) + "\n";
        for (const HistoryEntry& e : entries_) out += toLine(e) + "\n";
        return out;
    }

    const std::string& filename() const { return filename_; }
    const std::string& lastError() const { return lastError_; }

private:
    static constexpr const char* kHeader = "Timestamp,PartID,PartName,Action,Change,QtyChanged,UpdatedStock,User,Details";

    static std::string toLine(const HistoryEntry& e) {
        std::ostringstream os;
        os << util::csvEscape(e.timestamp) << ',' << e.partId << ',' << util::csvEscape(e.partName) << ',' << util::csvEscape(e.action)
           << ',' << e.change << ',' << e.qtyChanged << ',' << e.stockAfter << ',' << util::csvEscape(e.user) << ','
           << util::csvEscape(e.details);
        return os.str();
    }

    void load() {
        std::ifstream in(filename_);
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (first) {
                first = false;
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                    line.erase(0, 3);
            }
            if (util::trim(line).empty()) continue;
            const std::vector<std::string> f = util::splitCSV(line);
            if (f.size() < 8) continue;
            try {
                HistoryEntry e;
                e.timestamp = f[0];
                e.partId = std::stoi(f[1]);  // the header row throws here and is skipped
                e.partName = f[2];
                e.action = f[3];
                e.change = f[4].empty() ? '=' : f[4][0];
                e.qtyChanged = std::stoi(f[5]);
                e.stockAfter = std::stoi(f[6]);
                e.user = f[7];
                if (f.size() > 8) e.details = f[8];
                entries_.push_back(e);
            } catch (const std::exception&) {
            }
        }
    }

    std::string filename_;
    std::vector<HistoryEntry> entries_;
    std::string lastError_;
};

enum class SortKey { Id, Name, Price, Qty, System };

// ----------------------------------------------------------------------------
//  InventoryManager: owns the collection, CRUD, search, alerts, persistence,
//  the audit log and the automatic backups.
//
//  Every change to a part goes through addPart / updatePart / adjustStock /
//  removePart, which (1) change the data, (2) save parts.csv - first copying
//  the previous version into backups/ - and (3) append a line to history.csv.
//  findById() hands out const pointers only, so nothing can bypass the log.
// ----------------------------------------------------------------------------
class InventoryManager {
public:
    static constexpr const char* kBackupDir = "backups";

    explicit InventoryManager(std::string filename = "parts.csv", std::string historyFile = "history.csv")
        : filename_(std::move(filename)), history_(std::move(historyFile)) { load(); }

    // Who is making the changes (recorded in the history). Set by the console login / web session.
    void setActor(const std::string& user) { actor_ = user; }

    // ---------- Create ----------
    bool addPart(const Part& p) {
        if (findById(p.id())) return false;  // IDs must be unique
        lastWarning_.clear();
        parts_.push_back(p);
        save();
        log(p, "Add Part", p.quantity() > 0 ? '+' : '=', p.quantity(), "new part");
        return true;
    }

    // ---------- Read ----------
    const std::vector<Part>& all() const { return parts_; }

    const Part* findById(int id) const {
        for (const Part& p : parts_) if (p.id() == id) return &p;
        return nullptr;
    }

    // ---------- Update (name / system / price / quantity / minimum) ----------
    // 'np' carries the new values (its id is ignored). Returns false if the part does not exist.
    bool updatePart(int id, const Part& np) {
        Part* cur = mut(id);
        if (!cur) return false;
        std::string details;
        auto note = [&details](const std::string& what, const std::string& from, const std::string& to) {
            if (from == to) return;
            if (!details.empty()) details += "; ";
            details += what + ": " + from + " -> " + to;
        };
        note("name", cur->name(), np.name());
        note("system", cur->category(), np.category());
        note("price", util::money(cur->price()), util::money(np.price()));
        note("min", std::to_string(cur->minStock()), std::to_string(np.minStock()));
        const int delta = np.quantity() - cur->quantity();
        if (details.empty() && delta == 0) return true;  // nothing changed: no save, no log noise
        lastWarning_.clear();
        cur->setName(np.name());
        cur->setCategory(np.category());
        cur->setPrice(np.price());
        cur->setQuantity(np.quantity());
        cur->setMinStock(np.minStock());
        save();
        log(*cur, "Edit Part", delta > 0 ? '+' : delta < 0 ? '-' : '=', delta < 0 ? -delta : delta, details);
        return true;
    }

    // Adds (delta > 0) or removes (delta < 0) units. Returns "" on success or an error message.
    std::string adjustStock(int id, long long delta) {
        Part* p = mut(id);
        if (!p) return "Part not found.";
        if (delta == 0) return "The change must not be zero.";
        const long long n = static_cast<long long>(p->quantity()) + delta;
        if (n < 0) return "Cannot remove " + std::to_string(-delta) + ": only " + std::to_string(p->quantity()) + " in stock.";
        if (n > limits::kMaxQty) return "Stock cannot exceed " + std::to_string(limits::kMaxQty) + " units.";
        lastWarning_.clear();
        p->setQuantity(static_cast<int>(n));
        save();
        log(*p, "Adjust Stock", delta > 0 ? '+' : '-', static_cast<int>(delta < 0 ? -delta : delta), "");
        return "";
    }

    // ---------- Delete ----------
    bool removePart(int id) {
        const Part* found = findById(id);
        if (!found) return false;
        lastWarning_.clear();
        const Part gone = *found;  // copy: the original is about to be erased
        parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [id](const Part& p) { return p.id() == id; }), parts_.end());
        save();
        HistoryEntry e = makeEntry(gone, "Delete Part", gone.quantity() > 0 ? '-' : '=', gone.quantity(), "record removed");
        e.stockAfter = 0;
        if (!history_.record(e)) lastWarning_ += (lastWarning_.empty() ? "" : " ") + history_.lastError();
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

    // ---------- Sorting ----------
    // Sorts by one column. Equal values are always ordered by ID so the result is stable.
    std::vector<const Part*> sorted(SortKey key, bool ascending) const {
        std::vector<const Part*> rows;
        for (const Part& p : parts_) rows.push_back(&p);
        std::sort(rows.begin(), rows.end(), [key, ascending](const Part* a, const Part* b) {
            int cmp = 0;  // <0: a first, >0: b first (for ascending)
            switch (key) {
                case SortKey::Id: cmp = a->id() < b->id() ? -1 : a->id() > b->id() ? 1 : 0; break;
                case SortKey::Name: cmp = util::toLower(a->name()).compare(util::toLower(b->name())); break;
                case SortKey::Price: cmp = a->price() < b->price() ? -1 : a->price() > b->price() ? 1 : 0; break;
                case SortKey::Qty: cmp = a->quantity() < b->quantity() ? -1 : a->quantity() > b->quantity() ? 1 : 0; break;
                case SortKey::System: cmp = util::toLower(a->category()).compare(util::toLower(b->category())); break;
            }
            if (cmp != 0) return ascending ? cmp < 0 : cmp > 0;
            return a->id() < b->id();
        });
        return rows;
    }

    // ---------- Monitoring ----------
    std::vector<const Part*> lowStockParts() const {
        std::vector<const Part*> out;
        for (const Part& p : parts_) if (p.isLowStock()) out.push_back(&p);
        return out;
    }

    // Low-stock parts with the units needed to get back up to the minimum level,
    // grouped by vehicle system (handy when ordering from a supplier).
    struct ReorderLine { const Part* part; int needed; };
    std::vector<ReorderLine> reorderList() const {
        std::vector<ReorderLine> out;
        for (const Part& p : parts_)
            if (p.isLowStock()) out.push_back({&p, p.minStock() - p.quantity()});
        std::sort(out.begin(), out.end(), [](const ReorderLine& a, const ReorderLine& b) {
            const int c = util::toLower(a.part->category()).compare(util::toLower(b.part->category()));
            return c != 0 ? c < 0 : a.part->id() < b.part->id();
        });
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

    // ---------- History / export ----------
    const HistoryLog& history() const { return history_; }

    // The exact text of parts.csv as it would be saved right now (used by the download button).
    std::string toCsv() const {
        std::string out = "PartID,Name,Category,Price,Quantity,MinStock\n";
        for (const Part& p : parts_) out += p.toCSV() + "\n";
        return out;
    }

    // ---------- Persistence ----------
    // Called after every change. Writes to a temp file first, copies the previous
    // version into backups/, then swaps the new file in, so neither a crash mid-write
    // nor an accidental deletion can destroy the data.
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
            out << toCsv();
            out.flush();
            if (!out) { lastError_ = "Write error while saving '" + tmp + "'."; return false; }
        }
        backupPrevious();  // never blocks the save; a failure is reported through lastWarning()
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
    // Non-fatal problems of the last change (backup or history file could not be written).
    const std::string& lastWarning() const { return lastWarning_; }
    int skippedLines() const { return skipped_; }            // bad/duplicate rows ignored at load
    bool createdSampleData() const { return seeded_; }       // true on first run
    const std::string& loadProblem() const { return loadProblem_; }

private:
    // Retention: every backup of the last 7 days is kept; for days 8-90 only the newest
    // backup of each day is kept; anything older is removed. Only files named
    // backups/parts_backup_*.csv are ever touched.
    static constexpr int kKeepAllDays = 7;
    static constexpr int kKeepDailyDays = 90;

    Part* mut(int id) {
        for (Part& p : parts_) if (p.id() == id) return &p;
        return nullptr;
    }

    HistoryEntry makeEntry(const Part& p, const char* action, char change, int qty, const std::string& details) const {
        HistoryEntry e;
        e.timestamp = util::nowStamp();
        e.partId = p.id();
        e.partName = p.name();
        e.action = action;
        e.change = change;
        e.qtyChanged = qty;
        e.stockAfter = p.quantity();
        e.user = actor_.empty() ? "system" : actor_;
        e.details = details;
        return e;
    }

    void log(const Part& p, const char* action, char change, int qty, const std::string& details) {
        if (!history_.record(makeEntry(p, action, change, qty, details)))
            lastWarning_ += (lastWarning_.empty() ? "" : " ") + history_.lastError();
    }

    // Copies the CURRENT parts.csv (the state before this change) to
    // backups/parts_backup_YYYYMMDD_HHMMSS.csv.
    void backupPrevious() {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(filename_, ec) || fs::file_size(filename_, ec) == 0) return;  // nothing to protect yet
        fs::create_directories(kBackupDir, ec);
        if (ec) { lastWarning_ += (lastWarning_.empty() ? "" : " ") + std::string("Backup failed: could not create the '") + kBackupDir + "' folder."; return; }
        const std::string stamp = util::formatTime(std::chrono::system_clock::now(), "%Y%m%d_%H%M%S");
        std::string target;
        for (int n = 1; n < 1000; ++n) {  // several changes in the same second get _2, _3, ...
            target = std::string(kBackupDir) + "/parts_backup_" + stamp + (n > 1 ? "_" + std::to_string(n) : "") + ".csv";
            if (!fs::exists(target, ec)) break;
        }
        ec.clear();
        fs::copy_file(filename_, target, fs::copy_options::none, ec);
        if (ec) { lastWarning_ += (lastWarning_.empty() ? "" : " ") + ("Backup failed: " + ec.message()); return; }
        const std::string today = util::formatTime(std::chrono::system_clock::now(), "%Y%m%d");
        if (today != lastPruneDay_) { pruneBackups(); lastPruneDay_ = today; }
    }

    void pruneBackups() {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto now = std::chrono::system_clock::now();
        const std::string keepAll = util::formatTime(now - std::chrono::hours(24 * kKeepAllDays), "%Y%m%d");
        const std::string keepDaily = util::formatTime(now - std::chrono::hours(24 * kKeepDailyDays), "%Y%m%d");
        std::vector<std::string> names;
        for (fs::directory_iterator it(kBackupDir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            // parts_backup_YYYYMMDD_HHMMSS[_k].csv
            if (n.size() >= 28 && n.compare(0, 13, "parts_backup_") == 0 && n.compare(n.size() - 4, 4, ".csv") == 0 &&
                std::all_of(n.begin() + 13, n.begin() + 21, [](unsigned char c) { return std::isdigit(c) != 0; }))
                names.push_back(n);
        }
        std::sort(names.begin(), names.end());  // timestamps sort chronologically
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string day = names[i].substr(13, 8);
            const bool newestOfDay = i + 1 == names.size() || names[i + 1].compare(13, 8, day) != 0;
            const bool drop = day < keepDaily || (day < keepAll && !newestOfDay);
            if (drop) fs::remove(fs::path(kBackupDir) / names[i], ec);
        }
    }

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
    HistoryLog history_;
    std::vector<Part> parts_;
    std::string actor_;
    bool lastSaveOk_ = true;
    bool saveBlocked_ = false;
    bool seeded_ = false;
    int skipped_ = 0;
    std::string lastError_;
    std::string lastWarning_;
    std::string loadProblem_;
    std::string lastPruneDay_;
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
    row(kDouble, "Bajaj RE Components & Inventory Management System   v2.1", kCyan, nullptr, Align::Center);
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
    std::string contentType = "text/html; charset=utf-8";
    std::string attachment;  // non-empty => the browser downloads the body under this file name
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

    // Signs a user out everywhere (optionally keeping one session), e.g. after a password change.
    void destroyUser(const std::string& user, const std::string& keepToken = "") {
        for (auto it = sessions_.begin(); it != sessions_.end();)
            it = (it->second.user == user && it->first != keepToken) ? sessions_.erase(it) : std::next(it);
    }

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
                  << "\n  Sign in with the admin password you chose during setup."
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
    int failedReauth_ = 0;  // wrong "current password" attempts on the Security page
    SessionStore::Clock::time_point reauthLockedUntil_{};

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
            h << "HTTP/1.1 " << res.status << ' ' << text << "\r\nContent-Type: " << res.contentType << "\r\n"
              << "Content-Length: " << res.body.size() << "\r\n";
            if (!res.attachment.empty()) h << "Content-Disposition: attachment; filename=\"" << res.attachment << "\"\r\n";
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
    static Response redirectMsg(const std::string& msg, const std::string& kind = "ok", const std::string& extra = "") {
        return redirectTo("/?msg=" + urlEncode(msg) + "&kind=" + kind + extra);
    }
    static Response csvDownload(const std::string& baseName, const std::string& csv) {
        Response r;
        r.contentType = "text/csv; charset=utf-8";
        r.attachment = baseName + "_" + util::formatTime(std::chrono::system_clock::now(), "%Y%m%d_%H%M%S") + ".csv";
        r.body = "\xEF\xBB\xBF" + csv;  // UTF-8 BOM so Excel shows every character correctly
        return r;
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
        inv_.setActor(curUser_);  // every change made during this request is logged under this name

        if (r.method == "GET") {
            if (r.path == "/") return htmlPage(200, dashboard(r));
            if (r.path == "/history") return htmlPage(200, historyPage(r));
            if (r.path == "/reorder") return htmlPage(200, reorderPage());
            if (r.path == "/security") return htmlPage(200, securityPage(get(r.query, "msg"), get(r.query, "kind")));
            if (r.path == "/export.csv") return csvDownload("parts_export", inv_.toCsv());
            if (r.path == "/export-history.csv") return csvDownload("stock_history", inv_.history().toCsv());
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
            if (r.path == "/security/password") return doChangePassword(r, token);
            if (r.path == "/security/recovery") return doChangeRecovery(r);
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
a.sort{display:inline-flex;align-items:center;gap:6px;color:inherit;text-decoration:none}a.sort:hover{color:#fff}
th.num a.sort{justify-content:flex-end}.arr{font-size:.68rem;opacity:.4}th.on{color:#fff}th.on .arr{opacity:1;color:#5eead4}
.chg{display:inline-grid;place-items:center;width:26px;height:26px;border-radius:8px;font-weight:800}
.chg.plus{background:#dcfce7;color:#15803d}.chg.minus{background:#fee2e2;color:#b91c1c}.chg.eq{background:#e2e8f0;color:#475569}
.tw.short{max-height:340px}.hd{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:12px}.hd h2{margin:0}
td.det{white-space:normal;color:#475569;max-width:320px}.tot{display:flex;gap:26px;flex-wrap:wrap;margin-top:14px;font-weight:600}.tot span{color:var(--mut);font-weight:500}
.stat-line{display:flex;align-items:center;gap:8px;font-size:.88rem;margin:0 0 6px}.pill{padding:2px 10px;border-radius:99px;font-size:.74rem;font-weight:700}.pill.on{background:#dcfce7;color:#15803d}.pill.off{background:#fee2e2;color:#b91c1c}
.print-only{display:none}
@media print{
  body,body.app{background:#fff!important;color:#000!important}
  header.top,.noprint,.flash,.toolbar{display:none!important}
  main{margin:0;max-width:none;padding:0}
  .card{box-shadow:none!important;padding:0!important;border-radius:0}
  .tw{max-height:none!important;overflow:visible!important;border:0!important}
  thead th{position:static!important;background:#fff!important;color:#000!important;border-bottom:2px solid #000}
  td{border-bottom:1px solid #999!important;background:#fff!important}
  .print-only{display:block}
}
)html";

    static constexpr const char* kJs = R"html(
(function(){
  var $=function(i){return document.getElementById(i)};
  function dismiss(el){el.classList.add('gone');setTimeout(function(){el.remove()},260)}
  document.querySelectorAll('.flash[data-auto]').forEach(function(f){setTimeout(function(){if(f.isConnected)dismiss(f)},6000)});
  if(/[?&]msg=/.test(location.search)&&history.replaceState){var cu=new URL(location.href);cu.searchParams.delete('msg');cu.searchParams.delete('kind');history.replaceState(null,'',cu.pathname+cu.search)}
  var dm=$('dm');
  function closeDel(){if(dm)dm.hidden=true}
  document.addEventListener('click',function(e){
    var t=e.target.closest('[data-dismiss]');
    if(t){dismiss(t.closest('.flash,.alert'));return}
    t=e.target.closest('[data-toggle-pw]');
    if(t){var i=t.parentNode.querySelector('input');i.type=i.type==='password'?'text':'password';return}
    t=e.target.closest('[data-print]');
    if(t){window.print();return}
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
    var rows=[].slice.call(document.querySelectorAll('#inv tbody tr[data-s]'));
    var P=new URLSearchParams(location.search);
    if(P.get('q'))q.value=P.get('q');
    if(P.get('sys'))sys.value=P.get('sys');
    if(P.get('low')==='1')low.setAttribute('aria-pressed','true');
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
    // Sorting reloads the page (the server sorts); carry the active filters along.
    document.querySelectorAll('a.sort').forEach(function(a){
      a.addEventListener('click',function(e){
        e.preventDefault();
        var u=new URL(a.href);
        if(q.value.trim())u.searchParams.set('q',q.value.trim());
        if(sys.value)u.searchParams.set('sys',sys.value);
        if(low.getAttribute('aria-pressed')==='true')u.searchParams.set('low','1');
        location.href=u.pathname+u.search;
      });
    });
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
    static constexpr const char* kDownload = "<path d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3'/>";
    static constexpr const char* kClock = "<circle cx='12' cy='12' r='9'/><path d='M12 7v5l3 2'/>";
    static constexpr const char* kPrint = "<path d='M6 9V2h12v7M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2'/><path d='M6 14h12v8H6z'/>";
    static constexpr const char* kCart = "<circle cx='9' cy='20' r='1.4'/><circle cx='18' cy='20' r='1.4'/><path d='M2 3h3l2.7 12.4a2 2 0 0 0 2 1.6h7.7a2 2 0 0 0 2-1.5L21 7H6'/>";
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
        const std::string question = auth_.recoveryQuestion();
        if (question.empty()) {
            h += flashBox("No recovery phrase has been set up. Sign in and add one on the Security page, or reset the account from the console.", "err") +
                 "<a class='back' href='/login'>" + ic(kArrowL, 15) + "Back to sign in</a>";
            return authPage("Forgot password", h);
        }
        h += "<div class='qbox'>" + ic(kShield, 20) + "<span>" + htmlEscape(question) + "</span></div>"
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
        if (auth_.recoveryQuestion().empty()) return htmlPage(200, forgotPage(""));
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
        if (const std::string perr = auth::Authenticator::checkPassword(pw); !perr.empty()) return htmlPage(200, resetPage(token, perr));
        if (pw != get(r.form, "confirm")) return htmlPage(200, resetPage(token, "The two passwords do not match."));
        if (!auth_.setPassword(it->second.user, pw)) return htmlPage(200, forgotPage("Account not found."));
        const bool saved = auth_.lastSaveOk();
        sessions_.destroyUser(it->second.user);  // a reset signs the account out everywhere
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
                 htmlEscape(curUser_) + "</span><a class='btn out sm' href='/security'>" + ic(kShield, 15) + "Security</a>"
                 "<button class='btn out sm' type='submit'>" + ic(kLogout, 15) + "Log out</button></form>";
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

    // ---- Sorting (server side): /?sort=id|name|price|qty|system&dir=asc|desc ----
    struct SortSpec { SortKey key = SortKey::Id; bool asc = true; std::string keyName = "id", dirName = "asc"; };

    static SortSpec parseSort(const Params& q) {
        static const std::pair<const char*, SortKey> keys[] = {{"id", SortKey::Id}, {"name", SortKey::Name}, {"price", SortKey::Price},
                                                               {"qty", SortKey::Qty}, {"system", SortKey::System}};
        SortSpec sp;
        const std::string k = get(q, "sort"), d = get(q, "dir");
        for (const auto& kv : keys) if (k == kv.first) { sp.key = kv.second; sp.keyName = kv.first; }
        if (d == "desc") { sp.asc = false; sp.dirName = "desc"; }
        return sp;
    }

    static std::string sortQuery(const SortSpec& sp) { return "&sort=" + sp.keyName + "&dir=" + sp.dirName; }

    static std::string changeChip(const HistoryEntry& e) {
        const char* cls = e.change == '+' ? "plus" : e.change == '-' ? "minus" : "eq";
        const char* sym = e.change == '+' ? "+" : e.change == '-' ? "&minus;" : "=";
        return std::string("<span class='chg ") + cls + "' title='" + (e.change == '+' ? "Stock increased" : e.change == '-' ? "Stock decreased" : "Stock unchanged") +
               "'>" + sym + "</span>";
    }

    // The audit-log table (newest entry first). 'details' adds the free-text column.
    static std::string historyTable(const std::vector<const HistoryEntry*>& rows, bool details, bool shortBox) {
        std::string h = std::string("<div class='tw") + (shortBox ? " short" : "") + "'><table><thead><tr><th>Time</th><th>ID</th><th>Part name</th><th>Action</th>"
                        "<th>+/&minus;</th><th class='num'>Qty changed</th><th class='num'>Stock after</th><th>By</th>" +
                        (details ? "<th>Details</th>" : "") + "</tr></thead><tbody>";
        for (const HistoryEntry* e : rows) {
            h += "<tr><td>" + htmlEscape(e->timestamp) + "</td><td><a href='/history?id=" + std::to_string(e->partId) + "'>" + std::to_string(e->partId) +
                 "</a></td><td class='nm'>" + htmlEscape(e->partName) + "</td><td title='" + htmlEscape(e->details) + "'>" + htmlEscape(e->action) + "</td><td>" +
                 changeChip(*e) + "</td><td class='num'>" + std::to_string(e->qtyChanged) + "</td><td class='num'>" + std::to_string(e->stockAfter) +
                 "</td><td>" + htmlEscape(e->user) + "</td>" + (details ? "<td class='det'>" + htmlEscape(e->details) + "</td>" : "") + "</tr>";
        }
        if (rows.empty()) h += std::string("<tr><td class='empty' colspan='") + (details ? "9" : "8") + "'>No stock movements recorded yet.</td></tr>";
        return h + "</tbody></table></div>";
    }

    std::string dashboard(const Request& r) {
        double value = 0;
        long units = 0;
        for (const Part& p : inv_.all()) { value += p.stockValue(); units += p.quantity(); }
        auto low = inv_.lowStockParts();
        const SortSpec sp = parseSort(r.query);
        const std::vector<const Part*> rows = inv_.sorted(sp.key, sp.asc);

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

        // Clickable, sortable column header: first click = ascending, next click = descending.
        auto th = [&sp](const char* label, const char* key, SortKey sk, const char* cls) {
            const bool on = sp.key == sk;
            const char* nextDir = (on && sp.asc) ? "desc" : "asc";
            const char* arrow = on ? (sp.asc ? "&#9650;" : "&#9660;") : "&#8597;";
            return std::string("<th class='") + cls + (on ? " on" : "") + "' aria-sort='" + (on ? (sp.asc ? "ascending" : "descending") : "none") +
                   "'><a class='sort' href='/?sort=" + key + "&amp;dir=" + nextDir + "' title='Sort by " + label + "'>" + label + "<span class='arr'>" + arrow + "</span></a></th>";
        };

        h += "<div class='card'><div class='toolbar'><div class='search'>" + ic(kSearch) +
             "<input id='q' type='text' placeholder='Search by ID, name or system  ( press / )' autocomplete='off' value='" + htmlEscape(util::trim(get(r.query, "q"))) + "'>"
             "<button id='clr' class='x' type='button' aria-label='Clear search' hidden>" + ic(kX, 14) + "</button></div>"
             "<select id='sys' aria-label='Vehicle system'><option value=''>All vehicle systems</option>";
        for (const auto& c : inv_.categories()) h += "<option value='" + htmlEscape(c) + "'>" + htmlEscape(c) + "</option>";
        h += "</select><button id='lowOnly' class='btn ghost tog' type='button' aria-pressed='false'>" + ic(kWarn, 16) + "Low stock only</button>"
             "<button id='reset' class='btn ghost' type='button'>Reset</button><span class='sp'></span>"
             "<a class='btn ghost' href='/reorder'>" + ic(kCart, 16) + "Reorder list" + (low.empty() ? "" : " (" + std::to_string(low.size()) + ")") + "</a>"
             "<a class='btn ghost' href='/export.csv'>" + ic(kDownload, 16) + "Download CSV Export</a>"
             "<a class='btn teal' href='/add'>" + ic(kPlus, 16) + "Add new part</a></div>"
             "<div class='tw'><table id='inv'><thead><tr>" + th("ID", "id", SortKey::Id, "") + th("Part name", "name", SortKey::Name, "") +
             th("System", "system", SortKey::System, "") + th("Price (PHP)", "price", SortKey::Price, "num") + th("Qty", "qty", SortKey::Qty, "num") +
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
                 "'><input type='hidden' name='sort' value='" + sp.keyName + "'><input type='hidden' name='dir' value='" + sp.dirName +
                 "'><input type='number' name='qty' value='1' min='1' aria-label='Quantity'><button class='sq plus' name='op' value='add' title='Add stock'>+</button>"
                 "<button class='sq minus' name='op' value='remove' title='Remove stock'>&minus;</button></form></td>"
                 "<td><div class='acts'><a class='btn sm ghost' href='/edit?id=" + id + "'>" + ic(kEdit, 14) + "Edit</a>"
                 "<a class='btn sm danger' href='/delete?id=" + id + "' data-del data-id='" + id + "' data-name='" + htmlEscape(p->name()) + "'>" +
                 ic(kTrash, 14) + "Delete</a></div></td></tr>";
        }
        h += "</tbody></table><div id='none' class='empty'" + std::string(rows.empty() ? "" : " hidden") + ">No parts match your search.</div></div>"
             "<div class='meta'>Showing <b id='count'>" + std::to_string(rows.size()) + "</b> of " + std::to_string(rows.size()) + " parts &middot; click a column title to sort</div></div>";

        // ---- Stock history (latest movements) ----
        h += "<div class='card'><div class='hd'><h2>Stock history</h2><div class='acts'>"
             "<a class='btn sm ghost' href='/history'>" + ic(kClock, 14) + "Full history</a>"
             "<a class='btn sm ghost' href='/export-history.csv'>" + ic(kDownload, 14) + "Download history CSV</a></div></div>"
             "<p class='sub' style='margin-bottom:12px'>Every add, edit, delete and stock adjustment is recorded automatically in <b>" + htmlEscape(inv_.history().filename()) +
             "</b>. Showing the latest 15.</p>" + historyTable(inv_.history().recent(15), false, true) + "</div>";

        h += "<div class='modal-back' id='dm' hidden><div class='modal' role='dialog' aria-modal='true' aria-labelledby='dmT'>"
             "<div class='m-ic'>" + ic(kTrash, 26) + "</div><h3 id='dmT'>Delete this part?</h3><p><b id='dmText'></b></p>"
             "<p>This removes the record from the inventory. The previous data is kept in the automatic backups folder.</p><form method='post' action='/delete'>"
             "<input type='hidden' name='id' id='dmId'><div class='m-actions'><button type='button' class='btn ghost' data-close>Cancel</button>"
             "<button class='btn danger' type='submit'>Yes, delete</button></div></form></div></div>";
        return layout("Inventory", h, get(r.query, "msg"), get(r.query, "kind"));
    }

    // ---- Full history page (optionally filtered to one part: /history?id=105) ----
    std::string historyPage(const Request& r) {
        constexpr size_t kLimit = 500;
        int id = 0;
        const bool filtered = toInt(get(r.query, "id"), 1, limits::kMaxId, id);
        std::vector<const HistoryEntry*> rows;
        for (const HistoryEntry* e : inv_.history().recent(0)) {
            if (filtered && e->partId != id) continue;
            if (rows.size() >= kLimit) break;
            rows.push_back(e);
        }
        std::string h = "<div class='card'><div class='hd'><h2>Stock history" + (filtered ? " - part " + std::to_string(id) : std::string()) + "</h2><div class='acts'>" +
                        (filtered ? "<a class='btn sm ghost' href='/history'>All parts</a>" : "") +
                        "<a class='btn sm ghost' href='/export-history.csv'>" + ic(kDownload, 14) + "Download history CSV</a>"
                        "<a class='btn sm ghost' href='/'>" + ic(kArrowL, 14) + "Back to inventory</a></div></div>"
                        "<p class='sub' style='margin-bottom:12px'>Newest first. Showing " + std::to_string(rows.size()) + " of " +
                        (filtered ? "the matching" : std::to_string(inv_.history().entries().size())) + " record(s)" +
                        (rows.size() >= kLimit ? " (latest " + std::to_string(kLimit) + "; download the CSV for everything)" : "") + ".</p>" +
                        historyTable(rows, true, false) + "</div>";
        return layout("Stock history", h);
    }

    // ---- Printable reorder list ----
    std::string reorderPage() {
        const auto lines = inv_.reorderList();
        long long units = 0;
        double cost = 0;
        for (const auto& l : lines) { units += l.needed; cost += l.needed * l.part->price(); }
        std::string h = "<div class='card'><div class='hd'><h2>Reorder list</h2><div class='acts noprint'>"
                        "<button class='btn sm teal' type='button' data-print>" + ic(kPrint, 14) + "Print</button>"
                        "<a class='btn sm ghost' href='/'>" + ic(kArrowL, 14) + "Back to inventory</a></div></div>"
                        "<p class='sub'>Parts below their minimum stock, with the quantity needed to get back up to the minimum. Generated " +
                        htmlEscape(util::nowStamp()) + " by " + htmlEscape(curUser_) + ".</p>";
        if (lines.empty()) {
            h += flashBox("All parts are at or above their minimum stock. Nothing to reorder.", "ok");
        } else {
            h += "<div class='tw'><table><thead><tr><th>ID</th><th>Part name</th><th>System</th><th class='num'>In stock</th><th class='num'>Minimum</th>"
                 "<th class='num'>Reorder qty</th><th class='num'>Unit price (PHP)</th><th class='num'>Est. cost (PHP)</th></tr></thead><tbody>";
            for (const auto& l : lines) {
                const Part& p = *l.part;
                h += "<tr class='" + std::string(p.isOutOfStock() ? "out" : "low") + "'><td>" + std::to_string(p.id()) + "</td><td class='nm'>" + htmlEscape(p.name()) +
                     "</td><td>" + htmlEscape(p.category()) + "</td><td class='num'>" + std::to_string(p.quantity()) + "</td><td class='num'>" + std::to_string(p.minStock()) +
                     "</td><td class='num'><b>" + std::to_string(l.needed) + "</b></td><td class='num'>" + util::moneyFmt(p.price()) +
                     "</td><td class='num'>" + util::moneyFmt(l.needed * p.price()) + "</td></tr>";
            }
            h += "</tbody></table></div><div class='tot'><div><span>Parts to reorder</span><br>" + std::to_string(lines.size()) + "</div><div><span>Total units</span><br>" +
                 std::to_string(units) + "</div><div><span>Estimated cost</span><br>PHP " + util::moneyFmt(cost) + "</div></div>";
        }
        return layout("Reorder list", h + "</div>");
    }

    // ---- Security page: change password / recovery phrase ----
    std::string securityPage(const std::string& flash = "", const std::string& kind = "ok") const {
        const bool has = auth_.hasRecovery(curUser_);
        std::string h = "<div class='page-h'><h2>Security</h2><p>Change the admin password or the recovery phrase. Neither is stored in plain text.</p></div>"
                        "<div class='card' style='max-width:640px'><h2>Change password</h2><p class='sub'>Other devices signed in as this user are signed out.</p>"
                        "<form method='post' action='/security/password' autocomplete='off'>"
                        "<label for='cp'>Current password</label>" + pwField("cp", "current", "Current password") +
                        "<label for='np1'>New password (" + std::to_string(auth::policy::kMinPassword) + "+ characters)</label>" + pwField("np1", "password", "New password") +
                        "<label for='np2'>Confirm new password</label>" + pwField("np2", "confirm", "Repeat new password") +
                        "<div class='fa'><button class='btn teal' type='submit'>" + ic(kCheck, 16) + "Update password</button></div></form></div>"
                        "<div class='card' style='max-width:640px'><h2>Recovery phrase</h2>"
                        "<p class='stat-line'>Status: <span class='pill " + std::string(has ? "on'>SET UP" : "off'>NOT SET UP") + "</span></p>"
                        "<p class='sub'>Used on the &ldquo;Forgot password&rdquo; page. Treat it like a second password: anyone who knows the answer can reset the account.</p>"
                        "<form method='post' action='/security/recovery' autocomplete='off'>"
                        "<label for='rq'>Recovery question</label><input id='rq' type='text' name='question' required maxlength='" + std::to_string(auth::policy::kMaxQuestion) +
                        "' value='" + htmlEscape(auth_.recoveryQuestion()) + "' placeholder='e.g. What was the name of my first workshop?'>"
                        "<label for='ra1'>Recovery answer (" + std::to_string(auth::policy::kMinRecovery) + "+ characters, not case-sensitive)</label>" + pwField("ra1", "answer", "Secret answer") +
                        "<label for='ra2'>Confirm answer</label>" + pwField("ra2", "answer2", "Repeat the answer") +
                        "<label for='rp'>Current password (to confirm it is you)</label>" + pwField("rp", "current", "Current password") +
                        "<div class='fa'><button class='btn teal' type='submit'>" + ic(kShield, 16) + "Save recovery phrase</button></div></form></div>";
        return layout("Security", h, flash, kind);
    }

    // Throttles wrong "current password" guesses made from an already signed-in session.
    bool reauth(const std::string& password, std::string& error) {
        const auto now = SessionStore::Clock::now();
        if (now < reauthLockedUntil_) {
            error = "Too many wrong attempts. Try again in " +
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(reauthLockedUntil_ - now).count() + 1) + " seconds.";
            return false;
        }
        if (auth_.login(curUser_, password)) { failedReauth_ = 0; return true; }
        if (++failedReauth_ >= 5) { failedReauth_ = 0; reauthLockedUntil_ = now + std::chrono::seconds(30); }
        error = "The current password is incorrect.";
        return false;
    }

    Response doChangePassword(const Request& r, const std::string& token) {
        std::string err;
        if (!reauth(get(r.form, "current"), err)) return htmlPage(200, securityPage(err, "err"));
        if (get(r.form, "password") != get(r.form, "confirm")) return htmlPage(200, securityPage("The two new passwords do not match.", "err"));
        err = auth_.changePassword(curUser_, get(r.form, "current"), get(r.form, "password"));
        if (!err.empty()) return htmlPage(200, securityPage(err, "err"));
        sessions_.destroyUser(curUser_, token);
        return redirectTo("/security?msg=" + urlEncode("Password updated. Other sessions were signed out.") + "&kind=ok");
    }

    Response doChangeRecovery(const Request& r) {
        std::string err;
        if (!reauth(get(r.form, "current"), err)) return htmlPage(200, securityPage(err, "err"));
        if (get(r.form, "answer") != get(r.form, "answer2")) return htmlPage(200, securityPage("The two recovery answers do not match.", "err"));
        err = auth_.changeRecovery(curUser_, get(r.form, "current"), get(r.form, "question"), get(r.form, "answer"));
        if (!err.empty()) return htmlPage(200, securityPage(err, "err"));
        return redirectTo("/security?msg=" + urlEncode("Recovery phrase saved.") + "&kind=ok");
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
        const Part* p = toInt(get(r.query, "id"), 1, limits::kMaxId, id) ? inv_.findById(id) : nullptr;
        if (!p) return notFoundPage();
        return partForm("Update part " + std::to_string(id), "/edit", p, "");
    }

    // Fallback confirmation page (used only when JavaScript is unavailable).
    std::string deleteConfirm(const Request& r) {
        int id = 0;
        const Part* p = toInt(get(r.query, "id"), 1, limits::kMaxId, id) ? inv_.findById(id) : nullptr;
        if (!p) return notFoundPage();
        return layout("Delete part",
                      "<div class='card' style='max-width:520px'><h2>Delete this part?</h2><p class='sub'><b>[" + std::to_string(id) + "] " + htmlEscape(p->name()) +
                      "</b><br>" + htmlEscape(p->category()) + " &middot; " + std::to_string(p->quantity()) + " in stock</p>" +
                      flashBox("This removes the record (a backup of the previous data is kept).", "err") + "<form method='post' action='/delete'><input type='hidden' name='id' value='" +
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

    // Reports a failed disk write, or a non-fatal problem (backup / history file) after a successful one.
    Response afterSave(const std::string& okMsg, const std::string& extra = "") {
        if (!inv_.lastSaveOk()) return redirectMsg("Changed in memory but NOT saved to disk: " + inv_.lastError(), "err", extra);
        if (!inv_.lastWarning().empty()) return redirectMsg(okMsg + " Warning: " + inv_.lastWarning(), "err", extra);
        return redirectMsg(okMsg, "ok", extra);
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
        if (!toInt(get(r.form, "id"), 1, limits::kMaxId, id) || !inv_.findById(id)) return bad("Part not found.");
        Part p;
        std::string err = readPart(r.form, p, false);
        if (!err.empty()) return bad(err);
        inv_.updatePart(id, p);
        return afterSave("Part " + std::to_string(id) + " updated and saved.");
    }

    Response doAdjust(const Request& r) {
        int id = 0, qty = 0;
        const std::string back = sortQuery(parseSort(r.form));  // keep the table sorted the way the user had it
        if (!toInt(get(r.form, "id"), 1, limits::kMaxId, id) || !inv_.findById(id)) return bad("Part not found.");
        if (!toInt(get(r.form, "qty"), 1, limits::kMaxQty, qty)) return bad("Enter a whole number of 1 or more.");
        const std::string err = inv_.adjustStock(id, get(r.form, "op") == "remove" ? -static_cast<long long>(qty) : qty);
        if (!err.empty()) return redirectMsg(err, "err", back);
        return afterSave("Stock of part " + std::to_string(id) + " is now " + std::to_string(inv_.findById(id)->quantity()) + ".", back);
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
            inv_.setActor("");
            if (exitApp) break;
        }
        goodbye();
    }

    // Runs before anything else. A fresh install has NO password: the admin chooses one here
    // (plus a recovery question/answer). Returns false if setup could not be completed.
    bool ensureSetup() {
        if (!auth_.isConfigured()) return firstRunSetup();
        if (!auth_.hasRecovery(auth_.adminName())) {
            ui::notify(ui::Kind::Warning, "No recovery phrase is set for '" + auth_.adminName() +
                                              "', so \"Forgot password\" is disabled. You can add one now (or later from the menu / the web Security page).");
            if (ui::confirm("Set up the recovery phrase now?")) changeRecoveryFlow(auth_.adminName());
        }
        return true;
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
                    inv_.setActor(*name);
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
            auto c = ui::askInt("Select an option [0-14]: ", 0, 14);
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
                case 12: stockHistory(); break;
                case 13: reorderReport(); break;
                case 14: securityMenu(); break;
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
        item(12, "Stock history", "Audit log of every stock change");
        item(13, "Reorder list", "Low-stock parts + quantity to order");
        ui::section("WEB");
#ifndef RE_NO_WEB
        item(11, "Web dashboard (Chrome)", "Open this inventory in your browser");
#else
        item(11, "Web dashboard (disabled)", "Built with -DRE_NO_WEB");
#endif
        ui::section("SESSION");
        item(14, "Security settings", "Change password / recovery phrase");
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

    // Backup / history problems never block a save, but the user must hear about them.
    void reportWarning() {
        if (!inv_.lastWarning().empty()) ui::notify(ui::Kind::Warning, inv_.lastWarning());
    }

    // ============================ Audit log ===============================
    void stockHistory() {
        ui::screen("STOCK HISTORY");
        const auto rows = inv_.history().recent(25);
        if (rows.empty()) { ui::notify(ui::Kind::Info, "No stock movements recorded yet."); return; }
        static const size_t w[8] = {19, 6, 15, 12, 3, 5, 5, 6};
        static const char* heads[8] = {"Time", "ID", "Part Name", "Action", "+/-", "Qty", "Stock", "By"};
        auto line = [&](const char* l, const char* m, const char* r) {
            std::string t = l;
            for (int i = 0; i < 8; ++i) t += util::rep("─", w[i] + 2) + (i < 7 ? m : r);
            std::cout << t << '\n';
        };
        auto cell = [&](int i, const std::string& text, bool right, const char* color = nullptr) {
            const std::string c = util::clip(text, w[i]);
            return " " + ui::paint(right ? util::padLeft(c, w[i]) : util::padRight(c, w[i]), color) + " ";
        };
        std::cout << '\n';
        line("┌", "┬", "┐");
        std::cout << "│";
        for (int i = 0; i < 8; ++i) std::cout << cell(i, heads[i], i == 1 || (i >= 4 && i <= 6), ui::kBoldCyan) << "│";
        std::cout << '\n';
        line("├", "┼", "┤");
        for (const HistoryEntry* e : rows) {
            const char* color = e->change == '+' ? ui::kGreen : e->change == '-' ? ui::kYellow : nullptr;
            std::cout << "│" << cell(0, e->timestamp, false) << "│" << cell(1, std::to_string(e->partId), true) << "│" << cell(2, e->partName, false) << "│"
                      << cell(3, e->action, false) << "│" << cell(4, std::string(1, e->change), true, color) << "│" << cell(5, std::to_string(e->qtyChanged), true)
                      << "│" << cell(6, std::to_string(e->stockAfter), true) << "│" << cell(7, e->user, false) << "│\n";
        }
        line("└", "┴", "┘");
        std::cout << "  Latest " << rows.size() << " of " << inv_.history().entries().size() << " record(s), newest first. Full log: "
                  << inv_.history().filename() << "\n";
    }

    // ============================ Reorder list ============================
    void reorderReport() {
        ui::screen("REORDER LIST");
        const auto lines = inv_.reorderList();
        if (lines.empty()) { ui::notify(ui::Kind::Success, "All parts are at or above their minimum threshold. Nothing to reorder."); return; }
        static const size_t w[7] = {6, 20, 12, 6, 6, 9, 12};
        static const char* heads[7] = {"ID", "Part Name", "System", "Stock", "Min", "Order qty", "Est. cost"};
        auto line = [&](const char* l, const char* m, const char* r) {
            std::string t = l;
            for (int i = 0; i < 7; ++i) t += util::rep("─", w[i] + 2) + (i < 6 ? m : r);
            std::cout << t << '\n';
        };
        auto cell = [&](int i, const std::string& text, const char* color = nullptr) {
            const std::string c = util::clip(text, w[i]);
            return " " + ui::paint(i == 1 || i == 2 ? util::padRight(c, w[i]) : util::padLeft(c, w[i]), color) + " ";
        };
        std::cout << '\n';
        line("┌", "┬", "┐");
        std::cout << "│";
        for (int i = 0; i < 7; ++i) std::cout << cell(i, heads[i], ui::kBoldCyan) << "│";
        std::cout << '\n';
        line("├", "┼", "┤");
        long long units = 0;
        double cost = 0;
        for (const auto& l : lines) {
            const Part& p = *l.part;
            units += l.needed;
            cost += l.needed * p.price();
            const char* color = p.isOutOfStock() ? ui::kBoldRed : ui::kYellow;
            std::cout << "│" << cell(0, std::to_string(p.id()), color) << "│" << cell(1, p.name(), color) << "│" << cell(2, p.category(), color) << "│"
                      << cell(3, std::to_string(p.quantity()), color) << "│" << cell(4, std::to_string(p.minStock()), color) << "│"
                      << cell(5, std::to_string(l.needed), color) << "│" << cell(6, util::moneyFmt(l.needed * p.price()), color) << "│\n";
        }
        line("└", "┴", "┘");
        std::cout << "  " << lines.size() << " part(s) to reorder  |  " << units << " unit(s)  |  estimated cost PHP " << util::moneyFmt(cost)
                  << "\n  Tip: the web dashboard has a printable version of this list.\n";
    }

    // ===================== First-run setup & security =====================
    // Asks twice (hidden) until both entries match and pass 'check'. Returns false on EOF.
    bool askSecret(const std::string& label, const std::string& again, std::string& out, std::string (*check)(const std::string&)) {
        while (true) {
            std::string a, b;
            if (!ui::readPassword(label, a)) return false;
            if (check) {
                const std::string err = check(a);
                if (!err.empty()) { ui::problem(err); continue; }
            }
            if (!ui::readPassword(again, b)) return false;
            if (a != b) { ui::problem("The two entries do not match. Try again."); continue; }
            out = a;
            return true;
        }
    }

    static std::string checkAnswerOnly(const std::string& a) { return auth::Authenticator::checkRecovery("question", a); }

    // Asks for a recovery question + answer. Returns false on EOF.
    bool askRecovery(std::string& question, std::string& answer, const std::string& currentQuestion = "") {
        while (true) {
            if (!currentQuestion.empty()) std::cout << "  Current question: " << currentQuestion << '\n';
            ui::hint("Pick a question only you can answer, e.g. \"What was the name of my first workshop?\"");
            if (!ui::readLine("Recovery question: ", question)) return false;
            if (question.empty() && !currentQuestion.empty()) question = currentQuestion;
            const std::string err = auth::Authenticator::checkRecovery(question, std::string(auth::policy::kMinRecovery, 'x'));
            if (!err.empty()) { ui::problem(err); continue; }
            ui::hint("The answer is stored hashed, like a password. At least " + std::to_string(auth::policy::kMinRecovery) + " characters.");
            return askSecret("Recovery answer: ", "Repeat the answer: ", answer, &checkAnswerOnly);
        }
    }

    bool firstRunSetup() {
        ui::clear();
        ui::logo();
        ui::notify(ui::Kind::Info, "FIRST-TIME SETUP. This copy has no admin account yet. Choose a password and a recovery phrase. "
                                   "They are saved only as salted hashes in '" + std::string(auth::Authenticator::kUsersFile) + "'. "
                                   "(If you ever lose both, delete that file and start the program again.)");
        std::cout << "\n  Account name: " << auth::Authenticator::kAdminName << "\n\n";
        while (true) {
            std::string pw, q, a;
            ui::hint("Admin password (" + std::to_string(auth::policy::kMinPassword) + "+ characters):");
            if (!askSecret("New password: ", "Repeat password: ", pw, &auth::Authenticator::checkPassword)) { std::cerr << "\n  Setup cancelled.\n"; return false; }
            std::cout << '\n';
            if (!askRecovery(q, a)) { std::cerr << "\n  Setup cancelled.\n"; return false; }
            const std::string err = auth_.initialSetup(pw, q, a);
            if (err.empty()) {
                ui::notify(ui::Kind::Success, "Setup complete. Sign in as '" + std::string(auth::Authenticator::kAdminName) + "' with the password you just chose.");
                ui::pause();
                return true;
            }
            ui::notify(ui::Kind::Error, err);
        }
    }

    void changePasswordFlow(const std::string& user) {
        std::string cur, np;
        if (!ui::readPassword("Current password: ", cur)) return;
        if (!auth_.login(user, cur)) { ui::notify(ui::Kind::Error, "The current password is incorrect."); return; }
        if (!askSecret("New password: ", "Repeat new password: ", np, &auth::Authenticator::checkPassword)) return;
        const std::string err = auth_.changePassword(user, cur, np);
        ui::notify(err.empty() ? ui::Kind::Success : ui::Kind::Error, err.empty() ? "Password updated and saved." : err);
    }

    void changeRecoveryFlow(const std::string& user) {
        std::string cur, q, a;
        if (!ui::readPassword("Current password: ", cur)) return;
        if (!auth_.login(user, cur)) { ui::notify(ui::Kind::Error, "The current password is incorrect."); return; }
        if (!askRecovery(q, a, auth_.recoveryQuestion())) return;
        const std::string err = auth_.changeRecovery(user, cur, q, a);
        ui::notify(err.empty() ? ui::Kind::Success : ui::Kind::Error, err.empty() ? "Recovery phrase updated and saved." : err);
    }

    void securityMenu() {
        while (true) {
            ui::screen("SECURITY SETTINGS");
            std::cout << '\n';
            ui::top(ui::kSingle, "ACCOUNT", ui::kCyan);
            ui::row(ui::kSingle, "User: " + ui::gUser + "      Recovery phrase: " + (auth_.hasRecovery(ui::gUser) ? "set" : "NOT SET"), ui::kCyan);
            ui::row(ui::kSingle, "[1] Change password     [2] Change recovery question / answer     [0] Back", ui::kCyan);
            ui::bottom(ui::kSingle, ui::kCyan);
            auto c = ui::askInt("Select [0-2]: ", 0, 2);
            if (!c.ok || c.value == 0) return;
            std::cout << '\n';
            if (c.value == 1) changePasswordFlow(ui::gUser);
            else changeRecoveryFlow(ui::gUser);
            ui::pause();
        }
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
            reportWarning();
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
        const Part* p = inv_.findById(id.value);
        if (!p) { ui::notify(ui::Kind::Error, "No part with ID " + std::to_string(id.value) + "."); return; }
        ui::printDetail(*p);
    }

    // ============================== Update ================================
    void updatePart() {
        ui::screen("UPDATE PART");
        std::cout << '\n';
        auto id = ui::askInt("Part ID to update: ", 1, limits::kMaxId);
        if (!id.ok) return;
        const Part* p = inv_.findById(id.value);
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

        const Part updated(p->id(), newName, newSys, price.blank ? p->price() : price.value,
                           qty.blank ? p->quantity() : qty.value, minq.blank ? p->minStock() : minq.value);
        inv_.updatePart(id.value, updated);  // saves, backs up the old file and writes the history entry
        p = inv_.findById(id.value);

        if (saved()) ui::notify(ui::Kind::Success, "Part " + std::to_string(p->id()) + " updated and saved.");
        reportWarning();
        ui::printDetail(*p);
        ui::showLowStockAlert(inv_);  // re-check after stock/threshold change
    }

    void adjustStock() {
        ui::screen("ADJUST STOCK LEVEL");
        std::cout << '\n';
        auto id = ui::askInt("Part ID: ", 1, limits::kMaxId);
        if (!id.ok) return;
        const Part* p = inv_.findById(id.value);
        if (!p) { ui::notify(ui::Kind::Error, "No part with ID " + std::to_string(id.value) + "."); return; }
        std::cout << "\n  " << ui::paint(p->name(), ui::kBoldCyan) << " - current stock: " << p->quantity()
                  << " (min " << p->minStock() << ")\n";
        ui::hint("Enter a positive number to ADD stock (e.g. 10) or a negative one to REMOVE (e.g. -3).");
        auto delta = ui::askInt("Change: ", -limits::kMaxQty, limits::kMaxQty);
        if (!delta.ok) return;
        const std::string err = inv_.adjustStock(id.value, delta.value);
        if (!err.empty()) { ui::notify(ui::Kind::Error, err); return; }
        if (saved()) ui::notify(ui::Kind::Success, "Stock updated and saved. New quantity: " + std::to_string(inv_.findById(id.value)->quantity()));
        reportWarning();
        ui::showLowStockAlert(inv_);
    }

    // ============================== Delete ================================
    void deletePart() {
        ui::screen("DELETE PART");
        std::cout << '\n';
        auto id = ui::askInt("Part ID to delete: ", 1, limits::kMaxId);
        if (!id.ok) return;
        const Part* p = inv_.findById(id.value);
        if (!p) { ui::notify(ui::Kind::Error, "No part with ID " + std::to_string(id.value) + "."); return; }
        ui::printDetail(*p);
        std::cout << '\n';
        ui::notify(ui::Kind::Warning, "Deleting a part removes its record (the previous data is kept in the backups folder).");
        std::cout << '\n';
        if (ui::confirm("Permanently delete this part?")) {
            inv_.removePart(id.value);
            if (saved()) ui::notify(ui::Kind::Success, "Part deleted and changes saved.");
            reportWarning();
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
int main(int argc, char** argv) {
    ui::ConsoleSession console;  // UTF-8 + colour set-up, restored on exit
    const std::string arg = argc > 1 ? util::toLower(argv[1]) : "";
    App app;
    if (!app.ensureSetup()) return 1;  // first run: choose the admin password + recovery phrase
    if (arg == "web" || arg == "--web") app.runWeb();
    else app.run();
    return 0;
}