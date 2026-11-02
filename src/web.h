// web.h - HTTP endpoints, HTML templates and views (cpp-httplib).
#pragma once
#include <memory>
#include <string>

#include "auth.h"
#include "inventory.h"

namespace web {

struct Config {
    std::string host = "127.0.0.1";  // use 0.0.0.0 to let other PCs on the shop network connect
    int port = 8080;
    std::string shopName = "Brothers Motorshop";
    // Phase 4: when set, this origin may call /api/* from a browser (CORS). Use it only while a
    // front-end is served from another port (e.g. "http://localhost:5173"); empty = same origin only.
    std::string corsOrigin;
    // Set with --allow-embedding when the app is shown inside a frame on another page (a shop intranet
    // portal or a kiosk screen). Off by default: framing is how click-jacking attacks start.
    bool allowEmbedding = false;
};

class WebApp {
public:
    WebApp(inv::Inventory& inventory, auth::Authenticator& users, auth::AuditLog& audit, Config cfg);
    ~WebApp();
    WebApp(const WebApp&) = delete;
    WebApp& operator=(const WebApp&) = delete;

    bool run();  // blocks until the server stops; false if it could not bind the port

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  // keeps httplib.h out of this header
};

}  // namespace web
