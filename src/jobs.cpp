// jobs.cpp - Phase 4: job orders / repair tickets (see jobs.h for the file layouts and the rules).
#include "jobs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

#include "auth.h"
#include "crm.h"
#include "util.h"

namespace fs = std::filesystem;

namespace jobs {

namespace {
const char* kJobsHeader = "job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes,invoice_no";
const char* kLinesHeader = "job_no,line_no,kind,part_id,description,quantity,unit_price,labor_fee,service_id";
const char* kServicesHeader = "id,name,default_labor_fee,interval_months,interval_km,active";
const char* kBom = "\xEF\xBB\xBF";

constexpr int kMaxOdometer = 9999999;
constexpr int kMaxComplaint = 500;
constexpr int kMaxMechanic = 60;
constexpr int kMaxNotes = 500;
constexpr int kMaxDesc = 100;
constexpr int kMaxIntervalMonths = 600;
constexpr int kMaxIntervalKm = 1000000;

std::string datePart(const std::string& stamp) { return stamp.size() >= 10 ? stamp.substr(0, 10) : stamp; }

// "ABC 1234" / "abc-1234" / "ABC1234" -> "ABC1234": letters and digits only, so a plate typed with or
// without a hyphen still finds the vehicle (stored plates keep the hyphen).
std::string loosePlate(const std::string& in) {
    std::string o;
    for (unsigned char ch : in)
        if (std::isalnum(ch)) o += static_cast<char>(std::toupper(ch));
    return o;
}

// "JOB-000007" -> 7  (0 when it does not look like one of ours)
long long jobSeq(const std::string& jobNo) {
    long long n = 0;
    if (jobNo.rfind("JOB-", 0) == 0 && util::parseInt(jobNo.substr(4), 0, 999999999LL, n)) return n;
    return 0;
}
}  // namespace

// ================================================================ ServiceCatalog
void ServiceCatalog::open(const std::string& dataDir) {
    file_ = (fs::path(dataDir) / "services.csv").string();
    items_.clear();
    warning_.clear();
    int skipped = 0;
    for (const auto& f : util::readCsvFile(file_)) {
        long long id = 0;
        if (f.empty()) continue;
        if (!util::parseInt(f[0], 1, 999999, id)) {   // header row or junk
            if (!util::equalsIgnoreCase(util::trim(f[0]), "id")) ++skipped;
            continue;
        }
        ServiceItem s;
        s.id = static_cast<int>(id);
        if (f.size() > 1) s.name = util::oneLine(util::trim(f[1]));
        if (f.size() > 2) util::parseDouble(f[2], 0, inv::limits::kMaxPrice, s.defaultLaborFee);
        if (f.size() > 3) {
            long long m = 0;
            if (util::parseInt(f[3], 0, kMaxIntervalMonths, m)) s.intervalMonths = static_cast<int>(m);
        }
        if (f.size() > 4) {
            long long km = 0;
            if (util::parseInt(f[4], 0, kMaxIntervalKm, km)) s.intervalKm = static_cast<int>(km);
        }
        if (f.size() > 5) s.active = util::trim(f[5]) != "0";
        if (s.name.empty() || find(s.id)) { ++skipped; continue; }
        s.defaultLaborFee = util::round2(s.defaultLaborFee);
        items_.push_back(s);
    }
    std::error_code ec;
    const bool fresh = !fs::exists(file_, ec);
    if (fresh) {
        seedDefaults();   // Change Oil / Brake Pad Replacement / Engine Overhaul - the Admin edits them
        return;
    }
    if (skipped > 0) {
        fs::copy_file(file_, file_ + ".bak", fs::copy_options::overwrite_existing, ec);
        warning_ = std::to_string(skipped) + " malformed row(s) in services.csv were skipped (original kept as services.csv.bak).";
    }
}

void ServiceCatalog::seedDefaults() {
    items_ = {
        {1, "Change Oil", 300.00, 3, 3000, true},
        {2, "Brake Pad Replacement", 500.00, 12, 10000, true},
        {3, "Engine Overhaul", 5000.00, 36, 40000, true},
    };
    if (!write()) warning_ = "Could not write services.csv - check disk space/permissions.";
}

const ServiceItem* ServiceCatalog::find(int id) const {
    for (const ServiceItem& s : items_)
        if (s.id == id) return &s;
    return nullptr;
}

std::vector<ServiceItem> ServiceCatalog::active() const {
    std::vector<ServiceItem> out;
    for (const ServiceItem& s : items_)
        if (s.active) out.push_back(s);
    return out;
}

std::string ServiceCatalog::save(int id, ServiceItem in, int& newId) {
    in.name = util::oneLine(util::trim(in.name));
    if (in.name.empty() || in.name.size() > 60) return "Service name is required (max 60 characters).";
    if (!(in.defaultLaborFee >= 0.0 && in.defaultLaborFee <= inv::limits::kMaxPrice)) return "The labour fee is out of range.";
    if (in.intervalMonths < 0 || in.intervalMonths > kMaxIntervalMonths) return "The interval in months is out of range (0 = none).";
    if (in.intervalKm < 0 || in.intervalKm > kMaxIntervalKm) return "The interval in km is out of range (0 = none).";
    for (const ServiceItem& s : items_)
        if (s.id != id && util::equalsIgnoreCase(s.name, in.name)) return "A service with that name already exists.";
    const std::vector<ServiceItem> before = items_;
    if (id == 0) {
        int m = 0;
        for (const ServiceItem& s : items_) m = std::max(m, s.id);
        in.id = m + 1;
        in.active = true;
        items_.push_back(in);
    } else {
        ServiceItem* cur = const_cast<ServiceItem*>(find(id));
        if (!cur) return "Service not found.";
        in.id = id;
        in.active = cur->active;   // toggled separately
        *cur = in;
    }
    newId = in.id;
    if (!write()) {
        items_ = before;
        return "Could not write services.csv - check disk space/permissions.";
    }
    return "";
}

std::string ServiceCatalog::setActive(int id, bool on) {
    ServiceItem* s = const_cast<ServiceItem*>(find(id));
    if (!s) return "Service not found.";
    const bool old = s->active;
    s->active = on;
    if (!write()) {
        s->active = old;
        return "Could not write services.csv - check disk space/permissions.";
    }
    return "";
}

bool ServiceCatalog::write() {
    std::string out = std::string(kServicesHeader) + "\n";
    for (const ServiceItem& s : items_)
        out += std::to_string(s.id) + ',' + util::csvEscape(s.name) + ',' + util::money(s.defaultLaborFee) + ',' +
               std::to_string(s.intervalMonths) + ',' + std::to_string(s.intervalKm) + ',' + (s.active ? "1" : "0") + "\n";
    return util::writeFileAtomic(file_, out);
}

std::string ServiceCatalog::toCsv() const {
    std::string out = std::string(kBom) + kServicesHeader + "\r\n";
    for (const ServiceItem& s : items_)
        out += std::to_string(s.id) + ',' + util::csvSafe(s.name) + ',' + util::money(s.defaultLaborFee) + ',' +
               std::to_string(s.intervalMonths) + ',' + std::to_string(s.intervalKm) + ',' + (s.active ? "1" : "0") + "\r\n";
    return out;
}

// ================================================================ status workflow
const char* statusName(JobStatus s) {
    switch (s) {
        case JobStatus::Open: return "Open";
        case JobStatus::InProgress: return "In progress";
        case JobStatus::Done: return "Done";
        case JobStatus::Released: return "Released";
        case JobStatus::Cancelled: return "Cancelled";
    }
    return "Open";
}

std::optional<JobStatus> parseStatus(const std::string& sIn) {
    const std::string s = util::trim(sIn);
    const std::string k = util::toLower(s);
    if (k == "open") return JobStatus::Open;
    if (k == "in progress" || k == "inprogress" || k == "in_progress" || k == "in-progress") return JobStatus::InProgress;
    if (k == "done" || k == "finished") return JobStatus::Done;
    if (k == "released" || k == "released (billed)" || k == "billed") return JobStatus::Released;
    if (k == "cancelled" || k == "canceled") return JobStatus::Cancelled;
    if (s == "In progress") return JobStatus::InProgress;
    return std::nullopt;
}

std::vector<JobStatus> allowedNext(JobStatus from) {
    switch (from) {
        case JobStatus::Open: return {JobStatus::InProgress, JobStatus::Cancelled};
        case JobStatus::InProgress: return {JobStatus::Done, JobStatus::Open, JobStatus::Cancelled};
        case JobStatus::Done: return {JobStatus::Released, JobStatus::InProgress};
        case JobStatus::Released: return {};   // billed & handed over: final
        case JobStatus::Cancelled: return {};  // final
    }
    return {};
}

bool canTransition(JobStatus from, JobStatus to) {
    for (JobStatus s : allowedNext(from))
        if (s == to) return true;
    return false;
}

bool isEditable(JobStatus s) { return s == JobStatus::Open || s == JobStatus::InProgress; }
bool isOpen(JobStatus s) { return s == JobStatus::Open || s == JobStatus::InProgress; }

// ================================================================ lines
const char* JobLine::kindName(Kind k) { return k == Kind::Part ? "Part" : "Labor"; }

std::optional<JobLine::Kind> JobLine::parseKind(const std::string& sIn) {
    const std::string k = util::toLower(util::trim(sIn));
    if (k == "part" || k == "parts" || k == "p") return Kind::Part;
    if (k == "labor" || k == "labour" || k == "service" || k == "l") return Kind::Labor;
    return std::nullopt;
}

JobTotals totalsOf(const std::vector<JobLine>& lines) {
    JobTotals t;
    for (const JobLine& l : lines) {
        t.parts = util::round2(t.parts + l.partsAmount());
        t.labor = util::round2(t.labor + l.laborAmount());
    }
    return t;
}

JobTotals totalsOfJob(const Job& j) { return totalsOf(j.lines); }

// ================================================================ JobBook
void JobBook::open(const std::string& dataDir, inv::Inventory& inv, auth::AuditLog* audit, const crm::Registry* people) {
    std::lock_guard<std::mutex> lock(mtx_);
    dir_ = dataDir;
    jobsFile_ = (fs::path(dataDir) / "jobs.csv").string();
    linesFile_ = (fs::path(dataDir) / "job_lines.csv").string();
    inv_ = &inv;
    audit_ = audit;
    people_ = people;
    jobs_.clear();
    warning_.clear();
    services_.open(dataDir);
    loadLocked();
    loadLinesLocked();
    reconcileStockLocked();
}

// ---------------------------------------------------------------- persistence
void JobBook::loadLocked() {
    int skipped = 0;
    bool upgraded = false;
    std::set<std::string> seen;
    for (const auto& f : util::readCsvFile(jobsFile_)) {
        if (f.empty()) continue;
        if (util::equalsIgnoreCase(util::trim(f[0]), "job_no")) continue;   // header
        Job j;
        j.jobNo = util::trim(f[0]);
        if (j.jobNo.empty() || !seen.insert(j.jobNo).second) { ++skipped; continue; }
        if (f.size() > 1) j.opened = util::trim(f[1]);
        if (f.size() > 2) {
            long long v = 0;
            if (util::parseInt(f[2], 0, 999999, v)) j.customerId = static_cast<int>(v);
        }
        if (f.size() > 3) {
            long long v = 0;
            if (util::parseInt(f[3], 0, 999999, v)) j.vehicleId = static_cast<int>(v);
        }
        if (f.size() > 4) {
            long long v = 0;
            if (util::parseInt(f[4], 0, kMaxOdometer, v)) j.odometer = static_cast<int>(v);
        }
        if (f.size() > 5) j.complaint = util::oneLine(f[5]);
        if (f.size() > 6) j.mechanic = util::oneLine(util::trim(f[6]));
        if (f.size() > 7) {
            if (auto s = parseStatus(f[7])) j.status = *s;
            else {
                ++skipped;
                seen.erase(j.jobNo);
                continue;
            }
        }
        if (f.size() > 8) j.closed = util::trim(f[8]);
        if (f.size() > 9) j.notes = util::oneLine(f[9]);
        if (f.size() > 10) j.invoiceNo = util::trim(f[10]);
        else upgraded = true;
        if (j.opened.empty()) j.opened = util::nowStamp();
        jobs_.push_back(std::move(j));
    }
    std::error_code ec;
    if (skipped > 0) {
        fs::copy_file(jobsFile_, jobsFile_ + ".bak", fs::copy_options::overwrite_existing, ec);
        warning_ = std::to_string(skipped) + " malformed row(s) in jobs.csv were skipped (original kept as jobs.csv.bak).";
    }
    if (upgraded && !jobs_.empty()) saveJobsLocked();   // add the Phase 5 invoice_no column (backup kept the original)
}

void JobBook::loadLinesLocked() {
    int skipped = 0;
    for (const auto& f : util::readCsvFile(linesFile_)) {
        if (f.empty()) continue;
        if (util::equalsIgnoreCase(util::trim(f[0]), "job_no")) continue;   // header
        if (f.size() < 6) { ++skipped; continue; }
        long long lineNo = 0, partId = 0, qty = 0;
        const auto kind = JobLine::parseKind(f[2]);
        if (!kind || !util::parseInt(f[1], 1, 9999, lineNo) || !util::parseInt(f[5], 1, inv::limits::kMaxQty, qty)) { ++skipped; continue; }
        JobLine l;
        l.lineNo = static_cast<int>(lineNo);
        l.kind = *kind;
        util::parseInt(f[3], 0, inv::limits::kMaxId, partId);
        l.partId = static_cast<int>(partId);
        l.description = util::oneLine(f[4]);
        l.quantity = static_cast<int>(qty);
        util::parseDouble(f[6], 0, inv::limits::kMaxPrice, l.unitPrice);
        if (f.size() > 7) util::parseDouble(f[7], 0, inv::limits::kMaxPrice, l.laborFee);
        if (f.size() > 8) {
            long long sid = 0;
            if (util::parseInt(f[8], 0, 999999, sid)) l.serviceId = static_cast<int>(sid);
        }
        Job* j = mut(f[0]);
        if (!j) { ++skipped; continue; }   // line for a job that no longer exists
        j->lines.push_back(l);
    }
    std::error_code ec;
    if (skipped > 0) {
        fs::copy_file(linesFile_, linesFile_ + ".bak", fs::copy_options::overwrite_existing, ec);
        warning_ += (warning_.empty() ? "" : " ") + std::to_string(skipped) +
                    " malformed row(s) in job_lines.csv were skipped (original kept as job_lines.csv.bak).";
    }
    for (Job& j : jobs_)
        std::sort(j.lines.begin(), j.lines.end(), [](const JobLine& a, const JobLine& b) { return a.lineNo < b.lineNo; });
}

bool JobBook::saveJobsLocked() {
    std::string out = std::string(kJobsHeader) + "\n";
    for (const Job& j : jobs_)
        out += util::csvEscape(j.jobNo) + ',' + util::csvEscape(j.opened) + ',' + (j.customerId > 0 ? std::to_string(j.customerId) : "") + ',' +
               (j.vehicleId > 0 ? std::to_string(j.vehicleId) : "") + ',' + std::to_string(j.odometer) + ',' + util::csvEscape(j.complaint) + ',' +
               util::csvEscape(j.mechanic) + ',' + util::csvEscape(statusName(j.status)) + ',' + util::csvEscape(j.closed) + ',' +
               util::csvEscape(j.notes) + ',' + util::csvEscape(j.invoiceNo) + "\n";
    if (!util::writeFileAtomic(jobsFile_, out)) {
        warning_ = "Could not write jobs.csv - check disk space/permissions.";
        return false;
    }
    return true;
}

bool JobBook::saveLinesLocked() {
    std::string out = std::string(kLinesHeader) + "\n";
    for (const Job& j : jobs_)
        for (const JobLine& l : j.lines)
            out += util::csvEscape(j.jobNo) + ',' + std::to_string(l.lineNo) + ',' + JobLine::kindName(l.kind) + ',' +
                   (l.partId > 0 ? std::to_string(l.partId) : "0") + ',' + util::csvEscape(l.description) + ',' + std::to_string(l.quantity) + ',' +
                   util::money(l.unitPrice) + ',' + util::money(l.laborFee) + ',' + (l.serviceId > 0 ? std::to_string(l.serviceId) : "") + "\n";
    if (!util::writeFileAtomic(linesFile_, out)) {
        warning_ = "Could not write job_lines.csv - check disk space/permissions.";
        return false;
    }
    return true;
}

void JobBook::auditLocked(const std::string& user, const std::string& event, const std::string& details) {
    if (audit_) audit_->record(user, event, util::oneLine(details), "");
}

Job* JobBook::mut(const std::string& jobNo) {
    for (Job& j : jobs_)
        if (j.jobNo == jobNo) return &j;
    return nullptr;
}

const Job* JobBook::find(const std::string& jobNo) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const Job& j : jobs_)
        if (j.jobNo == jobNo) return &j;
    return nullptr;
}

std::optional<Job> JobBook::get(const std::string& jobNo) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const Job& j : jobs_)
        if (j.jobNo == jobNo) return j;
    return std::nullopt;
}

const std::vector<Job>& JobBook::all() const { return jobs_; }

std::string JobBook::nextJobNoLocked() const {
    long long maxN = 0;
    for (const Job& j : jobs_) maxN = std::max(maxN, jobSeq(j.jobNo));
    char buf[32];
    std::snprintf(buf, sizeof buf, "JOB-%06lld", maxN + 1);
    return buf;
}

std::string JobBook::nextJobNo() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return nextJobNoLocked();
}

// ---------------------------------------------------------------- crash recovery
int JobBook::issuedForLocked(const std::string& jobNo, int partId) const {
    int net = 0;
    if (!inv_) return 0;
    for (const inv::Movement& m : inv_->movements().all()) {
        if (m.ref != jobNo || m.partId != partId) continue;
        if (m.type == inv::MoveType::JobIssue) net += -m.qtyChanged;
        else if (m.type == inv::MoveType::JobReturn) net -= m.qtyChanged;
    }
    return net;
}

void JobBook::reconcileStockLocked() {
    if (!inv_ || jobs_.empty()) return;
    std::map<std::string, std::map<int, int>> issued;   // job_no -> part_id -> units currently spoken for
    for (const inv::Movement& m : inv_->movements().all()) {
        if (m.type == inv::MoveType::JobIssue) issued[m.ref][m.partId] += -m.qtyChanged;
        else if (m.type == inv::MoveType::JobReturn) issued[m.ref][m.partId] -= m.qtyChanged;
    }
    int fixed = 0;
    for (const Job& j : jobs_) {
        std::map<int, int> expected;   // a cancelled job must have everything back on the shelf
        if (j.status != JobStatus::Cancelled)
            for (const JobLine& l : j.lines)
                if (l.kind == JobLine::Kind::Part) expected[l.partId] += l.quantity;
        const std::map<int, int>& got = issued[j.jobNo];
        std::vector<inv::Inventory::StockDelta> issue, giveBack;
        std::set<int> ids;
        for (const auto& e : expected) ids.insert(e.first);
        for (const auto& g : got) ids.insert(g.first);
        for (int id : ids) {
            const int diff = (got.count(id) ? got.at(id) : 0) - (expected.count(id) ? expected.at(id) : 0);
            if (diff == 0) continue;
            // got > expected: too much was taken -> put it back. got < expected: the issue was lost -> take it now.
            if (diff > 0) giveBack.push_back({id, diff});
            else issue.push_back({id, diff});
        }
        if (!issue.empty() && inv_->applyStock(issue, inv::MoveType::JobIssue, "system", j.jobNo).ok) {
            fixed += static_cast<int>(issue.size());
            auditLocked("system", "JOB_STOCK_RECONCILED", j.jobNo + ": " + std::to_string(issue.size()) + " part line(s) issued during recovery");
        }
        if (!giveBack.empty() && inv_->applyStock(giveBack, inv::MoveType::JobReturn, "system", j.jobNo).ok) {
            fixed += static_cast<int>(giveBack.size());
            auditLocked("system", "JOB_STOCK_RECONCILED", j.jobNo + ": " + std::to_string(giveBack.size()) + " part line(s) returned during recovery");
        }
    }
    if (fixed > 0)
        warning_ += (warning_.empty() ? "" : " ") + std::to_string(fixed) +
                    " job part line(s) did not match movements.csv (a write was interrupted?) and have been reconciled.";
}

// ---------------------------------------------------------------- lifecycle
JobResult JobBook::createJob(const CreateJob& in, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mtx_);
    Job j;
    j.jobNo = nextJobNoLocked();
    j.opened = util::nowStamp();
    j.customerId = in.customerId;
    j.vehicleId = in.vehicleId;
    j.odometer = in.odometer;
    j.complaint = util::oneLine(util::trim(in.complaint));
    j.mechanic = util::oneLine(util::trim(in.mechanic));
    j.notes = util::oneLine(util::trim(in.notes));
    j.status = JobStatus::Open;

    if (j.odometer < 0 || j.odometer > kMaxOdometer) return JobResult::fail("Odometer must be 0 to " + std::to_string(kMaxOdometer) + " km.");
    if (j.complaint.size() > kMaxComplaint) return JobResult::fail("The complaint is too long (max " + std::to_string(kMaxComplaint) + " characters).");
    if (j.mechanic.size() > kMaxMechanic) return JobResult::fail("The mechanic name is too long (max " + std::to_string(kMaxMechanic) + " characters).");
    if (j.notes.size() > kMaxNotes) return JobResult::fail("The notes are too long (max " + std::to_string(kMaxNotes) + " characters).");
    if (people_) {
        if (j.customerId > 0) {
            const crm::Customer* k = people_->findCustomer(j.customerId);
            if (!k) return JobResult::fail("Customer " + std::to_string(j.customerId) + " does not exist.");
            if (k->archived) return JobResult::fail("Customer \"" + k->name + "\" is archived - restore it first.");
        }
        if (j.vehicleId > 0) {
            const crm::Vehicle* v = people_->findVehicle(j.vehicleId);
            if (!v) return JobResult::fail("Vehicle " + std::to_string(j.vehicleId) + " does not exist.");
            if (v->archived) return JobResult::fail("Vehicle " + v->plate + " is archived - restore it first.");
            if (j.customerId == 0) j.customerId = v->customerId;                       // plate typed first
            else if (v->customerId != j.customerId)
                return JobResult::fail("That vehicle belongs to another customer.");
        }
    }

    jobs_.push_back(j);
    if (!saveJobsLocked()) {
        jobs_.pop_back();
        return JobResult::fail(warning_);
    }
    auditLocked(actor, "JOB_CREATED", j.jobNo + ": " + (j.complaint.empty() ? "no complaint given" : j.complaint));
    return JobResult::success(j, "Job " + j.jobNo + " opened.");
}

JobResult JobBook::updateJob(const std::string& jobNo, const JobEdit& in, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mtx_);
    Job* j = mut(jobNo);
    if (!j) return JobResult::fail("Job " + jobNo + " does not exist.");
    if (!isEditable(j->status))
        return JobResult::fail(j->jobNo + " is " + statusName(j->status) + " - it can no longer be edited.");
    const Job before = *j;
    if (in.odometer) {
        if (*in.odometer < 0 || *in.odometer > kMaxOdometer) return JobResult::fail("Odometer must be 0 to " + std::to_string(kMaxOdometer) + " km.");
        j->odometer = *in.odometer;
    }
    if (in.complaint) {
        j->complaint = util::oneLine(util::trim(*in.complaint));
        if (j->complaint.size() > kMaxComplaint) {
            *j = before;
            return JobResult::fail("The complaint is too long (max " + std::to_string(kMaxComplaint) + " characters).");
        }
    }
    if (in.notes) {
        j->notes = util::oneLine(util::trim(*in.notes));
        if (j->notes.size() > kMaxNotes) {
            *j = before;
            return JobResult::fail("The notes are too long (max " + std::to_string(kMaxNotes) + " characters).");
        }
    }
    if (j->odometer == before.odometer && j->complaint == before.complaint && j->notes == before.notes)
        return JobResult::success(*j, "Nothing to change.");
    if (!saveJobsLocked()) {
        *j = before;
        return JobResult::fail(warning_);
    }
    auditLocked(actor, "JOB_UPDATED", j->jobNo + ": details changed");
    return JobResult::success(*j, "Job updated.");
}

Result JobBook::assignMechanic(const std::string& jobNo, const std::string& mechanicIn, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mtx_);
    Job* j = mut(jobNo);
    if (!j) return Result::fail("Job " + jobNo + " does not exist.");
    if (!isEditable(j->status)) return Result::fail(j->jobNo + " is " + statusName(j->status) + " - the mechanic can no longer be changed.");
    const std::string mechanic = util::oneLine(util::trim(mechanicIn));
    if (mechanic.size() > kMaxMechanic) return Result::fail("The mechanic name is too long (max " + std::to_string(kMaxMechanic) + " characters).");
    if (mechanic == j->mechanic) return Result::success("Mechanic unchanged.");
    const std::string old = j->mechanic;
    j->mechanic = mechanic;
    if (!saveJobsLocked()) {
        j->mechanic = old;
        return Result::fail(warning_);
    }
    auditLocked(actor, "JOB_MECHANIC", j->jobNo + ": " + (old.empty() ? "(nobody)" : old) + " -> " + (mechanic.empty() ? "(nobody)" : mechanic));
    return Result::success(mechanic.empty() ? "Mechanic cleared." : "Assigned to " + mechanic + ".");
}

Result JobBook::changeStatus(const std::string& jobNo, JobStatus to, const std::string& actor, const std::string& note) {
    std::lock_guard<std::mutex> lock(mtx_);
    return changeStatusLocked(jobNo, to, actor, note);
}

Result JobBook::changeStatusLocked(const std::string& jobNo, JobStatus to, const std::string& actor, const std::string& note) {
    Job* j = mut(jobNo);
    if (!j) return Result::fail("Job " + jobNo + " does not exist.");
    if (j->status == to) return Result::fail(j->jobNo + " is already " + statusName(to) + ".");
    if (!canTransition(j->status, to)) {
        std::string options;
        for (JobStatus s : allowedNext(j->status)) {
            if (!options.empty()) options += ", ";
            options += statusName(s);
        }
        return Result::fail(j->jobNo + " cannot go from " + statusName(j->status) + " to " + statusName(to) +
                            (options.empty() ? std::string(" - ") + statusName(j->status) + " is final." : ". Allowed: " + options + "."));
    }

    // Cancelling puts every part back on the shelf - before the status is written, so a failure leaves
    // the job exactly as it was.
    std::vector<inv::Inventory::StockDelta> restore;
    if (to == JobStatus::Cancelled && !inv_) return Result::fail("No inventory is loaded - the job cannot be cancelled.");
    if (to == JobStatus::Cancelled)
        for (const JobLine& l : j->lines)
            if (l.kind == JobLine::Kind::Part && l.quantity > 0) restore.push_back({l.partId, l.quantity});

    const JobStatus old = j->status;
    const std::string oldClosed = j->closed;
    j->status = to;
    j->closed = (to == JobStatus::Released || to == JobStatus::Cancelled) ? util::nowStamp() : std::string();
    if (!saveJobsLocked()) {
        j->status = old;
        j->closed = oldClosed;
        return Result::fail(warning_);
    }
    if (!restore.empty()) {
        const auto r = inv_->applyStock(restore, inv::MoveType::JobReturn, actor, j->jobNo);
        if (!r.ok) {   // could not put the parts back: undo the status change too, nothing is half-done
            j->status = old;
            j->closed = oldClosed;
            saveJobsLocked();
            return Result::fail(r.message + " The job was left " + statusName(old) + ".");
        }
    }
    std::string details = j->jobNo + ": " + statusName(old) + " -> " + statusName(to);
    const std::string clean = util::oneLine(util::trim(note));
    if (!clean.empty()) details += " - " + clean;
    auditLocked(actor, "JOB_STATUS", details);
    std::string msg = j->jobNo + " is now " + statusName(to) + ".";
    if (to == JobStatus::Cancelled && !restore.empty()) msg += " " + std::to_string(restore.size()) + " part line(s) returned to stock.";
    if (to == JobStatus::Released) msg += " The job is locked and cannot be edited.";
    return Result::success(msg);
}

Result JobBook::cancelJob(const std::string& jobNo, const std::string& reason, const std::string& actor) {
    return changeStatus(jobNo, JobStatus::Cancelled, actor, reason);
}

// ---------------------------------------------------------------- lines
LineResult JobBook::addLine(const std::string& jobNo, const AddLine& in, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mtx_);
    Job* j = mut(jobNo);
    if (!j) return LineResult::fail("Job " + jobNo + " does not exist.");
    if (!isEditable(j->status))
        return LineResult::fail(j->jobNo + " is " + statusName(j->status) + " - lines can no longer be added.");

    JobLine l;
    l.kind = in.kind;
    l.lineNo = 1;
    for (const JobLine& e : j->lines) l.lineNo = std::max(l.lineNo, e.lineNo + 1);

    if (l.kind == JobLine::Kind::Part) {
        if (!inv_) return LineResult::fail("No inventory is loaded - parts cannot be issued.");
        const inv::Part* p = inv_->find(in.partId);
        if (!p) return LineResult::fail("Part " + std::to_string(in.partId) + " does not exist.");
        if (in.quantity < 1 || in.quantity > inv::limits::kMaxQty)
            return LineResult::fail("Quantity must be 1 to " + std::to_string(inv::limits::kMaxQty) + ".");
        if (in.quantity > p->quantity())
            return LineResult::fail("Not enough stock for \"" + p->fullName() + "\": " + std::to_string(p->quantity()) + " available, " +
                                    std::to_string(in.quantity) + " requested.");
        l.partId = p->id();
        l.description = p->fullName();
        l.quantity = in.quantity;
        l.unitPrice = p->price();   // catalogue price, never the number in the request
        l.laborFee = in.laborFee > 0 ? util::round2(in.laborFee) : 0.0;
        if (l.laborFee > inv::limits::kMaxPrice) return LineResult::fail("The fitting fee is out of range.");
    } else {
        l.quantity = 1;
        l.partId = 0;
        if (in.serviceId > 0) {
            const ServiceItem* s = services_.find(in.serviceId);
            if (!s) return LineResult::fail("Service " + std::to_string(in.serviceId) + " does not exist.");
            l.serviceId = s->id;
            l.description = util::oneLine(util::trim(in.description));
            if (l.description.empty()) l.description = s->name;
            if (in.laborFee < 0) l.laborFee = s->defaultLaborFee;
        } else {
            l.description = util::oneLine(util::trim(in.description));
        }
        if (l.description.empty() || l.description.size() > kMaxDesc)
            return LineResult::fail("A labour line needs a description (max " + std::to_string(kMaxDesc) + " characters).");
        if (in.laborFee >= 0) l.laborFee = util::round2(in.laborFee);
        if (!(l.laborFee >= 0 && l.laborFee <= inv::limits::kMaxPrice)) return LineResult::fail("The labour fee is out of range.");
    }

    // 1. the business record first, then the stock it consumes. A crash in between is healed by
    //    reconcileStockLocked() on the next start, because job_lines.csv is the source of truth.
    j->lines.push_back(l);
    if (!saveLinesLocked()) {
        j->lines.pop_back();
        return LineResult::fail(warning_);
    }
    if (l.kind == JobLine::Kind::Part) {
        const auto r = inv_->applyStock({{l.partId, -l.quantity}}, inv::MoveType::JobIssue, actor, j->jobNo);
        if (!r.ok) {   // never leave stock and paperwork disagreeing
            j->lines.pop_back();
            saveLinesLocked();
            return LineResult::fail(r.message);
        }
    }
    auditLocked(actor, "JOB_LINE_ADDED",
                j->jobNo + " line " + std::to_string(l.lineNo) + ": " + l.description +
                    (l.kind == JobLine::Kind::Part ? " x" + std::to_string(l.quantity) + " (stock issued)" : " (labour)"));

    LineResult out;
    out.ok = true;
    out.message = l.kind == JobLine::Kind::Part ? "Part added and " + std::to_string(l.quantity) + " taken from stock." : "Labour added.";
    out.line = l;
    out.lines = j->lines;
    out.totals = totalsOf(j->lines);
    return out;
}

LineResult JobBook::removeLine(const std::string& jobNo, int lineNo, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mtx_);
    Job* j = mut(jobNo);
    if (!j) return LineResult::fail("Job " + jobNo + " does not exist.");
    if (!isEditable(j->status))
        return LineResult::fail(j->jobNo + " is " + statusName(j->status) + " - lines can no longer be removed.");
    auto it = std::find_if(j->lines.begin(), j->lines.end(), [lineNo](const JobLine& l) { return l.lineNo == lineNo; });
    if (it == j->lines.end()) return LineResult::fail("Line " + std::to_string(lineNo) + " does not exist on " + j->jobNo + ".");
    const JobLine line = *it;
    j->lines.erase(it);
    if (!saveLinesLocked()) {
        j->lines.insert(std::lower_bound(j->lines.begin(), j->lines.end(), line.lineNo,
                                         [](const JobLine& l, int n) { return l.lineNo < n; }),
                        line);
        return LineResult::fail(warning_);
    }
    if (line.kind == JobLine::Kind::Part && line.quantity > 0) {
        const auto r = inv_->applyStock({{line.partId, line.quantity}}, inv::MoveType::JobReturn, actor, j->jobNo);
        if (!r.ok) {   // put the line back rather than lose the stock record
            j->lines.insert(std::lower_bound(j->lines.begin(), j->lines.end(), line.lineNo,
                                             [](const JobLine& l, int n) { return l.lineNo < n; }),
                            line);
            saveLinesLocked();
            return LineResult::fail(r.message);
        }
    }
    auditLocked(actor, "JOB_LINE_REMOVED",
                j->jobNo + " line " + std::to_string(line.lineNo) + ": " + line.description +
                    (line.kind == JobLine::Kind::Part ? " (stock returned)" : " (labour)"));
    LineResult out;
    out.ok = true;
    out.message = line.kind == JobLine::Kind::Part ? "Line removed and " + std::to_string(line.quantity) + " returned to stock." : "Line removed.";
    out.line = line;
    out.lines = j->lines;
    out.totals = totalsOf(j->lines);
    return out;
}

LinesResult JobBook::lines(const std::string& jobNo) const {
    std::lock_guard<std::mutex> lock(mtx_);
    LinesResult out;
    for (const Job& j : jobs_)
        if (j.jobNo == jobNo) {
            out.ok = true;
            out.lines = j.lines;
            out.totals = totalsOf(j.lines);
            return out;
        }
    out.message = "Job " + jobNo + " does not exist.";
    return out;
}

// ---------------------------------------------------------------- queries
std::vector<Job> JobBook::list(const JobFilter& f) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::string plateQ = loosePlate(f.plate);
    const std::string q = util::toLower(util::trim(f.query));
    const std::string mechanic = util::toLower(util::trim(f.mechanic));
    std::vector<Job> out;
    for (const Job& j : jobs_) {
        if (f.status && j.status != *f.status) continue;
        if (!mechanic.empty() && util::toLower(j.mechanic) != mechanic) continue;
        if (!f.date.empty() && datePart(j.opened) != f.date) continue;
        if (!f.from.empty() && datePart(j.opened) < f.from) continue;
        if (!f.to.empty() && datePart(j.opened) > f.to) continue;
        if (f.customerId > 0 && j.customerId != f.customerId) continue;
        if (f.vehicleId > 0 && j.vehicleId != f.vehicleId) continue;
        if (!plateQ.empty()) {
            std::string plate;
            if (people_) {
                const crm::Vehicle* v = people_->findVehicle(j.vehicleId);
                if (v) plate = v->plate;
            }
            if (loosePlate(plate).find(plateQ) == std::string::npos) continue;
        }
        if (!q.empty()) {
            std::string hay = util::toLower(j.jobNo + " " + j.complaint + " " + j.mechanic + " " + j.notes);
            if (people_) {
                if (const crm::Customer* k = people_->findCustomer(j.customerId)) hay += " " + util::toLower(k->name);
                if (const crm::Vehicle* v = people_->findVehicle(j.vehicleId))
                    hay += " " + util::toLower(v->plate + " " + v->model);
            }
            if (hay.find(q) == std::string::npos) continue;
        }
        out.push_back(j);
    }
    std::sort(out.begin(), out.end(), [&](const Job& a, const Job& b) {
        if (f.newestFirst) return a.jobNo > b.jobNo;
        return a.jobNo < b.jobNo;
    });
    if (f.limit > 0 && out.size() > f.limit) out.resize(f.limit);
    return out;
}

std::vector<Job> JobBook::openJobs() const {
    JobFilter f;
    f.limit = 0;
    std::vector<Job> out;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const Job& j : jobs_)
            if (isOpen(j.status)) out.push_back(j);
    }
    std::sort(out.begin(), out.end(), [](const Job& a, const Job& b) { return a.jobNo < b.jobNo; });
    return out;
}

JobCard JobBook::card(const std::string& jobNo) const {
    std::lock_guard<std::mutex> lock(mtx_);
    JobCard c;
    for (const Job& j : jobs_) {
        if (j.jobNo != jobNo) continue;
        c.ok = true;
        c.job = j;
        c.lines = j.lines;
        c.totals = totalsOf(j.lines);
        c.allowedNext = allowedNext(j.status);
        for (JobStatus s : c.allowedNext) c.allowedNextNames.emplace_back(statusName(s));
        if (people_) {
            if (const crm::Customer* k = people_->findCustomer(j.customerId)) {
                c.customerName = k->name;
                c.customerPhone = k->phone;
            }
            if (const crm::Vehicle* v = people_->findVehicle(j.vehicleId)) {
                c.vehiclePlate = v->plate;
                c.vehicleModel = v->model;
            }
        }
        return c;
    }
    c.message = "Job " + jobNo + " does not exist.";
    return c;
}

std::vector<JobEvent> JobBook::history(const std::string& jobNo) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<JobEvent> out;
    if (!audit_) return out;
    for (const auth::AuditEntry& e : audit_->entries())
        if (e.details.rfind(jobNo, 0) == 0) out.push_back({e.timestamp, e.user, e.event, e.details});
    return out;   // oldest first, the order the events happened
}

int JobBook::issuedFor(const std::string& jobNo, int partId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return issuedForLocked(jobNo, partId);
}

// ---------------------------------------------------------------- service catalogue (Admin)
std::string JobBook::saveService(int id, ServiceItem in, int& newId, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::string err = services_.save(id, in, newId);
    if (err.empty())
        auditLocked(actor, id == 0 ? "SERVICE_ADDED" : "SERVICE_UPDATED",
                    std::to_string(newId) + ": " + in.name + " - labour " + util::moneyFmt(in.defaultLaborFee) +
                        (in.intervalMonths > 0 ? ", every " + std::to_string(in.intervalMonths) + " month(s)" : "") +
                        (in.intervalKm > 0 ? ", every " + std::to_string(in.intervalKm) + " km" : ""));
    return err;
}

std::string JobBook::setServiceActive(int id, bool on, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mtx_);
    const ServiceItem* s = services_.find(id);
    if (!s) return "Service not found.";
    const std::string err = services_.setActive(id, on);
    if (err.empty()) auditLocked(actor, "SERVICE_TOGGLED", std::to_string(id) + ": " + s->name + (on ? " activated" : " deactivated"));
    return err;
}

// ---------------------------------------------------------------- export (Phase 7)
std::string JobBook::jobsCsv() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string out = std::string(kBom) + kJobsHeader + "\r\n";
    for (const Job& j : jobs_)
        out += util::csvSafe(j.jobNo) + ',' + util::csvSafe(j.opened) + ',' + (j.customerId > 0 ? std::to_string(j.customerId) : "") + ',' +
               (j.vehicleId > 0 ? std::to_string(j.vehicleId) : "") + ',' + std::to_string(j.odometer) + ',' + util::csvSafe(j.complaint) + ',' +
               util::csvSafe(j.mechanic) + ',' + util::csvSafe(statusName(j.status)) + ',' + util::csvSafe(j.closed) + ',' +
               util::csvSafe(j.notes) + ',' + util::csvSafe(j.invoiceNo) + "\r\n";
    return out;
}

std::string JobBook::jobLinesCsv() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string out = std::string(kBom) + kLinesHeader + "\r\n";
    for (const Job& j : jobs_)
        for (const JobLine& l : j.lines)
            out += util::csvSafe(j.jobNo) + ',' + std::to_string(l.lineNo) + ',' + JobLine::kindName(l.kind) + ',' +
                   (l.partId > 0 ? std::to_string(l.partId) : "0") + ',' + util::csvSafe(l.description) + ',' + std::to_string(l.quantity) + ',' +
                   util::money(l.unitPrice) + ',' + util::money(l.laborFee) + ',' + (l.serviceId > 0 ? std::to_string(l.serviceId) : "") + "\r\n";
    return out;
}

}  // namespace jobs
