#include "inventory.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

#include "util.h"

namespace fs = std::filesystem;

namespace inv {

namespace {
const char* kBom = "\xEF\xBB\xBF";
const char* kPartsHeader = "PartID,Name,Category,Price,Quantity,MinStock";
const char* kInvoiceHeader = "invoice_no,timestamp,customer,username,line_type,part_id,description,qty,unit_price";

std::string key(std::string s) {  // header-name normaliser: "Min Stock" -> "minstock"
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
    return o;
}
}  // namespace

// ================================================================ Part
std::string Part::toCsv() const {
    return std::to_string(id_) + ',' + util::csvEscape(name_) + ',' + util::csvEscape(category_) + ',' + util::money(price_) + ',' +
           std::to_string(quantity_) + ',' + std::to_string(minStock_);
}

std::string Part::validate(int id, const std::string& name, const std::string& category, double price, int qty, int minStock) {
    if (id < 1 || id > limits::kMaxId) return "Part ID must be 1 to " + std::to_string(limits::kMaxId) + ".";
    if (name.empty() || name.size() > 80) return "Part name is required (max 80 characters).";
    if (category.empty() || category.size() > 40) return "Vehicle system is required (max 40 characters).";
    if (!(price >= 0.0 && price <= limits::kMaxPrice)) return "Price is out of range.";
    if (qty < 0 || qty > limits::kMaxQty) return "Quantity must be 0 to " + std::to_string(limits::kMaxQty) + ".";
    if (minStock < 0 || minStock > limits::kMaxQty) return "Minimum stock must be 0 to " + std::to_string(limits::kMaxQty) + ".";
    return "";
}

std::optional<Part> Part::fromCsv(const std::vector<std::string>& f) {
    long long id = 0, qty = 0, mn = 5;
    double price = 0;
    std::string name, cat = "General";
    if (f.size() >= 6) {
        if (!util::parseInt(f[0], 1, limits::kMaxId, id) || !util::parseDouble(f[3], 0, limits::kMaxPrice, price) ||
            !util::parseInt(f[4], 0, limits::kMaxQty, qty) || !util::parseInt(f[5], 0, limits::kMaxQty, mn))
            return std::nullopt;
        name = util::trim(f[1]);
        cat = util::trim(f[2]);
    } else if (f.size() == 4) {  // legacy: id,name,price,stock
        if (!util::parseInt(f[0], 1, limits::kMaxId, id) || !util::parseDouble(f[2], 0, limits::kMaxPrice, price) ||
            !util::parseInt(f[3], 0, limits::kMaxQty, qty))
            return std::nullopt;
        name = util::trim(f[1]);
    } else return std::nullopt;
    if (!Part::validate(static_cast<int>(id), name, cat, price, static_cast<int>(qty), static_cast<int>(mn)).empty()) return std::nullopt;
    return Part(static_cast<int>(id), name, cat, price, static_cast<int>(qty), static_cast<int>(mn));
}

const std::vector<std::string>& standardSystems() {
    static const std::vector<std::string> s = {"Engine", "Braking", "Electrical", "Transmission", "Chassis"};
    return s;
}

// ================================================================ Movements
const char* moveName(MoveType t) {
    switch (t) {
        case MoveType::Restock: return "RESTOCK";
        case MoveType::Sale: return "SALE";
        case MoveType::Adjustment: return "ADJUSTMENT";
        case MoveType::Delete: return "DELETE";
    }
    return "ADJUSTMENT";
}

std::optional<MoveType> parseMove(const std::string& s) {
    for (MoveType t : {MoveType::Restock, MoveType::Sale, MoveType::Adjustment, MoveType::Delete})
        if (util::equalsIgnoreCase(s, moveName(t))) return t;
    return std::nullopt;
}

const char* MovementLog::header() { return "timestamp,username,part_id,part_name,action_type,quantity_changed,resulting_stock,unit_price,ref"; }

static std::string movementLine(const Movement& m, bool forExport) {
    auto esc = [forExport](const std::string& s) { return forExport ? util::csvSafe(s) : util::csvEscape(s); };
    return esc(m.timestamp) + ',' + esc(m.user) + ',' + std::to_string(m.partId) + ',' + esc(m.partName) + ',' + moveName(m.type) + ',' +
           std::to_string(m.qtyChanged) + ',' + std::to_string(m.resultingStock) + ',' + util::money(m.unitPrice) + ',' + esc(m.ref);
}

MovementLog::MovementLog(std::string file) : file_(std::move(file)) {
    for (const auto& f : util::readCsvFile(file_)) {
        if (f.size() < 7) continue;
        long long pid, qc, rs;
        const auto type = parseMove(f[4]);
        if (!type || !util::parseInt(f[2], 0, 2000000000, pid) || !util::parseInt(f[5], -2000000000, 2000000000, qc) ||
            !util::parseInt(f[6], 0, 2000000000, rs))
            continue;  // header row or damaged line
        Movement m;
        m.timestamp = f[0]; m.user = f[1]; m.partId = static_cast<int>(pid); m.partName = f[3];
        m.type = *type; m.qtyChanged = static_cast<int>(qc); m.resultingStock = static_cast<int>(rs);
        if (f.size() > 7) util::parseDouble(f[7], 0, 1e12, m.unitPrice);
        if (f.size() > 8) m.ref = f[8];
        entries_.push_back(std::move(m));
    }
}

bool MovementLog::record(Movement m) {
    m.user = util::oneLine(m.user);
    m.partName = util::oneLine(m.partName);
    m.ref = util::oneLine(m.ref);
    entries_.push_back(m);
    return util::appendLine(file_, header(), movementLine(m, false));
}

std::vector<const Movement*> MovementLog::recent(size_t limit) const {
    std::vector<const Movement*> out;
    for (size_t i = entries_.size(); i-- > 0 && (limit == 0 || out.size() < limit);) out.push_back(&entries_[i]);
    return out;
}

std::string MovementLog::toCsv() const {
    std::string out = std::string(kBom) + header() + "\r\n";
    for (const Movement& m : entries_) out += movementLine(m, true) + "\r\n";
    return out;
}

// ================================================================ Invoices
double Invoice::total() const {
    double t = 0;
    for (const InvoiceLine& l : lines) t += l.total();
    return t;
}

InvoiceBook::InvoiceBook(std::string file) : file_(std::move(file)) {
    for (const auto& f : util::readCsvFile(file_)) {
        if (f.size() < 9 || f[0] == "invoice_no") continue;
        long long pid = 0, qty = 0;
        double price = 0;
        if (!util::parseInt(f[7], 1, limits::kMaxQty, qty) || !util::parseDouble(f[8], 0, 1e12, price)) continue;
        util::parseInt(f[5], 0, limits::kMaxId, pid);
        Invoice* cur = nullptr;
        for (Invoice& i : invoices_) if (i.number == f[0]) { cur = &i; break; }
        if (!cur) {
            invoices_.push_back({f[0], f[1], f[2], f[3], {}});
            cur = &invoices_.back();
        }
        InvoiceLine l;
        l.kind = f[4] == "SERVICE" ? InvoiceLine::Kind::Service : InvoiceLine::Kind::Part;
        l.partId = static_cast<int>(pid); l.description = f[6]; l.qty = static_cast<int>(qty); l.unitPrice = price;
        cur->lines.push_back(l);
    }
}

std::string InvoiceBook::nextNumber() const {
    long long maxN = 0;
    for (const Invoice& i : invoices_) {
        long long n = 0;
        if (i.number.rfind("INV-", 0) == 0 && util::parseInt(i.number.substr(4), 0, 999999999, n)) maxN = std::max(maxN, n);
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "INV-%06lld", maxN + 1);
    return buf;
}

bool InvoiceBook::add(const Invoice& inv) {
    invoices_.push_back(inv);
    bool ok = true;
    for (const InvoiceLine& l : inv.lines)
        ok &= util::appendLine(file_, kInvoiceHeader,
                               util::csvEscape(inv.number) + ',' + util::csvEscape(inv.timestamp) + ',' + util::csvEscape(inv.customer) + ',' +
                                   util::csvEscape(inv.user) + ',' + (l.kind == InvoiceLine::Kind::Service ? "SERVICE" : "PART") + ',' +
                                   std::to_string(l.partId) + ',' + util::csvEscape(l.description) + ',' + std::to_string(l.qty) + ',' +
                                   util::money(l.unitPrice));
    return ok;
}

const Invoice* InvoiceBook::find(const std::string& number) const {
    for (const Invoice& i : invoices_) if (i.number == number) return &i;
    return nullptr;
}

// ================================================================ Inventory
Inventory::Inventory(std::string dataDir)
    : dir_(std::move(dataDir)), moves_(path("movements.csv")), invoices_(path("invoices.csv")) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    load();
    reconcileLedger();
}

std::string Inventory::path(const char* name) const { return (fs::path(dir_) / name).string(); }

const Part* Inventory::find(int id) const {
    for (const Part& p : parts_) if (p.id() == id) return &p;
    return nullptr;
}
Part* Inventory::mut(int id) {
    for (Part& p : parts_) if (p.id() == id) return &p;
    return nullptr;
}

void Inventory::load() {
    const std::string file = path("parts.csv");
    std::error_code ec;
    if (!fs::exists(file, ec)) {  // first run: a few typical Bajaj RE parts
        parts_ = {Part(101, "Engine Oil 1L", "Engine", 450.00, 20, 8),        Part(102, "Brake Pad Set", "Braking", 850.50, 12, 5),
                  Part(103, "Spark Plug", "Electrical", 180.00, 30, 10),      Part(104, "Clutch Plate Set", "Transmission", 1250.00, 6, 4),
                  Part(105, "Front Fork Assembly", "Chassis", 3200.00, 3, 2), Part(106, "Headlight Bulb", "Electrical", 120.00, 4, 10)};
        seeded_ = true;
        save();
        return;
    }
    int skipped = 0;
    for (const auto& f : util::readCsvFile(file)) {
        if (f.empty()) continue;
        if (auto p = Part::fromCsv(f)) {
            if (!find(p->id())) parts_.push_back(*p); else ++skipped;
        } else if (!util::equalsIgnoreCase(util::trim(f[0]), "PartID") && !util::equalsIgnoreCase(util::trim(f[0]), "ID")) ++skipped;
    }
    if (skipped > 0) {  // keep an untouched copy before the next save rewrites the file
        fs::copy_file(file, file + ".bak", fs::copy_options::overwrite_existing, ec);
        warning_ = std::to_string(skipped) + " malformed row(s) in parts.csv were skipped (original kept as parts.csv.bak).";
    }
}

bool Inventory::save() {
    std::string out = std::string(kPartsHeader) + "\n";
    for (const Part& p : parts_) out += p.toCsv() + "\n";
    if (!util::writeFileAtomic(path("parts.csv"), out)) { warning_ = "Could not write parts.csv - check disk space/permissions."; return false; }
    return true;
}

void Inventory::log(const Part& p, MoveType t, int delta, double unitPrice, const std::string& actor, const std::string& ref) {
    Movement m;
    m.timestamp = util::nowStamp(); m.user = actor; m.partId = p.id(); m.partName = p.name(); m.type = t;
    m.qtyChanged = delta; m.resultingStock = p.quantity(); m.unitPrice = unitPrice; m.ref = ref;
    if (!moves_.record(m)) warning_ = "movements.csv could not be written.";
}

// Makes movements.csv agree with parts.csv at start-up, so reports have a starting point:
//  - a part with stock but no movements gets an "opening balance" RESTOCK (cost 0)
//  - a part whose stock differs from its last movement (file edited by hand) gets an ADJUSTMENT
void Inventory::reconcileLedger() {
    std::map<int, int> lastStock;
    for (const Movement& m : moves_.all()) lastStock[m.partId] = m.resultingStock;
    for (const Part& p : parts_) {
        auto it = lastStock.find(p.id());
        if (it == lastStock.end()) {
            if (p.quantity() > 0) log(p, MoveType::Restock, p.quantity(), 0.0, "system", "opening balance");
        } else if (it->second != p.quantity()) {
            log(p, MoveType::Adjustment, p.quantity() - it->second, 0.0, "system", "parts.csv changed outside the app");
        }
    }
}

Result Inventory::addPart(const Part& p, const std::string& actor) {
    const std::string err = Part::validate(p.id(), p.name(), p.category(), p.price(), p.quantity(), p.minStock());
    if (!err.empty()) return Result::fail(err);
    if (find(p.id())) return Result::fail("A part with ID " + std::to_string(p.id()) + " already exists.");
    warning_.clear();
    parts_.push_back(p);
    if (!save()) { parts_.pop_back(); return Result::fail(warning_); }
    if (p.quantity() > 0) log(p, MoveType::Restock, p.quantity(), 0.0, actor, "new part");
    return Result::success("Part added.");
}

Result Inventory::updatePart(int id, const std::string& name, const std::string& category, double price, int minStock, const std::string& actor) {
    (void)actor;
    Part* cur = mut(id);
    if (!cur) return Result::fail("Part not found.");
    const std::string err = Part::validate(id, name, category, price, cur->quantity(), minStock);
    if (!err.empty()) return Result::fail(err);
    const Part old = *cur;
    warning_.clear();
    cur->setName(name); cur->setCategory(category); cur->setPrice(price); cur->setMinStock(minStock);
    if (!save()) { *cur = old; return Result::fail(warning_); }
    return Result::success("Part updated.");
}

Result Inventory::removePart(int id, const std::string& actor) {
    const Part* f = find(id);
    if (!f) return Result::fail("Part not found.");
    const Part gone = *f;
    const std::vector<Part> before = parts_;
    warning_.clear();
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [id](const Part& p) { return p.id() == id; }), parts_.end());
    if (!save()) { parts_ = before; return Result::fail(warning_); }
    Part zero = gone;
    zero.setQuantity(0);  // resulting_stock is 0 once the record is gone
    log(zero, MoveType::Delete, -gone.quantity(), gone.price(), actor, "record deleted");
    return Result::success("Part deleted.");
}

Result Inventory::restock(int id, int qty, double unitCost, const std::string& actor, const std::string& ref) {
    Part* p = mut(id);
    if (!p) return Result::fail("Part not found.");
    if (qty < 1) return Result::fail("Restock quantity must be at least 1.");
    if (!(unitCost >= 0 && unitCost <= limits::kMaxPrice)) return Result::fail("Unit cost is out of range.");
    if (static_cast<long long>(p->quantity()) + qty > limits::kMaxQty) return Result::fail("Stock cannot exceed " + std::to_string(limits::kMaxQty) + " units.");
    warning_.clear();
    p->setQuantity(p->quantity() + qty);
    if (!save()) { p->setQuantity(p->quantity() - qty); return Result::fail(warning_); }
    log(*p, MoveType::Restock, qty, unitCost, actor, ref);
    return Result::success("Stock added.");
}

Result Inventory::adjust(int id, long long delta, const std::string& actor, const std::string& ref) {
    Part* p = mut(id);
    if (!p) return Result::fail("Part not found.");
    if (delta == 0) return Result::fail("The change must not be zero.");
    const long long n = static_cast<long long>(p->quantity()) + delta;
    if (n < 0) return Result::fail("Cannot remove " + std::to_string(-delta) + ": only " + std::to_string(p->quantity()) + " in stock.");
    if (n > limits::kMaxQty) return Result::fail("Stock cannot exceed " + std::to_string(limits::kMaxQty) + " units.");
    warning_.clear();
    const int old = p->quantity();
    p->setQuantity(static_cast<int>(n));
    if (!save()) { p->setQuantity(old); return Result::fail(warning_); }
    log(*p, MoveType::Adjustment, static_cast<int>(delta), 0.0, actor, ref);
    return Result::success("Stock updated.");
}

CheckoutResult Inventory::checkout(const std::string& customerIn, const std::vector<InvoiceLine>& in, const std::string& actor) {
    auto fail = [](const std::string& m) { return CheckoutResult{false, m, ""}; };
    const std::string customer = util::oneLine(util::trim(customerIn).empty() ? std::string("Walk-in customer") : util::trim(customerIn));
    if (customer.size() > 80) return fail("Customer name is too long (max 80).");
    if (in.empty()) return fail("Add at least one part or service.");

    // Validate everything first so a sale is all-or-nothing.
    std::map<int, long long> need;
    Invoice invoice;
    for (const InvoiceLine& l : in) {
        InvoiceLine line = l;
        if (l.qty < 1 || l.qty > limits::kMaxQty) return fail("Quantities must be between 1 and " + std::to_string(limits::kMaxQty) + ".");
        if (l.kind == InvoiceLine::Kind::Part) {
            const Part* p = find(l.partId);
            if (!p) return fail("Part " + std::to_string(l.partId) + " does not exist.");
            need[l.partId] += l.qty;
            line.description = p->name();
            line.unitPrice = p->price();  // catalogue price - never taken from the request
        } else {
            line.description = util::oneLine(util::trim(l.description));
            if (line.description.empty() || line.description.size() > 100) return fail("Service description is required (max 100 characters).");
            if (!(l.unitPrice >= 0 && l.unitPrice <= limits::kMaxPrice)) return fail("Service price is out of range.");
            line.partId = 0;
        }
        invoice.lines.push_back(line);
    }
    for (const auto& n : need) {
        const Part* p = find(n.first);
        if (n.second > p->quantity())
            return fail("Not enough stock for \"" + p->name() + "\": " + std::to_string(p->quantity()) + " available, " + std::to_string(n.second) + " requested.");
    }

    warning_.clear();
    const std::vector<Part> before = parts_;
    for (const auto& n : need) mut(n.first)->setQuantity(mut(n.first)->quantity() - static_cast<int>(n.second));
    if (!save()) { parts_ = before; return fail(warning_); }

    invoice.number = invoices_.nextNumber();
    invoice.timestamp = util::nowStamp();
    invoice.customer = customer;
    invoice.user = actor;
    // One SALE movement per part line; resulting_stock is the running level after that line.
    std::map<int, int> running;
    for (const auto& n : need) running[n.first] = mut(n.first)->quantity() + static_cast<int>(n.second);
    for (const InvoiceLine& l : invoice.lines) {
        if (l.kind != InvoiceLine::Kind::Part) continue;
        running[l.partId] -= l.qty;
        Movement m;
        m.timestamp = invoice.timestamp; m.user = actor; m.partId = l.partId; m.partName = l.description; m.type = MoveType::Sale;
        m.qtyChanged = -l.qty; m.resultingStock = running[l.partId]; m.unitPrice = l.unitPrice; m.ref = invoice.number;
        if (!moves_.record(m)) warning_ = "movements.csv could not be written.";
    }
    if (!invoices_.add(invoice)) warning_ += (warning_.empty() ? "" : " ") + std::string("invoices.csv could not be written.");
    return {true, "Sale recorded.", invoice.number};
}

// ---------------------------------------------------------------- CSV import
ImportSummary Inventory::importCsv(const std::string& text, const std::string& actor) {
    ImportSummary sum;
    auto rows = util::parseCsvText(text);
    if (rows.empty()) { sum.problems.push_back("The file is empty."); return sum; }

    // Column positions: by header name when a header row is present, otherwise parts.csv order.
    int cId = 0, cName = 1, cCat = 2, cPrice = 3, cQty = 4, cMin = 5, cCost = -1;
    size_t first = 0;
    long long dummy;
    if (!util::parseInt(rows[0][0], 0, limits::kMaxId, dummy)) {
        cId = cName = cCat = cPrice = cQty = cMin = -1;
        for (size_t i = 0; i < rows[0].size(); ++i) {
            const std::string k = key(rows[0][i]);
            const int c = static_cast<int>(i);
            if (k == "id" || k == "partid") cId = c;
            else if (k == "name" || k == "partname") cName = c;
            else if (k == "system" || k == "category" || k == "vehiclesystem") cCat = c;
            else if (k == "price" || k == "unitprice" || k == "pricephp") cPrice = c;
            else if (k == "quantity" || k == "qty" || k == "stock" || k == "stocklevel") cQty = c;
            else if (k == "minstock" || k == "min" || k == "minimum" || k == "reorderlevel") cMin = c;
            else if (k == "unitcost" || k == "cost") cCost = c;
        }
        first = 1;
        if (cId < 0 || cQty < 0) { sum.problems.push_back("Header row needs at least an ID column and a Quantity column."); return sum; }
    }
    auto cell = [](const std::vector<std::string>& r, int c) { return c >= 0 && c < static_cast<int>(r.size()) ? util::trim(r[c]) : std::string(); };

    warning_.clear();
    const std::vector<Part> before = parts_;
    struct Pending { Part part; int delta; double cost; bool isNew; };
    std::vector<Pending> ops;
    std::set<int> seen;
    for (size_t i = first; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const std::string where = "Line " + std::to_string(i + 1) + ": ";
        long long id, qty, mn = 0;
        if (!util::parseInt(cell(r, cId), 1, limits::kMaxId, id)) { sum.problems.push_back(where + "invalid part ID."); ++sum.skipped; continue; }
        if (!util::parseInt(cell(r, cQty), 0, limits::kMaxQty, qty)) { sum.problems.push_back(where + "invalid quantity."); ++sum.skipped; continue; }
        if (!seen.insert(static_cast<int>(id)).second) { sum.problems.push_back(where + "duplicate ID " + std::to_string(id) + " ignored."); ++sum.skipped; continue; }
        double cost = 0;
        if (!cell(r, cCost).empty() && !util::parseDouble(cell(r, cCost), 0, limits::kMaxPrice, cost)) { sum.problems.push_back(where + "invalid unit cost."); ++sum.skipped; continue; }

        const Part* cur = find(static_cast<int>(id));
        Part p = cur ? *cur : Part(static_cast<int>(id), "", "", 0, 0, 5);
        if (!cell(r, cName).empty()) p.setName(cell(r, cName));
        if (!cell(r, cCat).empty()) p.setCategory(cell(r, cCat));
        if (!cell(r, cPrice).empty()) {
            double pr;
            if (!util::parseDouble(cell(r, cPrice), 0, limits::kMaxPrice, pr)) { sum.problems.push_back(where + "invalid price."); ++sum.skipped; continue; }
            p.setPrice(pr);
        }
        if (!cell(r, cMin).empty()) {
            if (!util::parseInt(cell(r, cMin), 0, limits::kMaxQty, mn)) { sum.problems.push_back(where + "invalid minimum stock."); ++sum.skipped; continue; }
            p.setMinStock(static_cast<int>(mn));
        }
        const int delta = static_cast<int>(qty) - (cur ? cur->quantity() : 0);
        p.setQuantity(static_cast<int>(qty));
        const std::string err = Part::validate(p.id(), p.name(), p.category(), p.price(), p.quantity(), p.minStock());
        if (!err.empty()) { sum.problems.push_back(where + err + (cur ? "" : " (new parts need name, system and price)")); ++sum.skipped; continue; }
        ops.push_back({p, delta, cost, cur == nullptr});
    }

    for (const Pending& o : ops) {
        if (o.isNew) {
            parts_.push_back(o.part);
            ++sum.added;
        } else {
            Part* c = mut(o.part.id());
            const bool changed = c->name() != o.part.name() || c->category() != o.part.category() || c->price() != o.part.price() ||
                                 c->minStock() != o.part.minStock() || o.delta != 0;
            if (c->price() != o.part.price()) sum.priceChanges.push_back({c->id(), c->name(), c->price(), o.part.price()});
            *c = o.part;
            if (changed) ++sum.updated; else ++sum.unchanged;
        }
    }
    if (!ops.empty() && !save()) {
        parts_ = before;
        sum.added = sum.updated = sum.unchanged = 0;
        sum.priceChanges.clear();
        sum.problems.push_back(warning_);
        return sum;
    }
    for (const Pending& o : ops) {
        if (o.delta == 0) continue;
        const Part& p = *find(o.part.id());
        if (o.delta > 0) log(p, MoveType::Restock, o.delta, o.cost, actor, "csv import");
        else log(p, MoveType::Adjustment, o.delta, 0.0, actor, "csv import");
    }
    return sum;
}

std::string Inventory::partsCsv() const {
    std::string out = std::string(kBom) + kPartsHeader + "\r\n";
    for (const Part& p : parts_)
        out += std::to_string(p.id()) + ',' + util::csvSafe(p.name()) + ',' + util::csvSafe(p.category()) + ',' + util::money(p.price()) + ',' +
               std::to_string(p.quantity()) + ',' + std::to_string(p.minStock()) + "\r\n";
    return out;
}

// ---------------------------------------------------------------- backups
BackupSummary Inventory::backupFolder(const std::string& dir, int keepDays) {
    BackupSummary s;
    const fs::path dest = fs::path(dir) / "backups";
    s.directory = dest.string();
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) { s.problems.push_back("Cannot create " + s.directory + ": " + ec.message()); return s; }
    const std::string stamp = util::nowCompact();

    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2) || e.path().extension() != ".csv") continue;
        const fs::path target = dest / (e.path().stem().string() + "_backup_" + stamp + ".csv");
        fs::copy_file(e.path(), target, fs::copy_options::overwrite_existing, ec2);
        if (ec2) s.problems.push_back(e.path().filename().string() + ": " + ec2.message());
        else ++s.copied;
    }

    // Retention: delete backups whose filename timestamp is older than keepDays (0 = keep everything).
    if (keepDays > 0) {
        const std::time_t limit = std::time(nullptr) - static_cast<std::time_t>(keepDays) * 86400;
        for (const auto& e : fs::directory_iterator(dest, ec)) {
            const std::string name = e.path().filename().string();
            const size_t at = name.rfind("_backup_");
            int y, mo, d, h, mi, se;
            if (at == std::string::npos || std::sscanf(name.c_str() + at + 8, "%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) != 6) continue;
            if (auto t = util::parseStamp(std::to_string(y) + "-" + std::to_string(mo) + "-" + std::to_string(d) + " " + std::to_string(h) + ":" +
                                          std::to_string(mi) + ":" + std::to_string(se)))
                if (*t < limit) { std::error_code ec3; if (fs::remove(e.path(), ec3)) ++s.pruned; }
        }
    }
    return s;
}

}  // namespace inv
