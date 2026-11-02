// suppliers.h - Phase 2: suppliers (suppliers.csv) and purchase history (purchases.csv). Header-only on purpose.
//   suppliers.csv: id,name,contact_person,phone,address,notes,active
//   purchases.csv: timestamp,supplier_id,part_id,quantity,unit_cost,ref,username
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "util.h"

namespace sup {

struct Supplier {
    int id = 0;
    std::string name, contact, phone, address, notes;
    bool active = true;
};

class SupplierBook {
public:
    void open(const std::string& dir) {
        file_ = dir + "/suppliers.csv";
        items_.clear();
        for (const auto& f : util::readCsvFile(file_)) {
            long long id = 0;
            if (f.size() < 2 || !util::parseInt(f[0], 1, 999999, id)) continue;   // header or junk row
            Supplier s;
            s.id = static_cast<int>(id);
            s.name = util::trim(f[1]);
            if (f.size() > 2) s.contact = util::trim(f[2]);
            if (f.size() > 3) s.phone = util::trim(f[3]);
            if (f.size() > 4) s.address = util::trim(f[4]);
            if (f.size() > 5) s.notes = util::trim(f[5]);
            if (f.size() > 6) s.active = util::trim(f[6]) != "0";
            if (!s.name.empty()) items_.push_back(s);
        }
    }
    const std::vector<Supplier>& all() const { return items_; }
    const Supplier* find(int id) const {
        for (const Supplier& s : items_) if (s.id == id) return &s;
        return nullptr;
    }
    std::string nameOf(int id) const { const Supplier* s = find(id); return s ? s->name : std::string(); }

    // Adds (id == 0) or updates a supplier. Returns "" on success, otherwise the reason. newId gets the id used.
    std::string save(int id, Supplier in, int& newId) {
        in.name = util::oneLine(util::trim(in.name));
        in.contact = util::oneLine(util::trim(in.contact));
        in.phone = util::oneLine(util::trim(in.phone));
        in.address = util::oneLine(util::trim(in.address));
        in.notes = util::oneLine(util::trim(in.notes));
        if (in.name.empty() || in.name.size() > 60) return "Supplier name is required (max 60 characters).";
        if (in.contact.size() > 60 || in.phone.size() > 40 || in.address.size() > 120 || in.notes.size() > 200) return "One of the fields is too long.";
        for (const Supplier& s : items_)
            if (s.id != id && util::equalsIgnoreCase(s.name, in.name)) return "A supplier with that name already exists.";
        const std::vector<Supplier> before = items_;
        if (id == 0) {
            int m = 0;
            for (const Supplier& s : items_) m = std::max(m, s.id);
            in.id = m + 1;
            items_.push_back(in);
        } else {
            Supplier* cur = mut(id);
            if (!cur) return "Supplier not found.";
            in.id = id;
            in.active = cur->active;
            *cur = in;
        }
        newId = in.id;
        if (!write()) { items_ = before; return "Could not write suppliers.csv - check disk space/permissions."; }
        return "";
    }
    std::string setActive(int id, bool on) {
        Supplier* s = mut(id);
        if (!s) return "Supplier not found.";
        const bool old = s->active;
        s->active = on;
        if (!write()) { s->active = old; return "Could not write suppliers.csv - check disk space/permissions."; }
        return "";
    }

private:
    Supplier* mut(int id) { for (Supplier& s : items_) if (s.id == id) return &s; return nullptr; }
    bool write() const {
        std::string out = "id,name,contact_person,phone,address,notes,active\n";
        for (const Supplier& s : items_)
            out += std::to_string(s.id) + ',' + util::csvEscape(s.name) + ',' + util::csvEscape(s.contact) + ',' + util::csvEscape(s.phone) + ',' +
                   util::csvEscape(s.address) + ',' + util::csvEscape(s.notes) + ',' + (s.active ? "1" : "0") + "\n";
        return util::writeFileAtomic(file_, out);
    }
    std::string file_;
    std::vector<Supplier> items_;
};

struct Purchase {
    std::string timestamp, ref, user;
    int supplierId = 0, partId = 0, qty = 0;
    double unitCost = 0.0;
};

class PurchaseLog {
public:
    void open(const std::string& dir) {
        file_ = dir + "/purchases.csv";
        items_.clear();
        for (const auto& f : util::readCsvFile(file_)) {
            long long sid = 0, pid = 0, q = 0;
            double cost = 0;
            if (f.size() < 5 || !util::parseInt(f[1], 0, 999999, sid) || !util::parseInt(f[2], 1, 999999, pid) || !util::parseInt(f[3], 1, 999999, q)) continue;
            util::parseDouble(f[4], 0, 9999999.99, cost);
            Purchase p;
            p.timestamp = f[0]; p.supplierId = static_cast<int>(sid); p.partId = static_cast<int>(pid); p.qty = static_cast<int>(q); p.unitCost = cost;
            if (f.size() > 5) p.ref = f[5];
            if (f.size() > 6) p.user = f[6];
            items_.push_back(p);
        }
    }
    bool record(const Purchase& p) {
        const std::string line = p.timestamp + ',' + std::to_string(p.supplierId) + ',' + std::to_string(p.partId) + ',' + std::to_string(p.qty) + ',' +
                                 util::money(p.unitCost) + ',' + util::csvEscape(p.ref) + ',' + util::csvEscape(p.user);
        if (!util::appendLine(file_, "timestamp,supplier_id,part_id,quantity,unit_cost,ref,username", line)) return false;
        items_.push_back(p);
        return true;
    }
    const std::vector<Purchase>& all() const { return items_; }

private:
    std::string file_;
    std::vector<Purchase> items_;
};

}  // namespace sup
