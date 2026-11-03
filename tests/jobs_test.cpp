// Phase 4.1 tests: jobs.csv / job_lines.csv data layer (no network, no httplib).
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

#include "../src/jobs.h"
#include "../src/util.h"

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } \
    } while (0)

namespace fs = std::filesystem;

static std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string firstLine(const fs::path& p) {
    std::ifstream in(p);
    std::string l;
    std::getline(in, l);
    return l;
}

static fs::path freshDir(const char* name) {
    const fs::path d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

static jobs::Job sampleJob(const std::string& opened = "2026-03-05") {
    jobs::Job j;
    j.opened = opened;
    j.customerId = 1;
    j.vehicleId = 2;
    j.odometer = 12345;
    j.complaint = "Maingay ang makina";
    j.mechanic = "Mang Tonyo";
    return j;
}

int main() {
    // ---- pure helpers ----
    {
        CHECK(jobs::centsToMoney(0) == "0.00");
        CHECK(jobs::centsToMoney(85050) == "850.50");
        CHECK(jobs::centsToMoney(5) == "0.05");
        CHECK(jobs::centsToMoney(123456789) == "1234567.89");
        long long c = -1;
        CHECK(jobs::parseMoney("850.50", c) && c == 85050);
        CHECK(jobs::parseMoney("850.5", c) && c == 85050);
        CHECK(jobs::parseMoney("850", c) && c == 85000);
        CHECK(jobs::parseMoney(" 0.07 ", c) && c == 7);
        CHECK(!jobs::parseMoney("", c));
        CHECK(!jobs::parseMoney("-5", c));
        CHECK(!jobs::parseMoney("1.234", c));
        CHECK(!jobs::parseMoney("12.", c));
        CHECK(!jobs::parseMoney("abc", c));
        CHECK(!jobs::parseMoney("1e3", c));
        CHECK(!jobs::parseMoney("1,000", c));
        CHECK(jobs::makeJobNo(2026, 1) == "JO-2026-00001");
        CHECK(jobs::makeJobNo(2026, 123456) == "JO-2026-123456");
        int y = 0, q = 0;
        CHECK(jobs::parseJobNo("JO-2026-00042", y, q) && y == 2026 && q == 42);
        CHECK(jobs::parseJobNo("JO-2026-123456", y, q) && q == 123456);
        CHECK(!jobs::parseJobNo("JO-2026-0042", y, q));
        CHECK(!jobs::parseJobNo("JO-2026-00000", y, q));
        CHECK(!jobs::parseJobNo("jo-2026-00001", y, q));
        CHECK(!jobs::parseJobNo("INV-000001", y, q));
        CHECK(!jobs::parseJobNo("", y, q));
    }

    // ---- create / read, job_no generation ----
    {
        const fs::path dir = freshDir("re_jobs_basic");
        jobs::JobStore st(dir.string());
        CHECK(st.allJobs().empty());
        std::string no1, no2, no3, noNext;
        CHECK(st.createJob(sampleJob(), no1).empty() && no1 == "JO-2026-00001");
        CHECK(st.createJob(sampleJob(), no2).empty() && no2 == "JO-2026-00002");
        CHECK(st.createJob(sampleJob("2027-01-03"), no3).empty() && no3 == "JO-2027-00001");   // counter restarts each year
        CHECK(st.createJob(sampleJob("2026-12-31"), noNext).empty() && noNext == "JO-2026-00003");
        CHECK(firstLine(dir / "jobs.csv") == "job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes");

        // a caller cannot choose its own number
        jobs::Job forced = sampleJob();
        forced.jobNo = "JO-2026-00001";
        std::string no4;
        CHECK(st.createJob(forced, no4).empty() && no4 == "JO-2026-00004");

        // blank opened = today
        jobs::Job blank = sampleJob("");
        std::string no5;
        CHECK(st.createJob(blank, no5).empty());
        CHECK(st.findJob(no5)->opened == util::formatTime(std::time(nullptr), "%Y-%m-%d"));

        // everything survives a restart (new object, same folder)
        jobs::JobStore again(dir.string());
        CHECK(again.allJobs().size() == 6);
        const auto j = again.findJob("JO-2026-00001");
        CHECK(j && j->opened == "2026-03-05" && j->customerId == 1 && j->vehicleId == 2 && j->odometer == 12345);
        CHECK(j && j->complaint == "Maingay ang makina" && j->mechanic == "Mang Tonyo" && j->status == "Open" && j->closed.empty());
        CHECK(!again.findJob("JO-2026-99999"));
        std::string no6;
        CHECK(again.createJob(sampleJob(), no6).empty() && no6 == "JO-2026-00006");   // continues after the restart
        fs::remove_all(dir);
    }

    // ---- awkward text round-trips (commas, quotes, newlines) ----
    {
        const fs::path dir = freshDir("re_jobs_text");
        jobs::JobStore st(dir.string());
        jobs::Job j = sampleJob();
        j.complaint = "Ayaw mag-start, \"clicking\" sound\nat umuusok";
        j.notes = "  =cmd|' /C calc'!A0  ";
        std::string no;
        CHECK(st.createJob(j, no).empty());
        jobs::JobStore again(dir.string());
        const auto r = again.findJob(no);
        CHECK(r && r->complaint == "Ayaw mag-start, \"clicking\" sound at umuusok");   // newline -> space, one record = one line
        CHECK(r && r->notes == "=cmd|' /C calc'!A0");
        CHECK(again.warning().empty());
        fs::remove_all(dir);
    }

    // ---- update ----
    {
        const fs::path dir = freshDir("re_jobs_update");
        jobs::JobStore st(dir.string());
        std::string a, b;
        CHECK(st.createJob(sampleJob(), a).empty() && st.createJob(sampleJob(), b).empty());
        jobs::Job j = *st.findJob(a);
        j.status = "In progress";
        j.mechanic = "Kuya Ben";
        CHECK(st.updateJob(j).empty());
        CHECK(st.findJob(a)->status == "In progress" && st.findJob(a)->mechanic == "Kuya Ben");
        CHECK(st.findJob(b)->status == "Open");                                         // the other job is untouched
        jobs::Job ghost = sampleJob();
        ghost.jobNo = "JO-2026-00777";
        CHECK(!st.updateJob(ghost).empty());
        CHECK(st.allJobs().size() == 2);
        fs::remove_all(dir);
    }

    // ---- lines ----
    {
        const fs::path dir = freshDir("re_jobs_lines");
        jobs::JobStore st(dir.string());
        std::string a, b;
        CHECK(st.createJob(sampleJob(), a).empty() && st.createJob(sampleJob(), b).empty());

        jobs::JobLine labor;
        labor.kind = "Labor";
        labor.description = "Change Oil";
        labor.laborFeeCents = 15000;
        jobs::JobLine part;
        part.kind = "Part";
        part.partId = 101;
        part.description = "Engine Oil 1L, 20W-50";
        part.quantity = 3;
        part.unitPriceCents = 85050;

        int n = 0;
        CHECK(st.addLine(a, labor, n).empty() && n == 1);
        CHECK(st.addLine(a, part, n).empty() && n == 2);
        CHECK(st.addLine(b, labor, n).empty() && n == 1);                            // numbering is per job
        CHECK(!st.addLine("JO-2026-00099", labor, n).empty());                       // no orphan lines
        CHECK(firstLine(dir / "job_lines.csv") == "job_no,line_no,kind,part_id,description,quantity,unit_price,labor_fee");

        jobs::JobStore again(dir.string());
        const auto la = again.linesOf(a);
        CHECK(la.size() == 2 && la[0].lineNo == 1 && la[1].lineNo == 2);
        CHECK(la[0].kind == "Labor" && la[0].laborFeeCents == 15000 && la[0].unitPriceCents == 0 && la[0].description == "Change Oil");
        CHECK(la[1].kind == "Part" && la[1].partId == 101 && la[1].quantity == 3 && la[1].unitPriceCents == 85050 &&
              la[1].description == "Engine Oil 1L, 20W-50");
        CHECK(again.linesOf(b).size() == 1 && again.linesOf("JO-2026-00099").empty());
        CHECK(again.allLines().size() == 3);
        CHECK(readAll(dir / "job_lines.csv").find("850.50") != std::string::npos);   // CSV shows pesos, memory holds centavos
        fs::remove_all(dir);
    }

    // ---- bad / duplicate rows are skipped, the original is kept as .bak, valid rows survive ----
    {
        const fs::path dir = freshDir("re_jobs_bad");
        {
            std::ofstream f(dir / "jobs.csv");
            f << "job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes\n"
                 "JO-2026-00001,2026-03-05,1,2,100,Ok job,Ben,Open,,\n"
                 "JO-2026-00001,2026-03-06,1,2,100,DUPLICATE number,Ben,Open,,\n"
                 "garbage row\n"
                 "JO-2026-00002,2026-03-07,x,2,100,bad customer,Ben,Open,,\n"
                 "JO-2026-00003,2026-03-08,1,2,200,Another ok job,Ben,Done,2026-03-09,note\n";
        }
        const std::string original = readAll(dir / "jobs.csv");
        jobs::JobStore st(dir.string());
        const auto all = st.allJobs();
        CHECK(all.size() == 2 && all[0].jobNo == "JO-2026-00001" && all[0].complaint == "Ok job" && all[1].jobNo == "JO-2026-00003");
        CHECK(!st.warning().empty());
        CHECK(readAll(dir / "jobs.csv.bak") == original);
        std::string no;
        CHECK(st.createJob(sampleJob(), no).empty() && no == "JO-2026-00004");   // numbers are never reused
        CHECK(st.allJobs().size() == 3 && st.warning().empty());
        fs::remove_all(dir);
    }

    // ---- no duplicate job_no under concurrency: threads + two separate stores on one folder ----
    {
        const fs::path dir = freshDir("re_jobs_conc");
        jobs::JobStore shared(dir.string());
        jobs::JobStore other(dir.string());   // behaves like a second program instance: only the file lock protects us
        constexpr int kThreads = 8, kEach = 25;
        std::vector<std::thread> pool;
        std::vector<std::vector<std::string>> got(kThreads);
        std::atomic<int> errors{0};
        for (int t = 0; t < kThreads; ++t) {
            pool.emplace_back([&, t] {
                jobs::JobStore& s = (t % 2 == 0) ? shared : other;
                for (int i = 0; i < kEach; ++i) {
                    std::string no;
                    if (!s.createJob(sampleJob(), no).empty()) { ++errors; continue; }
                    got[t].push_back(no);
                    int ln = 0;
                    jobs::JobLine l;
                    l.kind = "Labor";
                    l.description = "x";
                    l.laborFeeCents = 100;
                    if (!s.addLine(no, l, ln).empty() || ln != 1) ++errors;
                }
            });
        }
        for (auto& th : pool) th.join();
        CHECK(errors == 0);
        std::set<std::string> unique;
        for (const auto& v : got) unique.insert(v.begin(), v.end());
        CHECK(static_cast<int>(unique.size()) == kThreads * kEach);                      // nobody got the same number
        jobs::JobStore check(dir.string());
        CHECK(static_cast<int>(check.allJobs().size()) == kThreads * kEach);             // nobody's write was lost
        CHECK(static_cast<int>(check.allLines().size()) == kThreads * kEach);
        CHECK(unique.count("JO-2026-00001") == 1 && unique.count(jobs::makeJobNo(2026, kThreads * kEach)) == 1);   // no gaps either
        CHECK(check.warning().empty());
        fs::remove_all(dir);
    }

    if (failures == 0) std::cout << "ALL JOBS (4.1) TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
