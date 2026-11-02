// TEST STUB of the cpp-httplib API subset used by web.cpp / api.cpp: routes in-process, no sockets.
//
// Routes are matched the way the real library does: an exact path first, then the regex patterns the
// /api/* endpoints register (with the captures handed to the handler in Request::matches).
#pragma once
#include <exception>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib {
using Params = std::multimap<std::string, std::string>;

struct Request {
    std::string method, path, remote_addr = "127.0.0.1", body;
    Params params;
    std::map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> path_params;   // :name captures (real httplib)
    using Match = std::smatch;
    Match matches;                                              // regex captures (real httplib)

    bool has_param(const std::string& k) const { return params.find(k) != params.end(); }
    std::string get_param_value(const std::string& k, size_t = 0) const {
        auto i = params.find(k);
        return i == params.end() ? "" : i->second;
    }
    std::string get_header_value(const std::string& k, size_t = 0) const {
        auto i = headers.find(k);
        return i == headers.end() ? "" : i->second;
    }
};

struct Response {
    int status = -1;
    std::string body;
    std::map<std::string, std::string> headers;
    void set_header(const std::string& k, const std::string& v) { headers[k] = v; }
    void set_content(const std::string& s, const std::string& t) {
        body = s;
        headers["Content-Type"] = t;
    }
    void set_redirect(const std::string& u, int s = 302) {
        set_header("Location", u);
        status = s;
    }
};

class Server {
public:
    using Handler = std::function<void(const Request&, Response&)>;
    using ExceptionHandler = std::function<void(const Request&, Response&, std::exception_ptr)>;
    static inline Server* last = nullptr;

    struct Route {
        std::string method, pattern;
        Handler fn;
    };
    std::vector<Route> routes;

    Server() { last = this; }
    Server& Get(const std::string& p, Handler h) { return add("GET", p, std::move(h)); }
    Server& Post(const std::string& p, Handler h) { return add("POST", p, std::move(h)); }
    Server& Delete(const std::string& p, Handler h) { return add("DELETE", p, std::move(h)); }
    Server& Put(const std::string& p, Handler h) { return add("PUT", p, std::move(h)); }
    Server& Options(const std::string& p, Handler h) { return add("OPTIONS", p, std::move(h)); }
    Server& set_payload_max_length(size_t) { return *this; }
    Server& set_exception_handler(ExceptionHandler) { return *this; }
    bool listen(const char*, int) { return true; }

    // Dispatches one request in-process. `body` is the raw request body (JSON for the API tests).
    Response call(const std::string& method, const std::string& path, Params params = {}, std::string cookie = "",
                  std::string body = "", std::map<std::string, std::string> headers = {}) {
        Request rq;
        rq.method = method;
        rq.path = path;
        rq.params = std::move(params);
        rq.body = std::move(body);
        rq.headers = std::move(headers);
        if (!cookie.empty()) rq.headers["Cookie"] = cookie;
        Response rs;
        for (const Route& r : routes) {   // exact paths first
            if (r.method != method || r.pattern != path) continue;
            r.fn(rq, rs);
            if (rs.status < 0) rs.status = 200;
            return rs;
        }
        for (const Route& r : routes) {   // then the regex patterns, in registration order
            if (r.method != method) continue;
            std::smatch m;
            if (!std::regex_match(path, m, std::regex(r.pattern))) continue;
            rq.matches = m;               // sub-matches point into `path`, which outlives this call
            r.fn(rq, rs);
            if (rs.status < 0) rs.status = 200;
            return rs;
        }
        rs.status = 404;
        return rs;
    }

private:
    Server& add(const std::string& method, const std::string& p, Handler h) {
        routes.push_back({method, p, std::move(h)});
        return *this;
    }
};

}  // namespace httplib
