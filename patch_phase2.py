#!/usr/bin/env python3
"""patch_phase2.py - Phase 2: Supplier Management (suppliers, purchase history, supplier cost vs retail price / margin).
Usage: python3 patch_phase2.py [path-to-Brothers_Motorshop]     (Windows: py patch_phase2.py [path])
Edits by exact text anchors; if ANY anchor does not match, NOTHING is changed. Originals are saved as *.p2.bak.
Data is backward compatible: parts.csv gets one extra column (supplier_id) on the next save; old files still load."""
import sys, os, shutil
root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
if not os.path.isfile(os.path.join(root, "src", "web.cpp")):
    sys.exit("ERROR: src/web.cpp not found in " + root + " - run this inside the project folder or pass its path.")

SUPPLIERS_H = r"""// suppliers.h - Phase 2: suppliers (suppliers.csv) and purchase history (purchases.csv). Header-only on purpose.
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
"""

def rd(p):
    with open(os.path.join(root, p), "rb") as f: return f.read().decode("utf-8")

results = {}
def edit(path, marker, pairs):
    raw = rd(path); crlf = "\r\n" in raw
    s = raw.replace("\r\n", "\n")
    if marker in s:
        print("skip (already patched):", path); return
    for old, new in pairs:
        n = s.count(old)
        if n != 1:
            sys.exit("ERROR: anchor found %d times in %s (expected 1):\n  %s\nNothing was changed." % (n, path, old[:100].replace("\n", "\\n")))
        s = s.replace(old, new, 1)
    results[path] = s.replace("\n", "\r\n") if crlf else s

# ---------------------------------------------------------------- inventory.h / inventory.cpp
edit("src/inventory.h", "supplierId_", [
 ("    const std::string& compatibleModels() const { return compatibleModels_; }\n",
  "    const std::string& compatibleModels() const { return compatibleModels_; }\n"
  "    int supplierId() const { return supplierId_; }                      // 0 = no supplier recorded\n"),
 ("    void setPartNumber(const std::string& v) { partNumber_ = v; }\n",
  "    void setPartNumber(const std::string& v) { partNumber_ = v; }\n    void setSupplierId(int v) { supplierId_ = v; }\n"),
 ("    std::string compatibleModels_;\n    std::string partNumber_;\n};",
  "    std::string compatibleModels_;\n    std::string partNumber_;\n    int supplierId_ = 0;\n};"),
 ('    Result restock(int id, int qty, double unitCost, const std::string& actor, const std::string& ref = "");\n',
  '    // Phase 2: sets the part\'s supplier (0 = none) and, if given, its cost price (what the shop pays). Saves parts.csv.\n'
  '    Result setSourcing(int id, int supplierId, std::optional<double> costPrice, const std::string& actor);\n'
  '    Result restock(int id, int qty, double unitCost, const std::string& actor, const std::string& ref = "");\n'),
])
edit("src/inventory.cpp", "setSourcing", [
 ("util::csvEscape(compatibleModels_) + ',' + util::csvEscape(partNumber_);",
  "util::csvEscape(compatibleModels_) + ',' + util::csvEscape(partNumber_) + ',' + std::to_string(supplierId_);"),
 ("    if (f.size() > 8) part.setPartNumber(util::oneLine(util::trim(f[8])));\n",
  "    if (f.size() > 8) part.setPartNumber(util::oneLine(util::trim(f[8])));\n"
  "    if (f.size() > 9) { long long sid = 0; if (util::parseInt(f[9], 0, 999999, sid)) part.setSupplierId(static_cast<int>(sid)); }\n"),
 ('    std::string out = std::string(kPartsHeader) + "\\n";\n    for (const Part& p : parts_) out += p.toCsv()',
  '    std::string out = std::string(kPartsHeader) + ",SupplierID\\n";\n    for (const Part& p : parts_) out += p.toCsv()'),
 ("Result Inventory::adjust(int id, long long delta, const std::string& actor, const std::string& ref) {\n",
  "Result Inventory::setSourcing(int id, int supplierId, std::optional<double> costPrice, const std::string& actor) {\n"
  "    (void)actor;\n"
  "    Part* p = mut(id);\n"
  "    if (!p) return Result::fail(\"Part not found.\");\n"
  "    if (supplierId < 0 || supplierId > limits::kMaxId) return Result::fail(\"Invalid supplier.\");\n"
  "    if (costPrice && !(*costPrice >= 0 && *costPrice <= limits::kMaxPrice)) return Result::fail(\"Cost price is out of range.\");\n"
  "    warning_.clear();\n"
  "    const int oldS = p->supplierId();\n"
  "    const double oldC = p->costPrice();\n"
  "    p->setSupplierId(supplierId);\n"
  "    if (costPrice) p->setCostPrice(util::round2(*costPrice));\n"
  "    if (!save()) { p->setSupplierId(oldS); p->setCostPrice(oldC); return Result::fail(warning_); }\n"
  "    return Result::success(\"Supplier details updated.\");\n"
  "}\n\n"
  "Result Inventory::adjust(int id, long long delta, const std::string& actor, const std::string& ref) {\n"),
])

# ---------------------------------------------------------------- auth
edit("src/auth.h", "ManageSuppliers", [
 ("    ManageUsers     // Admin\n};", "    ManageUsers,    // Admin\n    ManageSuppliers // Admin (suppliers, purchase prices, margins)\n};"),
])

# ---------------------------------------------------------------- web.cpp
WEB_BLOCK = r"""    // ------------------------------------------------------------------ suppliers (Phase 2)
    int readSupplierId(Ctx& c, int keep) {   // 0 = none; unknown / inactive suppliers are ignored unless it is the one already on the part
        long long v = 0;
        if (!util::parseInt(c.param("supplier"), 1, 999999, v)) return 0;
        const sup::Supplier* s = suppliers.find(static_cast<int>(v));
        return s && (s->active || s->id == keep) ? static_cast<int>(v) : 0;
    }
    std::string supplierSelect(int chosen) const {
        std::string o = "<select name='supplier'><option value=''>- none -</option>";
        for (const sup::Supplier& s : suppliers.all())
            if (s.active || s.id == chosen)
                o += "<option value='" + num(s.id) + "'" + (s.id == chosen ? " selected" : "") + ">" + h(s.name) + (s.active ? "" : " (inactive)") + "</option>";
        return o + "</select>";
    }
    static std::string marginCells(const inv::Part& p) {   // "<td>cost</td><td>profit (margin %)</td>"
        if (p.costPrice() <= 0) return "<td class='n mut'>n/a</td><td class='n mut'>n/a</td>";
        const double profit = p.price() - p.costPrice();
        const double pct = p.price() > 0 ? profit / p.price() * 100.0 : 0.0;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f%%", pct);
        return "<td class='n'>" + php(p.costPrice()) + "</td><td class='n'" + std::string(profit < 0 ? " style='color:#b42318;font-weight:700'" : "") + ">" +
               php(profit) + " <span class='mut'>(" + buf + ")</span></td>";
    }
    std::string sourcingCells(Ctx& c, const inv::Part& p) const {   // Admin-only inventory columns: Supplier | Cost | Margin
        if (!auth::allowed(c.role, auth::Permission::ViewReports)) return "";
        const std::string sn = suppliers.nameOf(p.supplierId());
        return "<td>" + (sn.empty() ? std::string("<span class='mut'>-</span>") : h(sn)) + "</td>" + marginCells(p);
    }
    void suppliersPage(Ctx& c) {
        const int editId = intParam(c, "edit", 1, 999999, 0), viewId = intParam(c, "view", 1, 999999, 0);
        const sup::Supplier* ed = suppliers.find(editId);
        struct St { int parts = 0, withCost = 0; double stockCost = 0, marginSum = 0; };
        std::map<int, St> st;
        for (const inv::Part& p : inv.parts()) {
            St& s = st[p.supplierId()];
            ++s.parts;
            s.stockCost += p.costPrice() * p.quantity();
            if (p.costPrice() > 0 && p.price() > 0) { ++s.withCost; s.marginSum += (p.price() - p.costPrice()) / p.price() * 100.0; }
        }
        auto avg = [](const St& s) { char buf[32]; if (!s.withCost) return std::string("n/a"); std::snprintf(buf, sizeof buf, "%.1f%%", s.marginSum / s.withCost); return std::string(buf); };
        std::string b = "<h1>Suppliers</h1><div class='card'><h2>" + std::string(ed ? "Edit supplier" : "Add supplier") + "</h2><form method='post' action='/suppliers/save'>" + tokenField(c) +
                        "<input type='hidden' name='id' value='" + num(ed ? ed->id : 0) + "'><div class='grid'>"
                        "<div><label>Supplier name</label><input name='name' maxlength='60' required value='" + h(ed ? ed->name : "") + "'></div>"
                        "<div><label>Contact person</label><input name='contact' maxlength='60' value='" + h(ed ? ed->contact : "") + "'></div>"
                        "<div><label>Phone</label><input name='phone' maxlength='40' value='" + h(ed ? ed->phone : "") + "'></div>"
                        "<div><label>Address</label><input name='address' maxlength='120' value='" + h(ed ? ed->address : "") + "'></div></div>"
                        "<label>Notes</label><input name='notes' maxlength='200' style='width:100%' value='" + h(ed ? ed->notes : "") + "'>"
                        "<p><button type='submit'>" + std::string(ed ? "Save changes" : "Add supplier") + "</button>" + (ed ? " <a class='btn sec' href='/suppliers'>Cancel</a>" : "") + "</p></form></div>";
        b += "<div class='card'><h2>Parts by supplier</h2><table><thead><tr><th>Supplier</th><th>Contact</th><th>Phone</th><th class='n'>Parts</th><th class='n'>Stock value (at cost)</th>"
             "<th class='n'>Avg margin</th><th>Status</th><th></th></tr></thead><tbody>";
        for (const sup::Supplier& s : suppliers.all()) {
            const St& x = st[s.id];
            b += "<tr><td><b>" + h(s.name) + "</b></td><td>" + h(s.contact) + "</td><td>" + h(s.phone) + "</td><td class='n'>" + num(x.parts) + "</td><td class='n'>" + php(x.stockCost) +
                 "</td><td class='n'>" + avg(x) + "</td><td><span class='tag " + (s.active ? "ok'>Active" : "out'>Inactive") + "</span></td><td class='right'>"
                 "<a class='btn sm sec' href='/suppliers?view=" + num(s.id) + "'>History</a> <a class='btn sm sec' href='/suppliers?edit=" + num(s.id) + "'>Edit</a> "
                 "<form method='post' action='/suppliers/toggle' style='display:inline'>" + tokenField(c) + "<input type='hidden' name='id' value='" + num(s.id) + "'><input type='hidden' name='on' value='" +
                 (s.active ? "0" : "1") + "'><button class='sm sec' type='submit'>" + (s.active ? "Deactivate" : "Activate") + "</button></form></td></tr>";
        }
        auto none = st.find(0);
        if (none != st.end()) b += "<tr><td class='mut'>(no supplier recorded)</td><td></td><td></td><td class='n'>" + num(none->second.parts) + "</td><td class='n'>" + php(none->second.stockCost) +
                                   "</td><td class='n'>" + avg(none->second) + "</td><td></td><td></td></tr>";
        if (suppliers.all().empty() && none == st.end()) b += "<tr><td colspan='8' class='mut'>No suppliers yet.</td></tr>";
        b += "</tbody></table></div>";
        if (const sup::Supplier* v = suppliers.find(viewId)) {
            b += "<div class='card'><h2>" + h(v->name) + " &middot; parts supplied</h2><table><thead><tr><th>Part Number</th><th>Part Description</th><th class='n'>Stock</th><th class='n'>Retail</th><th class='n'>Cost</th><th class='n'>Profit (margin)</th></tr></thead><tbody>";
            int shown = 0;
            for (const inv::Part& p : inv.parts()) {
                if (p.supplierId() != v->id) continue;
                ++shown;
                b += "<tr><td>" + h(partLabel(p)) + "</td><td>" + h(p.name()) + "</td><td class='n'>" + num(p.quantity()) + "</td><td class='n'>" + php(p.price()) + "</td>" + marginCells(p) + "</tr>";
            }
            if (!shown) b += "<tr><td colspan='6' class='mut'>No parts are linked to this supplier yet. Pick the supplier in Edit part or when restocking.</td></tr>";
            b += "</tbody></table></div><div class='card'><h2>Purchase history</h2><table><thead><tr><th>Date</th><th>Part</th><th class='n'>Qty</th><th class='n'>Unit cost</th><th class='n'>Total</th><th>Note</th><th>By</th></tr></thead><tbody>";
            int n = 0;
            const auto& all = purchases.all();
            for (auto it = all.rbegin(); it != all.rend() && n < 100; ++it) {
                if (it->supplierId != v->id) continue;
                ++n;
                const inv::Part* pp = inv.find(it->partId);
                b += "<tr><td>" + h(it->timestamp) + "</td><td>" + h(pp ? pp->fullName() : "Part " + num(it->partId)) + "</td><td class='n'>" + num(it->qty) + "</td><td class='n'>" + php(it->unitCost) +
                     "</td><td class='n'>" + php(it->unitCost * it->qty) + "</td><td>" + h(it->ref) + "</td><td>" + h(it->user) + "</td></tr>";
            }
            if (!n) b += "<tr><td colspan='7' class='mut'>No purchases recorded yet. They are recorded when you restock a part and pick this supplier.</td></tr>";
            b += "</tbody></table></div>";
        }
        html(c, layout(c, "Suppliers", b));
    }
    void doSupplierSave(Ctx& c) {
        sup::Supplier s;
        s.name = c.param("name"); s.contact = c.param("contact"); s.phone = c.param("phone"); s.address = c.param("address"); s.notes = c.param("notes");
        const int id = intParam(c, "id", 1, 999999, 0);
        int nid = 0;
        const std::string err = suppliers.save(id, s, nid);
        if (!err.empty()) return redirect(c, msgUrl(id ? "/suppliers?edit=" + num(id) : std::string("/suppliers"), err, false));
        audit.record(c.user(), id ? "SUPPLIER_UPDATED" : "SUPPLIER_ADDED", util::oneLine(util::trim(s.name)), c.ip);
        redirect(c, msgUrl("/suppliers", id ? "Supplier updated." : "Supplier added.", true));
    }
    void doSupplierToggle(Ctx& c) {
        const int id = intParam(c, "id", 1, 999999, 0);
        const bool on = c.param("on") == "1";
        const std::string err = suppliers.setActive(id, on);
        if (!err.empty()) return redirect(c, msgUrl("/suppliers", err, false));
        audit.record(c.user(), on ? "SUPPLIER_ACTIVATED" : "SUPPLIER_DEACTIVATED", suppliers.nameOf(id), c.ip);
        redirect(c, msgUrl("/suppliers", on ? "Supplier activated." : "Supplier deactivated (history kept).", true));
    }

"""
DOSTOCK = r"""r = inv.restock(id, static_cast<int>(qty), cost, c.user(), note);
            if (r.ok) {   // Phase 2: remember who we bought it from
                const inv::Part* rp = inv.find(id);
                const int oldSid = rp ? rp->supplierId() : 0;
                const int sid = readSupplierId(c, oldSid);
                if (sid > 0) {
                    sup::Purchase pu;
                    pu.timestamp = util::nowStamp(); pu.supplierId = sid; pu.partId = id; pu.qty = static_cast<int>(qty); pu.unitCost = cost; pu.ref = note; pu.user = c.user();
                    purchases.record(pu);
                    const bool setCost = auth::allowed(c.role, auth::Permission::ViewReports) && c.param("setcost") == "1" && cost > 0;
                    if (setCost || oldSid == 0) inv.setSourcing(id, sid, setCost ? std::optional<double>(cost) : std::nullopt, c.user());
                    audit.record(c.user(), "PURCHASE", "Part " + num(id) + " x" + num(qty) + " from " + suppliers.nameOf(sid) + " @ " + util::money(cost), c.ip);
                }
            }
"""
DOEDIT = r"""if (c.req.has_param("supplier") || c.req.has_param("cost")) {   // Phase 2: supplier + cost price
                    const inv::Part* np = inv.find(id);
                    const int oldSid = np ? np->supplierId() : 0;
                    const double oldCost = np ? np->costPrice() : 0.0;
                    const int sid = c.req.has_param("supplier") ? readSupplierId(c, oldSid) : oldSid;
                    std::optional<double> newCost;
                    double cv = 0;
                    if (c.req.has_param("cost")) {
                        if (util::trim(c.param("cost")).empty()) newCost = 0.0;
                        else if (parseMoneyText(c.param("cost"), cv)) newCost = cv;
                    }
                    if ((sid != oldSid || (newCost && *newCost != oldCost)) && inv.setSourcing(id, sid, newCost, c.user()).ok)
                        audit.record(c.user(), "PART_SOURCING_CHANGED", "Part " + num(id) + " " + oldName + ": supplier " + suppliers.nameOf(oldSid) + " -> " + suppliers.nameOf(sid) +
                                     ", cost " + util::money(oldCost) + " -> " + util::money(newCost ? *newCost : oldCost), c.ip);
                }
                if (price != oldPrice)
                    audit.record(c.user(), "PRICE_CHANGE"""
edit("src/web.cpp", "suppliers.h", [
 ('#include "logo_asset.h"\n', '#include "logo_asset.h"\n#include "suppliers.h"\n#include <cstdio>\n'),
 ("    std::mutex mtx;  // httplib is multi-threaded; the stores are not\n",
  "    std::mutex mtx;  // httplib is multi-threaded; the stores are not\n    sup::SupplierBook suppliers;   // Phase 2\n    sup::PurchaseLog purchases;\n"),
 ("        svr.set_payload_max_length(8 * 1024 * 1024);\n",
  "        suppliers.open(inv.dataDir());\n        purchases.open(inv.dataDir());\n        svr.set_payload_max_length(8 * 1024 * 1024);\n"),
 ('        add("GET", "/account", &Impl::accountPage);\n',
  '        add("GET", "/suppliers", &Impl::suppliersPage, P::ManageSuppliers);\n'
  '        add("POST", "/suppliers/save", &Impl::doSupplierSave, P::ManageSuppliers);\n'
  '        add("POST", "/suppliers/toggle", &Impl::doSupplierToggle, P::ManageSuppliers);\n'
  '        add("GET", "/account", &Impl::accountPage);\n'),
 ('            if (auth::allowed(c.role, P::ManageUsers)) o += link("/users", "Users");',
  '            if (auth::allowed(c.role, P::ManageSuppliers)) o += link("/suppliers", "Suppliers");\n            if (auth::allowed(c.role, P::ManageUsers)) o += link("/users", "Users");'),
 ("    // ------------------------------------------------------------------ stock: restock / adjust (Staff + Admin)\n",
  WEB_BLOCK + "    // ------------------------------------------------------------------ stock: restock / adjust (Staff + Admin)\n"),
 # restock form
 ("""                        "<label>Unit cost paid (&#8369;, optional - used for expense reports)</label><input name='cost' type='number' step='0.01' min='0'>"\n""",
  """                        "<label>Supplier (optional - recorded in the purchase history)</label>" + supplierSelect(p->supplierId()) +\n"""
  """                        "<label>Unit cost paid (&#8369;, optional - used for expense reports)</label><input name='cost' type='number' step='0.01' min='0'>" +\n"""
  """                        std::string(auth::allowed(c.role, auth::Permission::ViewReports) ? "<label class='inline'><input type='checkbox' name='setcost' value='1'> Use this as the part's cost price (for margin)</label>" : "") +\n"""),
 ("r = inv.restock(id, static_cast<int>(qty), cost, c.user(), note);\n", DOSTOCK),
 # add page
 ("""                        "<label>Compatible vehicle models <span class='mut'>(optional, separate with commas)</span></label>"\n""",
  """                        "<div class='grid'><div><label>Supplier (optional)</label>" + supplierSelect(0) + "</div><div><label>Cost price (&#8369;, what you pay - optional)</label><input name='cost' type='number' step='0.01' min='0'></div></div>"\n"""
  """                        "<label>Compatible vehicle models <span class='mut'>(optional, separate with commas)</span></label>"\n"""),
 ("""            part.setPartNumber(util::oneLine(util::trim(c.param("pn"))));\n""",
  """            part.setPartNumber(util::oneLine(util::trim(c.param("pn"))));\n"""
  """            part.setSupplierId(readSupplierId(c, 0));\n"""
  """            { double cv = 0; if (parseMoneyText(c.param("cost"), cv)) part.setCostPrice(cv); }\n"""),
 # edit page
 ("""                        "<label>Compatible vehicle models <span class='mut'>(separate with commas)</span></label>"\n""",
  """                        "<div class='grid'><div><label>Supplier</label>" + supplierSelect(p->supplierId()) + "</div><div><label>Cost price (&#8369;, what you pay the supplier)</label><input name='cost' type='number' step='0.01' min='0' value='" + (p->costPrice() > 0 ? util::money(p->costPrice()) : std::string()) + "'></div></div>"\n"""
  """                        "<label>Compatible vehicle models <span class='mut'>(separate with commas)</span></label>"\n"""),
 ("""if (price != oldPrice)\n                    audit.record(c.user(), "PRICE_CHANGE""", DOEDIT),
 # inventory table
 ("data-type='num'>Min</th><th>Status</th><th></th></tr></thead><tbody>\";",
  "data-type='num'>Min</th><th>Status</th>\" + std::string(auth::allowed(c.role, P::ViewReports) ? \"<th>Supplier</th><th class='n'>Cost</th><th class='n'>Margin</th>\" : \"\") + \"<th></th></tr></thead><tbody>\";"),
 ("statusTag(p) + \"</td><td class='right'>\";", "statusTag(p) + \"</td>\" + sourcingCells(c, p) + \"<td class='right'>\";"),
])

# ---------------------------------------------------------------- tests
edit("tests/web_e2e.cpp", "p2supplier", [
 ('''r = s.call("GET", "/", {}, ck); CHECK(!has(r.body, "<script>alert") && has(r.body, "&lt;script&gt;"));\n''',
  '''r = s.call("GET", "/", {}, ck); CHECK(!has(r.body, "<script>alert") && has(r.body, "&lt;script&gt;"));
    // p2supplier: suppliers, purchase history, supplier cost vs retail (margin)
    r = s.call("POST", "/suppliers/save", {{"csrf",csrf},{"id","0"},{"name","Bajaj Parts Depot"},{"phone","0917"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("GET", "/suppliers", {}, ck); CHECK(r.status == 200 && has(r.body, "Bajaj Parts Depot"));
    r = s.call("POST", "/suppliers/save", {{"csrf",csrf},{"id","0"},{"name","bajaj PARTS depot"}}, ck); CHECK(has(r.headers["Location"], "err"));   // duplicate name
    r = s.call("POST", "/stock", {{"csrf",csrf},{"id","101"},{"action","restock"},{"qty","2"},{"cost","100"},{"supplier","1"},{"setcost","1"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("GET", "/", {}, ck); CHECK(has(r.body, "Margin</th>") && has(r.body, "Bajaj Parts Depot") && has(r.body, "&#8369;100.00"));
    r = s.call("GET", "/suppliers", {{"view","1"}}, ck); CHECK(has(r.body, "Purchase history") && has(r.body, "Engine Oil 1L"));
    r = s.call("POST", "/suppliers/toggle", {{"csrf",csrf},{"id","1"},{"on","0"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("POST", "/suppliers/toggle", {{"csrf",csrf},{"id","1"},{"on","1"}}, ck); CHECK(has(r.headers["Location"], "ok"));
'''),
 ('''r = s.call("GET", "/", {}, ck2); CHECK(r.status == 200 && !has(r.body, "btn sm danger") && !has(r.body, ">Reports<") && has(r.body, ">Sell<"));\n''',
  '''r = s.call("GET", "/", {}, ck2); CHECK(r.status == 200 && !has(r.body, "btn sm danger") && !has(r.body, ">Reports<") && has(r.body, ">Sell<"));
    CHECK(!has(r.body, "Margin</th>") && !has(r.body, "Suppliers</a>"));   // p2supplier: Staff see no costs or margins
    r = s.call("GET", "/suppliers", {}, ck2); CHECK(r.status == 403);
'''),
])

# suppliers.h (new file)
newfile = os.path.join(root, "src", "suppliers.h")
if os.path.isfile(newfile) and not results:
    print("suppliers.h present and nothing else to patch.")
for p, s in results.items():
    if not os.path.isfile(os.path.join(root, p + ".p2.bak")): shutil.copy2(os.path.join(root, p), os.path.join(root, p + ".p2.bak"))
    with open(os.path.join(root, p), "wb") as f: f.write(s.encode("utf-8"))
    print("patched", p)
with open(newfile, "wb") as f: f.write(SUPPLIERS_H.lstrip("\n").encode("utf-8"))
print("created src/suppliers.h")
print("\nOK - Phase 2 installed. Next: build and run (Ctrl+F5 in the browser).")
