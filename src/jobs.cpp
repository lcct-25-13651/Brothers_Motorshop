#include "jobs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "util.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace fs = std::filesystem;

namespace jobs {

namespace {

const char* const kJobsHeader = "job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes";
const char* const kLinesHeader = "job_no,line_no,kind,part_id,description,quantity,unit_price,labor_fee";
const char* const kLockError = "Could not lock the job files (jobs.lock) - check folder permissions.";

std::string clean(const std::string& s) { return util::oneLine(util::trim(s)); }

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

// Year of a YYYY-MM-DD string; the current year when the text is not a date.
int yearOf(const std::string& date) {
    if (date.size() >= 4 && allDigits(date.substr(0, 4))) {
        const int y = std::stoi(date.substr(0, 4));
        if (y >= 2000 && y <= 2999) return y;
    }
    return std::stoi(util::formatTime(std::time(nullptr), "%Y"));
}

void keepCopy(const std::string& file) {   // untouched copy of a file that had unreadable rows
    std::error_code ec;
    if (fs::exists(file, ec)) fs::copy_file(file, file + ".bak", fs::copy_options::overwrite_existing, ec);
}

}  // namespace

// ------------------------------------------------------------------ helpers
std::string centsToMoney(long long cents) {
    const bool neg = cents < 0;
    const long long a = neg ? -cents : cents;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s%lld.%02lld", neg ? "-" : "", a / 100, a % 100);
    return buf;
}

bool parseMoney(const std::string& in, long long& cents) {
    const std::string s = util::trim(in);
    if (s.empty() || s.size() > 14) return false;
    const size_t dot = s.find('.');
    const std::string whole = s.substr(0, dot);
    const std::string frac = dot == std::string::npos ? "" : s.substr(dot + 1);
    if (!allDigits(whole) || frac.size() > 2) return false;
    if (dot != std::string::npos && frac.empty()) return false;          // "12." is not a number
    if (!frac.empty() && !allDigits(frac)) return false;
    const std::string f2 = frac + std::string(2 - frac.size(), '0');
    cents = std::stoll(whole) * 100 + std::stoll(f2);
    return cents <= 9999999999LL;                                          // 99,999,999.99
}

std::string makeJobNo(int year, int seq) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "JO-%04d-%05d", year, seq);
    return buf;
}

bool parseJobNo(const std::string& s, int& year, int& seq) {
    // JO-YYYY-NNNNN (the number part may be longer than 5 digits once a year passes 99,999 jobs)
    if (s.size() < 11 || s.compare(0, 3, "JO-") != 0 || s[7] != '-') return false;
    const std::string y = s.substr(3, 4), n = s.substr(8);
    if (!allDigits(y) || !allDigits(n) || n.size() < 5 || n.size() > 9) return false;
    year = std::stoi(y);
    seq = std::stoi(n);
    return seq >= 1 && makeJobNo(year, seq) == s;
}

// ------------------------------------------------------------------ file lock
FileLock::FileLock(const std::string& path) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) { CloseHandle(h); return; }
    handle_ = reinterpret_cast<std::intptr_t>(h);
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return;
    int rc;
    do { rc = ::flock(fd, LOCK_EX); } while (rc != 0 && errno == EINTR);
    if (rc != 0) { ::close(fd); return; }
    handle_ = fd;
#endif
}

FileLock::~FileLock() {
    if (handle_ == -1) return;
#if defined(_WIN32)
    HANDLE h = reinterpret_cast<HANDLE>(handle_);
    OVERLAPPED ov{};
    UnlockFileEx(h, 0, 1, 0, &ov);
    CloseHandle(h);
#else
    ::flock(static_cast<int>(handle_), LOCK_UN);
    ::close(static_cast<int>(handle_));
#endif
}

// ------------------------------------------------------------------ store
JobStore::JobStore(const std::string& dir)
    : jobsFile_(dir + "/jobs.csv"), linesFile_(dir + "/job_lines.csv"), lockFile_(dir + "/jobs.lock") {}

std::string JobStore::warning() const {
    std::lock_guard<std::mutex> g(mu_);
    return warning_;
}

JobStore::Snapshot JobStore::load() const {
    Snapshot s;
    int badJobs = 0, badLines = 0;

    for (const auto& f : util::readCsvFile(jobsFile_)) {
        if (!f.empty() && f[0] == "job_no") continue;   // header
        int y = 0, q = 0;
        long long cid = 0, vid = 0, odo = 0;
        if (f.size() < 8 || !parseJobNo(util::trim(f[0]), y, q) || !util::parseInt(f[2], 0, 999999, cid) ||
            !util::parseInt(f[3], 0, 999999, vid) || !util::parseInt(f[4], 0, 9999999, odo)) { ++badJobs; continue; }
        Job j;
        j.jobNo = util::trim(f[0]);
        j.opened = util::trim(f[1]);
        j.customerId = static_cast<int>(cid);
        j.vehicleId = static_cast<int>(vid);
        j.odometer = odo;
        j.complaint = clean(f[5]);
        j.mechanic = clean(f[6]);
        j.status = clean(f[7]);
        if (f.size() > 8) j.closed = util::trim(f[8]);
        if (f.size() > 9) j.notes = clean(f[9]);
        const bool dup = std::any_of(s.jobs.begin(), s.jobs.end(), [&](const Job& o) { return o.jobNo == j.jobNo; });
        if (dup) { ++badJobs; continue; }              // a job_no can never appear twice
        s.jobs.push_back(std::move(j));
    }

    for (const auto& f : util::readCsvFile(linesFile_)) {
        if (!f.empty() && f[0] == "job_no") continue;
        int y = 0, q = 0;
        long long ln = 0, pid = 0, qty = 0, price = 0, fee = 0;
        if (f.size() < 6 || !parseJobNo(util::trim(f[0]), y, q) || !util::parseInt(f[1], 1, 999999, ln) ||
            util::trim(f[2]).empty() || !util::parseInt(f[3], 0, 999999, pid) || !util::parseInt(f[5], 0, 999999, qty)) { ++badLines; continue; }
        const std::string up = f.size() > 6 ? util::trim(f[6]) : "", lf = f.size() > 7 ? util::trim(f[7]) : "";
        if ((!up.empty() && !parseMoney(up, price)) || (!lf.empty() && !parseMoney(lf, fee))) { ++badLines; continue; }
        JobLine l;
        l.jobNo = util::trim(f[0]);
        l.lineNo = static_cast<int>(ln);
        l.kind = clean(f[2]);
        l.partId = static_cast<int>(pid);
        l.description = clean(f[4]);
        l.quantity = static_cast<int>(qty);
        l.unitPriceCents = price;
        l.laborFeeCents = fee;
        const bool dup = std::any_of(s.lines.begin(), s.lines.end(),
                                     [&](const JobLine& o) { return o.jobNo == l.jobNo && o.lineNo == l.lineNo; });
        if (dup) { ++badLines; continue; }
        s.lines.push_back(std::move(l));
    }

    warning_.clear();
    if (badJobs > 0) keepCopy(jobsFile_);
    if (badLines > 0) keepCopy(linesFile_);
    if (badJobs + badLines > 0)
        warning_ = std::to_string(badJobs + badLines) + " malformed or duplicate row(s) in jobs.csv / job_lines.csv were skipped (originals kept as *.bak).";
    return s;
}

bool JobStore::writeJobs(const std::vector<Job>& jobs) const {
    std::string out = std::string(kJobsHeader) + "\n";
    for (const Job& j : jobs)
        out += util::csvEscape(j.jobNo) + ',' + util::csvEscape(j.opened) + ',' + std::to_string(j.customerId) + ',' +
               std::to_string(j.vehicleId) + ',' + std::to_string(j.odometer) + ',' + util::csvEscape(j.complaint) + ',' +
               util::csvEscape(j.mechanic) + ',' + util::csvEscape(j.status) + ',' + util::csvEscape(j.closed) + ',' +
               util::csvEscape(j.notes) + "\n";
    return util::writeFileAtomic(jobsFile_, out);
}

bool JobStore::writeLines(const std::vector<JobLine>& lines) const {
    std::string out = std::string(kLinesHeader) + "\n";
    for (const JobLine& l : lines)
        out += util::csvEscape(l.jobNo) + ',' + std::to_string(l.lineNo) + ',' + util::csvEscape(l.kind) + ',' +
               std::to_string(l.partId) + ',' + util::csvEscape(l.description) + ',' + std::to_string(l.quantity) + ',' +
               centsToMoney(l.unitPriceCents) + ',' + centsToMoney(l.laborFeeCents) + "\n";
    return util::writeFileAtomic(linesFile_, out);
}

std::string JobStore::createJob(Job in, std::string& jobNoOut) {
    std::lock_guard<std::mutex> g(mu_);
    FileLock lock(lockFile_);
    if (!lock.ok()) return kLockError;
    Snapshot s = load();

    in.opened = util::trim(in.opened);
    if (in.opened.empty()) in.opened = util::formatTime(std::time(nullptr), "%Y-%m-%d");
    in.complaint = clean(in.complaint);
    in.mechanic = clean(in.mechanic);
    in.status = clean(in.status);
    in.closed = util::trim(in.closed);
    in.notes = clean(in.notes);

    const int year = yearOf(in.opened);
    int maxSeq = 0;
    for (const Job& j : s.jobs) {
        int y = 0, q = 0;
        if (parseJobNo(j.jobNo, y, q) && y == year) maxSeq = std::max(maxSeq, q);
    }
    in.jobNo = makeJobNo(year, maxSeq + 1);
    s.jobs.push_back(in);
    if (!writeJobs(s.jobs)) return "Could not write jobs.csv - check disk space/permissions.";
    jobNoOut = in.jobNo;
    return "";
}

std::string JobStore::updateJob(const Job& inRaw) {
    std::lock_guard<std::mutex> g(mu_);
    FileLock lock(lockFile_);
    if (!lock.ok()) return kLockError;
    Snapshot s = load();
    for (Job& j : s.jobs) {
        if (j.jobNo != inRaw.jobNo) continue;
        Job in = inRaw;
        in.opened = util::trim(in.opened);
        in.complaint = clean(in.complaint);
        in.mechanic = clean(in.mechanic);
        in.status = clean(in.status);
        in.closed = util::trim(in.closed);
        in.notes = clean(in.notes);
        j = in;
        if (!writeJobs(s.jobs)) return "Could not write jobs.csv - check disk space/permissions.";
        return "";
    }
    return "Job not found.";
}

std::optional<Job> JobStore::findJob(const std::string& jobNo) const {
    std::lock_guard<std::mutex> g(mu_);
    FileLock lock(lockFile_);
    if (!lock.ok()) return std::nullopt;
    for (const Job& j : load().jobs) if (j.jobNo == jobNo) return j;
    return std::nullopt;
}

std::vector<Job> JobStore::allJobs() const {
    std::lock_guard<std::mutex> g(mu_);
    FileLock lock(lockFile_);
    if (!lock.ok()) return {};
    return load().jobs;
}

std::string JobStore::addLine(const std::string& jobNo, JobLine in, int& lineNoOut) {
    std::lock_guard<std::mutex> g(mu_);
    FileLock lock(lockFile_);
    if (!lock.ok()) return kLockError;
    Snapshot s = load();
    const bool exists = std::any_of(s.jobs.begin(), s.jobs.end(), [&](const Job& j) { return j.jobNo == jobNo; });
    if (!exists) return "Job not found.";

    int maxLine = 0;
    for (const JobLine& l : s.lines) if (l.jobNo == jobNo) maxLine = std::max(maxLine, l.lineNo);
    in.jobNo = jobNo;
    in.lineNo = maxLine + 1;
    in.kind = clean(in.kind);
    in.description = clean(in.description);
    s.lines.push_back(in);
    if (!writeLines(s.lines)) return "Could not write job_lines.csv - check disk space/permissions.";
    lineNoOut = in.lineNo;
    return "";
}

std::vector<JobLine> JobStore::linesOf(const std::string& jobNo) const {
    std::lock_guard<std::mutex> g(mu_);
    FileLock lock(lockFile_);
    if (!lock.ok()) return {};
    std::vector<JobLine> out;
    for (const JobLine& l : load().lines) if (l.jobNo == jobNo) out.push_back(l);
    std::sort(out.begin(), out.end(), [](const JobLine& a, const JobLine& b) { return a.lineNo < b.lineNo; });
    return out;
}

std::vector<JobLine> JobStore::allLines() const {
    std::lock_guard<std::mutex> g(mu_);
    FileLock lock(lockFile_);
    if (!lock.ok()) return {};
    return load().lines;
}

}  // namespace jobs
