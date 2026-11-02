// End-to-end test for the Phase 4 JSON API (/api/*). Runs api.cpp against the in-process httplib stub,
// so no sockets are involved - the same trick tests/web_e2e.cpp uses for the HTML layer.
//
//   make apie2e
//
// It exercises the endpoints the way a browser would: session + CSRF gate (supplied here by a stand-in
// for the web layer), JSON bodies, status codes, and the stock rules behind them.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "../src/api.h"
#include "../src/auth.h"
#include "../src/crm.h"
#include "../src/inventory.h"
#include "../src/jobs.h"
#include "../src/json.h"
#include "httplib.h"

static int fails = 0;
#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; ++fails; } \
    } while (0)

using P = httplib::Params;

namespace {

// Stands in for web.cpp's session gate: it is the part the API deliberately does not own.
struct FakeSession {
    bool loggedIn = true;
    std::string user = "boss";
    auth::Role role = auth::Role::Admin;
    std::string csrf = "csrftokencsrftokencsrftokencsrftokencsrftokencsrftokencsrftokencsrf";
    bool sendCsrf = true;

    api::Authorize fn() {
        return [this](const httplib::Request& req, httplib::Response& res, auth::Permission need, api::Credentials& out) -> bool {
            auto deny = [&res](int status, const std::string& error) {
                res.status = status;
                res.set_content(json::obj({{"ok", json::boolean(false)}, {"error", json::str(error)}}), "application/json");
                return false;
            };
            if (!loggedIn) return deny(401, "Not signed in.");
            if (!auth::allowed(role, need)) return deny(403, "Your role is not allowed to do that.");
            if (req.method != "GET" && req.method != "OPTIONS") {
                const std::string header = req.get_header_value("X-CSRF-Token");
                const std::string given = !header.empty() ? header : req.get_param_value("csrf");
                if (!sendCsrf || given != csrf) return deny(403, "Security token missing or expired.");
            }
            out.user = user;
            out.role = role;
            out.ip = req.remote_addr;
            return true;
        };
    }
};

json::Value parse(const std::string& text) {
    auto v = json::parse(text);
    if (!v) {
        std::cerr << "FAIL: response is not JSON: " << text.substr(0, 200) << "\n";
        ++fails;
        return json::Value{};
    }
    return *v;
}
std::string text(const json::Value& v, const std::string& key) { return v.find(key) ? v.find(key)->asString() : std::string(); }

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / "brothers_api_e2e").string();
    fs::remove_all(dir);
    fs::create_directories(dir);

    inv::Inventory in(dir);
    auth::AuditLog audit(dir + "/audit.csv");
    crm::Registry people;
    people.open(dir);
    jobs::JobBook book;
    book.open(dir, in, &audit, &people);

    crm::Customer k;
    k.name = "Juan dela Cruz";
    k.phone = "09171234567";
    int cid = 0;
    CHECK(people.saveCustomer(0, k, cid).empty());
    crm::Vehicle v;
    v.customerId = cid;
    v.plate = "ABC 1234";
    v.model = "Bajaj RE 4S";
    int vid = 0;
    CHECK(people.saveVehicle(0, v, vid).empty());

    FakeSession sess;
    httplib::Server svr;
    api::Deps deps;
    deps.inv = &in;
    deps.jobs = &book;
    deps.people = &people;
    deps.audit = &audit;
    deps.shopName = "Brothers Motorshop";
    deps.corsOrigin = "http://localhost:5173";
    api::registerRoutes(svr, deps, sess.fn());

    const std::map<std::string, std::string> csrfHeader{{"X-CSRF-Token", sess.csrf}};
    const std::map<std::string, std::string> jsonType{{"Content-Type", "application/json"}};

    // ---------------------------------------------------------------- service catalogue
    {
        auto r = svr.call("GET", "/api/services");
        CHECK(r.status == 200);
        CHECK(r.headers["Content-Type"] == "application/json; charset=utf-8");
        auto j = parse(r.body);
        CHECK(j.find("ok")->asBool() == true);
        CHECK(j.find("services")->items.size() == 3);
        CHECK(text(j.find("services")->items[0], "name") == "Change Oil");
        r = svr.call("GET", "/api/services", {{"all", "1"}});
        CHECK(parse(r.body).find("count")->asInt() == 3);
    }

    // ---------------------------------------------------------------- open a job
    std::string no;
    {
        auto r = svr.call("POST", "/api/jobs",
                          {{"customer_id", std::to_string(cid)},
                           {"vehicle_id", std::to_string(vid)},
                           {"odometer", "23450"},
                           {"complaint", "Engine knocking"},
                           {"mechanic", "Boy"}},
                          "", "", csrfHeader);
        CHECK(r.status == 201);
        auto j = parse(r.body);
        no = text(*j.find("job"), "job_no");
        CHECK(no == "JOB-000001");
        CHECK(text(*j.find("job"), "status") == "Open");
        CHECK(text(*j.find("job"), "customer") == "Juan dela Cruz");
        CHECK(text(*j.find("job"), "plate") == "ABC1234");
        CHECK(j.find("job")->find("locked")->asBool() == false);
        CHECK(j.find("job")->find("allowed_next")->items.size() == 2);
        CHECK(j.find("job")->find("allowed_next")->items[0].asString() == "In progress");
    }
    // opened by plate instead of the internal id
    {
        auto r = svr.call("POST", "/api/jobs", {{"plate", "abc-1234"}, {"complaint", "Change oil"}}, "", "", csrfHeader);
        CHECK(r.status == 201);
        auto j = parse(r.body);
        CHECK(text(*j.find("job"), "job_no") == "JOB-000002");
        CHECK(text(*j.find("job"), "customer") == "Juan dela Cruz");   // the vehicle's owner
        r = svr.call("POST", "/api/jobs", {{"plate", "ZZZ 9999"}}, "", "", csrfHeader);
        CHECK(r.status == 400);
        CHECK(text(parse(r.body), "code") == "bad_request");
        CHECK(text(parse(r.body), "error").find("No vehicle with plate") != std::string::npos);
    }

    // ---------------------------------------------------------------- lines move stock
    {
        const int before = in.find(101)->quantity();
        auto r = svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":"Part","part_id":101,"quantity":2})", csrfHeader);
        CHECK(r.status == 201);
        auto j = parse(r.body);
        CHECK(text(*j.find("line"), "description").find("Engine Oil") != std::string::npos);
        CHECK(j.find("line")->find("unit_price")->asNumber() == 450.0);
        CHECK(j.find("totals")->find("parts_total")->asNumber() == 900.0);
        CHECK(in.find(101)->quantity() == before - 2);
        CHECK(book.issuedFor(no, 101) == 2);

        // labour from the catalogue
        r = svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":"Labor","service_id":1})", csrfHeader);
        CHECK(r.status == 201);
        j = parse(r.body);
        CHECK(j.find("line")->find("labor_fee")->asNumber() == 300.0);
        CHECK(j.find("totals")->find("labor_total")->asNumber() == 300.0);
        CHECK(j.find("totals")->find("grand_total")->asNumber() == 1200.0);

        // more than the shelf holds
        r = svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":"Part","part_id":105,"quantity":99})", csrfHeader);
        CHECK(r.status == 409);
        CHECK(text(parse(r.body), "code") == "conflict");
        CHECK(text(parse(r.body), "error").find("Not enough stock") != std::string::npos);
        CHECK(in.find(105)->quantity() == 3);

        // a broken body
        r = svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":)", csrfHeader);
        CHECK(r.status == 400);
        CHECK(text(parse(r.body), "error").find("not valid JSON") != std::string::npos);

        r = svr.call("GET", "/api/jobs/" + no + "/lines");
        CHECK(r.status == 200);
        CHECK(parse(r.body).find("count")->asInt() == 2);
    }

    // ---------------------------------------------------------------- removing a line returns stock
    {
        const int before = in.find(101)->quantity();
        auto r = svr.call("DELETE", "/api/jobs/" + no + "/lines/1", {}, "", "", csrfHeader);
        CHECK(r.status == 200);
        CHECK(in.find(101)->quantity() == before + 2);
        CHECK(book.issuedFor(no, 101) == 0);
        CHECK(parse(r.body).find("totals")->find("parts_total")->asNumber() == 0.0);
        r = svr.call("DELETE", "/api/jobs/" + no + "/lines/1", {}, "", "", csrfHeader);
        CHECK(r.status == 404);
        // same thing over POST for clients that cannot send DELETE
        r = svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":"Part","part_id":101,"quantity":1})", csrfHeader);
        CHECK(r.status == 201);
        const int now = in.find(101)->quantity();
        r = svr.call("POST", "/api/jobs/" + no + "/lines/3/delete", {}, "", "", csrfHeader);
        CHECK(r.status == 200);
        CHECK(in.find(101)->quantity() == now + 1);
    }

    // ---------------------------------------------------------------- status buttons
    {
        auto r = svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"Released"})", csrfHeader);
        CHECK(r.status == 409);                                    // Open -> Released is not allowed
        CHECK(text(parse(r.body), "error").find("cannot go from") != std::string::npos);
        r = svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"Nonsense"})", csrfHeader);
        CHECK(r.status == 400);
        r = svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"In progress"})", csrfHeader);
        CHECK(r.status == 200);
        r = svr.call("POST", "/api/jobs/" + no + "/mechanic", {}, "", R"({"mechanic":"Karding"})", csrfHeader);
        CHECK(r.status == 200);
        CHECK(text(*parse(r.body).find("job"), "mechanic") == "Karding");
        r = svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"Done"})", csrfHeader);
        CHECK(r.status == 200);
        // Done is not editable - the UI must reopen the job first
        r = svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":"Part","part_id":103,"quantity":1})", csrfHeader);
        CHECK(r.status == 409);
        r = svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"In progress"})", csrfHeader);
        CHECK(r.status == 200);
        r = svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":"Part","part_id":103,"quantity":1})", csrfHeader);
        CHECK(r.status == 201);
        r = svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"Done"})", csrfHeader);
        CHECK(r.status == 200);
        r = svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"Released"})", csrfHeader);
        CHECK(r.status == 200);
        auto j = parse(r.body);
        CHECK(text(*j.find("job"), "status") == "Released");
        CHECK(j.find("job")->find("locked")->asBool() == true);
        CHECK(!text(*j.find("job"), "closed").empty());

        // released = frozen
        const int stock = in.find(103)->quantity();
        CHECK(svr.call("POST", "/api/jobs/" + no + "/lines", {}, "", R"({"kind":"Part","part_id":103,"quantity":1})", csrfHeader).status == 409);
        CHECK(svr.call("DELETE", "/api/jobs/" + no + "/lines/4", {}, "", "", csrfHeader).status == 409);
        CHECK(svr.call("POST", "/api/jobs/" + no + "/mechanic", {}, "", R"({"mechanic":"Pedro"})", csrfHeader).status == 409);
        CHECK(svr.call("POST", "/api/jobs/" + no, {}, "", R"({"odometer":99999})", csrfHeader).status == 409);
        CHECK(svr.call("POST", "/api/jobs/" + no + "/status", {}, "", R"({"status":"Done"})", csrfHeader).status == 409);
        CHECK(in.find(103)->quantity() == stock);
    }

    // ---------------------------------------------------------------- cancelling returns everything
    {
        const int b101 = in.find(101)->quantity(), b103 = in.find(103)->quantity();
        auto r = svr.call("POST", "/api/jobs", {{"customer_id", std::to_string(cid)}, {"complaint", "Brake job"}}, "", "", csrfHeader);
        const std::string no2 = text(*parse(r.body).find("job"), "job_no");
        CHECK(svr.call("POST", "/api/jobs/" + no2 + "/lines", {}, "", R"({"kind":"Part","part_id":101,"quantity":3})", csrfHeader).status == 201);
        CHECK(svr.call("POST", "/api/jobs/" + no2 + "/lines", {}, "", R"({"kind":"Part","part_id":103,"quantity":4})", csrfHeader).status == 201);
        const int mid101 = in.find(101)->quantity(), mid103 = in.find(103)->quantity();
        CHECK(mid101 == b101 - 3 && mid103 == b103 - 4);
        r = svr.call("POST", "/api/jobs/" + no2 + "/cancel", {}, "", R"({"reason":"Customer changed mind"})", csrfHeader);
        CHECK(r.status == 200);
        CHECK(in.find(101)->quantity() == b101 && in.find(103)->quantity() == b103);
        CHECK(book.issuedFor(no2, 101) == 0 && book.issuedFor(no2, 103) == 0);
        CHECK(text(*parse(r.body).find("job"), "status") == "Cancelled");
        CHECK(svr.call("POST", "/api/jobs/" + no2 + "/status", {}, "", R"({"status":"Open"})", csrfHeader).status == 409);
    }

    // ---------------------------------------------------------------- list, filters, card, history
    {
        auto r = svr.call("GET", "/api/jobs");
        auto j = parse(r.body);
        CHECK(j.find("count")->asInt() == 3);
        CHECK(j.find("totals")->find("grand_total")->asNumber() > 0);
        r = svr.call("GET", "/api/jobs", {{"status", "Open"}});
        CHECK(parse(r.body).find("count")->asInt() == 1);
        r = svr.call("GET", "/api/jobs", {{"status", "Rele ased"}});
        CHECK(r.status == 400);
        r = svr.call("GET", "/api/jobs", {{"plate", "ABC1234"}});
        CHECK(parse(r.body).find("count")->asInt() == 2);
        r = svr.call("GET", "/api/jobs", {{"mechanic", "Karding"}});
        CHECK(parse(r.body).find("count")->asInt() == 1);
        r = svr.call("GET", "/api/jobs", {{"q", "knocking"}});
        CHECK(parse(r.body).find("count")->asInt() == 1);
        r = svr.call("GET", "/api/jobs", {{"limit", "1"}});
        CHECK(parse(r.body).find("count")->asInt() == 1);

        r = svr.call("GET", "/api/jobs/" + no + "/card");
        CHECK(r.status == 200);
        j = parse(r.body);
        CHECK(text(j, "shop") == "Brothers Motorshop");
        CHECK(text(j, "customer_name") == "Juan dela Cruz");
        CHECK(text(j, "plate") == "ABC1234");
        CHECK(j.find("lines")->items.size() == 2);
        CHECK(j.find("allowed_next") == nullptr || j.find("allowed_next")->items.empty());   // released: nothing left

        r = svr.call("GET", "/api/jobs/" + no + "/history");
        j = parse(r.body);
        CHECK(j.find("count")->asInt() >= 6);
        bool sawStatus = false;
        for (const json::Value& e : j.find("events")->items)
            if (text(e, "event") == "JOB_STATUS") sawStatus = true;
        CHECK(sawStatus);

        r = svr.call("GET", "/api/jobs/JOB-999999");
        CHECK(r.status == 404);
        CHECK(text(parse(r.body), "code") == "not_found");
    }

    // ---------------------------------------------------------------- part picker
    {
        auto r = svr.call("GET", "/api/parts", {{"q", "oil"}});
        auto j = parse(r.body);
        CHECK(j.find("count")->asInt() == 1);
        CHECK(text(j.find("parts")->items[0], "name").find("Engine Oil") != std::string::npos);
        r = svr.call("GET", "/api/parts", {{"q", "brake"}, {"limit", "5"}});
        CHECK(parse(r.body).find("count")->asInt() >= 1);
        r = svr.call("GET", "/api/parts", {{"q", "101"}});
        CHECK(parse(r.body).find("count")->asInt() == 1);
    }

    // ---------------------------------------------------------------- roles, CSRF, CORS
    {
        sess.role = auth::Role::Staff;
        CHECK(svr.call("GET", "/api/services").status == 200);
        CHECK(svr.call("POST", "/api/services", {}, "", R"({"name":"Wash"})", csrfHeader).status == 403);
        CHECK(svr.call("POST", "/api/services/1/active", {}, "", R"({"active":"0"})", csrfHeader).status == 403);
        CHECK(svr.call("POST", "/api/jobs", {{"complaint", "x"}}, "", "", csrfHeader).status == 201);   // jobs are for everyone
        sess.role = auth::Role::Admin;

        sess.sendCsrf = false;                                     // a cross-site form post
        CHECK(svr.call("POST", "/api/jobs", {{"complaint", "x"}}, "", "", csrfHeader).status == 403);
        CHECK(svr.call("POST", "/api/jobs", {{"complaint", "x"}, {"csrf", "wrong"}}).status == 403);
        sess.sendCsrf = true;
        // the token may come as a form field too (HTML forms)
        CHECK(svr.call("POST", "/api/jobs", {{"complaint", "x"}, {"csrf", sess.csrf}}).status == 201);

        sess.loggedIn = false;
        CHECK(svr.call("GET", "/api/jobs").status == 401);
        sess.loggedIn = true;

        auto pre = svr.call("OPTIONS", "/api/jobs", {}, "", "", {{"Origin", "http://localhost:5173"}});
        CHECK(pre.status == 204);
        CHECK(pre.headers["Access-Control-Allow-Origin"] == "http://localhost:5173");
        auto wrong = svr.call("OPTIONS", "/api/jobs", {}, "", "", {{"Origin", "http://evil.example"}});
        CHECK(wrong.headers.count("Access-Control-Allow-Origin") == 0);
    }

    // ---------------------------------------------------------------- what landed on disk
    {
        CHECK(std::filesystem::exists(dir + "/jobs.csv"));
        CHECK(std::filesystem::exists(dir + "/job_lines.csv"));
        CHECK(std::filesystem::exists(dir + "/services.csv"));
        inv::Inventory in2(dir);                    // re-read everything from disk
        jobs::JobBook book2;
        book2.open(dir, in2, &audit, &people);
        CHECK(book2.all().size() == book.all().size());
        CHECK(book2.warning().empty());             // no stock drift after a restart
        int issues = 0;
        for (const jobs::Job& j : book2.all()) {
            for (const jobs::JobLine& l : j.lines)
                if (l.kind == jobs::JobLine::Kind::Part && j.status != jobs::JobStatus::Cancelled && book2.issuedFor(j.jobNo, l.partId) != l.quantity)
                    ++issues;
        }
        CHECK(issues == 0);
    }

    fs::remove_all(dir);
    if (fails == 0) std::cout << "ALL PHASE 4 API TESTS PASSED\n";
    return fails == 0 ? 0 : 1;
}
