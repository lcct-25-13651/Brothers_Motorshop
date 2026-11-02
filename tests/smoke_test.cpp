// Self-checking smoke test for the non-web modules (no network, no httplib needed).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "../src/auth.h"
#include "../src/crm.h"
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
        CHECK(rows.size() == 3 && rows[0].size() == 13 && rows[1][9] == "1000.00" && rows[1][10] == "Paid" && rows[1][11].empty() && rows[1][12].empty());
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

    // ================= Phase 3: customer & vehicle records =================
    {
        // plate normalisation
        CHECK(crm::normalizePlate(" abc  1234 ") == "ABC1234" && crm::normalizePlate("ab-12\t3") == "AB-123" && crm::normalizePlate("   ").empty());
        CHECK(crm::validPlate("ABC1234") && crm::validPlate("AB-123") && !crm::validPlate("A") && !crm::validPlate("AB#12") && !crm::validPlate(std::string(13, 'A')));

        const fs::path cd = fs::temp_directory_path() / "re_parts_smoke_crm";
        fs::remove_all(cd);
        fs::create_directories(cd);
        crm::Registry reg;
        reg.open(cd.string());
        CHECK(reg.customers().empty() && reg.vehicles().empty() && reg.warning().empty());
        int id = 0, vid = 0;

        // ---- customers ----
        crm::Customer juan; juan.name = "  Juan   dela Cruz "; juan.phone = "0917 555 1234"; juan.address = "Dasma, Cavite"; juan.notes = "Tricycle operator";
        CHECK(reg.saveCustomer(0, juan, id).empty() && id == 1);
        CHECK(reg.findCustomer(1)->phone == "0917 555 1234" && reg.findCustomer(1)->name.rfind("Juan", 0) == 0);
        crm::Customer maria; maria.name = "Maria Santos"; maria.phone = "0928-111-2222";
        CHECK(reg.saveCustomer(0, maria, id).empty() && id == 2);
        crm::Customer dup = juan; dup.phone = "09175551234";
        CHECK(!reg.saveCustomer(0, dup, id).empty());                        // same name + same phone digits = same person entered twice
        crm::Customer pedro; pedro.name = "Pedro Reyes"; pedro.phone = "0999 000 0000";
        CHECK(reg.saveCustomer(0, pedro, id).empty() && id == 3);
        crm::Customer bad; bad.name = "   ";
        CHECK(!reg.saveCustomer(0, bad, id).empty());                        // name required
        bad.name = "walk-in CUSTOMER";
        CHECK(!reg.saveCustomer(0, bad, id).empty());                        // reserved word
        bad.name = std::string(61, 'x');
        CHECK(!reg.saveCustomer(0, bad, id).empty());
        CHECK(reg.customers().size() == 3);

        // ---- vehicles: plate stored UPPERCASE with no spaces, unique across everyone ----
        crm::Vehicle v1; v1.customerId = 1; v1.plate = " abc  1234 "; v1.model = "Bajaj RE Maxima"; v1.year = 2019; v1.color = "Green";
        CHECK(reg.saveVehicle(0, v1, vid).empty() && vid == 1 && reg.findVehicle(1)->plate == "ABC1234");
        crm::Vehicle v2 = v1; v2.customerId = 2; v2.plate = "abc1234";
        CHECK(!reg.saveVehicle(0, v2, vid).empty());                         // duplicate plate, other owner
        v2.plate = "A B C 1 2 3 4";
        CHECK(!reg.saveVehicle(0, v2, vid).empty());                         // duplicate once spaces are removed
        v2.plate = "XYZ 987"; v2.year = 1800;
        CHECK(!reg.saveVehicle(0, v2, vid).empty());                         // year out of range
        v2.year = 0;
        CHECK(reg.saveVehicle(0, v2, vid).empty() && vid == 2 && reg.findVehicle(2)->year == 0 && reg.findVehicle(2)->plate == "XYZ987");
        crm::Vehicle v3 = v1; v3.customerId = 99; v3.plate = "NOPE1";
        CHECK(!reg.saveVehicle(0, v3, vid).empty());                         // unknown owner
        v3.customerId = 1; v3.plate = "bad#plate";
        CHECK(!reg.saveVehicle(0, v3, vid).empty());                         // illegal characters
        CHECK(reg.findByPlate("abc 1234") && reg.findByPlate("abc 1234")->id == 1 && reg.findByPlate("xyz987")->id == 2 && !reg.findByPlate("zzz") && !reg.findByPlate("  "));
        CHECK(reg.saveVehicle(1, [&] { crm::Vehicle e = v1; e.customerId = 2; e.plate = "abc 1234"; e.color = "Yellow"; return e; }(), vid).empty());   // edit keeps its own plate
        CHECK(reg.findVehicle(1)->color == "Yellow" && reg.findVehicle(1)->customerId == 1);                          // owner cannot be changed through an edit
        crm::Vehicle v4; v4.customerId = 3; v4.plate = "DEF 555"; v4.model = "Bajaj RE Compact";
        CHECK(reg.saveVehicle(0, v4, vid).empty() && vid == 3);

        // ---- search: name / phone / plate ----
        auto names = [](const std::vector<crm::SearchHit>& h) { std::string o; for (const auto& x : h) o += std::to_string(x.customer->id) + ","; return o; };
        CHECK(names(reg.search("")) == "1,2,3,");                                   // everybody, by name
        CHECK(names(reg.search("maria")) == "2," && names(reg.search("JUAN CRUZ")) == "1,");     // every word must appear, any order / case
        CHECK(names(reg.search("cruz juan")) == "1,");
        CHECK(names(reg.search("0928")) == "2," && names(reg.search("(0917) 555-1234")) == "1,");   // phone: digits only
        CHECK(names(reg.search("abc 1234")) == "1," && names(reg.search("abc1234")) == "1," && names(reg.search("ABC")) == "1,");  // plate, any spelling
        { auto h = reg.search("xyz98"); CHECK(h.size() == 1 && h[0].customer->id == 2 && h[0].plateMatches.size() == 1 && h[0].plateMatches[0]->plate == "XYZ987"); }
        CHECK(reg.search("nobody at all").empty() && reg.search("zq").empty());
        CHECK(reg.search("91").empty());                                             // 2 digits is not a phone match (needs 3) and no plate contains it
        CHECK(names(reg.search("12")) == "1,");                                      // ...but 2 characters are enough for a plate fragment (ABC1234)
        CHECK(reg.search("maria")[0].plateMatches.empty());                          // matched by name, not plate

        // ---- archive / delete rules ----
        CHECK(reg.setCustomerArchived(2, true).empty() && reg.findCustomer(2)->archived);
        CHECK(names(reg.search("maria")) == "2," && reg.search("")[reg.search("").size() - 1].customer->id == 2);   // archived are still returned (caller hides them), sorted last
        CHECK(!reg.saveVehicle(0, [&] { crm::Vehicle e; e.customerId = 2; e.plate = "NEW 111"; return e; }(), vid).empty());   // no new vehicle for an archived customer
        CHECK(reg.setCustomerArchived(2, false).empty());
        CHECK(!reg.removeCustomer(1, false).empty());                                // still owns a vehicle
        crm::Customer temp; temp.name = "Temp Person";
        int tid = 0, tvid = 0;
        CHECK(reg.saveCustomer(0, temp, tid).empty() && tid == 4);
        crm::Vehicle tv; tv.customerId = tid; tv.plate = "TMP 1";
        CHECK(reg.saveVehicle(0, tv, tvid).empty() && tvid == 4);
        CHECK(!reg.removeCustomer(tid, false).empty() && reg.findCustomer(tid));     // owns a vehicle -> refused
        CHECK(!reg.removeVehicle(tvid, true).empty() && reg.findVehicle(tvid));      // vehicle has history -> refused
        CHECK(reg.removeVehicle(tvid, false).empty() && !reg.findVehicle(tvid));
        CHECK(!reg.removeCustomer(tid, true).empty() && reg.findCustomer(tid));      // customer has history -> refused
        CHECK(reg.removeCustomer(tid, false).empty() && !reg.findCustomer(tid));     // no history, no vehicle -> gone
        CHECK(reg.setVehicleArchived(3, true).empty() && names(reg.search("def")) == "3,");   // archived vehicle still finds its owner
        CHECK(reg.findByPlate("DEF555") && reg.findByPlate("DEF555")->archived);

        // ---- persistence + hand-edited / short files ----
        crm::Registry again;
        again.open(cd.string());
        CHECK(again.customers().size() == 3 && again.vehicles().size() == 3 && again.findVehicle(1)->plate == "ABC1234" && again.findVehicle(3)->archived && again.findCustomer(1)->notes == "Tricycle operator");
        CHECK(again.findVehicle(2)->year == 0 && again.findVehicle(1)->year == 2019);
        {
            const fs::path od = fs::temp_directory_path() / "re_parts_smoke_crm_old";
            fs::remove_all(od);
            fs::create_directories(od);
            { std::ofstream f(od / "customers.csv"); f << "id,name,phone,address,notes\n7,\"Lolo, Jose\",0917,Manila,ok\n"; }          // 5-column file (as in the spec)
            { std::ofstream f(od / "vehicles.csv"); f << "id,customer_id,plate,model,year,color,notes\n4,7,\"ab 12\",RE,2015,Red,\n"; }  // 7-column file
            crm::Registry r5;
            r5.open(od.string());
            CHECK(r5.customers().size() == 1 && r5.customers()[0].name == "Lolo, Jose" && !r5.customers()[0].archived && r5.warning().empty());
            CHECK(r5.vehicles().size() == 1 && r5.vehicles()[0].plate == "AB12" && r5.vehicles()[0].year == 2015);
            { std::ofstream f(od / "customers.csv", std::ios::app); f << "oops,not a number\n"; }
            crm::Registry r6;
            r6.open(od.string());
            CHECK(!r6.warning().empty() && fs::exists(od / "customers.csv.bak") && r6.customers().size() == 1);   // damaged row: original kept before any rewrite
            fs::remove_all(od);
        }

        // ---- invoices carry customer_id / vehicle_id; old invoices keep working ----
        inv::InvoiceLine pl; pl.kind = inv::InvoiceLine::Kind::Part; pl.partId = 103; pl.qty = 1;
        const size_t invoicesBefore = in.invoices().all().size();
        auto linked = in.checkout("Juan dela Cruz", {pl}, "juan", -1.0, "Cash", 1, 1);
        CHECK(linked.ok && in.invoices().find(linked.invoiceNumber)->customerId == 1 && in.invoices().find(linked.invoiceNumber)->vehicleId == 1);
        auto walkin = in.checkout("", {pl}, "juan");
        CHECK(walkin.ok && in.invoices().find(walkin.invoiceNumber)->customerId == 0 && in.invoices().find(walkin.invoiceNumber)->vehicleId == 0);
        auto linkedCredit = in.checkout("Juan dela Cruz", {pl}, "juan", 0.0, "Cash", 1, 1);
        auto otherVeh = in.checkout("Juan dela Cruz", {pl}, "juan", -1.0, "Cash", 1, 3);   // (the web layer validates ownership; core just stores)
        CHECK(linkedCredit.ok && otherVeh.ok && in.invoices().all().size() == invoicesBefore + 4);
        {   // reload from disk: ids survive; blanks stay blank
            inv::Inventory reloaded(d);
            const inv::Invoice* a = reloaded.invoices().find(linked.invoiceNumber);
            const inv::Invoice* w = reloaded.invoices().find(walkin.invoiceNumber);
            CHECK(a && a->customerId == 1 && a->vehicleId == 1 && w && w->customerId == 0 && w->vehicleId == 0);
            const auto rows = util::readCsvFile((fs::path(d) / "invoices.csv").string());
            bool sawBlank = false, sawIds = false;
            for (const auto& r : rows) if (r.size() == 13 && r[0] == linked.invoiceNumber) sawIds = r[11] == "1" && r[12] == "1";
            for (const auto& r : rows) if (r.size() == 13 && r[0] == walkin.invoiceNumber) sawBlank = r[11].empty() && r[12].empty();
            CHECK(sawIds && sawBlank);
        }
        // a later payment rewrites invoices.csv: the ids must survive that too
        CHECK(in.recordPayment(linkedCredit.invoiceNumber, 50.0, "Cash", "", "juan").ok);
        { inv::Inventory r2(d); CHECK(r2.invoices().find(linkedCredit.invoiceNumber)->customerId == 1 && r2.invoices().find(linkedCredit.invoiceNumber)->vehicleId == 1 && r2.invoices().find(linkedCredit.invoiceNumber)->amountPaid == 50.0); }

        // ---- history: newest first, per vehicle and per customer; balance matches the receivables report ----
        const auto& invs = in.invoices().all();
        auto vh = crm::vehicleHistory(invs, 1);
        CHECK(vh.size() == 2 && vh[0].ref == linkedCredit.invoiceNumber && vh[1].ref == linked.invoiceNumber && vh[0].balance > 0 && vh[1].balance == 0);   // same second -> later number first
        CHECK(crm::vehicleHistory(invs, 3).size() == 1 && crm::vehicleHistory(invs, 99).empty() && crm::vehicleHistory(invs, 0).empty());
        CHECK(crm::vehicleHasHistory(invs, 1) && !crm::vehicleHasHistory(invs, 2) && crm::customerHasHistory(invs, 1) && !crm::customerHasHistory(invs, 3));

        // old free-text invoices of the same name: part of the customer's history/balance (flagged "by name"), but only while the name is unique
        auto oldCredit = in.checkout("  juan DELA cruz ", {pl}, "juan", 0.0, "Cash");   // typed free text, no link (the situation before Phase 3)
        CHECK(oldCredit.ok);
        auto ch = crm::customerHistory(in.invoices().all(), again, *again.findCustomer(1));
        CHECK(ch.size() >= 4);
        bool oldFlagged = false, allSorted = true;
        for (size_t i = 0; i < ch.size(); ++i) {
            if (ch[i].ref == oldCredit.invoiceNumber) oldFlagged = !ch[i].linked;
            if (i > 0 && ch[i - 1].timestamp < ch[i].timestamp) allSorted = false;
        }
        CHECK(oldFlagged && allSorted);
        const crm::Balance jb = crm::customerBalance(in.invoices().all(), again, *again.findCustomer(1));
        double receivableForJuan = 0;
        for (const auto& d2 : reports::receivablesByCustomer(in.invoices().all(), now))
            if (reports::customerKey(d2.customer) == reports::customerKey("Juan dela Cruz")) receivableForJuan = d2.balance;
        CHECK(jb.balance == receivableForJuan && jb.balance > 0 && jb.openInvoices == 2);   // identical to what the Receivables page shows
        CHECK(crm::balancesByCustomer(in.invoices().all(), again).at(1).balance == jb.balance);
        {   // a namesake record makes the by-name match ambiguous: nobody is charged for it
            crm::Registry two;
            two.open(cd.string());
            crm::Customer ns; ns.name = "JUAN dela cruz"; ns.phone = "0999";
            int nid = 0;
            CHECK(two.saveCustomer(0, ns, nid).empty());                    // a namesake with another phone number is allowed
            ns.phone = "0999";
            CHECK(!two.saveCustomer(0, ns, nid).empty());                   // ...the same phone again is not
            const crm::Balance linkedOnly = crm::customerBalance(in.invoices().all(), two, *two.findCustomer(1));
            CHECK(linkedOnly.openInvoices == 1 && linkedOnly.balance < jb.balance);
            CHECK(crm::balancesByCustomer(in.invoices().all(), two).count(nid) == 0);
        }

        // ---- a Phase 2 (11-column) invoices.csv is upgraded without changing a single value ----
        {
            const fs::path pd = fs::temp_directory_path() / "re_parts_smoke_p2";
            fs::remove_all(pd);
            fs::create_directories(pd);
            const std::string p2 =
                "invoice_no,timestamp,customer,username,line_type,part_id,description,qty,unit_price,amount_paid,payment_status\n"
                "INV-000001,2026-02-01 09:00:00,Mang Tonyo,juan,PART,101,Engine Oil 1L,2,450.00,300.00,Partial\n"
                "INV-000001,2026-02-01 09:00:00,Mang Tonyo,juan,SERVICE,0,Labor,1,100.00,300.00,Partial\n"
                "INV-000002,2026-02-02 10:00:00,Aling Nena,juan,SERVICE,0,Tune up,1,500.00,500.00,Paid\n"
                "INV-000003,2026-02-03 11:00:00,Walk-in customer,juan,SERVICE,0,Wash,1,50.00,0.00,Unpaid\n";
            { std::ofstream f(pd / "invoices.csv"); f << p2; }
            inv::Inventory p2inv(pd.string());
            const auto& oi = p2inv.invoices().all();
            CHECK(oi.size() == 3 && oi[0].customerId == 0 && oi[0].vehicleId == 0 && oi[0].customer == "Mang Tonyo" && oi[0].lines.size() == 2);
            CHECK(oi[0].total() == 1000.0 && oi[0].amountPaid == 300.0 && oi[0].balance() == 700.0 && oi[0].status() == inv::PayStatus::Partial);
            CHECK(oi[1].status() == inv::PayStatus::Paid && oi[2].status() == inv::PayStatus::Unpaid && oi[2].balance() == 50.0);
            const auto owing = reports::receivablesByCustomer(oi, now);
            CHECK(owing.size() == 2 && owing[0].customer == "Mang Tonyo" && owing[0].balance == 700.0 && owing[1].customer == "Walk-in customer" && reports::totalReceivable(oi) == 750.0);
            const auto after = util::readCsvFile((pd / "invoices.csv").string());
            const auto before = util::parseCsvText(p2);
            CHECK(after.size() == before.size() && after[0].size() == 13 && after[0][11] == "customer_id" && after[0][12] == "vehicle_id");
            bool same = after.size() == before.size();
            for (size_t i = 1; same && i < after.size(); ++i) {
                same = after[i].size() == 13 && after[i][11].empty() && after[i][12].empty();
                for (size_t j = 0; same && j < 11; ++j) same = after[i][j] == before[i][j];   // every original field identical
            }
            CHECK(same);
            // the next sale on the upgraded file is linked, the old ones stay blank
            inv::Inventory p2again(pd.string());
            CHECK(p2again.invoices().all().size() == 3 && p2again.invoices().nextNumber() == "INV-000004");
            fs::remove_all(pd);
        }
        fs::remove_all(cd);
    }

    CHECK(in.removePart(106, "owner").ok);
    CHECK(in.movements().all().back().type == inv::MoveType::Delete && in.movements().all().back().resultingStock == 0);

    fs::remove_all(dir);
    if (failures == 0) std::cout << "ALL SMOKE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
