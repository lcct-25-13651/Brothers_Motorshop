// Self-checking smoke test for the non-web modules (no network, no httplib needed).
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "../src/auth.h"
#include "../src/inventory.h"
#include "../src/reports.h"
#include "../src/util.h"

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } \
    } while (0)

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "re_parts_smoke";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string d = dir.string();

    // ---- auth / RBAC ----
    {
        auth::Authenticator a((dir / "users.csv").string());
        CHECK(!a.isConfigured());
        CHECK(a.initialSetup("owner", "correct-horse", "First pet name?", "fluffy-the-cat").empty());
        CHECK(a.login("OWNER", "correct-horse").has_value());
        CHECK(!a.login("owner", "wrong-password").has_value());
        CHECK(a.addUser("juan", "staffpass1", auth::Role::Staff).empty());
        CHECK(!a.addUser("juan", "staffpass1", auth::Role::Staff).empty());  // duplicate
        CHECK(!a.removeUser("owner").empty());                               // last admin
        CHECK(!a.setRole("owner", auth::Role::Staff).empty());
        auth::Authenticator reload((dir / "users.csv").string());            // persisted + salted
        CHECK(reload.users().size() == 2);
        CHECK(reload.login("juan", "staffpass1")->second == auth::Role::Staff);
        CHECK(auth::allowed(auth::Role::Staff, auth::Permission::Sell));
        CHECK(auth::allowed(auth::Role::Staff, auth::Permission::AdjustStock));
        CHECK(!auth::allowed(auth::Role::Staff, auth::Permission::DeletePart));
        CHECK(!auth::allowed(auth::Role::Staff, auth::Permission::EditPart));
        CHECK(!auth::allowed(auth::Role::Staff, auth::Permission::ViewReports));
        CHECK(auth::allowed(auth::Role::Admin, auth::Permission::ManageUsers));
        CHECK(reload.resetPassword("juan", "newpass123").empty());
        CHECK(!reload.login("juan", "staffpass1").has_value());
        CHECK(reload.recoverPassword("owner", "  Fluffy-The-Cat ", "another-pass1").empty());
        auth::LoginThrottle th;
        for (int i = 0; i < 5; ++i) th.fail("k");
        CHECK(th.blocked("k"));
        auth::AuditLog audit((dir / "audit.csv").string());
        audit.record("owner", "LOGIN", "ok", "127.0.0.1");
        auth::AuditLog audit2((dir / "audit.csv").string());
        CHECK(audit2.entries().size() == 1);
    }

    // ---- backups run BEFORE loading, copy every csv ----
    {
        auto b = inv::Inventory::backupFolder(d);
        CHECK(b.copied >= 2);  // users.csv + audit.csv exist so far
    }

    // ---- inventory ----
    inv::Inventory in(d);
    CHECK(in.parts().size() == 6);
    CHECK(in.movements().all().size() == 6);  // opening balances
    CHECK(in.restock(101, 10, 300.0, "owner").ok);
    CHECK(in.find(101)->quantity() == 30);
    CHECK(!in.adjust(101, -999, "juan").ok);
    CHECK(in.adjust(101, -1, "juan").ok);

    inv::InvoiceLine p1; p1.kind = inv::InvoiceLine::Kind::Part; p1.partId = 102; p1.qty = 2; p1.unitPrice = 1;  // price must be ignored
    inv::InvoiceLine s1; s1.kind = inv::InvoiceLine::Kind::Service; s1.description = "Oil change labor"; s1.qty = 1; s1.unitPrice = 150;
    auto co = in.checkout("Juan dela Cruz", {p1, s1}, "juan");
    CHECK(co.ok);
    CHECK(in.find(102)->quantity() == 10);
    const inv::Invoice* inc = in.invoices().find(co.invoiceNumber);
    CHECK(inc && inc->lines[0].unitPrice == 850.50 && inc->total() == 2 * 850.50 + 150);

    inv::InvoiceLine big = p1; big.qty = 500;
    CHECK(!in.checkout("x", {big}, "juan").ok);        // not enough stock
    inv::InvoiceLine twice = p1; twice.qty = 6;
    CHECK(!in.checkout("x", {twice, twice}, "juan").ok);  // 12 > 10: duplicates are summed
    CHECK(in.find(102)->quantity() == 10);              // all-or-nothing

    // ---- reports ----
    const std::time_t now = std::time(nullptr);
    auto top = reports::topSelling(in.movements().all(), 30, 10, now);
    CHECK(top.size() == 1 && top[0].partId == 102 && top[0].unitsSold == 2 && top[0].revenue == 1701.0);
    auto daily = reports::revenueByPeriod(in.movements().all(), in.invoices().all(), reports::Period::Daily, 7, now);
    CHECK(daily.size() == 7);
    CHECK(daily.back().partsRevenue == 1701.0 && daily.back().serviceRevenue == 150.0 && daily.back().expenses == 3000.0);
    auto weekly = reports::revenueByPeriod(in.movements().all(), in.invoices().all(), reports::Period::Weekly, 4, now);
    auto monthly = reports::revenueByPeriod(in.movements().all(), in.invoices().all(), reports::Period::Monthly, 12, now);
    CHECK(weekly.back().revenue() == 1851.0 && monthly.back().revenue() == 1851.0);
    CHECK(reports::deadStock(in.parts(), in.movements().all(), 30, now).empty());
    auto dead = reports::deadStock(in.parts(), in.movements().all(), 30, now + 45LL * 86400);
    CHECK(dead.size() == 6);
    auto reorder = reports::reorderList(in.parts());
    CHECK(reorder.size() == 1 && reorder[0].part->id() == 106 && reorder[0].suggestedQty == 6);

    // ---- CSV import / export ----
    auto imp = in.importCsv("id,name,system,price,quantity,min_stock,unit_cost\n"
                            "103,,,200,50,,120\n"            // update price + stock (restock @120)
                            "999,Chain Sprocket,Transmission,650,15,5,\n"  // new part
                            "abc,Bad,Engine,1,1,1,\n"        // bad id
                            "104,,,,2,,\n",                  // lower stock -> ADJUSTMENT
                            "owner");
    CHECK(imp.added == 1 && imp.updated == 2 && imp.skipped == 1);
    CHECK(imp.priceChanges.size() == 1 && imp.priceChanges[0].newPrice == 200);
    CHECK(in.find(103)->quantity() == 50 && in.find(104)->quantity() == 2 && in.find(999));
    const std::string csv = in.partsCsv();
    CHECK(csv.rfind("\xEF\xBB\xBF", 0) == 0);
    CHECK(util::csvSafe("=cmd|' /C calc'!A0").front() == '\'' || util::csvSafe("=1+1").find("'=1+1") != std::string::npos);

    // ---- persistence: reload sees the same state and a consistent ledger ----
    const size_t moves = in.movements().all().size();
    inv::Inventory again(d);
    CHECK(again.parts().size() == in.parts().size());
    CHECK(again.movements().all().size() == moves);  // reconcile adds nothing
    CHECK(again.invoices().nextNumber() == "INV-000002");

    CHECK(in.removePart(106, "owner").ok);
    CHECK(in.movements().all().back().type == inv::MoveType::Delete && in.movements().all().back().resultingStock == 0);

    fs::remove_all(dir);
    if (failures == 0) std::cout << "ALL SMOKE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
