#include "inventory.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#include "util.h"

namespace fs = std::filesystem;

namespace inv {

namespace {
const char* kBom = "\xEF\xBB\xBF";
const char* kPartsHeader = "PartID,Name,Category,Price,Quantity,MinStock,CostPrice,CompatibleModels";
const char* kInvoiceHeader = "invoice_no,timestamp,customer,username,line_type,part_id,description,qty,unit_price,amount_paid,payment_status";
const char* kPaymentHeader = "timestamp,invoice_no,username,amount,method,balance_after,note";

std::string key(std::string s) {  // header-name normaliser: "Min Stock" -> "minstock"
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
    return o;
}
}  // namespace

// ================================================================ Part
std::string Part::toCsv() const {
    return std::to_string(id_) + ',' + util::csvEscape(name_) + ',' + util::csvEscape(category_) + ',' + util::money(price_) + ',' +
           std::to_string(quantity_) + ',' + std::to_string(minStock_) + ',' + util::money(costPrice_) + ',' +
           util::csvEscape(compatibleModels_);
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
    Part part(static_cast<int>(id), name, cat, price, static_cast<int>(qty), static_cast<int>(mn));
    double cost = 0;
    if (f.size() > 6 && util::parseDouble(f[6], 0, limits::kMaxPrice, cost)) part.setCostPrice(cost);  // optional columns
    if (f.size() > 7) part.setCompatibleModels(util::oneLine(util::trim(f[7])));
    return part;
}

std::vector<std::string> splitModels(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        std::string t;  // trim + collapse inner whitespace
        bool space = false;
        for (unsigned char ch : cur) {
            if (std::isspace(ch)) { space = !t.empty(); continue; }
            if (space) t += ' ';
            space = false;
            t += static_cast<char>(ch);
        }
        cur.clear();
        if (t.empty()) return;
        for (const std::string& e : out) if (util::equalsIgnoreCase(e, t)) return;
        out.push_back(t);
    };
    for (char ch : text) {
        if (ch == ',' || ch == ';' || ch == '|' || ch == '\n' || ch == '\r') flush(); else cur += ch;
    }
    flush();
    return out;
}

std::vector<std::string> Part::modelList() const { return splitModels(compatibleModels_); }

bool Part::fitsModel(const std::string& model) const {
    const std::string want = util::trim(model);
    if (want.empty()) return false;
    for (const std::string& m : splitModels(compatibleModels_)) if (util::equalsIgnoreCase(m, want)) return true;
    return false;
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

// ================================================================ Payments / Invoices
const char* payStatusName(PayStatus s) {
    switch (s) {
        case PayStatus::Paid: return "Paid";
        case PayStatus::Partial: return "Partial";
        case PayStatus::Unpaid: return "Unpaid";
    }
    return "Paid";
}

const std::vector<std::string>& paymentMethods() {
    static const std::vector<std::string> m = {"Cash", "GCash", "Maya", "Bank transfer", "Card"};
    return m;
}

std::string canonicalMethod(const std::string& s) {
    const std::string t = util::trim(s);
    for (const std::string& m : paymentMethods()) if (util::equalsIgnoreCase(m, t)) return m;
    return "";
}

double Invoice::total() const {
    double t = 0;
    for (const InvoiceLine& l : lines) t += l.total();
    return t;
}

double Invoice::balance() const { return std::max(0.0, util::round2(util::round2(total()) - amountPaid)); }

PayStatus Invoice::status() const {
    if (balance() <= 0.0) return PayStatus::Paid;
    return amountPaid <= 0.0 ? PayStatus::Unpaid : PayStatus::Partial;
}

PaymentLog::PaymentLog(std::string file) : file_(std::move(file)) {
    for (const auto& f : util::readCsvFile(file_)) {
        if (f.size() < 5 || f[0] == "timestamp") continue;
        double amount = 0, bal = 0;
        if (!util::parseDouble(f[3], 0, 1e12, amount)) continue;
        if (f.size() > 5) util::parseDouble(f[5], 0, 1e12, bal);
        Payment p;
        p.timestamp = f[0]; p.invoiceNo = f[1]; p.user = f[2]; p.amount = amount; p.method = f[4]; p.balanceAfter = bal;
        if (f.size() > 6) p.note = f[6];
        entries_.push_back(std::move(p));
    }
}

bool PaymentLog::record(Payment p) {
    p.user = util::oneLine(p.user);
    p.note = util::oneLine(p.note);
    entries_.push_back(p);
    return util::appendLine(file_, kPaymentHeader,
                            util::csvEscape(p.timestamp) + ',' + util::csvEscape(p.invoiceNo) + ',' + util::csvEscape(p.user) + ',' + util::money(p.amount) + ',' +
                                util::csvEscape(p.method) + ',' + util::money(p.balanceAfter) + ',' + util::csvEscape(p.note));
}

std::vector<const Payment*> PaymentLog::forInvoice(const std::string& number) const {
    std::vector<const Payment*> out;
    for (const Payment& p : entries_) if (p.invoiceNo == number) out.push_back(&p);
    return out;
}

double PaymentLog::paidFor(const std::string& number) const {
    double t = 0;
    for (const Payment& p : entries_) if (p.invoiceNo == number) t = util::round2(t + p.amount);
    return t;
}

namespace {
std::string invoiceRow(const Invoice& inv, const InvoiceLine& l) {
    return util::csvEscape(inv.number) + ',' + util::csvEscape(inv.timestamp) + ',' + util::csvEscape(inv.customer) + ',' + util::csvEscape(inv.user) + ',' +
           (l.kind == InvoiceLine::Kind::Service ? "SERVICE" : "PART") + ',' + std::to_string(l.partId) + ',' + util::csvEscape(l.description) + ',' +
           std::to_string(l.qty) + ',' + util::money(l.unitPrice) + ',' + util::money(inv.amountPaid) + ',' + payStatusName(inv.status());
}
}  // namespace

InvoiceBook::InvoiceBook(std::string file) : file_(std::move(file)) {
    std::map<std::string, size_t> index;
    int skipped = 0;
    bool legacy = false;
    for (const auto& f : util::readCsvFile(file_)) {
        if (f.empty()) continue;
        if (f[0] == "invoice_no") { legacy = f.size() < 11; continue; }
        long long pid = 0, qty = 0;
        double price = 0;
        if (f.size() < 9 || !util::parseInt(f[7], 1, limits::kMaxQty, qty) || !util::parseDouble(f[8], 0, 1e12, price)) { ++skipped; continue; }
        util::parseInt(f[5], 0, limits::kMaxId, pid);
        auto it = index.find(f[0]);
        if (it == index.end()) {
            Invoice inv;
            inv.number = f[0]; inv.timestamp = f[1]; inv.customer = f[2]; inv.user = f[3];
            inv.amountPaid = -1.0;  // resolved below once every line is known
            if (f.size() > 9) { double paid = 0; inv.amountPaid = util::parseDouble(f[9], 0, 1e12, paid) ? util::round2(paid) : 0.0; }
            invoices_.push_back(std::move(inv));
            it = index.emplace(f[0], invoices_.size() - 1).first;
        }
        Invoice& cur = invoices_[it->second];
        InvoiceLine l;
        l.kind = f[4] == "SERVICE" ? InvoiceLine::Kind::Service : InvoiceLine::Kind::Part;
        l.partId = static_cast<int>(pid); l.description = f[6]; l.qty = static_cast<int>(qty); l.unitPrice = price;
        cur.lines.push_back(l);
    }
    for (Invoice& inv : invoices_) {
        const double total = util::round2(inv.total());
        if (inv.amountPaid < 0) inv.amountPaid = total;               // 9-column (pre-credit) file: those sales were paid at the till
        inv.amountPaid = std::min(inv.amountPaid, total);             // never more than the bill
    }
    if (skipped > 0) {  // the rewrite below/at the next payment would drop damaged rows: keep an untouched copy first
        std::error_code ec;
        fs::copy_file(file_, file_ + ".bak", fs::copy_options::overwrite_existing, ec);
        warning_ = std::to_string(skipped) + " malformed row(s) in invoices.csv were skipped (original kept as invoices.csv.bak).";
    }
    if (legacy && !invoices_.empty()) save();  // upgrade the file to the 11-column layout (the start-up backup holds the original)
}

bool InvoiceBook::save() {
    std::string out = std::string(kInvoiceHeader) + "\n";
    for (const Invoice& inv : invoices_)
        for (const InvoiceLine& l : inv.lines) out += invoiceRow(inv, l) + "\n";
    if (!util::writeFileAtomic(file_, out)) { warning_ = "Could not write invoices.csv - check disk space/permissions."; return false; }
    return true;
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
    for (const InvoiceLine& l : inv.lines) ok &= util::appendLine(file_, kInvoiceHeader, invoiceRow(inv, l));
    return ok;
}

const Invoice* InvoiceBook::find(const std::string& number) const {
    for (const Invoice& i : invoices_) if (i.number == number) return &i;
    return nullptr;
}

bool InvoiceBook::setAmountPaid(const std::string& number, double newPaid) {
    for (Invoice& i : invoices_) {
        if (i.number != number) continue;
        const double old = i.amountPaid;
        i.amountPaid = util::round2(newPaid);
        if (!save()) { i.amountPaid = old; return false; }
        return true;
    }
    return false;
}

// ================================================================ Inventory
Inventory::Inventory(std::string dataDir)
    : dir_(std::move(dataDir)), moves_(path("movements.csv")), invoices_(path("invoices.csv")), payments_(path("payments.csv")) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    load();
    reconcileLedger();
    reconcilePayments();
    if (!invoices_.warning().empty()) warning_ += (warning_.empty() ? "" : " ") + invoices_.warning();
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
    if (p.compatibleModels().size() > kMaxModelsText) return Result::fail("Compatible models text is too long (max " + std::to_string(kMaxModelsText) + " characters).");
    warning_.clear();
    parts_.push_back(p);
    if (!save()) { parts_.pop_back(); return Result::fail(warning_); }
    if (p.quantity() > 0) log(p, MoveType::Restock, p.quantity(), 0.0, actor, "new part");
    return Result::success("Part added.");
}

Result Inventory::updatePart(int id, const std::string& name, const std::string& category, double price, int minStock, const std::string& actor,
                             const std::optional<std::string>& models) {
    (void)actor;
    Part* cur = mut(id);
    if (!cur) return Result::fail("Part not found.");
    const std::string err = Part::validate(id, name, category, price, cur->quantity(), minStock);
    if (!err.empty()) return Result::fail(err);
    if (models && models->size() > kMaxModelsText) return Result::fail("Compatible models text is too long (max " + std::to_string(kMaxModelsText) + " characters).");
    const Part old = *cur;
    warning_.clear();
    cur->setName(name); cur->setCategory(category); cur->setPrice(price); cur->setMinStock(minStock);
    if (models) cur->setCompatibleModels(util::oneLine(util::trim(*models)));
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

CheckoutResult Inventory::checkout(const std::string& customerIn, const std::vector<InvoiceLine>& in, const std::string& actor, double amountPaid,
                                   const std::string& methodIn) {
    auto fail = [](const std::string& m) { return CheckoutResult{false, m, "", 0, 0}; };
    const bool anonymous = util::trim(customerIn).empty() || util::equalsIgnoreCase(util::trim(customerIn), "Walk-in customer");
    const std::string customer = util::oneLine(anonymous ? std::string("Walk-in customer") : util::trim(customerIn));
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

    // Payment at the till: negative = in full. Less than the bill = credit sale, which needs a named customer.
    const double total = util::round2(invoice.total());
    const double paidNow = amountPaid < 0 ? total : util::round2(amountPaid);
    if (!(paidNow <= total)) return fail("The amount paid (" + util::moneyFmt(paidNow) + ") is more than the invoice total (" + util::moneyFmt(total) + ").");
    std::string method = canonicalMethod(methodIn);
    if (paidNow > 0 && method.empty()) return fail("Choose a valid payment method.");
    if (paidNow < total && anonymous) return fail("A credit sale (partial or no payment) needs a customer name - walk-in customers must pay in full.");

    warning_.clear();
    const std::vector<Part> before = parts_;
    for (const auto& n : need) mut(n.first)->setQuantity(mut(n.first)->quantity() - static_cast<int>(n.second));
    if (!save()) { parts_ = before; return fail(warning_); }

    invoice.number = invoices_.nextNumber();
    invoice.timestamp = util::nowStamp();
    invoice.customer = customer;
    invoice.user = actor;
    invoice.amountPaid = paidNow;
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
    if (paidNow > 0) {
        Payment pay;
        pay.timestamp = invoice.timestamp; pay.invoiceNo = invoice.number; pay.user = actor; pay.amount = paidNow; pay.method = method;
        pay.balanceAfter = util::round2(total - paidNow); pay.note = "paid at sale";
        if (!payments_.record(pay)) warning_ += (warning_.empty() ? "" : " ") + std::string("payments.csv could not be written.");
    }
    return {true, "Sale recorded.", invoice.number, paidNow, util::round2(total - paidNow)};
}

PaymentResult Inventory::recordPayment(const std::string& invoiceNo, double amountIn, const std::string& methodIn, const std::string& noteIn,
                                       const std::string& actor, double expectedBalance) {
    auto fail = [](const std::string& m) { PaymentResult r; r.message = m; return r; };
    const Invoice* inv = invoices_.find(invoiceNo);
    if (!inv) return fail("Invoice not found.");
    const double balance = inv->balance();
    if (balance <= 0) return fail(invoiceNo + " is already fully paid.");
    if (expectedBalance >= 0 && std::abs(util::round2(expectedBalance) - balance) > 0.004)
        return fail("The balance of " + invoiceNo + " changed since you opened the payment page (now " + util::moneyFmt(balance) + "). Check the invoice and try again.");
    const double amount = util::round2(amountIn);
    if (!(amount > 0)) return fail("Enter a payment amount greater than zero.");
    if (amount > balance) return fail("The payment (" + util::moneyFmt(amount) + ") is more than the balance owed (" + util::moneyFmt(balance) + ").");
    const std::string method = canonicalMethod(methodIn);
    if (method.empty()) return fail("Choose a valid payment method.");
    const std::string note = util::oneLine(util::trim(noteIn));
    if (note.size() > 80) return fail("Note is too long (max 80 characters).");

    warning_.clear();
    const double newPaid = util::round2(inv->amountPaid + amount);
    if (!invoices_.setAmountPaid(invoiceNo, newPaid)) return fail(invoices_.warning());
    inv = invoices_.find(invoiceNo);
    Payment p;
    p.timestamp = util::nowStamp(); p.invoiceNo = invoiceNo; p.user = actor; p.amount = amount; p.method = method; p.note = note;
    p.balanceAfter = inv->balance();
    if (!payments_.record(p)) warning_ = "payments.csv could not be written.";
    PaymentResult r;
    r.ok = true; r.applied = amount; r.balance = inv->balance(); r.status = inv->status(); r.customer = inv->customer;
    r.message = "Payment of " + util::moneyFmt(amount) + " recorded for " + invoiceNo + (r.balance > 0 ? "; balance " + util::moneyFmt(r.balance) + "." : " - fully paid.");
    return r;
}

// Every payment on an invoice must be in payments.csv so the daily cash report adds up. Invoices from before credit
// tracking (or whose payment row was lost to a failed write) get an opening "payment" at the invoice time.
void Inventory::reconcilePayments() {
    for (const Invoice& inv : invoices_.all()) {
        const double missing = util::round2(inv.amountPaid - payments_.paidFor(inv.number));
        if (missing <= 0) continue;
        Payment p;
        p.timestamp = inv.timestamp; p.invoiceNo = inv.number; p.user = "system"; p.amount = missing; p.method = "Cash";
        p.balanceAfter = util::round2(util::round2(inv.total()) - inv.amountPaid); p.note = "opening balance";
        if (!payments_.record(p)) { warning_ = "payments.csv could not be written."; return; }
    }
}

std::vector<std::string> Inventory::allModels() const {
    std::vector<std::string> out;
    for (const Part& p : parts_)
        for (const std::string& m : p.modelList()) {
            bool seen = false;
            for (const std::string& e : out) if (util::equalsIgnoreCase(e, m)) { seen = true; break; }
            if (!seen) out.push_back(m);
        }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return util::toLower(a) < util::toLower(b); });
    return out;
}

// ---------------------------------------------------------------- CSV import
ImportSummary Inventory::importCsv(const std::string& text, const std::string& actor) {
    ImportSummary sum;
    auto rows = util::parseCsvText(text);
    if (rows.empty()) { sum.problems.push_back("The file is empty."); return sum; }

    // Column positions: by header name when a header row is present, otherwise parts.csv order.
    int cId = 0, cName = 1, cCat = 2, cPrice = 3, cQty = 4, cMin = 5, cCost = -1, cModels = -1;
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
            else if (k == "compatiblemodels" || k == "models" || k == "compatible" || k == "vehiclemodels") cModels = c;
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
        if (!cell(r, cModels).empty()) {
            const std::string models = util::oneLine(cell(r, cModels));
            if (models.size() > kMaxModelsText) { sum.problems.push_back(where + "compatible models text is too long."); ++sum.skipped; continue; }
            p.setCompatibleModels(models);
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
                                 c->minStock() != o.part.minStock() || c->compatibleModels() != o.part.compatibleModels() || o.delta != 0;
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
               std::to_string(p.quantity()) + ',' + std::to_string(p.minStock()) + ',' + util::money(p.costPrice()) + ',' +
               util::csvSafe(p.compatibleModels()) + "\r\n";
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
