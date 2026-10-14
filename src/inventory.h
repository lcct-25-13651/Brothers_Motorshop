// inventory.h - parts catalogue, CRUD, stock movements (movements.csv), invoices + payments (invoices.csv, payments.csv),
// CSV import/export, backups.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace inv {

namespace limits {
constexpr int kMaxId = 999999;
constexpr int kMaxQty = 999999;
constexpr double kMaxPrice = 9999999.99;
}  // namespace limits

// ---------------------------------------------------------------- Part
class Part {
public:
    Part() = default;
    Part(int id, std::string name, std::string category, double price, int quantity, int minStock)
        : id_(id), name_(std::move(name)), category_(std::move(category)), price_(price), quantity_(quantity), minStock_(minStock) {}

    int id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& category() const { return category_; }
    double price() const { return price_; }
    int quantity() const { return quantity_; }
    int minStock() const { return minStock_; }
    double costPrice() const { return costPrice_; }                     // what one unit costs the shop (0 = unknown)
    const std::string& compatibleModels() const { return compatibleModels_; }
    // compatible_models split into individual vehicle models ("Bajaj RE 4S; Bajaj RE Compact" -> 2 entries).
    // Separators: comma, semicolon, pipe (a slash is NOT one, so "RE 4S/6S" stays a single model). Blanks and duplicates dropped.
    std::vector<std::string> modelList() const;
    bool fitsModel(const std::string& model) const;   // case-insensitive exact match on one listed model
    bool isLowStock() const { return quantity_ < minStock_; }
    double stockValue() const { return price_ * quantity_; }

    void setName(const std::string& v) { name_ = v; }
    void setCategory(const std::string& v) { category_ = v; }
    void setPrice(double v) { price_ = v; }
    void setQuantity(int v) { quantity_ = v; }
    void setMinStock(int v) { minStock_ = v; }
    void setCostPrice(double v) { costPrice_ = v; }
    void setCompatibleModels(const std::string& v) { compatibleModels_ = v; }

    std::string toCsv() const;
    static std::optional<Part> fromCsv(const std::vector<std::string>& fields);  // 8-col, 6-col or legacy 4-col
    // Empty string = valid, otherwise the reason.
    static std::string validate(int id, const std::string& name, const std::string& category, double price, int qty, int minStock);

private:
    int id_ = 0;
    std::string name_, category_;
    double price_ = 0.0;
    int quantity_ = 0, minStock_ = 0;
    double costPrice_ = 0.0;
    std::string compatibleModels_;
};

// Vehicle systems offered in drop-downs (data may contain others; they are listed too).
const std::vector<std::string>& standardSystems();

// Splits a free-text compatible_models value into clean model names (see Part::modelList).
std::vector<std::string> splitModels(const std::string& text);
constexpr size_t kMaxModelsText = 300;

// ---------------------------------------------------------------- Stock movements
enum class MoveType { Restock, Sale, Adjustment, Delete };
const char* moveName(MoveType t);
std::optional<MoveType> parseMove(const std::string& s);

// movements.csv columns:
//   timestamp,username,part_id,part_name,action_type,quantity_changed,resulting_stock,unit_price,ref
// quantity_changed is signed (SALE/DELETE are negative). unit_price is the selling price for a
// SALE and the unit COST for a RESTOCK (0 = unknown). ref = invoice number / note.
struct Movement {
    std::string timestamp, user;
    int partId = 0;
    std::string partName;
    MoveType type = MoveType::Adjustment;
    int qtyChanged = 0, resultingStock = 0;
    double unitPrice = 0.0;
    std::string ref;
};

class MovementLog {
public:
    explicit MovementLog(std::string file);
    bool record(Movement m);  // append to memory + file
    const std::vector<Movement>& all() const { return entries_; }
    std::vector<const Movement*> recent(size_t limit) const;
    std::string toCsv() const;  // Excel friendly (UTF-8 BOM)
    static const char* header();

private:
    std::string file_;
    std::vector<Movement> entries_;
};

// ---------------------------------------------------------------- Invoices (invoices.csv)
struct InvoiceLine {
    enum class Kind { Part, Service };
    Kind kind = Kind::Part;
    int partId = 0;             // Kind::Part
    std::string description;    // part name or service text
    int qty = 0;
    double unitPrice = 0.0;
    double total() const { return qty * unitPrice; }
};

// ---------------------------------------------------------------- Payments (credit sales / "utang")
enum class PayStatus { Paid, Partial, Unpaid };
const char* payStatusName(PayStatus s);

// Ways a customer can pay (drop-down + validation). Returns the canonical spelling or "" if unknown.
const std::vector<std::string>& paymentMethods();
std::string canonicalMethod(const std::string& s);

// invoices.csv columns (one row per line, invoice-level fields repeated on every row):
//   invoice_no,timestamp,customer,username,line_type,part_id,description,qty,unit_price,amount_paid,payment_status
// amount_paid is the running total received; payment_status (Paid/Partial/Unpaid) is derived from it and
// rewritten on every change. Older 9-column files are read as fully paid.
struct Invoice {
    std::string number, timestamp, customer, user;
    std::vector<InvoiceLine> lines;
    double amountPaid = 0.0;
    double total() const;
    double balance() const;     // what the customer still owes (never negative)
    PayStatus status() const;   // Paid (incl. zero-total), Partial, Unpaid
};

// payments.csv: one row per payment received (including the part paid at the till when the sale was made).
//   timestamp,invoice_no,username,amount,method,balance_after,note
struct Payment {
    std::string timestamp, invoiceNo, user, method, note;
    double amount = 0.0, balanceAfter = 0.0;
};

class PaymentLog {
public:
    explicit PaymentLog(std::string file);
    bool record(Payment p);   // append to memory + file
    const std::vector<Payment>& all() const { return entries_; }
    std::vector<const Payment*> forInvoice(const std::string& number) const;  // oldest first
    double paidFor(const std::string& number) const;

private:
    std::string file_;
    std::vector<Payment> entries_;
};

class InvoiceBook {
public:
    explicit InvoiceBook(std::string file);
    std::string nextNumber() const;  // INV-000001, INV-000002, ...
    bool add(const Invoice& inv);
    const Invoice* find(const std::string& number) const;
    const std::vector<Invoice>& all() const { return invoices_; }
    // Sets the amount received so far and rewrites invoices.csv atomically. On failure nothing changes.
    bool setAmountPaid(const std::string& number, double newPaid);
    const std::string& warning() const { return warning_; }

private:
    bool save();
    std::string file_, warning_;
    std::vector<Invoice> invoices_;
};

// ---------------------------------------------------------------- results
struct Result {
    bool ok = false;
    std::string message;
    explicit operator bool() const { return ok; }
    static Result success(std::string m = "") { return {true, std::move(m)}; }
    static Result fail(std::string m) { return {false, std::move(m)}; }
};

struct CheckoutResult {
    bool ok = false;
    std::string message, invoiceNumber;
    double paid = 0, balance = 0;   // what was received at the till / what is left owing
};

struct PaymentResult {
    bool ok = false;
    std::string message;
    double applied = 0, balance = 0;        // balance = still owing after this payment
    PayStatus status = PayStatus::Unpaid;
    std::string customer;
};

struct PriceChange { int partId; std::string name; double oldPrice, newPrice; };

struct ImportSummary {
    int added = 0, updated = 0, unchanged = 0, skipped = 0;
    std::vector<std::string> problems;   // one line per rejected row
    std::vector<PriceChange> priceChanges;  // for the audit trail
};

struct BackupSummary {
    int copied = 0, pruned = 0;
    std::string directory;
    std::vector<std::string> problems;
};

// ---------------------------------------------------------------- Inventory (the facade the web layer uses)
// NOT thread-safe by itself: the web layer serialises calls with a mutex.
class Inventory {
public:
    explicit Inventory(std::string dataDir = ".");

    const std::vector<Part>& parts() const { return parts_; }
    const Part* find(int id) const;
    const MovementLog& movements() const { return moves_; }
    const InvoiceBook& invoices() const { return invoices_; }
    const PaymentLog& payments() const { return payments_; }
    const std::string& dataDir() const { return dir_; }
    const std::string& lastWarning() const { return warning_; }   // e.g. "movements.csv could not be written"

    Result addPart(const Part& p, const std::string& actor);
    // models == nullopt keeps the part's compatible_models unchanged.
    Result updatePart(int id, const std::string& name, const std::string& category, double price, int minStock, const std::string& actor,
                      const std::optional<std::string>& models = std::nullopt);
    Result removePart(int id, const std::string& actor);                       // logs DELETE
    Result restock(int id, int qty, double unitCost, const std::string& actor, const std::string& ref = "");
    Result adjust(int id, long long delta, const std::string& actor, const std::string& ref = "");  // ADJUSTMENT

    // A sale = invoice. Part lines use the catalogue price (callers cannot override it).
    // amountPaid < 0 means "paid in full". Less than the total makes it a credit sale (utang): a named customer is
    // then required. Whatever is paid now is logged in payments.csv with `method`.
    CheckoutResult checkout(const std::string& customer, const std::vector<InvoiceLine>& lines, const std::string& actor,
                            double amountPaid = -1.0, const std::string& method = "Cash");

    // Logs a payment against an open invoice: updates invoices.csv (amount_paid / payment_status) and payments.csv.
    // expectedBalance (>= 0) is the balance the cashier was looking at; if it has changed meanwhile (double click,
    // another cashier) the payment is refused instead of being applied twice. Pass -1 to skip that check.
    PaymentResult recordPayment(const std::string& invoiceNo, double amount, const std::string& method, const std::string& note,
                                const std::string& actor, double expectedBalance = -1.0);

    ImportSummary importCsv(const std::string& text, const std::string& actor);

    std::string partsCsv() const;  // Excel-compatible export (UTF-8 BOM, formula-safe)
    std::string movementsCsv() const { return moves_.toCsv(); }
    std::vector<std::string> allModels() const;  // distinct vehicle models over all parts, sorted (for the filter drop-down)

    // Copies every *.csv in the data folder to backups/<name>_backup_YYYYMMDD_HHMMSS.csv.
    BackupSummary backupAll(int keepDays = 90) const { return backupFolder(dir_, keepDays); }
    // Static so it can run BEFORE the data is loaded (start-up backs up the files exactly as found).
    static BackupSummary backupFolder(const std::string& dir, int keepDays = 90);

    bool seeded() const { return seeded_; }

private:
    std::string path(const char* name) const;
    Part* mut(int id);
    bool save();
    void load();
    void reconcileLedger();  // opening-balance / external-edit entries so movements.csv matches parts.csv
    void reconcilePayments();  // every invoice's amount_paid is backed by rows in payments.csv (legacy invoices = paid in full)
    void log(const Part& p, MoveType t, int delta, double unitPrice, const std::string& actor, const std::string& ref);

    std::string dir_;
    std::vector<Part> parts_;
    MovementLog moves_;
    InvoiceBook invoices_;
    PaymentLog payments_;
    std::string warning_;
    bool seeded_ = false;
};

}  // namespace inv
