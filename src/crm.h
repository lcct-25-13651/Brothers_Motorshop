// crm.h - Phase 3: customer & vehicle records. Header-only on purpose (same style as suppliers.h).
//   customers.csv: id,name,phone,address,notes,archived
//   vehicles.csv : id,customer_id,plate,model,year,color,notes,archived
// "archived" is the only column added to the Phase 3 spec; files without it (5 / 7 columns) load fine (= not archived).
// Plates are stored UPPERCASE with every space removed and are unique across ALL vehicles (archived ones too).
// Nothing here ever deletes history: a customer / vehicle that has invoices can only be archived.
#pragma once
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "inventory.h"
#include "reports.h"
#include "util.h"

namespace crm {

// ---------------------------------------------------------------- normalisers
// "abc 1234" / " Abc  1234 " -> "ABC1234". Every whitespace character is removed (also the non-breaking space).
inline std::string normalizePlate(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(in[i]);
        if (ch == 0xC2 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0xA0) { ++i; continue; }  // NBSP
        if (std::isspace(ch)) continue;
        out += static_cast<char>(std::toupper(ch));
    }
    return out;
}
// A usable plate: 2-12 characters, letters / digits / hyphen only (already normalised).
inline bool validPlate(const std::string& norm) {
    if (norm.size() < 2 || norm.size() > 12) return false;
    for (unsigned char ch : norm) if (!(std::isalnum(ch) || ch == '-')) return false;
    return true;
}
inline std::string phoneDigits(const std::string& s) {
    std::string d;
    for (unsigned char ch : s) if (std::isdigit(ch)) d += static_cast<char>(ch);
    return d;
}

// ---------------------------------------------------------------- records
struct Customer {
    int id = 0;
    std::string name, phone, address, notes;
    bool archived = false;
};

struct Vehicle {
    int id = 0, customerId = 0;
    std::string plate, model, color, notes;
    int year = 0;            // 0 = unknown
    bool archived = false;
};

struct SearchHit {
    const Customer* customer = nullptr;
    std::vector<const Vehicle*> plateMatches;   // vehicles whose plate matched the query (empty = matched by name / phone)
};

constexpr int kMaxNameLen = 60;

// ---------------------------------------------------------------- the registry (both files)
// NOT thread-safe by itself: the web layer serialises calls with its mutex.
class Registry {
public:
    void open(const std::string& dir) {
        customerFile_ = dir + "/customers.csv";
        vehicleFile_ = dir + "/vehicles.csv";
        customers_.clear();
        vehicles_.clear();
        warning_.clear();
        int skipped = 0;
        for (const auto& f : util::readCsvFile(customerFile_)) {
            long long id = 0;
            if (f.empty() || !util::parseInt(f[0], 1, 999999, id)) { if (!f.empty() && f[0] != "id") ++skipped; continue; }   // header row is fine
            Customer k;
            k.id = static_cast<int>(id);
            if (f.size() > 1) k.name = util::oneLine(util::trim(f[1]));
            if (f.size() > 2) k.phone = util::oneLine(util::trim(f[2]));
            if (f.size() > 3) k.address = util::oneLine(util::trim(f[3]));
            if (f.size() > 4) k.notes = util::oneLine(util::trim(f[4]));
            if (f.size() > 5) k.archived = util::trim(f[5]) == "1";
            if (k.name.empty() || findCustomer(k.id)) { ++skipped; continue; }
            customers_.push_back(std::move(k));
        }
        for (const auto& f : util::readCsvFile(vehicleFile_)) {
            long long id = 0, cid = 0, year = 0;
            if (f.empty() || !util::parseInt(f[0], 1, 999999, id)) { if (!f.empty() && f[0] != "id") ++skipped; continue; }
            Vehicle v;
            v.id = static_cast<int>(id);
            if (f.size() < 3 || !util::parseInt(f[1], 1, 999999, cid)) { ++skipped; continue; }
            v.customerId = static_cast<int>(cid);
            v.plate = normalizePlate(f[2]);
            if (f.size() > 3) v.model = util::oneLine(util::trim(f[3]));
            if (f.size() > 4 && util::parseInt(util::trim(f[4]), 1900, 2200, year)) v.year = static_cast<int>(year);
            if (f.size() > 5) v.color = util::oneLine(util::trim(f[5]));
            if (f.size() > 6) v.notes = util::oneLine(util::trim(f[6]));
            if (f.size() > 7) v.archived = util::trim(f[7]) == "1";
            if (v.plate.empty() || findVehicle(v.id)) { ++skipped; continue; }
            vehicles_.push_back(std::move(v));
        }
        if (skipped > 0) {   // the next save rewrites the files: keep an untouched copy of what was there
            std::error_code ec;
            for (const std::string& f : {customerFile_, vehicleFile_})
                if (std::filesystem::exists(f, ec)) std::filesystem::copy_file(f, f + ".bak", std::filesystem::copy_options::overwrite_existing, ec);
            warning_ = std::to_string(skipped) + " malformed row(s) in customers.csv / vehicles.csv were skipped (originals kept as *.bak).";
        }
    }
    const std::string& warning() const { return warning_; }

    // ------------------------------------------------------------ lookups
    const std::vector<Customer>& customers() const { return customers_; }
    const std::vector<Vehicle>& vehicles() const { return vehicles_; }
    const Customer* findCustomer(int id) const {
        for (const Customer& k : customers_) if (k.id == id) return &k;
        return nullptr;
    }
    const Vehicle* findVehicle(int id) const {
        for (const Vehicle& v : vehicles_) if (v.id == id) return &v;
        return nullptr;
    }
    // Any spelling of a plate ("abc 123", "ABC-123 ") -> the vehicle (archived ones included).
    const Vehicle* findByPlate(const std::string& text) const {
        const std::string p = normalizePlate(text);
        if (p.empty()) return nullptr;
        for (const Vehicle& v : vehicles_) if (v.plate == p) return &v;
        return nullptr;
    }
    std::vector<const Vehicle*> vehiclesOf(int customerId, bool includeArchived = true) const {
        std::vector<const Vehicle*> out;
        for (const Vehicle& v : vehicles_) if (v.customerId == customerId && (includeArchived || !v.archived)) out.push_back(&v);
        return out;
    }
    // Customers whose name is the same ignoring case and extra spaces (archived ones included).
    std::vector<const Customer*> findByName(const std::string& name) const {
        std::vector<const Customer*> out;
        const std::string key = reports::customerKey(name);
        if (key.empty()) return out;
        for (const Customer& k : customers_) if (reports::customerKey(k.name) == key) out.push_back(&k);
        return out;
    }

    // Search by name (every typed word must appear), phone (digits, at least 3) or plate (any spelling, at least 2 characters).
    // Archived customers ARE returned - the caller decides whether to show them. Active customers first, then by name.
    std::vector<SearchHit> search(const std::string& query) const {
        const std::string q = util::trim(query);
        std::vector<std::string> words;
        {
            std::string cur;
            for (unsigned char ch : util::toLower(q)) {
                if (std::isspace(ch)) { if (!cur.empty()) words.push_back(cur); cur.clear(); }
                else cur += static_cast<char>(ch);
            }
            if (!cur.empty()) words.push_back(cur);
        }
        const std::string plateQ = normalizePlate(q), digits = phoneDigits(q);
        std::vector<SearchHit> out;
        for (const Customer& k : customers_) {
            SearchHit hit;
            hit.customer = &k;
            if (!q.empty()) {
                bool byName = !words.empty();
                const std::string lname = util::toLower(k.name);
                for (const std::string& w : words) if (lname.find(w) == std::string::npos) { byName = false; break; }
                const bool byPhone = digits.size() >= 3 && phoneDigits(k.phone).find(digits) != std::string::npos;
                if (plateQ.size() >= 2)
                    for (const Vehicle& v : vehicles_) if (v.customerId == k.id && v.plate.find(plateQ) != std::string::npos) hit.plateMatches.push_back(&v);
                if (!byName && !byPhone && hit.plateMatches.empty()) continue;
            }
            out.push_back(std::move(hit));
        }
        std::sort(out.begin(), out.end(), [](const SearchHit& a, const SearchHit& b) {
            if (a.customer->archived != b.customer->archived) return !a.customer->archived;
            const std::string x = util::toLower(a.customer->name), y = util::toLower(b.customer->name);
            return x != y ? x < y : a.customer->id < b.customer->id;
        });
        return out;
    }

    // ------------------------------------------------------------ customers
    // Adds (id == 0) or updates a customer. Returns "" on success, otherwise the reason. newId gets the id used.
    std::string saveCustomer(int id, Customer in, int& newId) {
        in.name = util::oneLine(util::trim(in.name));
        in.phone = util::oneLine(util::trim(in.phone));
        in.address = util::oneLine(util::trim(in.address));
        in.notes = util::oneLine(util::trim(in.notes));
        if (in.name.empty() || in.name.size() > static_cast<size_t>(kMaxNameLen)) return "Customer name is required (max " + std::to_string(kMaxNameLen) + " characters).";
        if (util::equalsIgnoreCase(in.name, "Walk-in customer")) return "\"Walk-in customer\" is reserved for sales without a customer record.";
        if (in.phone.size() > 30 || in.address.size() > 120 || in.notes.size() > 200) return "One of the fields is too long (phone 30, address 120, notes 200).";
        for (const Customer& k : customers_)   // same person entered twice
            if (k.id != id && reports::customerKey(k.name) == reports::customerKey(in.name) && phoneDigits(k.phone) == phoneDigits(in.phone))
                return "A customer named \"" + k.name + "\" with the same phone number already exists (#" + std::to_string(k.id) + ")" +
                       (phoneDigits(in.phone).empty() ? " - add a phone number to tell two people with the same name apart." : ".");
        const std::vector<Customer> before = customers_;
        if (id == 0) {
            int m = 0;
            for (const Customer& k : customers_) m = std::max(m, k.id);
            if (m >= 999999) return "Customer limit reached.";
            in.id = m + 1;
            in.archived = false;
            customers_.push_back(in);
        } else {
            Customer* cur = mutCustomer(id);
            if (!cur) return "Customer not found.";
            in.id = id;
            in.archived = cur->archived;
            *cur = in;
        }
        newId = in.id;
        if (!writeCustomers()) { customers_ = before; return "Could not write customers.csv - check disk space/permissions."; }
        return "";
    }
    std::string setCustomerArchived(int id, bool on) {
        Customer* k = mutCustomer(id);
        if (!k) return "Customer not found.";
        const bool old = k->archived;
        k->archived = on;
        if (!writeCustomers()) { k->archived = old; return "Could not write customers.csv - check disk space/permissions."; }
        return "";
    }
    // Only a customer with NO history and no vehicles can be removed; otherwise archive. hasHistory = "has at least one invoice".
    std::string removeCustomer(int id, bool hasHistory) {
        if (!findCustomer(id)) return "Customer not found.";
        if (hasHistory) return "This customer has invoice history and cannot be deleted - archive instead.";
        if (!vehiclesOf(id).empty()) return "Remove (or archive) the customer's vehicles first - or simply archive the customer.";
        const std::vector<Customer> before = customers_;
        customers_.erase(std::remove_if(customers_.begin(), customers_.end(), [&](const Customer& k) { return k.id == id; }), customers_.end());
        if (!writeCustomers()) { customers_ = before; return "Could not write customers.csv - check disk space/permissions."; }
        return "";
    }

    // ------------------------------------------------------------ vehicles
    // Adds (id == 0, needs in.customerId) or updates a vehicle (the owner never changes on update). "" = success.
    std::string saveVehicle(int id, Vehicle in, int& newId) {
        in.plate = normalizePlate(in.plate);
        in.model = util::oneLine(util::trim(in.model));
        in.color = util::oneLine(util::trim(in.color));
        in.notes = util::oneLine(util::trim(in.notes));
        if (!validPlate(in.plate)) return "Enter a plate number (2-12 letters, digits or hyphens; spaces are removed automatically).";
        if (in.model.size() > 60 || in.color.size() > 30 || in.notes.size() > 200) return "One of the fields is too long (model 60, color 30, notes 200).";
        if (in.year != 0 && (in.year < 1900 || in.year > currentYear() + 1)) return "Year must be between 1900 and " + std::to_string(currentYear() + 1) + " (or blank).";
        for (const Vehicle& v : vehicles_)
            if (v.id != id && v.plate == in.plate) {
                const Customer* o = findCustomer(v.customerId);
                return "Plate " + in.plate + " is already registered" + (o ? " to " + o->name : std::string()) + ".";
            }
        const std::vector<Vehicle> before = vehicles_;
        if (id == 0) {
            const Customer* owner = findCustomer(in.customerId);
            if (!owner) return "Customer not found.";
            if (owner->archived) return "This customer is archived - restore the customer before adding a vehicle.";
            int m = 0;
            for (const Vehicle& v : vehicles_) m = std::max(m, v.id);
            if (m >= 999999) return "Vehicle limit reached.";
            in.id = m + 1;
            in.archived = false;
            vehicles_.push_back(in);
        } else {
            Vehicle* cur = mutVehicle(id);
            if (!cur) return "Vehicle not found.";
            in.id = id;
            in.customerId = cur->customerId;
            in.archived = cur->archived;
            *cur = in;
        }
        newId = in.id;
        if (!writeVehicles()) { vehicles_ = before; return "Could not write vehicles.csv - check disk space/permissions."; }
        return "";
    }
    std::string setVehicleArchived(int id, bool on) {
        Vehicle* v = mutVehicle(id);
        if (!v) return "Vehicle not found.";
        const bool old = v->archived;
        v->archived = on;
        if (!writeVehicles()) { v->archived = old; return "Could not write vehicles.csv - check disk space/permissions."; }
        return "";
    }
    std::string removeVehicle(int id, bool hasHistory) {
        if (!findVehicle(id)) return "Vehicle not found.";
        if (hasHistory) return "This vehicle has invoice history and cannot be deleted - archive it instead.";
        const std::vector<Vehicle> before = vehicles_;
        vehicles_.erase(std::remove_if(vehicles_.begin(), vehicles_.end(), [&](const Vehicle& v) { return v.id == id; }), vehicles_.end());
        if (!writeVehicles()) { vehicles_ = before; return "Could not write vehicles.csv - check disk space/permissions."; }
        return "";
    }

private:
    static int currentYear() {
        long long y = 2026;
        util::parseInt(util::formatTime(std::time(nullptr), "%Y"), 1970, 9999, y);
        return static_cast<int>(y);
    }
    Customer* mutCustomer(int id) { for (Customer& k : customers_) if (k.id == id) return &k; return nullptr; }
    Vehicle* mutVehicle(int id) { for (Vehicle& v : vehicles_) if (v.id == id) return &v; return nullptr; }
    bool writeCustomers() const {
        std::string out = "id,name,phone,address,notes,archived\n";
        for (const Customer& k : customers_)
            out += std::to_string(k.id) + ',' + util::csvEscape(k.name) + ',' + util::csvEscape(k.phone) + ',' + util::csvEscape(k.address) + ',' + util::csvEscape(k.notes) + ',' + (k.archived ? "1" : "0") + "\n";
        return util::writeFileAtomic(customerFile_, out);
    }
    bool writeVehicles() const {
        std::string out = "id,customer_id,plate,model,year,color,notes,archived\n";
        for (const Vehicle& v : vehicles_)
            out += std::to_string(v.id) + ',' + std::to_string(v.customerId) + ',' + util::csvEscape(v.plate) + ',' + util::csvEscape(v.model) + ',' + (v.year ? std::to_string(v.year) : std::string()) + ',' +
                   util::csvEscape(v.color) + ',' + util::csvEscape(v.notes) + ',' + (v.archived ? "1" : "0") + "\n";
        return util::writeFileAtomic(vehicleFile_, out);
    }
    std::string customerFile_, vehicleFile_, warning_;
    std::vector<Customer> customers_;
    std::vector<Vehicle> vehicles_;
};

// ---------------------------------------------------------------- history & balances (read-only over invoices)
// One row per event on a customer's / vehicle's timeline. Today every row is an invoice; Phase 4 job orders can be added
// as another `kind` and merged into the same list, because the lists are sorted newest first by timestamp.
struct HistoryRow {
    std::string timestamp, kind, ref, summary;     // kind: "Invoice"
    double total = 0, balance = 0;
    inv::PayStatus status = inv::PayStatus::Paid;
    int vehicleId = 0;
    bool linked = true;                            // false = old free-text invoice matched to the customer only by name
    const inv::Invoice* invoice = nullptr;
};

inline std::string summarize(const inv::Invoice& v, size_t maxLen = 90) {
    std::string s;
    for (const inv::InvoiceLine& l : v.lines) {
        if (!s.empty()) s += ", ";
        s += l.description;
        if (l.qty > 1) s += " x" + std::to_string(l.qty);
    }
    if (s.size() > maxLen) s = s.substr(0, maxLen - 3) + "...";
    return s;
}

inline void sortNewestFirst(std::vector<HistoryRow>& rows) {
    std::stable_sort(rows.begin(), rows.end(), [](const HistoryRow& a, const HistoryRow& b) { return a.timestamp > b.timestamp; });
}
inline HistoryRow rowOf(const inv::Invoice& v, bool linked = true) {
    HistoryRow r;
    r.timestamp = v.timestamp; r.kind = "Invoice"; r.ref = v.number; r.summary = summarize(v);
    r.total = util::round2(v.total()); r.balance = v.balance(); r.status = v.status(); r.vehicleId = v.vehicleId; r.linked = linked; r.invoice = &v;
    return r;
}

// Every invoice recorded against this vehicle, newest first (same-second invoices: the later number first).
inline std::vector<HistoryRow> vehicleHistory(const std::vector<inv::Invoice>& invoices, int vehicleId) {
    std::vector<HistoryRow> out;
    for (size_t i = invoices.size(); i-- > 0;)
        if (invoices[i].vehicleId == vehicleId && vehicleId > 0) out.push_back(rowOf(invoices[i]));
    sortNewestFirst(out);
    return out;
}
inline bool vehicleHasHistory(const std::vector<inv::Invoice>& invoices, int vehicleId) {
    for (const inv::Invoice& i : invoices) if (i.vehicleId == vehicleId) return true;
    return false;
}
inline bool customerHasHistory(const std::vector<inv::Invoice>& invoices, int customerId) {
    for (const inv::Invoice& i : invoices) if (i.customerId == customerId) return true;
    return false;
}

struct Balance { double balance = 0; int openInvoices = 0; };

// Utang of one customer = open invoices linked to the record, PLUS old free-text invoices (customer_id blank) whose name
// is the same as this customer's - but only when that name belongs to exactly one customer record, so nobody is
// charged for a namesake's debt. This keeps the figure equal to the Receivables page, which groups by name.
inline bool nameIsUnique(const Registry& reg, const Customer& k) { return reg.findByName(k.name).size() == 1; }

inline std::vector<HistoryRow> customerHistory(const std::vector<inv::Invoice>& invoices, const Registry& reg, const Customer& k) {
    std::vector<HistoryRow> out;
    const std::string key = reports::customerKey(k.name);
    const bool byName = nameIsUnique(reg, k);
    for (size_t i = invoices.size(); i-- > 0;) {
        const inv::Invoice& v = invoices[i];
        if (v.customerId == k.id) out.push_back(rowOf(v, true));
        else if (v.customerId == 0 && byName && reports::customerKey(v.customer) == key) out.push_back(rowOf(v, false));
    }
    sortNewestFirst(out);
    return out;
}

inline Balance customerBalance(const std::vector<inv::Invoice>& invoices, const Registry& reg, const Customer& k) {
    Balance b;
    for (const HistoryRow& r : customerHistory(invoices, reg, k))
        if (r.balance > 0) { b.balance = util::round2(b.balance + r.balance); ++b.openInvoices; }
    return b;
}

// Balances for many customers in one pass over the invoices (used by the customer list).
inline std::map<int, Balance> balancesByCustomer(const std::vector<inv::Invoice>& invoices, const Registry& reg) {
    std::map<int, Balance> out;
    std::map<std::string, Balance> unlinked;     // old free-text invoices, by name key
    std::map<std::string, int> nameCount;
    for (const Customer& k : reg.customers()) ++nameCount[reports::customerKey(k.name)];
    for (const inv::Invoice& v : invoices) {
        if (v.balance() <= 0) continue;
        Balance& b = v.customerId > 0 ? out[v.customerId] : unlinked[reports::customerKey(v.customer)];
        b.balance = util::round2(b.balance + v.balance());
        ++b.openInvoices;
    }
    for (const Customer& k : reg.customers()) {
        const std::string key = reports::customerKey(k.name);
        auto u = unlinked.find(key);
        if (u == unlinked.end() || nameCount[key] != 1) continue;
        Balance& b = out[k.id];
        b.balance = util::round2(b.balance + u->second.balance);
        b.openInvoices += u->second.openInvoices;
    }
    return out;
}

}  // namespace crm
