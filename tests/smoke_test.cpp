// Self-checking smoke test for the non-web modules (no network, no httplib needed).
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    // ================= Phase 2: credit sales / payments =================
    {
        // the sale above was paid in full at the till: status Paid, payment row logged
        const inv::Invoice* first = in.invoices().find("INV-000001");
        CHECK(first && first->status() == inv::PayStatus::Paid && first->balance() == 0 && first->amountPaid == first->total());
        CHECK(in.payments().forInvoice("INV-000001").size() == 1 && in.payments().all()[0].method == "Cash");

        inv::InvoiceLine one; one.kind = inv::InvoiceLine::Kind::Part; one.partId = 103; one.qty = 4;   // 4 x 200 = 800
        CHECK(!in.checkout("", {one}, "juan", 100.0, "Cash").ok);                 // walk-in cannot owe
        CHECK(!in.checkout("Mang Tonyo", {one}, "juan", 900.0, "Cash").ok);       // more than the bill
        CHECK(!in.checkout("Mang Tonyo", {one}, "juan", 100.0, "Barter").ok);     // unknown method
        CHECK(in.find(103)->quantity() == 50);                                    // nothing was taken from stock
        auto credit = in.checkout("Mang Tonyo", {one}, "juan", 300.0, "GCash");
        CHECK(credit.ok && credit.paid == 300.0 && credit.balance == 500.0 && in.find(103)->quantity() == 46);
        const inv::Invoice* ci = in.invoices().find(credit.invoiceNumber);
        CHECK(ci && ci->status() == inv::PayStatus::Partial && ci->balance() == 500.0);
        auto unpaid = in.checkout("  mang   tonyo ", {one}, "juan", 0.0);
        CHECK(unpaid.ok && in.invoices().find(unpaid.invoiceNumber)->status() == inv::PayStatus::Unpaid);
        CHECK(in.payments().forInvoice(unpaid.invoiceNumber).empty());            // nothing paid -> no payment row

        auto both = reports::receivablesByCustomer(in.invoices().all(), now);   // "Mang Tonyo" and "  mang   tonyo " are one customer
        CHECK(both.size() == 1 && both[0].openInvoices == 2 && both[0].balance == 1300.0 && both[0].paid == 300.0 && both[0].billed == 1600.0);

        // record payments
        CHECK(!in.recordPayment(credit.invoiceNumber, 0, "Cash", "", "juan").ok);
        CHECK(!in.recordPayment(credit.invoiceNumber, 600, "Cash", "", "juan").ok);           // overpayment refused
        CHECK(!in.recordPayment(credit.invoiceNumber, 100, "Crypto", "", "juan").ok);
        CHECK(!in.recordPayment("INV-999999", 100, "Cash", "", "juan").ok);
        CHECK(!in.recordPayment(credit.invoiceNumber, 100, "Cash", "", "juan", 123.0).ok);    // stale balance (double submit guard)
        auto p1 = in.recordPayment(credit.invoiceNumber, 200.0, "cash", "installment 2", "owner", 500.0);
        CHECK(p1.ok && p1.balance == 300.0 && p1.status == inv::PayStatus::Partial && p1.customer == "Mang Tonyo");
        CHECK(!in.recordPayment(credit.invoiceNumber, 200.0, "Cash", "", "owner", 500.0).ok);  // same form submitted twice
        auto p2 = in.recordPayment(credit.invoiceNumber, 300.0, "Maya", "", "owner", 300.0);
        CHECK(p2.ok && p2.balance == 0 && p2.status == inv::PayStatus::Paid);
        CHECK(!in.recordPayment(credit.invoiceNumber, 1, "Cash", "", "owner").ok);             // already paid
        CHECK(in.payments().forInvoice(credit.invoiceNumber).size() == 3);
        CHECK(in.payments().paidFor(credit.invoiceNumber) == 800.0);

        // invoices.csv carries amount_paid + payment_status; reload sees the same state
        inv::Inventory again2(d);
        const inv::Invoice* ri = again2.invoices().find(credit.invoiceNumber);
        CHECK(ri && ri->amountPaid == 800.0 && ri->status() == inv::PayStatus::Paid && ri->lines.size() == 1);
        CHECK(again2.invoices().find(unpaid.invoiceNumber)->status() == inv::PayStatus::Unpaid);
        CHECK(again2.payments().all().size() == in.payments().all().size());                   // reconcile added nothing
        std::string inv_csv;
        for (const auto& row : util::readCsvFile((dir / "invoices.csv").string())) if (row[0] == credit.invoiceNumber) inv_csv = row[9] + "/" + row[10];
        CHECK(inv_csv == "800.00/Paid");

        // receivables
        auto owing = reports::receivablesByCustomer(in.invoices().all(), now);
        CHECK(owing.size() == 1 && reports::customerKey(owing[0].customer) == "mang tonyo" && owing[0].balance == 800.0 && owing[0].openInvoices == 1);
        CHECK(reports::totalReceivable(in.invoices().all()) == 800.0 && reports::openInvoiceCount(in.invoices().all()) == 1);

        // daily cash report for today
        const std::string today = util::formatTime(now, "%Y-%m-%d");
        CHECK(reports::validDate(today) && reports::validDate("2028-02-29") && !reports::validDate("2026-13-40") && !reports::validDate("2026-02-30") && !reports::validDate("yesterday"));
        auto rep = reports::dailyCashReport(in.invoices().all(), in.payments().all(), today);
        // invoices today: INV-1 (1851), credit (800), unpaid (800); collected: 1851 + 300 + 200 + 300
        CHECK(rep.invoiceCount == 3 && rep.grossSales == 3451.0 && rep.serviceSales == 150.0 && rep.partsSales == 3301.0);
        CHECK(rep.collected == 2651.0 && rep.collectedOnToday == 2651.0 && rep.collectedOnOlder == 0);
        CHECK(rep.paidCount == 2 && rep.partialCount == 0 && rep.unpaidCount == 1 && rep.creditExtended == 800.0);
        CHECK(rep.outstandingNow == 800.0 && rep.customersOwing == 1);
        double cashSum = 0, gcash = 0, maya = 0;
        for (const auto& m : rep.byMethod) { if (m.method == "Cash") cashSum = m.amount; if (m.method == "GCash") gcash = m.amount; if (m.method == "Maya") maya = m.amount; }
        CHECK(cashSum == 2051.0 && gcash == 300.0 && maya == 300.0);   // 1851 + 200 cash
        auto empty = reports::dailyCashReport(in.invoices().all(), in.payments().all(), "2001-01-01");
        CHECK(empty.invoiceCount == 0 && empty.collected == 0 && empty.byMethod.empty() && empty.outstandingNow == 800.0);
    }

    // ---- daily cash report across days (synthetic ledger): utang paid later shows up on the day the money arrives ----
    {
        inv::Invoice a; a.number = "INV-000101"; a.timestamp = "2026-03-01 09:00:00"; a.customer = "Aling Nena"; a.user = "juan";
        inv::InvoiceLine l; l.kind = inv::InvoiceLine::Kind::Part; l.partId = 1; l.description = "Part"; l.qty = 1; l.unitPrice = 1000; a.lines.push_back(l);
        a.amountPaid = 1000;   // fully paid by now
        inv::Invoice b = a; b.number = "INV-000102"; b.timestamp = "2026-03-02 09:00:00"; b.amountPaid = 0;
        std::vector<inv::Payment> pays(3);
        pays[0].timestamp = "2026-03-01 09:00:00"; pays[0].invoiceNo = "INV-000101"; pays[0].amount = 400; pays[0].method = "Cash";
        pays[1].timestamp = "2026-03-02 15:00:00"; pays[1].invoiceNo = "INV-000101"; pays[1].amount = 600; pays[1].method = "GCash";
        pays[2].timestamp = "2026-03-02 16:00:00"; pays[2].invoiceNo = "INV-000102"; pays[2].amount = 0.0; pays[2].method = "Cash";
        pays.pop_back();
        const std::vector<inv::Invoice> invs = {a, b};
        auto d1 = reports::dailyCashReport(invs, pays, "2026-03-01");
        CHECK(d1.invoiceCount == 1 && d1.grossSales == 1000 && d1.collected == 400 && d1.creditExtended == 600 && d1.partialCount == 1 && d1.paidCount == 0);  // as it stood that evening
        auto d2 = reports::dailyCashReport(invs, pays, "2026-03-02");
        CHECK(d2.invoiceCount == 1 && d2.unpaidCount == 1 && d2.collected == 600 && d2.collectedOnOlder == 600 && d2.collectedOnToday == 0 && d2.creditExtended == 1000);
        CHECK(d2.byMethod.size() == 1 && d2.byMethod[0].method == "GCash" && d2.outstandingNow == 1000 && d2.customersOwing == 1);
    }

    // ---- legacy 9-column invoices.csv: read as paid, upgraded in place, payments ledger back-filled ----
    {
        const fs::path ld = fs::temp_directory_path() / "re_parts_smoke_legacy";
        fs::remove_all(ld);
        fs::create_directories(ld);
        {
            std::ofstream f(ld / "invoices.csv");
            f << "invoice_no,timestamp,customer,username,line_type,part_id,description,qty,unit_price\n"
                 "INV-000007,2026-01-05 10:00:00,Old Customer,juan,PART,101,Engine Oil 1L,2,450.00\n"
                 "INV-000007,2026-01-05 10:00:00,Old Customer,juan,SERVICE,0,Labor,1,100.00\n"
                 "garbage row\n";
        }
        inv::Inventory old(ld.string());
        const inv::Invoice* o = old.invoices().find("INV-000007");
        CHECK(o && o->status() == inv::PayStatus::Paid && o->amountPaid == 1000.0 && o->lines.size() == 2);
        CHECK(old.payments().forInvoice("INV-000007").size() == 1 && old.payments().paidFor("INV-000007") == 1000.0);
        CHECK(fs::exists(ld / "invoices.csv.bak"));                           // damaged row kept before the rewrite
        CHECK(old.lastWarning().find("invoices.csv") != std::string::npos);
        const auto rows = util::readCsvFile((ld / "invoices.csv").string());
        CHECK(rows.size() == 3 && rows[0].size() == 11 && rows[1][9] == "1000.00" && rows[1][10] == "Paid");
        CHECK(old.invoices().nextNumber() == "INV-000008");
        inv::Inventory old2(ld.string());
        CHECK(old2.payments().all().size() == 1);                              // idempotent
        fs::remove_all(ld);
    }

    // ================= Phase 2: vehicle compatibility =================
    {
        auto models = inv::splitModels(" Bajaj RE 4S ; Bajaj  RE Compact,bajaj re 4s|Bajaj RE Maxima,, \n");
        CHECK(models.size() == 3 && models[0] == "Bajaj RE 4S" && models[1] == "Bajaj RE Compact" && models[2] == "Bajaj RE Maxima");
        inv::Part cp(2001, "Gasket", "Engine", 50, 5, 1);
        cp.setCompatibleModels("Bajaj RE 4S, Bajaj RE Compact");
        CHECK(cp.fitsModel("bajaj re compact") && cp.fitsModel("Bajaj RE 4S") && !cp.fitsModel("Bajaj RE") && !cp.fitsModel("Bajaj RE Maxima") && !cp.fitsModel(""));
        CHECK(in.addPart(cp, "owner").ok);
        inv::Part mp(2002, "Piston", "Engine", 900, 5, 1);
        mp.setCompatibleModels("Bajaj RE Maxima; Bajaj RE Compact");
        CHECK(in.addPart(mp, "owner").ok);
        inv::Part bad(2003, "X", "Engine", 1, 1, 1);
        bad.setCompatibleModels(std::string(400, 'a'));
        CHECK(!in.addPart(bad, "owner").ok && !in.find(2003));
        const auto all = in.allModels();
        CHECK(all.size() == 3 && all[0] == "Bajaj RE 4S" && all[1] == "Bajaj RE Compact" && all[2] == "Bajaj RE Maxima");
        int fits = 0;
        for (const inv::Part& p : in.parts()) fits += p.fitsModel("Bajaj RE Compact");
        CHECK(fits == 2);
        CHECK(in.updatePart(2001, "Gasket", "Engine", 50, 1, "owner", std::string("Bajaj RE 4S")).ok && !in.find(2001)->fitsModel("Bajaj RE Compact"));
        CHECK(in.updatePart(2001, "Gasket", "Engine", 55, 1, "owner").ok && in.find(2001)->fitsModel("Bajaj RE 4S"));   // no models arg = unchanged
        CHECK(in.partsCsv().find("Bajaj RE Maxima; Bajaj RE Compact") != std::string::npos);
        auto im = in.importCsv("id,name,system,price,quantity,compatible_models\n2002,,,,5,\"Bajaj RE Maxima, Bajaj RE Compact, Bajaj RE 4S\"\n", "owner");
        CHECK(im.updated == 1 && in.find(2002)->fitsModel("Bajaj RE 4S"));
        inv::Inventory persisted(d);
        CHECK(persisted.find(2002) && persisted.find(2002)->fitsModel("Bajaj RE 4S"));
    }

    CHECK(in.removePart(106, "owner").ok);
    CHECK(in.movements().all().back().type == inv::MoveType::Delete && in.movements().all().back().resultingStock == 0);

    fs::remove_all(dir);
    if (failures == 0) std::cout << "ALL SMOKE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
