// api.h - Phase 4: the JSON REST layer the web UI talks to (everything under /api/).
//
// The endpoints are plain HTTP + JSON so any front-end can use them (fetch from the pages this server
// serves, or a separate TypeScript app). Auth, sessions and CSRF stay where they already are: the web
// layer hands this module an `Authorize` callback and this module only deals with job-order logic.
//
// Every answer is JSON:
//   success  {"ok":true, ...}
//   failure  {"ok":false,"error":"human readable","code":"not_found|conflict|bad_request|...","status":404}
#pragma once
#include <functional>
#include <mutex>
#include <string>

#include "auth.h"
#include "inventory.h"
#include "jobs.h"

namespace crm { class Registry; }
namespace httplib {
struct Request;
struct Response;
class Server;
}  // namespace httplib

namespace api {

// Filled in by the web layer's gate: who is making the request.
struct Credentials {
    std::string user;   // "" = nobody
    std::string ip;
    auth::Role role = auth::Role::Staff;
};

// Returns true when the request may continue. On false the callee has already written a
// 401 (not signed in) or 403 (role / CSRF) JSON response and the handler must stop.
using Authorize = std::function<bool(const httplib::Request&, httplib::Response&, auth::Permission need, Credentials& out)>;

struct Deps {
    inv::Inventory* inv = nullptr;
    jobs::JobBook* jobs = nullptr;
    crm::Registry* people = nullptr;   // may be null: customer / plate lookups are then skipped
    auth::AuditLog* audit = nullptr;
    std::string shopName;
    // The mutex that serialises every request (the web layer's, or the caller's in a test).
    // Optional: when it is null the caller must serialise calls itself. Lock order is always
    // dataLock -> JobBook::mutex(), never the other way round.
    std::mutex* dataLock = nullptr;
    // When set, that origin is allowed to call the API from a browser (CORS). Empty = same origin only,
    // which is how the built-in pages work. Set it with --cors-origin when a dev server (Vite, ...) is
    // served from another port.
    std::string corsOrigin;
};

// Registers every /api/... route on `svr`. Call once, after the stores have been loaded.
void registerRoutes(httplib::Server& svr, const Deps& deps, Authorize authorize);

}  // namespace api
