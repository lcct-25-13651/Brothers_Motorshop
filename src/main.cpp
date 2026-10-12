// RE Parts Tracker v3 - Bajaj RE parts & inventory web app (C++17, cpp-httplib, CSV storage).
//
//   re_parts_tracker [--port 8080] [--host 127.0.0.1] [--data <folder>] [--shop "Shop name"]
//
// Start-up order matters: (1) back up every CSV exactly as found, (2) load data,
// (3) start the web server.
#include <cstdlib>
#include <iostream>
#include <string>

#include "auth.h"
#include "inventory.h"
#include "web.h"

namespace {
void usage() {
    std::cout << "Usage: re_parts_tracker [--port N] [--host ADDR] [--data DIR] [--shop NAME] [--no-backup]\n"
                 "  --host 0.0.0.0   allow other computers on the shop network to connect (default: this PC only)\n";
}
}  // namespace

int main(int argc, char** argv) {
    web::Config cfg;
    std::string dataDir = ".";
    bool backup = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--port") { cfg.port = std::atoi(next().c_str()); if (cfg.port < 1 || cfg.port > 65535) { usage(); return 2; } }
        else if (a == "--host") cfg.host = next();
        else if (a == "--data") dataDir = next();
        else if (a == "--shop") cfg.shopName = next();
        else if (a == "--no-backup") backup = false;
        else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }

    if (backup) {
        const inv::BackupSummary b = inv::Inventory::backupFolder(dataDir);
        std::cout << "Backup: " << b.copied << " file(s) copied to " << b.directory;
        if (b.pruned) std::cout << " (" << b.pruned << " old backup(s) removed)";
        std::cout << '\n';
        for (const std::string& p : b.problems) std::cerr << "  backup warning: " << p << '\n';
    }

    inv::Inventory inventory(dataDir);
    auth::Authenticator users(dataDir + "/users.csv");
    auth::AuditLog audit(dataDir + "/audit.csv");
    if (!users.isConfigured()) std::cout << "No accounts yet: open the page in your browser to create the Admin account.\n";
    if (!inventory.lastWarning().empty()) std::cerr << "Warning: " << inventory.lastWarning() << '\n';

    web::WebApp app(inventory, users, audit, cfg);
    if (!app.run()) {
        std::cerr << "Could not listen on " << cfg.host << ':' << cfg.port << " (port already in use?)\n";
        return 1;
    }
    return 0;
}
