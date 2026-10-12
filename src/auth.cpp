#include "auth.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>

#include "util.h"

namespace auth {

namespace {
constexpr int kHashRounds = 20000;
constexpr size_t kMinPassword = 8, kMaxPassword = 128;
}  // namespace

// ---------------------------------------------------------------- roles / RBAC
const char* roleName(Role r) { return r == Role::Admin ? "Admin" : "Staff"; }

std::optional<Role> parseRole(const std::string& s) {
    if (util::equalsIgnoreCase(s, "admin")) return Role::Admin;
    if (util::equalsIgnoreCase(s, "staff")) return Role::Staff;
    return std::nullopt;
}

bool allowed(Role role, Permission p) {
    if (role == Role::Admin) return true;
    switch (p) {  // Staff: sell, view inventory, adjust stock - nothing else.
        case Permission::ViewInventory:
        case Permission::Sell:
        case Permission::AdjustStock:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------- hashing (SHA-256, no external library)
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

std::string randomSalt(const std::string& username) {
    std::random_device rd;
    static const char* digits = "0123456789abcdef";
    std::string s = "RE-PT:" + username + ":";
    for (int i = 0; i < 16; ++i) s += digits[rd() % 16];
    s += digits[static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) % 16];
    return s;
}

std::string newToken() {
    static std::random_device rd;
    static std::uint64_t counter = 0;
    std::string seed;
    for (int i = 0; i < 8; ++i) seed += std::to_string(rd()) + ":";
    seed += std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ":" +
            std::to_string(++counter) + ":" + std::to_string(reinterpret_cast<std::uintptr_t>(&seed));
    return toHex(sha256(seed));
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

// ---------------------------------------------------------------- Authenticator
// users.csv:  username,salt,hash,role,recoverySalt,recoveryHash,question
// The older 6-column file (username,salt,hash,recSalt,recHash,question) still loads; its
// accounts become Admin and are rewritten in the new format on the next save.
Authenticator::Authenticator(std::string file) : file_(std::move(file)) { load(); }

std::string Authenticator::normAnswer(const std::string& s) { return util::toLower(util::trim(s)); }

const Credential* Authenticator::find(const std::string& u) const {
    for (const Credential& c : users_) if (util::equalsIgnoreCase(c.username, u)) return &c;
    return nullptr;
}
Credential* Authenticator::findMutable(const std::string& u) {
    for (Credential& c : users_) if (util::equalsIgnoreCase(c.username, u)) return &c;
    return nullptr;
}

std::vector<UserInfo> Authenticator::users() const {
    std::vector<UserInfo> out;
    for (const Credential& c : users_) out.push_back({c.username, c.role, !c.recHash.empty()});
    return out;
}
std::optional<Role> Authenticator::roleOf(const std::string& u) const {
    const Credential* c = find(u);
    if (!c) return std::nullopt;
    return c->role;
}
std::string Authenticator::canonicalName(const std::string& u) const {
    const Credential* c = find(u);
    return c ? c->username : std::string();
}
int Authenticator::adminCount() const {
    return static_cast<int>(std::count_if(users_.begin(), users_.end(), [](const Credential& c) { return c.role == Role::Admin; }));
}

std::string Authenticator::checkUsername(const std::string& n) {
    if (n.size() < 3 || n.size() > 32) return "Username must be 3 to 32 characters.";
    for (unsigned char c : n)
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return "Username may only contain letters, digits, '_', '.' and '-'.";
    return "";
}
std::string Authenticator::checkPassword(const std::string& pw) {
    if (pw.size() < kMinPassword || pw.size() > kMaxPassword)
        return "Password must be " + std::to_string(kMinPassword) + " to " + std::to_string(kMaxPassword) + " characters.";
    return "";
}

std::string Authenticator::initialSetup(const std::string& username, const std::string& password,
                                        const std::string& question, const std::string& answer) {
    if (isConfigured()) return "An account already exists.";
    std::string err = checkUsername(username);
    if (err.empty()) err = checkPassword(password);
    if (!err.empty()) return err;
    const std::string q = util::trim(question), a = normAnswer(answer);
    if (q.size() < 5 || q.size() > 120) return "The recovery question must be 5 to 120 characters.";
    if (a.size() < 8 || a.size() > 128) return "The recovery answer must be 8 to 128 characters.";
    if (a == util::toLower(util::trim(password))) return "The recovery answer must differ from the password.";
    Credential c;
    c.username = username;
    c.role = Role::Admin;
    c.salt = randomSalt(c.username);
    c.hash = hashPassword(c.salt, password);
    c.recQuestion = util::oneLine(q);
    c.recSalt = randomSalt(c.username + ":rec");
    c.recHash = hashPassword(c.recSalt, a);
    users_.push_back(c);
    if (!save()) { users_.clear(); return "Could not write '" + file_ + "'. Check that the folder is writable."; }
    return "";
}

std::optional<std::pair<std::string, Role>> Authenticator::login(const std::string& username, const std::string& password) const {
    const Credential* f = find(username);
    const std::string salt = f ? f->salt : std::string("RE-PT:no-such-user");
    const std::string computed = hashPassword(salt, password);  // same work either way (no user enumeration)
    const bool ok = constantTimeEquals(computed, f ? f->hash : std::string(64, '0')) && f != nullptr;
    if (!ok) return std::nullopt;
    return std::make_pair(f->username, f->role);
}

std::string Authenticator::addUser(const std::string& username, const std::string& password, Role role) {
    std::string err = checkUsername(username);
    if (err.empty()) err = checkPassword(password);
    if (!err.empty()) return err;
    if (find(username)) return "That username is already taken.";
    Credential c;
    c.username = username;
    c.role = role;
    c.salt = randomSalt(username);
    c.hash = hashPassword(c.salt, password);
    users_.push_back(c);
    if (!save()) { users_.pop_back(); return "Could not save " + file_ + "."; }
    return "";
}

std::string Authenticator::setRole(const std::string& username, Role role) {
    Credential* c = findMutable(username);
    if (!c) return "User not found.";
    if (c->role == role) return "";
    if (c->role == Role::Admin && adminCount() <= 1) return "There must be at least one Admin.";
    const Role old = c->role;
    c->role = role;
    if (!save()) { c->role = old; return "Could not save " + file_ + "."; }
    return "";
}

std::string Authenticator::removeUser(const std::string& username) {
    Credential* c = findMutable(username);
    if (!c) return "User not found.";
    if (c->role == Role::Admin && adminCount() <= 1) return "You cannot delete the last Admin.";
    const std::vector<Credential> backup = users_;
    users_.erase(std::remove_if(users_.begin(), users_.end(),
                                [&](const Credential& x) { return util::equalsIgnoreCase(x.username, username); }), users_.end());
    if (!save()) { users_ = backup; return "Could not save " + file_ + "."; }
    return "";
}

std::string Authenticator::resetPassword(const std::string& username, const std::string& newPassword) {
    Credential* c = findMutable(username);
    if (!c) return "User not found.";
    const std::string err = checkPassword(newPassword);
    if (!err.empty()) return err;
    const std::string oldSalt = c->salt, oldHash = c->hash;
    c->salt = randomSalt(c->username);
    c->hash = hashPassword(c->salt, newPassword);
    if (!save()) { c->salt = oldSalt; c->hash = oldHash; return "Could not save " + file_ + "."; }
    return "";
}

std::string Authenticator::changePassword(const std::string& username, const std::string& current, const std::string& newPassword) {
    if (!login(username, current)) return "The current password is incorrect.";
    return resetPassword(username, newPassword);
}

std::string Authenticator::recoveryQuestion(const std::string& username) const {
    const Credential* c = find(username);
    return (c && !c->recHash.empty()) ? c->recQuestion : std::string();
}

std::string Authenticator::recoverPassword(const std::string& username, const std::string& answer, const std::string& newPassword) {
    const Credential* c = find(username);
    const bool has = c && !c->recHash.empty();
    const std::string salt = has ? c->recSalt : std::string("RE-PT:no-such-recovery");
    const std::string computed = hashPassword(salt, normAnswer(answer));
    if (!(constantTimeEquals(computed, has ? c->recHash : std::string(64, '0')) && has)) return "Username or recovery answer is incorrect.";
    return resetPassword(c->username, newPassword);
}

// Temp file + rename: a crash can never leave a half-written users.csv.
bool Authenticator::save() {
    std::string out;
    for (const Credential& c : users_)
        out += util::csvEscape(c.username) + ',' + c.salt + ',' + c.hash + ',' + roleName(c.role) + ',' + c.recSalt + ',' + c.recHash + ',' +
               util::csvEscape(c.recQuestion) + '\n';
    if (!util::writeFileAtomic(file_, out)) return false;
    std::error_code ec;  // best effort: owner-only (honoured on Linux/macOS)
    std::filesystem::permissions(file_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return true;
}

void Authenticator::load() {
    const auto rows = util::readCsvFile(file_);
    for (const auto& f : rows) {
        if (f.size() < 3 || f[0].empty() || f[2].size() != 64 || find(f[0])) continue;
        Credential c;
        c.username = f[0]; c.salt = f[1]; c.hash = f[2];
        if (f.size() >= 7 && parseRole(f[3])) {            // current format
            c.role = *parseRole(f[3]);
            if (f[5].size() == 64) { c.recSalt = f[4]; c.recHash = f[5]; c.recQuestion = f[6]; }
        } else {                                           // legacy: single admin account
            c.role = Role::Admin;
            if (f.size() >= 6 && f[4].size() == 64) { c.recSalt = f[3]; c.recHash = f[4]; c.recQuestion = f[5]; }
        }
        users_.push_back(c);
    }
    if (!rows.empty() && users_.empty()) {  // unreadable file: keep a copy before setup could replace it
        std::error_code ec;
        std::filesystem::copy_file(file_, file_ + ".corrupt", std::filesystem::copy_options::overwrite_existing, ec);
    }
}

// ---------------------------------------------------------------- LoginThrottle
bool LoginThrottle::blocked(const std::string& key) {
    auto it = recs_.find(key);
    return it != recs_.end() && std::chrono::steady_clock::now() < it->second.until;
}
void LoginThrottle::fail(const std::string& key) {
    Rec& r = recs_[key];
    if (++r.fails >= 5) { r.until = std::chrono::steady_clock::now() + std::chrono::minutes(5); r.fails = 0; }
}
void LoginThrottle::ok(const std::string& key) { recs_.erase(key); }

// ---------------------------------------------------------------- AuditLog
namespace { const char* kAuditHeader = "timestamp,username,event,details,ip"; }

AuditLog::AuditLog(std::string file) : file_(std::move(file)) {
    for (const auto& f : util::readCsvFile(file_)) {
        if (f.size() < 4 || f[0] == "timestamp") continue;
        entries_.push_back({f[0], f[1], f[2], f[3], f.size() > 4 ? f[4] : ""});
    }
}

bool AuditLog::record(const std::string& user, const std::string& event, const std::string& details, const std::string& ip) {
    AuditEntry e{util::nowStamp(), util::oneLine(user), event, util::oneLine(details), util::oneLine(ip)};
    entries_.push_back(e);
    return util::appendLine(file_, kAuditHeader,
                            util::csvEscape(e.timestamp) + ',' + util::csvEscape(e.user) + ',' + util::csvEscape(e.event) + ',' +
                                util::csvEscape(e.details) + ',' + util::csvEscape(e.ip));
}

std::vector<const AuditEntry*> AuditLog::recent(size_t limit) const {
    std::vector<const AuditEntry*> out;
    for (size_t i = entries_.size(); i-- > 0 && (limit == 0 || out.size() < limit);) out.push_back(&entries_[i]);
    return out;
}

}  // namespace auth
