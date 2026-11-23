// auth.h - the owner's account, salted password hashing, audit trail.
// The system is used by ONE person only: the Business Owner (Admin). There are no other accounts, roles or permissions.
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace auth {

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
    std::string recQuestion, recSalt, recHash;  // optional recovery phrase (empty = none)
};

// All mutating members return "" on success or a human-readable error.
class Authenticator {
public:
    explicit Authenticator(std::string file = "users.csv");

    bool isConfigured() const { return !users_.empty(); }
    bool exists(const std::string& username) const;                  // is this the owner's account?
    std::string canonicalName(const std::string& username) const;  // "" if unknown

    static std::string checkUsername(const std::string& name);
    static std::string checkPassword(const std::string& pw);

    // First-run: creates the Business Owner's (Admin) account, with a recovery question/answer.
    // Only one account can ever exist.
    std::string initialSetup(const std::string& username, const std::string& password,
                             const std::string& question, const std::string& answer);

    // Returns the canonical username on success. Does equal work for unknown users.
    std::optional<std::string> login(const std::string& username, const std::string& password) const;

    std::string resetPassword(const std::string& username, const std::string& newPassword);  // used by recovery / tests
    std::string changePassword(const std::string& username, const std::string& current, const std::string& newPassword);

    // "Forgot password": only for accounts that set a recovery phrase.
    std::string recoveryQuestion(const std::string& username) const;      // "" = none
    std::string recoverPassword(const std::string& username, const std::string& answer, const std::string& newPassword,
                                const std::string& newUsername = "");

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
//         PASSWORD_CHANGED, BACKUP, IMPORT,
//         CREDIT_SALE, PAYMENT_RECORDED, PART_MODELS_CHANGED,
//         JOB_CREATED, JOB_UPDATED, JOB_MECHANIC, JOB_STATUS, JOB_LINE_ADDED, JOB_LINE_REMOVED,
//         JOB_STOCK_RECONCILED, SERVICE_ADDED, SERVICE_UPDATED, SERVICE_TOGGLED
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
