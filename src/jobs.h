// jobs.h - Phase 4: job orders / repair tickets.
//
//   jobs.csv      job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes,invoice_no
//   job_lines.csv job_no,line_no,kind,part_id,description,quantity,unit_price,labor_fee,service_id
//   services.csv  id,name,default_labor_fee,interval_months,interval_km,active
//
// The two extra columns beyond the Phase 4 spec are appended at the end (the loader accepts files
// without them, like invoices.csv does):
//   * jobs.csv      `invoice_no` - Phase 5 writes it when a job is released & billed (Job <-> Invoice).
//   * job_lines.csv `service_id` - Phase 6 needs it to raise a maintenance reminder from a labour line.
//
// Stock rule (the heart of this phase):
//   adding a Part line issues stock immediately (movements.csv, JOB_ISSUE, ref = job_no);
//   removing that line, or cancelling the job, puts it back (JOB_RETURN, same ref).
//   Billing a job (Phase 5) never touches stock again.
//
// Threading: every public method locks the book's mutex, so the job store is safe to call from several
// threads. The web layer additionally holds its own request mutex around every request; locks are always
// taken in that order (request -> job book) and never the other way round, so they cannot deadlock.
#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "inventory.h"
#include "util.h"

namespace crm { class Registry; }
namespace auth { class AuditLog; }

namespace jobs {

// ---------------------------------------------------------------- service catalogue (services.csv)
struct ServiceItem {
    int id = 0;
    std::string name;
    double defaultLaborFee = 0.0;
    int intervalMonths = 0;   // 0 = no time-based reminder (Phase 6)
    int intervalKm = 0;       // 0 = no distance-based reminder (Phase 6)
    bool active = true;

    bool scheduled() const { return intervalMonths > 0 || intervalKm > 0; }
};

// The catalogue is edited by an Admin (labour fees + reminder intervals) and is what pre-fills a
// labour line on a job order. Header-only-friendly, same shape as sup::SupplierBook.
class ServiceCatalog {
public:
    void open(const std::string& dataDir);
    // Adds (id == 0) or updates a service. Returns "" on success, otherwise the reason. newId gets the id used.
    std::string save(int id, ServiceItem in, int& newId);
    std::string setActive(int id, bool on);

    const std::vector<ServiceItem>& all() const { return items_; }
    std::vector<ServiceItem> active() const;
    const ServiceItem* find(int id) const;
    const std::string& warning() const { return warning_; }

    std::string toCsv() const;   // export (Phase 7)

private:
    bool write();
    void seedDefaults();
    std::string file_;
    std::vector<ServiceItem> items_;
    std::string warning_;
};

// ---------------------------------------------------------------- status workflow
//   Open -> In progress -> Done -> Released   (plus Cancelled, which is terminal)
//   In progress may go back to Open; Done may be reopened to In progress. Released and Cancelled are final.
enum class JobStatus { Open, InProgress, Done, Released, Cancelled };

const char* statusName(JobStatus s);
std::optional<JobStatus> parseStatus(const std::string& s);
std::vector<JobStatus> allowedNext(JobStatus from);
bool canTransition(JobStatus from, JobStatus to);
// Lines and job details can still be changed while the job is Open or In progress. Done is a hand-off
// state (reopen it to change something), Released and Cancelled are locked for good.
bool isEditable(JobStatus s);
bool isOpen(JobStatus s);   // still on the floor: Open or In progress

// ---------------------------------------------------------------- lines (job_lines.csv)
struct JobLine {
    enum class Kind { Part, Labor };
    static const char* kindName(Kind k);
    static std::optional<Kind> parseKind(const std::string& s);

    int lineNo = 0;
    Kind kind = Kind::Part;
    int partId = 0;             // Kind::Part only
    int serviceId = 0;          // Kind::Labor: which catalogue service it came from (0 = one-off)
    std::string description;
    int quantity = 1;           // parts: units issued; labour: always 1
    double unitPrice = 0.0;     // parts: selling price per unit (copied from the catalogue when added)
    double laborFee = 0.0;      // labour: the fee for the job (a part line may carry a small fitting fee)

    double partsAmount() const { return kind == Kind::Part ? util::round2(quantity * unitPrice) : 0.0; }
    double laborAmount() const { return util::round2(laborFee); }
    double lineTotal() const { return util::round2(partsAmount() + laborAmount()); }
};

struct JobTotals {
    double parts = 0.0;
    double labor = 0.0;
    double grand() const { return util::round2(parts + labor); }
};

JobTotals totalsOf(const std::vector<JobLine>& lines);

// ---------------------------------------------------------------- job (jobs.csv)
struct Job {
    std::string jobNo;        // "JOB-000001"
    std::string opened;       // "YYYY-MM-DD HH:MM:SS"
    int customerId = 0;       // 0 = walk-in (no customer record)
    int vehicleId = 0;        // 0 = no vehicle recorded
    int odometer = 0;         // km reading as the vehicle came in (0 = unknown)
    std::string complaint;    // what the customer reported
    std::string mechanic;
    JobStatus status = JobStatus::Open;
    std::string closed;       // timestamp of Released / Cancelled (blank while open)
    std::string notes;
    std::string invoiceNo;    // Phase 5: the invoice this job was billed on (blank until then)

    std::vector<JobLine> lines;   // not a column: read from job_lines.csv

    bool isLocked() const { return !isEditable(status); }
    std::vector<JobStatus> next() const { return allowedNext(status); }
};

JobTotals totalsOfJob(const Job& j);

// ---------------------------------------------------------------- request/result types
struct Result {
    bool ok = false;
    std::string message;
    explicit operator bool() const { return ok; }
    static Result success(std::string m = "") { return {true, std::move(m)}; }
    static Result fail(std::string m) { return {false, std::move(m)}; }
};

// A job snapshot: returned by create/update/find so the caller never holds a pointer into the store
// while another request is mutating it.
struct JobResult {
    bool ok = false;
    std::string message;
    Job job;
    static JobResult success(Job j, std::string m = "") { return {true, std::move(m), std::move(j)}; }
    static JobResult fail(std::string m) { return {false, std::move(m), Job{}}; }
};

struct LineResult {
    bool ok = false;
    std::string message;
    JobLine line;
    std::vector<JobLine> lines;   // the job's lines after the change
    JobTotals totals;
    static LineResult fail(std::string m) { return {false, std::move(m), JobLine{}, {}, {}}; }
};

struct LinesResult {
    bool ok = false;
    std::string message;
    std::vector<JobLine> lines;
    JobTotals totals;
};

// What the UI needs to render / print a job card.
struct JobCard {
    bool ok = false;
    std::string message;
    Job job;
    std::vector<JobLine> lines;
    JobTotals totals;
    std::string customerName, customerPhone, vehiclePlate, vehicleModel;
    std::vector<JobStatus> allowedNext;
    std::vector<std::string> allowedNextNames;
};

// One row of a job's audit trail.
struct JobEvent {
    std::string timestamp, user, event, details;
};

struct CreateJob {
    int customerId = 0;
    int vehicleId = 0;
    int odometer = 0;
    std::string complaint;
    std::string mechanic;
    std::string notes;
};

struct JobEdit {
    std::optional<int> odometer;
    std::optional<std::string> complaint;
    std::optional<std::string> notes;
};

struct AddLine {
    JobLine::Kind kind = JobLine::Kind::Part;
    int partId = 0;
    int serviceId = 0;
    std::string description;
    int quantity = 1;
    double laborFee = -1.0;   // < 0 = take the default (catalogue fee for labour, 0 for parts)
    double unitPrice = -1.0;  // parts: ignored, the catalogue price is always used
};

struct JobFilter {
    std::optional<JobStatus> status;
    std::string mechanic;    // exact match, ignoring case; "" = anybody
    std::string date;        // opened on this day, "YYYY-MM-DD"
    std::string from, to;    // inclusive date range on `opened` (alternative to `date`)
    std::string plate;       // any spelling, substring match ("abc" finds "ABC1234")
    std::string query;       // job no / complaint / customer name / plate - free text
    int customerId = 0, vehicleId = 0;
    size_t limit = 200;      // 0 = no limit
    bool newestFirst = true;
};

// ---------------------------------------------------------------- the job book
class JobBook {
public:
    JobBook() = default;
    JobBook(const JobBook&) = delete;
    JobBook& operator=(const JobBook&) = delete;

    // Loads jobs.csv / job_lines.csv / services.csv from `dataDir` and binds the stores the book works
    // with: `inv` holds the parts + movements.csv (stock), `audit` gets one row per state change
    // (may be null - the unit tests pass null), `people` is used to resolve plates and names (may be null).
    void open(const std::string& dataDir, inv::Inventory& inv, auth::AuditLog* audit, const crm::Registry* people = nullptr);

    const std::vector<Job>& all() const;
    // Snapshot of one job (copy - safe to keep across requests).
    std::optional<Job> get(const std::string& jobNo) const;
    const Job* find(const std::string& jobNo) const;   // pointer valid until the next change (use under a lock)
    JobCard card(const std::string& jobNo) const;
    LinesResult lines(const std::string& jobNo) const;
    std::vector<JobEvent> history(const std::string& jobNo) const;
    std::vector<Job> list(const JobFilter& f = {}) const;   // copies, filtered and sorted
    std::string nextJobNo() const;
    // Units of `partId` currently issued to `jobNo` (JOB_ISSUE minus JOB_RETURN in movements.csv).
    // The same number the job's Part lines say - reconcileStockLocked() enforces it at start-up.
    int issuedFor(const std::string& jobNo, int partId) const;

    const ServiceCatalog& services() const { return services_; }
    const std::string& warning() const { return warning_; }
    std::mutex& mutex() const { return mtx_; }

    // ---- lifecycle
    JobResult createJob(const CreateJob& in, const std::string& actor);
    JobResult updateJob(const std::string& jobNo, const JobEdit& in, const std::string& actor);
    Result assignMechanic(const std::string& jobNo, const std::string& mechanic, const std::string& actor);
    // Rejects anything the state machine does not allow. Cancelling restores every part line to stock.
    Result changeStatus(const std::string& jobNo, JobStatus to, const std::string& actor, const std::string& note = "");
    Result cancelJob(const std::string& jobNo, const std::string& reason, const std::string& actor);

    // ---- lines (Part lines move stock)
    LineResult addLine(const std::string& jobNo, const AddLine& in, const std::string& actor);
    LineResult removeLine(const std::string& jobNo, int lineNo, const std::string& actor);

    // ---- service catalogue (Admin)
    std::string saveService(int id, ServiceItem in, int& newId, const std::string& actor);
    std::string setServiceActive(int id, bool on, const std::string& actor);

    // ---- export helpers (Phase 7 reuses these)
    std::string jobsCsv() const;
    std::string jobLinesCsv() const;
    std::string servicesCsv() const { return services_.toCsv(); }

    // Jobs waiting on the floor, oldest first - handy for the dashboard and for tests.
    std::vector<Job> openJobs() const;

private:
    // Every method below assumes mtx_ is already held.
    Job* mut(const std::string& jobNo);
    bool saveJobsLocked();
    bool saveLinesLocked();
    void loadLocked();
    void loadLinesLocked();
    void auditLocked(const std::string& user, const std::string& event, const std::string& details);
    Result changeStatusLocked(const std::string& jobNo, JobStatus to, const std::string& actor, const std::string& note);
    // Crash recovery: compares what each job should have issued (its part lines) with the JOB_ISSUE /
    // JOB_RETURN rows in movements.csv and applies the difference. A half-finished write (line saved,
    // stock not yet deducted, power cut) is healed on the next start instead of leaking stock.
    void reconcileStockLocked();
    int issuedForLocked(const std::string& jobNo, int partId) const;
    std::string nextJobNoLocked() const;

    mutable std::mutex mtx_;
    std::string dir_, jobsFile_, linesFile_;
    inv::Inventory* inv_ = nullptr;
    auth::AuditLog* audit_ = nullptr;
    const crm::Registry* people_ = nullptr;
    std::vector<Job> jobs_;
    ServiceCatalog services_;
    std::string warning_;
};

}  // namespace jobs
