// inventory.h - parts catalogue, CRUD, stock movements (movements.csv), invoices, CSV import/export, backups.
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
    bool isLowStock() const { return quantity_ < minStock_; }
    double stockValue() const { return price_ * quantity_; }

    void setName(const std::string& v) { name_ = v; }
    void setCategory(const std::string& v) { category_ = v; }
    void setPrice(double v) { price_ = v; }
    void setQuantity(int v) { quantity_ = v; }
    void setMinStock(int v) { minStock_ = v; }

    std::string toCsv() const;
    static std::optional<Part> fromCsv(const std::vector<std::string>& fields);  // 6-col or legacy 4-col
    // Empty string = valid, otherwise the reason.
    static std::string validate(int id, const std::string& name, const std::string& category, double price, int qty, int minStock);

private:
    int id_ = 0;
    std::string name_, category_;
    double price_ = 0.0;
    int quantity_ = 0, minStock_ = 0;
};

// Vehicle systems offered in drop-downs (data may contain others; they are listed too).
const std::vector<std::string>& standardSystems();

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

struct Invoice {
    std::string number, timestamp, customer, user;
    std::vector<InvoiceLine> lines;
    double total() const;
};

class InvoiceBook {
public:
    explicit InvoiceBook(std::string file);
    std::string nextNumber() const;  // INV-000001, INV-000002, ...
    bool add(const Invoice& inv);
    const Invoice* find(const std::string& number) const;
    const std::vector<Invoice>& all() const { return invoices_; }

private:
    std::string file_;
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
    const std::string& dataDir() const { return dir_; }
    const std::string& lastWarning() const { return warning_; }   // e.g. "movements.csv could not be written"

    Result addPart(const Part& p, const std::string& actor);
    Result updatePart(int id, const std::string& name, const std::string& category, double price, int minStock, const std::string& actor);
    Result removePart(int id, const std::string& actor);                       // logs DELETE
    Result restock(int id, int qty, double unitCost, const std::string& actor, const std::string& ref = "");
    Result adjust(int id, long long delta, const std::string& actor, const std::string& ref = "");  // ADJUSTMENT

    // A sale = invoice. Part lines use the catalogue price (callers cannot override it).
    CheckoutResult checkout(const std::string& customer, const std::vector<InvoiceLine>& lines, const std::string& actor);

    ImportSummary importCsv(const std::string& text, const std::string& actor);

    std::string partsCsv() const;  // Excel-compatible export (UTF-8 BOM, formula-safe)
    std::string movementsCsv() const { return moves_.toCsv(); }

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
    void log(const Part& p, MoveType t, int delta, double unitPrice, const std::string& actor, const std::string& ref);

    std::string dir_;
    std::vector<Part> parts_;
    MovementLog moves_;
    InvoiceBook invoices_;
    std::string warning_;
    bool seeded_ = false;
};

}  // namespace inv
