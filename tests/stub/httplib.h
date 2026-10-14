// TEST STUB of the cpp-httplib API subset used by web.cpp: routes in-process, no sockets.
#pragma once
#include <exception>
#include <functional>
#include <map>
#include <string>
namespace httplib {
using Params = std::multimap<std::string, std::string>;
struct Request {
    std::string method, path, remote_addr = "127.0.0.1", body; Params params; std::map<std::string, std::string> headers;
    bool has_param(const std::string& k) const { return params.find(k) != params.end(); }
    std::string get_param_value(const std::string& k, size_t = 0) const { auto i = params.find(k); return i == params.end() ? "" : i->second; }
    std::string get_header_value(const std::string& k, size_t = 0) const { auto i = headers.find(k); return i == headers.end() ? "" : i->second; }
};
struct Response {
    int status = -1; std::string body; std::map<std::string, std::string> headers;
    void set_header(const std::string& k, const std::string& v) { headers[k] = v; }
    void set_content(const std::string& s, const std::string& t) { body = s; headers["Content-Type"] = t; }
    void set_redirect(const std::string& u, int s = 302) { set_header("Location", u); status = s; }
};
class Server {
public:
    using Handler = std::function<void(const Request&, Response&)>;
    using ExceptionHandler = std::function<void(const Request&, Response&, std::exception_ptr)>;
    static inline Server* last = nullptr;
    std::map<std::string, Handler> routes;
    Server() { last = this; }
    Server& Get(const std::string& p, Handler h) { routes["GET " + p] = h; return *this; }
    Server& Post(const std::string& p, Handler h) { routes["POST " + p] = h; return *this; }
    Server& set_payload_max_length(size_t) { return *this; }
    Server& set_exception_handler(ExceptionHandler) { return *this; }
    bool listen(const char*, int) { return true; }
    Response call(const std::string& method, const std::string& path, Params params = {}, std::string cookie = "") {
        Request rq; rq.method = method; rq.path = path; rq.params = params; if (!cookie.empty()) rq.headers["Cookie"] = cookie;
        Response rs; auto it = routes.find(method + " " + path); if (it == routes.end()) { rs.status = 404; return rs; }
        it->second(rq, rs); if (rs.status < 0) rs.status = 200; return rs;
    }
};
}
