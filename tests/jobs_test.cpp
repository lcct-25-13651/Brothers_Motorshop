// Self-checking test for Phase 4 (job orders). No network, no httplib needed.
//
//   make jobstest     (or: g++ -std=c++17 -O2 -Isrc tests/jobs_test.cpp src/*.cpp -o jobs_test -pthread)
//
// Covers the Phase 4 "definition of done":
//   * stock is exactly right after add / remove / cancel (and after a reload from disk),
//   * two requests at the same moment can never over-issue the last unit,
//   * grand totals are right, and a Released job is frozen.
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/auth.h"
#include "../src/crm.h"
#include "../src/inventory.h"
#include "../src/jobs.h"
#include "../src/util.h"

static int failures = 0;
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } \
    } while (0)
#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        const auto va = (a); const auto vb = (b);                                                            \
        if (!(va == vb)) { std::cerr << "FAIL line " << __LINE__ << ": " #a " == " #b " (" << va << " vs " << vb << ")\n"; ++failures; } \
    } while (0)

using namespace jobs;

namespace {

// Replays movements.csv from zero and counts the rows whose `resulting_stock` disagrees with the
// running balance. It must be 0: the ledger is the audit trail, so a wrong number there is a bug
// even when parts.csv happens to be right.
int ledgerErrors(const inv::Inventory& in) {
    std::map<int, int> running;
    int errors = 0;
    for (const inv::Movement& m : in.movements().all()) {
        const int from = running.count(m.partId) ? running[m.partId] : 0;
        running[m.partId] = from + m.qtyChanged;
        if (running[m.partId] != m.resultingStock) ++errors;
    }
    return errors;
}

struct Fixture {
    std::string dir;
    Fixture() {
        namespace fs = std::filesystem;
        dir = (fs::temp_directory_path() / "brothers_jobs_test").string();
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Fixture() { std::filesystem::remove_all(dir); }
};

}  // namespace

int main() {
    Fixture fx;
    const std::string d = fx.dir;

    // ---------------------------------------------------------------- service catalogue
    {
        inv::Inventory in(d);
        JobBook book;
        book.open(d, in, nullptr);
        CHECK_EQ(book.services().all().size(), size_t(3));                 // seeded on a fresh install
        CHECK(book.services().find(1) && book.services().find(1)->name == "Change Oil");
        CHECK(book.services().find(1)->intervalMonths == 3 && book.services().find(1)->intervalKm == 3000);
        CHECK(book.services().find(2)->name == "Brake Pad Replacement" && book.services().find(3)->name == "Engine Overhaul");
        CHECK(book.services().active().size() == 3);
        ServiceItem s;
        s.name = "Wheel Bearing Replacement";
        s.defaultLaborFee = 750;
        s.intervalMonths = 24;
        s.intervalKm = 20000;
        int newId = 0;
        CHECK(book.saveService(0, s, newId, "boss").empty());
        CHECK_EQ(newId, 4);
        ServiceItem dup = s;
        dup.name = "change OIL";
        CHECK(!book.saveService(0, dup, newId, "boss").empty());          // names are unique
        CHECK(!book.saveService(9, s, newId, "boss").empty());            // unknown id
        CHECK(book.setServiceActive(4, false, "boss").empty());
        CHECK(!book.services().find(4)->active && book.services().active().size() == 3);
        CHECK(book.setServiceActive(4, true, "boss").empty());
        CHECK(book.servicesCsv().find("Change Oil") != std::string::npos);
    }

    // ---------------------------------------------------------------- create + lines + stock + totals
    inv::Inventory in(d);
    auth::AuditLog audit(d + "/audit.csv");
    crm::Registry people;
    people.open(d);
    JobBook book;
    book.open(d, in, &audit, &people);

    crm::Customer k;
    k.name = "Juan dela Cruz";
    k.phone = "0917 123 4567";
    int cid = 0;
    CHECK(people.saveCustomer(0, k, cid).empty());
    crm::Vehicle v;
    v.customerId = cid;
    v.plate = "abc 1234";
    v.model = "Bajaj RE 4S";
    int vid = 0;
    CHECK(people.saveVehicle(0, v, vid).empty());

    {
        CreateJob cj;
        cj.customerId = cid;
        cj.vehicleId = vid;
        cj.odometer = 23450;
        cj.complaint = "Engine knocking, difficult start";
        cj.mechanic = "Boy";
        const auto r = book.createJob(cj, "juan");
        CHECK(r.ok);
        CHECK_EQ(r.job.jobNo, std::string("JOB-000001"));
        CHECK(r.job.status == JobStatus::Open && !r.job.isLocked());
        CHECK_EQ(r.job.odometer, 23450);
        CHECK_EQ(book.nextJobNo(), std::string("JOB-000002"));

        // a job for a vehicle that belongs to somebody else is refused
        CreateJob bad = cj;
        bad.customerId = 0;
        bad.vehicleId = 9999;
        CHECK(!book.createJob(bad, "juan").ok);
        CreateJob walkIn;
        walkIn.complaint = "Walk-in oil change";
        const auto w = book.createJob(walkIn, "juan");
        CHECK(w.ok && w.job.customerId == 0 && w.job.vehicleId == 0);
        CHECK_EQ(w.job.jobNo, std::string("JOB-000002"));
    }

    const std::string no = "JOB-000001";
    const int start101 = in.find(101)->quantity();   // 20 x 450.00
    {
        AddLine a;
        a.kind = JobLine::Kind::Part;
        a.partId = 101;
        a.quantity = 2;
        const auto r = book.addLine(no, a, "juan");
        CHECK(r.ok);
        CHECK_EQ(in.find(101)->quantity(), start101 - 2);               // deducted immediately
        CHECK_EQ(book.issuedFor(no, 101), 2);
        CHECK_EQ(r.line.unitPrice, 450.0);                               // catalogue price, not the request
        CHECK_EQ(r.totals.parts, 900.0);
        CHECK_EQ(r.totals.grand(), 900.0);
        const inv::Movement& m = in.movements().all().back();
        CHECK(m.type == inv::MoveType::JobIssue && m.ref == no && m.qtyChanged == -2 && m.resultingStock == start101 - 2);

        // a price in the request is ignored
        AddLine tampered;
        tampered.kind = JobLine::Kind::Part;
        tampered.partId = 103;
        tampered.quantity = 1;
        tampered.unitPrice = 1;
        const auto t = book.addLine(no, tampered, "juan");
        CHECK(t.ok && t.line.unitPrice == 180.0);                        // part 103 = 180.00
    }
    {
        AddLine lab;
        lab.kind = JobLine::Kind::Labor;
        lab.serviceId = 1;                                               // Change Oil -> default fee 300
        const auto r = book.addLine(no, lab, "juan");
        CHECK(r.ok);
        CHECK_EQ(r.line.laborFee, 300.0);
        CHECK_EQ(r.line.description, std::string("Change Oil"));
        CHECK_EQ(r.line.serviceId, 1);
        CHECK_EQ(r.totals.parts, 900.0 + 180.0);
        CHECK_EQ(r.totals.labor, 300.0);
        CHECK_EQ(r.totals.grand(), 1380.0);                              // 900 + 180 parts + 300 labour

        AddLine own;
        own.kind = JobLine::Kind::Labor;
        own.description = "Fabricate bracket";
        own.laborFee = 250.50;
        const auto r2 = book.addLine(no, own, "juan");
        CHECK(r2.ok && r2.line.serviceId == 0);
        CHECK_EQ(r2.totals.labor, 550.5);
        CHECK_EQ(r2.totals.grand(), 1630.5);

        AddLine empty;
        empty.kind = JobLine::Kind::Labor;
        CHECK(!book.addLine(no, empty, "juan").ok);                      // labour needs a description
    }
    {
        // no more stock than the shelf holds
        AddLine tooMany;
        tooMany.kind = JobLine::Kind::Part;
        tooMany.partId = 105;                                            // 3 in stock
        tooMany.quantity = 4;
        const auto r = book.addLine(no, tooMany, "juan");
        CHECK(!r.ok);
        CHECK(r.message.find("Not enough stock") != std::string::npos);
        CHECK_EQ(in.find(105)->quantity(), 3);

        tooMany.quantity = 3;
        CHECK(book.addLine(no, tooMany, "juan").ok);
        CHECK_EQ(in.find(105)->quantity(), 0);
        AddLine oneMore;
        oneMore.kind = JobLine::Kind::Part;
        oneMore.partId = 105;
        oneMore.quantity = 1;
        CHECK(!book.addLine(no, oneMore, "juan").ok);                    // now empty
        CHECK(!book.addLine(no, AddLine{JobLine::Kind::Part, 9999, 0, "", 1}, "juan").ok);   // unknown part
        CHECK(!book.addLine("JOB-999999", oneMore, "juan").ok);          // unknown job
    }

    // ---------------------------------------------------------------- removing a line restores stock
    {
        const int before = in.find(105)->quantity();
        const auto lines = book.lines(no);
        CHECK(lines.ok && lines.lines.size() == 5);
        int lineNo = 0;
        for (const JobLine& l : lines.lines)
            if (l.partId == 105) lineNo = l.lineNo;
        CHECK(lineNo > 0);
        const auto r = book.removeLine(no, lineNo, "juan");
        CHECK(r.ok);
        CHECK_EQ(in.find(105)->quantity(), before + 3);
        CHECK_EQ(book.issuedFor(no, 105), 0);
        CHECK(!book.removeLine(no, lineNo, "juan").ok);                  // already gone
        const auto m = in.movements().all().back();
        CHECK(m.type == inv::MoveType::JobReturn && m.ref == no && m.qtyChanged == 3);
        CHECK_EQ(r.totals.parts, 1080.0);                                // 900 + 180 again
        CHECK_EQ(ledgerErrors(in), 0);
    }

    // ---------------------------------------------------------------- status workflow
    {
        CHECK(book.assignMechanic(no, "Karding", "juan").ok);
        CHECK_EQ(book.get(no)->mechanic, std::string("Karding"));
        CHECK(!book.changeStatus(no, JobStatus::Released, "juan").ok);    // must pass through Done
        CHECK(book.changeStatus(no, JobStatus::InProgress, "juan").ok);
        CHECK(!book.changeStatus(no, JobStatus::InProgress, "juan").ok);  // already there
        CHECK(book.changeStatus(no, JobStatus::Done, "juan").ok);
        AddLine a;
        a.kind = JobLine::Kind::Part;
        a.partId = 103;
        a.quantity = 1;
        CHECK(!book.addLine(no, a, "juan").ok);                           // Done is not editable
        CHECK(book.changeStatus(no, JobStatus::InProgress, "juan").ok);   // reopen and carry on
        CHECK(book.addLine(no, a, "juan").ok);
        CHECK(book.changeStatus(no, JobStatus::Done, "juan").ok);
        const auto rel = book.changeStatus(no, JobStatus::Released, "juan");
        CHECK(rel.ok);
        CHECK(!book.get(no)->closed.empty());
        CHECK(book.get(no)->isLocked());

        // Released is final: nothing may be touched any more
        CHECK(!book.changeStatus(no, JobStatus::Done, "juan").ok);
        CHECK(!book.addLine(no, a, "juan").ok);
        CHECK(!book.removeLine(no, 1, "juan").ok);
        CHECK(!book.assignMechanic(no, "Pedro", "juan").ok);
        JobEdit ed;
        ed.odometer = 99999;
        CHECK(!book.updateJob(no, ed, "juan").ok);
        CHECK_EQ(book.get(no)->odometer, 23450);                          // unchanged
        const int before = in.find(103)->quantity();
        CHECK(!book.addLine(no, a, "juan").ok);
        CHECK_EQ(in.find(103)->quantity(), before);                       // and no stock moved
    }
    {
        const JobStatus s = JobStatus::Released;
        CHECK(allowedNext(s).empty());
        CHECK(canTransition(JobStatus::Open, JobStatus::InProgress));
        CHECK(canTransition(JobStatus::Open, JobStatus::Cancelled));
        CHECK(!canTransition(JobStatus::Open, JobStatus::Done));
        CHECK(!canTransition(JobStatus::Open, JobStatus::Released));
        CHECK(canTransition(JobStatus::InProgress, JobStatus::Open));     // back to the waiting list
        CHECK(canTransition(JobStatus::Done, JobStatus::InProgress));     // reopen a finished job
        CHECK(!canTransition(JobStatus::Cancelled, JobStatus::Open));
        CHECK(*parseStatus("in progress") == JobStatus::InProgress);
        CHECK(*parseStatus("RELEASED") == JobStatus::Released);
        CHECK(!parseStatus("halfway").has_value());
    }

    // ---------------------------------------------------------------- cancelling returns every part
    {
        CreateJob cj;
        cj.customerId = cid;
        cj.vehicleId = vid;
        cj.complaint = "Brake squeal";
        const auto r = book.createJob(cj, "juan");
        CHECK(r.ok);
        const std::string no2 = r.job.jobNo;
        const int b103 = in.find(103)->quantity(), b101 = in.find(101)->quantity();
        AddLine a;
        a.kind = JobLine::Kind::Part;
        a.partId = 103;
        a.quantity = 5;
        CHECK(book.addLine(no2, a, "juan").ok);
        a.partId = 101;
        a.quantity = 1;
        CHECK(book.addLine(no2, a, "juan").ok);
        AddLine lab;
        lab.kind = JobLine::Kind::Labor;
        lab.serviceId = 2;
        CHECK(book.addLine(no2, lab, "juan").ok);
        CHECK_EQ(in.find(103)->quantity(), b103 - 5);
        CHECK_EQ(in.find(101)->quantity(), b101 - 1);

        const auto c = book.cancelJob(no2, "Customer changed mind", "boss");
        CHECK(c.ok);
        CHECK_EQ(in.find(103)->quantity(), b103);
        CHECK_EQ(in.find(101)->quantity(), b101);
        CHECK_EQ(book.issuedFor(no2, 103), 0);
        CHECK_EQ(book.issuedFor(no2, 101), 0);
        CHECK(book.get(no2)->status == JobStatus::Cancelled && !book.get(no2)->closed.empty());
        CHECK(!book.changeStatus(no2, JobStatus::Open, "boss").ok);       // cancelled is final
        CHECK(!book.addLine(no2, a, "boss").ok);
        CHECK_EQ(ledgerErrors(in), 0);
    }

    // ---------------------------------------------------------------- filters
    {
        CreateJob cj;
        cj.mechanic = "Boy";
        cj.complaint = "Change oil";
        const auto r = book.createJob(cj, "juan");
        CHECK(r.ok);
        JobFilter f;
        f.mechanic = "boy";
        CHECK_EQ(book.list(f).size(), size_t(1));
        f.mechanic.clear();
        f.status = JobStatus::Open;
        for (const Job& j : book.list(f)) CHECK(j.status == JobStatus::Open);
        f.status = std::nullopt;
        f.plate = "ABC1234";
        CHECK_EQ(book.list(f).size(), size_t(2));                         // the two jobs booked on this plate
        f.plate = "abc-1234";                                             // any spelling
        CHECK_EQ(book.list(f).size(), size_t(2));
        f.plate = "XYZ9999";
        CHECK(book.list(f).empty());
        f.plate.clear();
        f.query = "knocking";
        CHECK_EQ(book.list(f).size(), size_t(1));
        f.query = "juan dela cruz";                                       // customer name
        CHECK_EQ(book.list(f).size(), size_t(2));
        f.query.clear();
        f.date = util::formatTime(std::time(nullptr), "%Y-%m-%d");
        CHECK_EQ(book.list(f).size(), book.all().size());                 // everything was opened today
        f.date.clear();
        f.limit = 2;
        CHECK_EQ(book.list(f).size(), size_t(2));
        CHECK(book.list()[0].jobNo > book.list()[1].jobNo);               // newest first
        CHECK_EQ(book.openJobs().size(), size_t(2));                      // the walk-in job and "Boy"'s
    }

    // ---------------------------------------------------------------- job card + history
    {
        const auto card = book.card(no);
        CHECK(card.ok);
        CHECK_EQ(card.customerName, std::string("Juan dela Cruz"));
        CHECK_EQ(card.vehiclePlate, std::string("ABC1234"));
        CHECK_EQ(card.totals.grand(), book.lines(no).totals.grand());
        CHECK(card.allowedNext.empty());                                  // released
        CHECK(!book.card("JOB-999999").ok);
        const auto hist = book.history(no);
        CHECK(hist.size() >= 6);                                          // created, lines, every status change
        bool sawStatus = false;
        for (const JobEvent& e : hist)
            if (e.event == "JOB_STATUS") sawStatus = true;
        CHECK(sawStatus);
        size_t auditRows = 0;
        for (const auth::AuditEntry& e : audit.entries())
            if (e.details.rfind(no, 0) == 0) ++auditRows;
        CHECK_EQ(auditRows, hist.size());
    }

    // ---------------------------------------------------------------- reload from disk
    {
        const auto before = book.card("JOB-000003");
        const int stock103 = in.find(103)->quantity();
        inv::Inventory in2(d);                 // reads parts.csv / movements.csv from disk
        auth::AuditLog audit2(d + "/audit.csv");
        crm::Registry people2;
        people2.open(d);
        JobBook book2;
        book2.open(d, in2, &audit2, &people2);
        CHECK_EQ(book2.all().size(), book.all().size());
        const auto after = book2.card("JOB-000003");
        CHECK(after.ok);
        CHECK_EQ(after.totals.grand(), before.totals.grand());
        CHECK_EQ(after.lines.size(), before.lines.size());
        CHECK_EQ(in2.find(103)->quantity(), stock103);                    // reconcile changed nothing
        CHECK_EQ(book2.issuedFor("JOB-000001", 103), 2);                  // two lines of part 103
        CHECK_EQ(book2.issuedFor("JOB-000003", 103), 0);                  // cancelled: everything back
        CHECK_EQ(ledgerErrors(in2), 0);
        CHECK(book2.warning().empty());
        CHECK_EQ(book2.nextJobNo(), book.nextJobNo());
        CHECK(!book2.jobsCsv().empty() && !book2.jobLinesCsv().empty());
        // the CSV files really hold the rows
        const auto rows = util::readCsvFile(d + "/jobs.csv");
        CHECK_EQ(rows.size(), book.all().size() + 1);                     // + header
        CHECK(rows[0].size() == 11 && rows[0][10] == "invoice_no");
        const auto lrows = util::readCsvFile(d + "/job_lines.csv");
        CHECK(lrows[0].size() == 9 && lrows[0][8] == "service_id");
    }

    // ---------------------------------------------------------------- concurrency: many requests at once
    {
        CreateJob cj;
        cj.complaint = "Concurrency";
        const auto made = book.createJob(cj, "juan");
        CHECK(made.ok);
        const std::string no4 = made.job.jobNo;
        const int start = in.find(103)->quantity();
        CHECK(start >= 20);
        const int workers = start + 12;                                   // more requests than units
        std::atomic<int> added{0}, refused{0};
        std::vector<std::thread> pool;
        std::mutex gate;                       // stands in for the web layer's request mutex
        for (int i = 0; i < workers; ++i) {
            pool.emplace_back([&, i]() {
                std::lock_guard<std::mutex> lock(gate);                   // one "request" at a time
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                AddLine a;
                a.kind = JobLine::Kind::Part;
                a.partId = 103;
                a.quantity = 1;
                const auto r = book.addLine(no4, a, "thread" + std::to_string(i));
                if (r.ok) ++added;
                else ++refused;
            });
        }
        for (std::thread& t : pool) t.join();
        CHECK_EQ(added.load(), start);                                    // every unit issued, not one more
        CHECK_EQ(refused.load(), 12);
        CHECK_EQ(in.find(103)->quantity(), 0);
        CHECK_EQ(book.issuedFor(no4, 103), start);
        const auto lines = book.lines(no4);
        int lineSum = 0;
        for (const JobLine& l : lines.lines) lineSum += l.quantity;
        CHECK_EQ(lineSum, start);                                         // paperwork matches the shelf
        CHECK_EQ(ledgerErrors(in), 0);
        CHECK_EQ(lines.totals.parts, util::round2(180.0 * lineSum));      // totals still add up
        CHECK_EQ(static_cast<int>(lines.lines.size()), start);            // one line per request
    }

    // ---------------------------------------------------------------- concurrency: two cashiers, one unit
    {
        inv::Inventory in3(d + "_race2");
        auth::AuditLog a3(d + "_race2/audit.csv");
        JobBook book3;
        book3.open(d + "_race2", in3, &a3);
        CreateJob cj;
        cj.complaint = "Race";
        const auto r = book3.createJob(cj, "juan");
        CHECK(r.ok);
        const int have = in3.find(106)->quantity();                       // 4
        CHECK(have >= 2);
        std::atomic<int> ok{0};
        std::vector<std::thread> pool;
        for (int i = 0; i < 2; ++i) {
            pool.emplace_back([&, i]() {
                AddLine a;
                a.kind = JobLine::Kind::Part;
                a.partId = 106;
                a.quantity = have - 1;                                    // both want all but one
                if (book3.addLine(r.job.jobNo, a, "t" + std::to_string(i)).ok) ++ok;
            });
        }
        for (std::thread& t : pool) t.join();
        CHECK_EQ(ok.load(), 1);                                           // exactly one of them got it
        CHECK_EQ(in3.find(106)->quantity(), 1);
        CHECK_EQ(book3.issuedFor(r.job.jobNo, 106), have - 1);
        std::filesystem::remove_all(d + "_race2");
    }

    // ---------------------------------------------------------------- crash recovery
    {
        namespace fs = std::filesystem;
        const std::string cd = d + "_crash";
        fs::remove_all(cd);
        fs::create_directories(cd);
        // A job with two part lines, but movements.csv never got the rows (power cut mid-write).
        {
            std::ofstream j(cd + "/jobs.csv");
            j << "job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes,invoice_no\n"
              << "JOB-000001,2026-02-01 09:00:00,0,0,1000,Overhaul,Boy,In progress,,, \n";
            std::ofstream l(cd + "/job_lines.csv");
            l << "job_no,line_no,kind,part_id,description,quantity,unit_price,labor_fee,service_id\n"
              << "JOB-000001,1,Part,101,Engine Oil 1L,3,450.00,0.00,\n"
              << "JOB-000001,2,Labor,0,Engine Overhaul,1,0.00,5000.00,3\n";
        }
        inv::Inventory in4(cd);
        JobBook book4;
        const int before = in4.find(101)->quantity();
        book4.open(cd, in4, nullptr);
        CHECK_EQ(in4.find(101)->quantity(), before - 3);                  // the missing issue is applied
        CHECK_EQ(book4.issuedFor("JOB-000001", 101), 3);
        CHECK(book4.warning().find("reconciled") != std::string::npos);
        CHECK_EQ(book4.lines("JOB-000001").totals.grand(), 5000.0 + 3 * 450.0);
        // ... and a second start does NOT do it again
        JobBook book5;
        book5.open(cd, in4, nullptr);
        CHECK_EQ(in4.find(101)->quantity(), before - 3);
        CHECK(book5.warning().empty());
        fs::remove_all(cd);
    }

    // ---------------------------------------------------------------- permissions reference
    CHECK(auth::allowed(auth::Role::Staff, auth::Permission::Jobs));
    CHECK(auth::allowed(auth::Role::Admin, auth::Permission::Jobs));
    CHECK(auth::allowed(auth::Role::Admin, auth::Permission::ServiceCatalog));
    CHECK(!auth::allowed(auth::Role::Staff, auth::Permission::ServiceCatalog));

    if (failures == 0) std::cout << "ALL PHASE 4 CORE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
