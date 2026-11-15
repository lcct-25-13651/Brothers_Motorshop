// api.cpp - Phase 4: JSON REST endpoints for job orders (see api.h for the contract).
#include "api.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "crm.h"
#include "httplib.h"
#include "json.h"
#include "util.h"

namespace api {
namespace {

// ------------------------------------------------------------------ response helpers
void commonHeaders(httplib::Response& res) {
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Referrer-Policy", "same-origin");
    res.set_header("Cache-Control", "no-store");
    res.set_header("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
}

void send(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "application/json; charset=utf-8");
}

std::string errorBody(const std::string& message, const std::string& code, int status) {
    return json::obj({{"ok", json::boolean(false)},
                      {"error", json::str(message)},
                      {"code", json::str(code)},
                      {"status", json::num(status)}});
}

// Business-rule refusals are not the same as a malformed request: a client that reads `code` can show
// the right thing ("reload, somebody took the last unit" vs "this cannot be done").
const char* codeFor(int status) {
    switch (status) {
        case 404: return "not_found";
        case 409: return "conflict";
        case 403: return "forbidden";
        case 401: return "unauthorized";
        default: return "bad_request";
    }
}

int statusFor(const std::string& message) {
    if (message.find("does not exist") != std::string::npos) return 404;
    if (message.find("Not enough stock") != std::string::npos) return 409;
    if (message.find("cannot go from") != std::string::npos) return 409;
    if (message.find("no longer be") != std::string::npos) return 409;
    if (message.find("is final") != std::string::npos) return 409;
    return 400;
}

void fail(httplib::Response& res, const std::string& message) {
    const int status = statusFor(message);
    send(res, status, errorBody(message, codeFor(status), status));
}

// ------------------------------------------------------------------ request helpers
// "ABC 1234" / "abc-1234" / "ABC1234" all name the same vehicle (stored plates keep the hyphen).
std::string loosePlate(const std::string& in) {
    std::string o;
    for (unsigned char ch : in)
        if (std::isalnum(ch)) o += static_cast<char>(std::toupper(ch));
    return o;
}

// Exact match first, then ignore spaces and hyphens - people type plates every which way.
const crm::Vehicle* findPlate(const crm::Registry& reg, const std::string& text) {
    if (const crm::Vehicle* v = reg.findByPlate(text)) return v;
    const std::string want = loosePlate(text);
    if (want.empty()) return nullptr;
    for (const crm::Vehicle& v : reg.vehicles())
        if (loosePlate(v.plate) == want) return &v;
    return nullptr;
}

// A value may arrive as a JSON body field, as a query/form parameter or (for POST) in both.
std::string field(const httplib::Request& req, const json::Value& body, const std::string& name) {
    if (const json::Value* v = body.find(name)) {
        if (!v->isNull()) {
            const std::string s = v->asString();
            if (!s.empty()) return s;
        }
    }
    if (req.has_param(name)) return req.get_param_value(name);
    return "";
}

bool hasField(const httplib::Request& req, const json::Value& body, const std::string& name) {
    if (const json::Value* v = body.find(name)) if (!v->isNull()) return true;
    return req.has_param(name);
}

double moneyField(const std::string& s, double dflt) {
    if (s.empty()) return dflt;
    std::string t;
    for (char c : util::trim(s))
        if (c != ',') t += c;
    double v = 0;
    return util::parseDouble(t, 0, inv::limits::kMaxPrice, v) ? v : dflt;
}

int intField(const std::string& s, int dflt) {
    long long v = 0;
    return util::parseInt(s, 0, 999999999LL, v) ? static_cast<int>(v) : dflt;
}

// ------------------------------------------------------------------ serialisers
std::string totalsJson(const jobs::JobTotals& t) {
    return json::obj({{"parts_total", json::money(t.parts)},
                      {"labor_total", json::money(t.labor)},
                      {"grand_total", json::money(t.grand())}});
}

std::string lineJson(const jobs::JobLine& l, const Deps& deps) {
    int available = -1;
    if (l.kind == jobs::JobLine::Kind::Part && deps.inv)
        if (const inv::Part* p = deps.inv->find(l.partId)) available = p->quantity();
    return json::obj({{"line_no", json::num(l.lineNo)},
                      {"kind", json::str(jobs::JobLine::kindName(l.kind))},
                      {"part_id", json::num(l.partId)},
                      {"service_id", json::num(l.serviceId)},
                      {"description", json::str(l.description)},
                      {"quantity", json::num(l.quantity)},
                      {"unit_price", json::money(l.unitPrice)},
                      {"labor_fee", json::money(l.laborFee)},
                      {"line_total", json::money(l.lineTotal())},
                      {"available_stock", available >= 0 ? json::num(available) : ""}});
}

std::string linesArray(const std::vector<jobs::JobLine>& lines, const Deps& deps) {
    std::vector<std::string> items;
    for (const jobs::JobLine& l : lines) items.push_back(lineJson(l, deps));
    return json::arr(items);
}

std::string jobJson(const jobs::Job& j, const Deps& deps) {
    const jobs::JobTotals t = jobs::totalsOfJob(j);
    std::string customer, phone, plate, model;
    if (deps.people) {
        if (const crm::Customer* k = deps.people->findCustomer(j.customerId)) {
            customer = k->name;
            phone = k->phone;
        }
        if (const crm::Vehicle* v = deps.people->findVehicle(j.vehicleId)) {
            plate = v->plate;
            model = v->model;
        }
    }
    std::vector<std::string> next;
    for (jobs::JobStatus s : jobs::allowedNext(j.status)) next.push_back(json::str(jobs::statusName(s)));
    return json::obj({{"job_no", json::str(j.jobNo)},
                      {"opened", json::str(j.opened)},
                      {"opened_date", json::str(j.opened.size() >= 10 ? j.opened.substr(0, 10) : j.opened)},
                      {"customer_id", json::num(j.customerId)},
                      {"customer", json::str(customer)},
                      {"phone", json::str(phone)},
                      {"vehicle_id", json::num(j.vehicleId)},
                      {"plate", json::str(plate)},
                      {"vehicle_model", json::str(model)},
                      {"odometer", json::num(j.odometer)},
                      {"complaint", json::str(j.complaint)},
                      {"mechanic", json::str(j.mechanic)},
                      {"status", json::str(jobs::statusName(j.status))},
                      {"closed", json::str(j.closed)},
                      {"notes", json::str(j.notes)},
                      {"invoice_no", json::str(j.invoiceNo)},
                      {"line_count", json::num(static_cast<int>(j.lines.size()))},
                      {"parts_total", json::money(t.parts)},
                      {"labor_total", json::money(t.labor)},
                      {"grand_total", json::money(t.grand())},
                      {"locked", json::boolean(j.isLocked())},
                      {"allowed_next", next.empty() ? "" : json::arr(next)}});
}

std::string jobsArray(const std::vector<jobs::Job>& list, const Deps& deps) {
    std::vector<std::string> items;
    for (const jobs::Job& j : list) items.push_back(jobJson(j, deps));
    return json::arr(items);
}

std::string serviceJson(const jobs::ServiceItem& s) {
    return json::obj({{"id", json::num(s.id)},
                      {"name", json::str(s.name)},
                      {"default_labor_fee", json::money(s.defaultLaborFee)},
                      {"interval_months", json::num(s.intervalMonths)},
                      {"interval_km", json::num(s.intervalKm)},
                      {"active", json::boolean(s.active)},
                      {"scheduled", json::boolean(s.scheduled())}});
}

std::string partJson(const inv::Part& p) {
    return json::obj({{"id", json::num(p.id())},
                      {"part_number", json::str(p.partNumber())},
                      {"name", json::str(p.fullName())},
                      {"system", json::str(p.category())},
                      {"price", json::money(p.price())},
                      {"stock", json::num(p.quantity())},
                      {"min_stock", json::num(p.minStock())},
                      {"low_stock", json::boolean(p.isLowStock())}});
}

}  // namespace

// ============================================================================ routes
void registerRoutes(httplib::Server& svr, const Deps& deps, Authorize authorize) {
    using Body = std::function<void(const httplib::Request&, httplib::Response&, const Deps&, const Credentials&, const json::Value&)>;

    // Every route: common headers -> CORS -> session/role/CSRF gate -> parse the JSON body -> run `fn`.
    auto add = [&svr, deps, authorize](const char* method, const std::string& pattern, auth::Permission need, Body fn) {
        auto handler = [deps, authorize, need, fn](const httplib::Request& req, httplib::Response& res) {
            commonHeaders(res);
            if (!deps.corsOrigin.empty()) {
                const std::string origin = req.get_header_value("Origin");
                if (origin == deps.corsOrigin) {
                    res.set_header("Access-Control-Allow-Origin", origin);
                    res.set_header("Access-Control-Allow-Credentials", "true");
                    res.set_header("Access-Control-Allow-Headers", "Content-Type, X-CSRF-Token");
                    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
                    res.set_header("Vary", "Origin");
                }
            }
            std::unique_lock<std::mutex> lock;
            if (deps.dataLock) lock = std::unique_lock<std::mutex>(*deps.dataLock);
            Credentials cred;
            if (!authorize(req, res, need, cred)) return;
            json::Value body;
            if (!req.body.empty()) {
                if (auto parsed = json::parse(req.body)) body = *parsed;
                else if (!util::trim(req.body).empty() && req.body[0] == '{') {
                    fail(res, "The request body is not valid JSON.");
                    return;
                }
            }
            fn(req, res, deps, cred, body);
        };
        const std::string m = method;
        if (m == "GET") svr.Get(pattern, handler);
        else if (m == "POST") svr.Post(pattern, handler);
        else if (m == "DELETE") svr.Delete(pattern, handler);
        else if (m == "OPTIONS") svr.Options(pattern, handler);
    };

    // A pre-flight request is answered before the session gate (browsers send no cookies with it).
    auto preflight = [&svr, deps](const std::string& pattern) {
        svr.Options(pattern, [deps](const httplib::Request& req, httplib::Response& res) {
            commonHeaders(res);
            const std::string origin = req.get_header_value("Origin");
            if (!deps.corsOrigin.empty() && origin == deps.corsOrigin) {
                res.set_header("Access-Control-Allow-Origin", origin);
                res.set_header("Access-Control-Allow-Credentials", "true");
                res.set_header("Access-Control-Allow-Headers", "Content-Type, X-CSRF-Token");
                res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            }
            res.status = 204;
        });
    };

    using P = auth::Permission;

    // ---------------------------------------------------------------- GET /api/jobs - the job list
    add("GET", "/api/jobs", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials&,
                                        const json::Value&) {
        jobs::JobFilter f;
        const std::string status = req.get_param_value("status");
        if (!status.empty()) {
            if (auto s = jobs::parseStatus(status)) f.status = *s;
            else {
                fail(res, "\"" + status + "\" is not a job status. Use Open, In progress, Done, Released or Cancelled.");
                return;
            }
        }
        f.mechanic = req.get_param_value("mechanic");
        f.date = req.get_param_value("date");
        f.from = req.get_param_value("from");
        f.to = req.get_param_value("to");
        f.plate = req.get_param_value("plate");
        f.query = req.get_param_value("q");
        f.customerId = intField(req.get_param_value("customer_id"), 0);
        f.vehicleId = intField(req.get_param_value("vehicle_id"), 0);
        const int limit = intField(req.get_param_value("limit"), 200);
        f.limit = static_cast<size_t>(std::max(0, std::min(limit, 1000)));
        f.newestFirst = req.get_param_value("order") != "oldest";

        const std::vector<jobs::Job> list = deps.jobs->list(f);
        jobs::JobTotals sum;
        int openCount = 0;
        for (const jobs::Job& j : list) {
            const jobs::JobTotals t = jobs::totalsOfJob(j);
            sum.parts = util::round2(sum.parts + t.parts);
            sum.labor = util::round2(sum.labor + t.labor);
            if (jobs::isOpen(j.status)) ++openCount;
        }
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"count", json::num(static_cast<int>(list.size()))},
                        {"open_count", json::num(openCount)},
                        {"jobs", jobsArray(list, deps)},
                        {"totals", totalsJson(sum)}}));
    });

    // ---------------------------------------------------------------- POST /api/jobs - open a job
    add("POST", "/api/jobs", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials& cred,
                                         const json::Value& body) {
        jobs::CreateJob in;
        in.customerId = intField(field(req, body, "customer_id"), 0);
        in.vehicleId = intField(field(req, body, "vehicle_id"), 0);
        in.odometer = intField(field(req, body, "odometer"), 0);
        in.complaint = field(req, body, "complaint");
        in.mechanic = field(req, body, "mechanic");
        in.notes = field(req, body, "notes");
        const std::string plate = field(req, body, "plate");
        // The UI usually knows the plate, not the internal id.
        if (in.vehicleId == 0 && !plate.empty() && deps.people) {
            if (const crm::Vehicle* v = findPlate(*deps.people, plate)) {
                in.vehicleId = v->id;
                if (in.customerId == 0) in.customerId = v->customerId;
            } else {
                fail(res, "No vehicle with plate \"" + util::trim(plate) + "\" is on file. Add it on the vehicle's page first.");
                return;
            }
        }
        const auto r = deps.jobs->createJob(in, cred.user);
        if (!r.ok) {
            fail(res, r.message);
            return;
        }
        send(res, 201,
             json::obj({{"ok", json::boolean(true)}, {"message", json::str(r.message)}, {"job", jobJson(r.job, deps)}}));
    });

    // ---------------------------------------------------------------- GET /api/jobs/<no>
    add("GET", "/api/jobs/([^/]+)", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials&,
                                                const json::Value&) {
        const std::string no = util::trim(req.matches[1].str());
        const auto j = deps.jobs->get(no);
        if (!j) {
            fail(res, "Job " + no + " does not exist.");
            return;
        }
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"job", jobJson(*j, deps)},
                        {"lines", linesArray(j->lines, deps)},
                        {"totals", totalsJson(jobs::totalsOfJob(*j))}}));
    });

    // ---------------------------------------------------------------- POST /api/jobs/<no> - edit details
    add("POST", "/api/jobs/([^/]+)", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials& cred,
                                                 const json::Value& body) {
        const std::string no = util::trim(req.matches[1].str());
        const auto current = deps.jobs->get(no);
        if (!current) {
            fail(res, "Job " + no + " does not exist.");
            return;
        }
        // A mechanic sent here is accepted too, so one "save" from the UI is enough.
        std::string message;
        bool changed = false;
        if (hasField(req, body, "mechanic")) {
            const auto r = deps.jobs->assignMechanic(no, field(req, body, "mechanic"), cred.user);
            if (!r.ok) {
                fail(res, r.message);   // e.g. the job is Released
                return;
            }
            message = r.message;
            changed = true;
        }
        jobs::JobEdit ed;
        if (hasField(req, body, "odometer")) ed.odometer = intField(field(req, body, "odometer"), 0);
        if (hasField(req, body, "complaint")) ed.complaint = field(req, body, "complaint");
        if (hasField(req, body, "notes")) ed.notes = field(req, body, "notes");
        if (ed.odometer || ed.complaint || ed.notes) {
            const auto r = deps.jobs->updateJob(no, ed, cred.user);
            if (!r.ok) {
                fail(res, r.message);
                return;
            }
            if (r.message != "Nothing to change.") {
                message = message.empty() ? r.message : message + " " + r.message;
                changed = true;
            }
        }
        const auto after = deps.jobs->get(no);
        if (!after) {
            fail(res, "Job " + no + " does not exist.");
            return;
        }
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"message", json::str(changed ? message : std::string("Nothing to change."))},
                        {"job", jobJson(*after, deps)}}));
    });

    // ---------------------------------------------------------------- POST /api/jobs/<no>/status
    add("POST", "/api/jobs/([^/]+)/status", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                        const Credentials& cred, const json::Value& body) {
        const std::string no = util::trim(req.matches[1].str());
        const std::string want = field(req, body, "status");
        const auto to = jobs::parseStatus(want);
        if (!to) {
            fail(res, "Give a status of Open, In progress, Done, Released or Cancelled.");
            return;
        }
        const auto r = deps.jobs->changeStatus(no, *to, cred.user, field(req, body, "note"));
        if (!r.ok) {
            fail(res, r.message);
            return;
        }
        const auto j = deps.jobs->get(no);
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"message", json::str(r.message)},
                        {"status", json::str(jobs::statusName(*to))},
                        {"job", j ? jobJson(*j, deps) : ""}}));
    });

    // ---------------------------------------------------------------- POST /api/jobs/<no>/mechanic
    add("POST", "/api/jobs/([^/]+)/mechanic", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                          const Credentials& cred, const json::Value& body) {
        const std::string no = util::trim(req.matches[1].str());
        const auto r = deps.jobs->assignMechanic(no, field(req, body, "mechanic"), cred.user);
        if (!r.ok) {
            fail(res, r.message);
            return;
        }
        const auto j = deps.jobs->get(no);
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"message", json::str(r.message)},
                        {"job", j ? jobJson(*j, deps) : ""}}));
    });

    // ---------------------------------------------------------------- POST /api/jobs/<no>/cancel
    add("POST", "/api/jobs/([^/]+)/cancel", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                        const Credentials& cred, const json::Value& body) {
        const std::string no = util::trim(req.matches[1].str());
        const auto r = deps.jobs->cancelJob(no, field(req, body, "reason"), cred.user);
        if (!r.ok) {
            fail(res, r.message);
            return;
        }
        const auto j = deps.jobs->get(no);
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"message", json::str(r.message)},
                        {"job", j ? jobJson(*j, deps) : ""}}));
    });

    // ---------------------------------------------------------------- GET /api/jobs/<no>/lines
    add("GET", "/api/jobs/([^/]+)/lines", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                      const Credentials&, const json::Value&) {
        const std::string no = util::trim(req.matches[1].str());
        const auto r = deps.jobs->lines(no);
        if (!r.ok) {
            fail(res, r.message);
            return;
        }
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"job_no", json::str(no)},
                        {"count", json::num(static_cast<int>(r.lines.size()))},
                        {"lines", linesArray(r.lines, deps)},
                        {"totals", totalsJson(r.totals)}}));
    });

    // ---------------------------------------------------------------- POST /api/jobs/<no>/lines - add
    // A Part line takes the units off the shelf there and then; a Labour line only carries a fee.
    add("POST", "/api/jobs/([^/]+)/lines", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                       const Credentials& cred, const json::Value& body) {
        const std::string no = util::trim(req.matches[1].str());
        const std::string kindText = field(req, body, "kind");
        const auto kind = jobs::JobLine::parseKind(kindText.empty() ? "Part" : kindText);
        if (!kind) {
            fail(res, "kind must be Part or Labor.");
            return;
        }
        jobs::AddLine in;
        in.kind = *kind;
        in.partId = intField(field(req, body, "part_id"), 0);
        in.serviceId = intField(field(req, body, "service_id"), 0);
        in.description = field(req, body, "description");
        in.quantity = std::max(1, intField(field(req, body, "quantity"), 1));
        in.laborFee = hasField(req, body, "labor_fee") ? moneyField(field(req, body, "labor_fee"), 0.0) : -1.0;
        if (in.kind == jobs::JobLine::Kind::Part && in.partId == 0) {
            // "part" / part number typed instead of the internal id
            const std::string ref = field(req, body, "part");
            if (!ref.empty() && deps.inv) {
                if (const inv::Part* byNumber = deps.inv->findByNumber(ref))
                    in.partId = byNumber->id();
                else {
                    long long id = 0;
                    if (util::parseInt(ref, 1, inv::limits::kMaxId, id)) in.partId = static_cast<int>(id);
                }
            }
            if (in.partId == 0) {
                fail(res, "A part line needs part_id (or part, the part number).");
                return;
            }
        }
        const auto r = deps.jobs->addLine(no, in, cred.user);
        if (!r.ok) {
            fail(res, r.message);
            return;
        }
        send(res, 201,
             json::obj({{"ok", json::boolean(true)},
                        {"message", json::str(r.message)},
                        {"job_no", json::str(no)},
                        {"line", lineJson(r.line, deps)},
                        {"lines", linesArray(r.lines, deps)},
                        {"totals", totalsJson(r.totals)}}));
    });

    // ---------------------------------------------------------------- DELETE /api/jobs/<no>/lines/<line>
    auto removeLine = [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials& cred,
                         const json::Value&) {
        const std::string no = util::trim(req.matches[1].str());
        const int lineNo = intField(util::trim(req.matches[2].str()), 0);
        if (lineNo < 1) {
            fail(res, "Give the line number to remove.");
            return;
        }
        const auto r = deps.jobs->removeLine(no, lineNo, cred.user);
        if (!r.ok) {
            fail(res, r.message);
            return;
        }
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"message", json::str(r.message)},
                        {"job_no", json::str(no)},
                        {"line", lineJson(r.line, deps)},
                        {"lines", linesArray(r.lines, deps)},
                        {"totals", totalsJson(r.totals)}}));
    };
    add("DELETE", "/api/jobs/([^/]+)/lines/([0-9]+)", P::Jobs, removeLine);
    // Same thing over POST, for clients that cannot send a DELETE (plain HTML forms).
    add("POST", "/api/jobs/([^/]+)/lines/([0-9]+)/delete", P::Jobs, removeLine);

    // ---------------------------------------------------------------- GET /api/jobs/<no>/card - print
    add("GET", "/api/jobs/([^/]+)/card", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                     const Credentials&, const json::Value&) {
        const std::string no = util::trim(req.matches[1].str());
        const auto c = deps.jobs->card(no);
        if (!c.ok) {
            fail(res, c.message);
            return;
        }
        std::vector<std::string> next;
        for (const std::string& s : c.allowedNextNames) next.push_back(json::str(s));
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"shop", json::str(deps.shopName)},
                        {"printed", json::str(util::nowStamp())},
                        {"job", jobJson(c.job, deps)},
                        {"customer_name", json::str(c.customerName.empty() ? std::string("Walk-in customer") : c.customerName)},
                        {"customer_phone", json::str(c.customerPhone)},
                        {"plate", json::str(c.vehiclePlate)},
                        {"vehicle_model", json::str(c.vehicleModel)},
                        {"lines", linesArray(c.lines, deps)},
                        {"totals", totalsJson(c.totals)},
                        {"allowed_next", next.empty() ? "" : json::arr(next)}}));
    });

    // ---------------------------------------------------------------- GET /api/jobs/<no>/history
    add("GET", "/api/jobs/([^/]+)/history", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                        const Credentials&, const json::Value&) {
        const std::string no = util::trim(req.matches[1].str());
        if (!deps.jobs->get(no)) {
            fail(res, "Job " + no + " does not exist.");
            return;
        }
        std::vector<std::string> items;
        for (const jobs::JobEvent& e : deps.jobs->history(no))
            items.push_back(json::obj({{"timestamp", json::str(e.timestamp)},
                                       {"user", json::str(e.user)},
                                       {"event", json::str(e.event)},
                                       {"details", json::str(e.details)}}));
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"job_no", json::str(no)},
                        {"count", json::num(static_cast<int>(items.size()))},
                        {"events", json::arr(items)}}));
    });

    // ---------------------------------------------------------------- GET /api/services
    add("GET", "/api/services", P::Jobs, [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials&,
                                            const json::Value&) {
        const bool all = req.get_param_value("all") == "1" || req.get_param_value("all") == "true";
        std::vector<std::string> items;
        for (const jobs::ServiceItem& s : deps.jobs->services().all())
            if (all || s.active) items.push_back(serviceJson(s));
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"count", json::num(static_cast<int>(items.size()))},
                        {"services", json::arr(items)}}));
    });

    // ---------------------------------------------------------------- POST /api/services (Admin)
    add("POST", "/api/services", P::ServiceCatalog, [](const httplib::Request& req, httplib::Response& res, const Deps& deps,
                                                       const Credentials& cred, const json::Value& body) {
        jobs::ServiceItem s;
        const int id = intField(field(req, body, "id"), 0);
        s.name = field(req, body, "name");
        s.defaultLaborFee = moneyField(field(req, body, "default_labor_fee"), 0.0);
        s.intervalMonths = intField(field(req, body, "interval_months"), 0);
        s.intervalKm = intField(field(req, body, "interval_km"), 0);
        int newId = 0;
        const std::string err = deps.jobs->saveService(id, s, newId, cred.user);
        if (!err.empty()) {
            fail(res, err);
            return;
        }
        const jobs::ServiceItem* saved = deps.jobs->services().find(newId);
        send(res, id == 0 ? 201 : 200,
             json::obj({{"ok", json::boolean(true)},
                        {"message", json::str(id == 0 ? "Service added." : "Service updated.")},
                        {"service", saved ? serviceJson(*saved) : ""}}));
    });

    // ---------------------------------------------------------------- POST /api/services/<id>/active (Admin)
    add("POST", "/api/services/([0-9]+)/active", P::ServiceCatalog, [](const httplib::Request& req, httplib::Response& res,
                                                                      const Deps& deps, const Credentials& cred, const json::Value& body) {
        const int id = intField(util::trim(req.matches[1].str()), 0);
        const std::string v = field(req, body, "active");
        const bool on = v.empty() ? true : !(v == "0" || util::equalsIgnoreCase(v, "false"));
        const std::string err = deps.jobs->setServiceActive(id, on, cred.user);
        if (!err.empty()) {
            fail(res, err);
            return;
        }
        const jobs::ServiceItem* s = deps.jobs->services().find(id);
        send(res, 200, json::obj({{"ok", json::boolean(true)}, {"service", s ? serviceJson(*s) : ""}}));
    });

    // ---------------------------------------------------------------- GET /api/vehicles - plates for the job form
    add("GET", "/api/vehicles", P::Customers, [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials&,
                                                 const json::Value&) {
        const std::string q = util::toLower(util::trim(req.get_param_value("q")));
        const int limit = std::max(1, std::min(intField(req.get_param_value("limit"), 200), 1000));
        std::vector<std::string> items;
        if (deps.people) {
            for (const crm::Vehicle& v : deps.people->vehicles()) {
                const crm::Customer* k = deps.people->findCustomer(v.customerId);
                if (v.archived || (k && k->archived)) continue;   // archived vehicles cannot take a job
                if (!q.empty()) {
                    std::string hay = util::toLower(v.plate + " " + v.model);
                    if (k) hay += " " + util::toLower(k->name);
                    if (hay.find(q) == std::string::npos) continue;
                }
                items.push_back(json::obj({{"id", json::num(v.id)},
                                           {"plate", json::str(v.plate)},
                                           {"model", json::str(v.model)},
                                           {"customer_id", json::num(v.customerId)},
                                           {"customer", json::str(k ? k->name : std::string())}}));
                if (static_cast<int>(items.size()) >= limit) break;
            }
        }
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"count", json::num(static_cast<int>(items.size()))},
                        {"vehicles", json::arr(items)}}));
    });

    // ---------------------------------------------------------------- GET /api/parts - picker for lines
    add("GET", "/api/parts", P::ViewInventory, [](const httplib::Request& req, httplib::Response& res, const Deps& deps, const Credentials&,
                                                  const json::Value&) {
        const std::string q = util::toLower(util::trim(req.get_param_value("q")));
        const int limit = std::max(1, std::min(intField(req.get_param_value("limit"), 50), 500));
        const bool inStockOnly = req.get_param_value("in_stock") == "1";
        std::vector<std::string> items;
        int matches = 0;
        for (const inv::Part& p : deps.inv->parts()) {
            if (inStockOnly && p.quantity() <= 0) continue;
            if (!q.empty()) {
                bool hits = util::containsIgnoreCase(p.fullName(), q) || util::containsIgnoreCase(p.category(), q) ||
                            util::containsIgnoreCase(p.partNumber(), q) || util::containsIgnoreCase(p.compatibleModels(), q) ||
                            util::containsIgnoreCase(std::to_string(p.id()), q);
                if (!hits) continue;
            }
            ++matches;
            if (static_cast<int>(items.size()) < limit) items.push_back(partJson(p));
        }
        send(res, 200,
             json::obj({{"ok", json::boolean(true)},
                        {"count", json::num(static_cast<int>(items.size()))},
                        {"matches", json::num(matches)},
                        {"parts", json::arr(items)}}));
    });

    // Browsers send OPTIONS before a cross-origin POST/DELETE with a JSON body.
    preflight("/api/jobs");
    preflight("/api/jobs/([^/]+)");
    preflight("/api/jobs/([^/]+)/lines");
    preflight("/api/jobs/([^/]+)/lines/([0-9]+)");
    preflight("/api/jobs/([^/]+)/status");
    preflight("/api/jobs/([^/]+)/mechanic");
    preflight("/api/jobs/([^/]+)/cancel");
    preflight("/api/services");
    preflight("/api/services/([0-9]+)/active");
    preflight("/api/parts");
    preflight("/api/vehicles");
}

}  // namespace api
