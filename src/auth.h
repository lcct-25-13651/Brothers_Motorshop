// auth.h - accounts, salted password hashing, role-based access control, audit trail.
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace auth {

// ---------------------------------------------------------------- roles / RBAC
enum class Role { Admin, Staff };

const char* roleName(Role r);
std::optional<Role> parseRole(const std::string& s);

enum class Permission {
    ViewInventory,  // everyone
    Sell,           // create invoices / sales
    AdjustStock,    // restock + stock adjustments
    AddPart,        // sets a price  -> Admin
    EditPart,       // price/name/min stock -> Admin
    DeletePart,     // Admin
    ViewReports,    // Admin
    ViewAudit,      // Admin
    ExportData,     // Admin
    ImportData,     // Admin
    RunBackup,      // Admin
    ManageUsers     // Admin
};

// The single place that decides what each role may do.
bool allowed(Role role, Permission perm);

// ---------------------------------------------------------------- hashing
using Digest = std::array<std::uint8_t, 32>;
Digest sha256(const std::string& message);
std::string toHex(const Digest& d);
// Salted, stretched hash (20,000 rounds of SHA-256). Returns 64 hex chars.
std::string hashPassword(const std::string& salt, const std::string& password);
std::string randomSalt(const std::string& username);
std::string newToken();                                       // 64 hex chars, for sessions
bool constantTimeEquals(const std::string& a, const std::string& b);

// ---------------------------------------------------------------- accounts
struct Credential {
    std::string username;
    std::string salt;
    std::string hash;
    Role role = Role::Staff;
    std::string recQuestion, recSalt, recHash;  // optional recovery phrase (empty = none)
};

struct UserInfo {
    std::string username;
    Role role;
    bool hasRecovery;
};

// All mutating members return "" on success or a human-readable error.
class Authenticator {
public:
    explicit Authenticator(std::string file = "users.csv");

    bool isConfigured() const { return !users_.empty(); }
    std::vector<UserInfo> users() const;
    std::optional<Role> roleOf(const std::string& username) const;
    std::string canonicalName(const std::string& username) const;  // "" if unknown
    int adminCount() const;

    static std::string checkUsername(const std::string& name);
    static std::string checkPassword(const std::string& pw);

    // First-run: creates the first Admin (with a recovery question/answer).
    std::string initialSetup(const std::string& username, const std::string& password,
                             const std::string& question, const std::string& answer);

    // Returns the canonical username + role on success. Does equal work for unknown users.
    std::optional<std::pair<std::string, Role>> login(const std::string& username, const std::string& password) const;

    std::string addUser(const std::string& username, const std::string& password, Role role);
    std::string setRole(const std::string& username, Role role);          // keeps >= 1 Admin
    std::string removeUser(const std::string& username);                  // keeps >= 1 Admin
    std::string resetPassword(const std::string& username, const std::string& newPassword);  // admin action
    std::string changePassword(const std::string& username, const std::string& current, const std::string& newPassword);

    // "Forgot password": only for accounts that set a recovery phrase.
    std::string recoveryQuestion(const std::string& username) const;      // "" = none
    std::string recoverPassword(const std::string& username, const std::string& answer, const std::string& newPassword);

private:
    static std::string normAnswer(const std::string& s);
    const Credential* find(const std::string& username) const;
    Credential* findMutable(const std::string& username);
    bool save();
    void load();

    std::string file_;
    std::vector<Credential> users_;
};

// Throttles brute-force logins per key (IP + username): 5 failures -> 5 minutes lock.
class LoginThrottle {
public:
    bool blocked(const std::string& key);
    void fail(const std::string& key);
    void ok(const std::string& key);

private:
    struct Rec { int fails = 0; std::chrono::steady_clock::time_point until{}; };
    std::map<std::string, Rec> recs_;
};

// ---------------------------------------------------------------- audit trail (audit.csv)
struct AuditEntry {
    std::string timestamp, user, event, details, ip;
};

// Events: LOGIN, LOGIN_FAILED, LOGOUT, PRICE_CHANGE, RECORD_DELETED, PASSWORD_RESET,
//         PASSWORD_CHANGED, USER_CREATED, USER_DELETED, ROLE_CHANGED, BACKUP, IMPORT, ACCESS_DENIED
class AuditLog {
public:
    explicit AuditLog(std::string file = "audit.csv");
    bool record(const std::string& user, const std::string& event, const std::string& details, const std::string& ip = "");
    const std::vector<AuditEntry>& entries() const { return entries_; }
    std::vector<const AuditEntry*> recent(size_t limit) const;  // newest first

private:
    std::string file_;
    std::vector<AuditEntry> entries_;
};

}  // namespace auth
