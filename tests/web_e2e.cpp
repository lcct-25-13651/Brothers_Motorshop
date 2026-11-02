#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include "httplib.h"
#include "web.h"
#include "reports.h"
using P = httplib::Params;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; ++fails; } } while (0)
static bool has(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }
static std::string csrfOf(const std::string& html) { auto p = html.find("name='csrf' value='"); return p == std::string::npos ? "" : html.substr(p + 19, 64); }
int main() {
    namespace fs = std::filesystem;
    auto dir = (fs::temp_directory_path() / "re_web_e2e").string(); fs::remove_all(dir); fs::create_directories(dir);
    inv::Inventory in(dir); auth::Authenticator us(dir + "/users.csv"); auth::AuditLog au(dir + "/audit.csv");
    web::WebApp app(in, us, au, {});
    auto& s = *httplib::Server::last;
    auto r = s.call("GET", "/"); CHECK(r.status == 303 && r.headers["Location"] == "/setup");
    r = s.call("POST", "/setup", {{"username","boss"},{"password","bosspass1"},{"password2","bosspass1"},{"question","First bike name?"},{"answer","thunderbolt1"}}); CHECK(r.status == 303);
    r = s.call("GET", "/"); CHECK(r.headers["Location"] == "/login");
    r = s.call("POST", "/login", {{"username","boss"},{"password","nope"}}); CHECK(has(r.headers["Location"], "err"));
    r = s.call("POST", "/login", {{"username","boss"},{"password","bosspass1"}}); CHECK(r.status == 303);
    std::string ck = r.headers["Set-Cookie"].substr(0, r.headers["Set-Cookie"].find(';'));
    r = s.call("GET", "/", {}, ck); CHECK(r.status == 200 && has(r.body, "Engine Oil 1L") && has(r.body, "id='sys'") && has(r.body, "data-sort='price'"));
    CHECK(has(r.body, "Delete") && has(r.body, "Reports"));
    std::string csrf = csrfOf(r.body); CHECK(csrf.size() == 64);
    // CSRF enforced
    r = s.call("POST", "/users/add", {{"username","juan"},{"password","juanpass1"},{"role","Staff"}}, ck); CHECK(r.status == 403);
    r = s.call("POST", "/users/add", {{"csrf",csrf},{"username","juan"},{"password","juanpass1"},{"role","Staff"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    // XSS escaping
    r = s.call("POST", "/add", {{"csrf",csrf},{"id","500"},{"name","<script>alert(1)</script>"},{"category","Engine"},{"price","10"},{"qty","3"},{"min","1"}}, ck); CHECK(r.status == 303);
    r = s.call("GET", "/", {}, ck); CHECK(!has(r.body, "<script>alert") && has(r.body, "&lt;script&gt;"));
    // p2supplier: suppliers, purchase history, supplier cost vs retail (margin)
    r = s.call("POST", "/suppliers/save", {{"csrf",csrf},{"id","0"},{"name","Bajaj Parts Depot"},{"phone","0917"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("GET", "/suppliers", {}, ck); CHECK(r.status == 200 && has(r.body, "Bajaj Parts Depot"));
    r = s.call("POST", "/suppliers/save", {{"csrf",csrf},{"id","0"},{"name","bajaj PARTS depot"}}, ck); CHECK(has(r.headers["Location"], "err"));   // duplicate name
    r = s.call("POST", "/stock", {{"csrf",csrf},{"id","101"},{"action","restock"},{"qty","2"},{"cost","100"},{"supplier","1"},{"setcost","1"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("GET", "/", {}, ck); CHECK(has(r.body, "Margin</th>") && has(r.body, "Bajaj Parts Depot") && has(r.body, "&#8369;100.00"));
    r = s.call("GET", "/suppliers", {{"view","1"}}, ck); CHECK(has(r.body, "Purchase history") && has(r.body, "Engine Oil 1L"));
    r = s.call("POST", "/suppliers/toggle", {{"csrf",csrf},{"id","1"},{"on","0"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("POST", "/suppliers/toggle", {{"csrf",csrf},{"id","1"},{"on","1"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    // price edit -> audit
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","8"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    // p1alert: low-stock banner + sidebar counter appear when a part is below its minimum
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","99999"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("GET", "/", {}, ck); CHECK(has(r.body, "alert-low") && has(r.body, "nbadge") && has(r.body, "Out of stock") && has(r.body, "href='/?low=1'"));
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","8"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    // Staff session
    auto lj = s.call("POST", "/login", {{"username","juan"},{"password","juanpass1"}});
    std::string ck2 = lj.headers["Set-Cookie"].substr(0, lj.headers["Set-Cookie"].find(';'));
    r = s.call("GET", "/", {}, ck2); CHECK(r.status == 200 && !has(r.body, "btn sm danger") && !has(r.body, ">Reports<") && has(r.body, ">Sell<"));
    CHECK(!has(r.body, "Margin</th>") && !has(r.body, "Suppliers</a>"));   // p2supplier: Staff see no costs or margins
    r = s.call("GET", "/suppliers", {}, ck2); CHECK(r.status == 403);
    std::string csrf2 = csrfOf(r.body);
    CHECK(s.call("GET", "/reports", {}, ck2).status == 403);
    CHECK(s.call("GET", "/users", {}, ck2).status == 403);
    CHECK(s.call("GET", "/audit", {}, ck2).status == 403);
    CHECK(s.call("GET", "/export/inventory.csv", {}, ck2).status == 403);
    CHECK(s.call("POST", "/delete", {{"csrf",csrf2},{"id","101"}}, ck2).status == 403);
    CHECK(s.call("POST", "/edit", {{"csrf",csrf2},{"id","101"},{"name","x"},{"category","Engine"},{"price","1"},{"min","1"}}, ck2).status == 403);
    CHECK(s.call("POST", "/add", {{"csrf",csrf2},{"id","900"},{"name","x"},{"category","Engine"},{"price","1"},{"qty","1"},{"min","1"}}, ck2).status == 403);
    CHECK(in.find(101) && in.find(101)->price() == 499.5);
    r = s.call("POST", "/stock", {{"csrf",csrf2},{"id","101"},{"action","adjust"},{"qty","-2"},{"note","damaged"}}, ck2); CHECK(has(r.headers["Location"], "ok"));
    // staff sells (price tampering ignored: no price field for parts)
    r = s.call("POST", "/invoice", {{"csrf",csrf2},{"customer","Pedro <b>Santos</b>"},{"p_id_1","101"},{"p_qty_1","2"},{"s_desc_1","Labor"},{"s_qty_1","1"},{"s_price_1","200"}}, ck2);
    CHECK(r.status == 303 && has(r.headers["Location"], "INV-000001"));
    r = s.call("GET", "/invoice", {{"no","INV-000001"}}, ck2); CHECK(has(r.body, "Pedro &lt;b&gt;Santos") && has(r.body, "1,199.00") && has(r.body, "Print receipt") && has(r.body, "Labor"));
    r = s.call("POST", "/invoice", {{"csrf",csrf2},{"p_id_1","101"},{"p_qty_1","9999"}}, ck2); CHECK(has(r.headers["Location"], "Not%20enough%20stock"));
    // ===== Phase 2: credit sales (utang) =====
    CHECK(has(s.call("GET", "/", {}, ck2).body, "Receivables / Utang") && has(s.call("GET", "/", {}, ck2).body, "Daily cash report"));
    r = s.call("POST", "/invoice", {{"csrf",csrf2},{"customer",""},{"p_id_1","103"},{"p_qty_1","2"},{"amount_paid","50"},{"method","Cash"}}, ck2);
    CHECK(has(r.headers["Location"], "needs%20a%20customer") && in.find(103)->quantity() == 30);        // walk-in cannot owe, stock untouched
    r = s.call("POST", "/invoice", {{"csrf",csrf2},{"customer","Mang Tonyo"},{"p_id_1","103"},{"p_qty_1","2"},{"amount_paid","abc"}}, ck2);
    CHECK(has(r.headers["Location"], "valid%20amount") && in.find(103)->quantity() == 30);
    r = s.call("POST", "/invoice", {{"csrf",csrf2},{"customer","Mang Tonyo"},{"p_id_1","103"},{"p_qty_1","2"},{"amount_paid","100"},{"method","GCash"}}, ck2);
    CHECK(r.status == 303 && has(r.headers["Location"], "INV-000002"));     // part 103 is 180 x 2 = 360, 100 paid -> 260 owing
    CHECK(in.invoices().find("INV-000002")->status() == inv::PayStatus::Partial && in.invoices().find("INV-000002")->balance() == 260.0);
    r = s.call("GET", "/invoice", {{"no","INV-000002"}}, ck2);
    CHECK(has(r.body, "BALANCE DUE") && has(r.body, "260.00") && has(r.body, ">Partial<") && has(r.body, "GCash") && has(r.body, "Record payment"));
    r = s.call("GET", "/invoice", {{"no","INV-000001"}}, ck2); CHECK(has(r.body, ">Paid<") && !has(r.body, "BALANCE DUE") && !has(r.body, "Record payment"));
    r = s.call("GET", "/invoices", {}, ck2); CHECK(has(r.body, "INV-000001") && has(r.body, "INV-000002") && has(r.body, "Balance"));
    r = s.call("GET", "/invoices", {{"status","partial"}}, ck2); CHECK(has(r.body, "INV-000002") && !has(r.body, "INV-000001"));
    r = s.call("GET", "/invoices", {{"status","unpaid"}}, ck2); CHECK(has(r.body, "No invoices with that status."));
    for (auto* who : {&ck, &ck2}) {
        r = s.call("GET", "/receivables", {}, *who);
        CHECK(r.status == 200 && has(r.body, "Mang Tonyo") && has(r.body, "260.00") && has(r.body, "Total outstanding") && has(r.body, "href='/payment?no=INV-000002'"));
    }
    r = s.call("GET", "/receivables", {{"customer","mang tonyo"}}, ck); CHECK(has(r.body, "Open invoices - Mang Tonyo") && has(r.body, "Show all customers"));
    CHECK(s.call("GET", "/receivables").headers["Location"] == "/login");
    r = s.call("GET", "/payment", {{"no","INV-000002"}}, ck2);
    CHECK(r.status == 200 && has(r.body, "name='balance' value='260.00'") && has(r.body, "Save payment") && has(r.body, "<option>GCash</option>"));
    CHECK(s.call("POST", "/payment", {{"no","INV-000002"},{"amount","50"},{"method","Cash"}}, ck2).status == 403);      // CSRF
    r = s.call("POST", "/payment", {{"csrf",csrf2},{"no","INV-000002"},{"amount","300"},{"method","Cash"},{"balance","260.00"}}, ck2);
    CHECK(has(r.headers["Location"], "more%20than") && in.invoices().find("INV-000002")->amountPaid == 100.0);          // overpayment refused
    r = s.call("POST", "/payment", {{"csrf",csrf2},{"no","INV-000002"},{"amount","1,000x"},{"method","Cash"}}, ck2); CHECK(has(r.headers["Location"], "err"));
    r = s.call("POST", "/payment", {{"csrf",csrf2},{"no","INV-000002"},{"amount","100"},{"method","Cash"},{"note","2nd <i>installment</i>"},{"balance","260.00"}}, ck2);
    CHECK(has(r.headers["Location"], "kind=ok") && has(r.headers["Location"], "INV-000002") && in.invoices().find("INV-000002")->balance() == 160.0);
    r = s.call("POST", "/payment", {{"csrf",csrf2},{"no","INV-000002"},{"amount","100"},{"method","Cash"},{"balance","260.00"}}, ck2);   // same form sent again
    CHECK(has(r.headers["Location"], "changed") && in.invoices().find("INV-000002")->balance() == 160.0);
    r = s.call("GET", "/invoice", {{"no","INV-000002"}}, ck2); CHECK(has(r.body, "2nd &lt;i&gt;installment&lt;/i&gt;") && has(r.body, "160.00") && !has(r.body, "<i>installment"));
    r = s.call("POST", "/payment", {{"csrf",csrf},{"no","INV-000002"},{"amount","160"},{"method","Maya"},{"balance","160.00"}}, ck);     // admin settles the rest
    CHECK(has(r.headers["Location"], "fully%20paid") && in.invoices().find("INV-000002")->status() == inv::PayStatus::Paid);
    r = s.call("GET", "/receivables", {}, ck); CHECK(!has(r.body, "Mang Tonyo") && has(r.body, "Nobody owes the shop anything"));
    r = s.call("GET", "/payment", {{"no","INV-000002"}}, ck); CHECK(r.status == 303 && has(r.headers["Location"], "already%20fully%20paid"));
    {   // invoices.csv holds the updated balance, payments.csv the instalments
        std::string csvTxt, payTxt;
        { std::ifstream f(dir + "/invoices.csv"); csvTxt.assign(std::istreambuf_iterator<char>(f), {}); }
        { std::ifstream f(dir + "/payments.csv"); payTxt.assign(std::istreambuf_iterator<char>(f), {}); }
        CHECK(has(csvTxt, "amount_paid,payment_status") && has(csvTxt, "360.00,Paid") && !has(csvTxt, "Partial"));
        CHECK(has(payTxt, "INV-000002") && has(payTxt, "GCash") && has(payTxt, "Maya") && has(payTxt, "paid at sale"));
    }
    // a second, still-unpaid credit sale for the cash report
    r = s.call("POST", "/invoice", {{"csrf",csrf2},{"customer","Aling Nena"},{"p_id_1","103"},{"p_qty_1","1"},{"amount_paid","0"}}, ck2);
    CHECK(has(r.headers["Location"], "INV-000003") && in.invoices().find("INV-000003")->status() == inv::PayStatus::Unpaid);

    // ===== Phase 2: daily cash report =====
    for (auto* who : {&ck, &ck2}) {
        r = s.call("GET", "/cash-report", {}, *who);
        CHECK(r.status == 200 && has(r.body, "Daily cash report") && has(r.body, "Payment method breakdown") && has(r.body, "Net collected today") && has(r.body, "Gross sales") &&
              has(r.body, "data-print") && has(r.body, "Expected cash in drawer") && has(r.body, "GCash") && has(r.body, "Maya") && has(r.body, "Aling Nena") && has(r.body, "Prepared by"));
    }
    r = s.call("GET", "/cash-report", {}, ck2);
    CHECK(has(r.body, "&#8369;1,199.00") && has(r.body, "&#8369;1,739.00"));      // gross: 1199 + 360 + 180; the 180 sale is all credit -> collected 1199 + 100 + 100 + 160 = 1559
    CHECK(has(r.body, "&#8369;1,559.00") && has(r.body, "&#8369;180.00"));
    r = s.call("GET", "/cash-report", {{"date","2001-01-01"}}, ck2); CHECK(has(r.body, "No invoices on this date.") && has(r.body, "No payments were received"));
    r = s.call("GET", "/cash-report", {{"date","2026-02-30"}}, ck2); CHECK(has(r.body, "not a valid date"));
    CHECK(s.call("GET", "/cash-report").headers["Location"] == "/login");

    // ===== Phase 2: vehicle compatibility =====
    r = s.call("POST", "/add", {{"csrf",csrf},{"id","600"},{"name","Piston Kit"},{"category","Engine"},{"price","900"},{"qty","4"},{"min","1"},{"models","Bajaj RE 4S, Bajaj RE Maxima; <b>x</b>"}}, ck);
    CHECK(r.status == 303 && in.find(600) && in.find(600)->fitsModel("Bajaj RE Maxima") && in.find(600)->fitsModel("Bajaj RE 4S"));
    r = s.call("POST", "/add", {{"csrf",csrf},{"id","601"},{"name","Clutch Cable"},{"category","Transmission"},{"price","150"},{"qty","4"},{"min","1"},{"models","Bajaj RE Compact"}}, ck);
    CHECK(r.status == 303 && in.find(601));
    r = s.call("POST", "/add", {{"csrf",csrf},{"id","602"},{"name","Long"},{"category","Engine"},{"price","1"},{"qty","1"},{"min","1"},{"models",std::string(400, 'z')}}, ck);
    CHECK(has(r.headers["Location"], "too%20long") && !in.find(602));
    r = s.call("GET", "/", {}, ck2);
    CHECK(has(r.body, "<select id='model'") && has(r.body, "All vehicle models") && has(r.body, "<option>Bajaj RE 4S</option>") && has(r.body, "<option>Bajaj RE Maxima</option>") &&
          has(r.body, "<option>Bajaj RE Compact</option>") && has(r.body, "data-models='bajaj re 4s|bajaj re maxima|&lt;b&gt;x&lt;/b&gt;'") && has(r.body, "Fits (vehicle models)") &&
          has(r.body, "Bajaj RE 4S, Bajaj RE Maxima, &lt;b&gt;x&lt;/b&gt;") && !has(r.body, "<b>x</b>") && has(r.body, "data-models=''") && has(r.body, "vehicle model"));
    r = s.call("GET", "/", {{"model","bajaj re compact"}}, ck2); CHECK(has(r.body, "<option selected>Bajaj RE Compact</option>") && !has(r.body, "<option selected>Bajaj RE 4S"));
    r = s.call("GET", "/static/app.js"); CHECK(has(r.body, "getElementById('model')") && has(r.body, "r.hay"));
    r = s.call("GET", "/edit", {{"id","601"}}, ck); CHECK(has(r.body, "name='models'") && has(r.body, "value='Bajaj RE Compact'"));
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","601"},{"name","Clutch Cable"},{"category","Transmission"},{"price","160"},{"min","1"}}, ck);   // old form without the field
    CHECK(has(r.headers["Location"], "ok") && in.find(601)->fitsModel("Bajaj RE Compact"));
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","601"},{"name","Clutch Cable"},{"category","Transmission"},{"price","160"},{"min","1"},{"models","Bajaj RE Compact, Bajaj RE Maxima"}}, ck);
    CHECK(has(r.headers["Location"], "ok") && in.find(601)->fitsModel("Bajaj RE Maxima"));
    CHECK(s.call("POST", "/edit", {{"csrf",csrf2},{"id","601"},{"name","x"},{"category","Engine"},{"price","1"},{"min","1"},{"models","hack"}}, ck2).status == 403);
    CHECK(!in.find(601)->fitsModel("hack"));
    r = s.call("POST", "/import", {{"csrf",csrf},{"csv","id,name,system,price,quantity,compatible_models\n601,,,,4,Bajaj RE Compact\n"}}, ck); CHECK(has(r.body, "1 updated") && !in.find(601)->fitsModel("Bajaj RE Maxima"));

    // ===== Phase 3: customer & vehicle records =====
    {
        const size_t oldInvoiceCount = in.invoices().all().size();
        const double oldReceivable = reports::totalReceivable(in.invoices().all());
        CHECK(s.call("GET", "/customers").headers["Location"] == "/login");
        CHECK(s.call("GET", "/vehicle", {{"id","1"}}).headers["Location"] == "/login");
        for (auto* who : {&ck, &ck2}) {   // Staff and Admin both work with customers
            r = s.call("GET", "/customers", {}, *who); CHECK(r.status == 200 && has(r.body, "Search by name, phone or plate number") && has(r.body, "No customers yet") && has(r.body, ">Customers</a>"));
        }
        CHECK(s.call("POST", "/customer/save", {{"name","No Token"}}, ck2).status == 403);                      // CSRF
        r = s.call("POST", "/customer/save", {{"csrf",csrf2},{"id","0"},{"name",""}}, ck2); CHECK(has(r.headers["Location"], "err") && in.invoices().all().size() == oldInvoiceCount);
        r = s.call("POST", "/customer/save", {{"csrf",csrf2},{"id","0"},{"name","Mang Tonyo"},{"phone","0917 111 2222"},{"address","Dasmarinas"},{"notes","<b>trike</b>"}}, ck2);
        CHECK(has(r.headers["Location"], "kind=ok") && has(r.headers["Location"], "/customer?id=1"));
        r = s.call("POST", "/customer/save", {{"csrf",csrf2},{"id","0"},{"name","Aling Nena"},{"phone","0918 222 3333"}}, ck2);
        CHECK(has(r.headers["Location"], "/customer?id=2"));
        r = s.call("POST", "/customer/save", {{"csrf",csrf2},{"id","0"},{"name","aling  NENA"},{"phone","09182223333"}}, ck2); CHECK(has(r.headers["Location"], "err") && has(r.headers["Location"], "already%20exists"));
        r = s.call("GET", "/customer/edit", {{"name","aling  NENA"}}, ck2); CHECK(r.status == 200 && has(r.body, "value='aling  NENA'"));   // typed values survive an error
        r = s.call("POST", "/customer/save", {{"csrf",csrf2},{"id","0"},{"name","<script>alert(2)</script>"}}, ck2); CHECK(has(r.headers["Location"], "kind=ok"));
        r = s.call("GET", "/customers", {}, ck2); CHECK(has(r.body, "Mang Tonyo") && has(r.body, "Aling Nena") && has(r.body, "&lt;script&gt;alert(2)") && !has(r.body, "<script>alert(2)"));
        r = s.call("GET", "/customer", {{"id","1"}}, ck2); CHECK(has(r.body, "&lt;b&gt;trike&lt;/b&gt;") && !has(r.body, "<b>trike</b>") && has(r.body, "Customer info") && has(r.body, "Invoice history"));
        CHECK(has(s.call("GET", "/customer", {{"id","77"}}, ck2).headers["Location"], "not%20found"));

        // vehicles: plate saved UPPERCASE without spaces, must be unique
        r = s.call("POST", "/vehicle/save", {{"csrf",csrf2},{"id","0"},{"customer_id","2"},{"plate"," abc  123 "},{"model","Bajaj RE Maxima"},{"year","2018"},{"color","Green"}}, ck2);
        CHECK(has(r.headers["Location"], "kind=ok") && has(r.headers["Location"], "/vehicle?id=1"));
        r = s.call("POST", "/vehicle/save", {{"csrf",csrf2},{"id","0"},{"customer_id","1"},{"plate","ABC123"}}, ck2); CHECK(has(r.headers["Location"], "err") && has(r.headers["Location"], "already%20registered"));
        r = s.call("POST", "/vehicle/save", {{"csrf",csrf2},{"id","0"},{"customer_id","1"},{"plate","tr 99"},{"year","abc"}}, ck2); CHECK(has(r.headers["Location"], "err") && has(r.headers["Location"], "Year"));
        r = s.call("POST", "/vehicle/save", {{"csrf",csrf2},{"id","0"},{"customer_id","1"},{"plate","tr 99"},{"model","RE Compact"}}, ck2); CHECK(has(r.headers["Location"], "/vehicle?id=2"));
        r = s.call("POST", "/vehicle/save", {{"csrf",csrf2},{"id","0"},{"customer_id","55"},{"plate","ZZZ 1"}}, ck2); CHECK(has(r.headers["Location"], "err"));
        r = s.call("GET", "/vehicle/edit", {{"customer","1"}}, ck2); CHECK(r.status == 200 && has(r.body, "name='plate'") && has(r.body, "Mang Tonyo"));
        CHECK(in.invoices().all().size() == oldInvoiceCount && reports::totalReceivable(in.invoices().all()) == oldReceivable);   // records alone change no invoice or balance
        r = s.call("GET", "/customer/edit", {{"id","1"}}, ck2); CHECK(r.status == 200 && has(r.body, "value='Mang Tonyo'") && has(r.body, "value='0917 111 2222'"));
        r = s.call("POST", "/customer/save", {{"csrf",csrf2},{"id","1"},{"name","Mang Tonyo"},{"phone","0917 111 9999"},{"address","Dasmarinas"},{"notes","trike"}}, ck2);
        CHECK(has(r.headers["Location"], "kind=ok") && in.invoices().all().size() == oldInvoiceCount);
        r = s.call("GET", "/customers", {{"q","9999"}}, ck2); CHECK(has(r.body, "Mang Tonyo"));
        r = s.call("POST", "/customer/save", {{"csrf",csrf2},{"id","1"},{"name","Aling Nena"},{"phone","0918 222 3333"}}, ck2); CHECK(has(r.headers["Location"], "err"));   // would duplicate #2

        // find a customer by plate / phone / name
        r = s.call("GET", "/customers", {{"q","abc 123"}}, ck2); CHECK(has(r.body, "Aling Nena") && has(r.body, "<b>ABC123</b>") && has(r.body, "plate match") && !has(r.body, "Mang Tonyo"));
        r = s.call("GET", "/customers", {{"q","tr99"}}, ck2); CHECK(has(r.body, "Mang Tonyo") && !has(r.body, "Aling Nena"));
        r = s.call("GET", "/customers", {{"q","0917"}}, ck2); CHECK(has(r.body, "Mang Tonyo") && !has(r.body, "Aling Nena"));
        r = s.call("GET", "/customers", {{"q","nena"}}, ck2); CHECK(has(r.body, "Aling Nena") && !has(r.body, "Mang Tonyo"));
        r = s.call("GET", "/customers", {{"q","nobody here"}}, ck2); CHECK(has(r.body, "Nobody matches"));
        r = s.call("GET", "/vehicle", {{"plate","abc 123"}}, ck2); CHECK(r.status == 303 && r.headers["Location"] == "/vehicle?id=1");
        CHECK(has(s.call("GET", "/vehicle", {{"plate","nope"}}, ck2).headers["Location"], "No%20vehicle"));
        r = s.call("GET", "/vehicle", {{"id","1"}}, ck2); CHECK(r.status == 200 && has(r.body, "ABC123") && has(r.body, "Bajaj RE Maxima") && has(r.body, "No invoices yet."));

        // Aling Nena already owes 180 on the OLD free-text invoice INV-000003: her record shows it ("by name"), same number as Receivables
        r = s.call("GET", "/customer", {{"id","2"}}, ck2); CHECK(has(r.body, "INV-000003") && has(r.body, "(by name)") && has(r.body, "&#8369;180.00") && has(r.body, "Utang balance"));
        r = s.call("GET", "/receivables", {}, ck2); CHECK(has(r.body, "href='/customer?id=2'") && has(r.body, "&#8369;180.00"));

        // New sale: pick a saved customer + vehicle
        r = s.call("GET", "/invoice", {}, ck2);
        CHECK(has(r.body, "id='cust-in'") && has(r.body, "id='veh-in'") && has(r.body, "<option value='Aling Nena - 0918 222 3333' data-id='2'>") && has(r.body, "data-id='1' data-cid='2'>") &&
              has(r.body, "ABC123 - Bajaj RE Maxima (Aling Nena)") && has(r.body, "href='/customer/edit?from=sale'"));
        r = s.call("GET", "/static/app.js"); CHECK(has(r.body, "cust-in") && has(r.body, "veh-in"));
        auto sale = [&](const std::string& cust, const std::string& veh, const std::string& paid, const std::string& cid = "", const std::string& vidp = "") {
            return s.call("POST", "/invoice", {{"csrf",csrf2},{"customer",cust},{"vehicle",veh},{"customer_id",cid},{"vehicle_id",vidp},{"p_id_1","103"},{"p_qty_1","1"},{"amount_paid",paid}}, ck2);
        };
        r = sale("Aling Nena - 0918 222 3333", "ABC123 - Bajaj RE Maxima (Aling Nena)", "100", "2", "1");
        CHECK(r.status == 303 && has(r.headers["Location"], "INV-000004"));
        { const inv::Invoice* v = in.invoices().find("INV-000004"); CHECK(v && v->customerId == 2 && v->vehicleId == 1 && v->customer == "Aling Nena" && v->balance() == 80.0); }
        r = s.call("GET", "/invoice", {{"no","INV-000004"}}, ck2); CHECK(has(r.body, "Vehicle:") && has(r.body, "ABC123") && has(r.body, "href='/customer?id=2'") && has(r.body, "href='/vehicle?id=1'") && has(r.body, "BALANCE DUE"));
        r = sale("aling nena", "abc 123", "");   // typed by hand: name + plate, both resolved on the server (no JS needed); paid in full
        CHECK(has(r.headers["Location"], "INV-000005") && in.invoices().find("INV-000005")->customerId == 2 && in.invoices().find("INV-000005")->vehicleId == 1);
        r = sale("", "abc123", "");               // plate only: the owner becomes the customer
        CHECK(has(r.headers["Location"], "INV-000006") && in.invoices().find("INV-000006")->customerId == 2 && in.invoices().find("INV-000006")->customer == "Aling Nena");
        r = sale("Mang Tonyo", "ABC123", ""); CHECK(has(r.headers["Location"], "belongs%20to") && in.invoices().find("INV-000007") == nullptr);   // someone else's vehicle
        r = sale("Pedro Walkin", "ABC123", ""); CHECK(has(r.headers["Location"], "saved%20customer"));
        r = sale("Aling Nena", "QQQ 000", ""); CHECK(has(r.headers["Location"], "not%20in%20the%20customer%20records") && in.invoices().find("INV-000007") == nullptr);
        { const int stock = in.find(103)->quantity(); r = sale("Aling Nena", "QQQ 000", ""); CHECK(in.find(103)->quantity() == stock); }   // refused sale takes nothing from stock
        // free text still works exactly as before: no record -> stays free text, credit rules unchanged
        r = sale("Brand New Free Text", "", "10");
        CHECK(has(r.headers["Location"], "INV-000007") && in.invoices().find("INV-000007")->customerId == 0 && in.invoices().find("INV-000007")->vehicleId == 0 && in.invoices().find("INV-000007")->customer == "Brand New Free Text");
        r = sale("", "", "10"); CHECK(has(r.headers["Location"], "needs%20a%20customer"));
        r = sale("Mang Tonyo", "", "0");        // typed name equals a saved customer -> linked automatically, credit allowed
        CHECK(has(r.headers["Location"], "INV-000008") && in.invoices().find("INV-000008")->customerId == 1 && in.invoices().find("INV-000008")->vehicleId == 0 && in.invoices().find("INV-000008")->status() == inv::PayStatus::Unpaid);
        r = s.call("GET", "/invoice", {{"customer","Aling Nena - 0918 222 3333"},{"vehicle","bad"}}, ck2); CHECK(has(r.body, "value='Aling Nena - 0918 222 3333'") && has(r.body, "value='bad'"));

        // histories
        r = s.call("GET", "/vehicle", {{"id","1"}}, ck2);
        CHECK(has(r.body, "INV-000004") && has(r.body, "INV-000005") && has(r.body, "INV-000006") && !has(r.body, "INV-000003") && !has(r.body, "No invoices yet."));
        CHECK(r.body.find("INV-000006") < r.body.find("INV-000005") && r.body.find("INV-000005") < r.body.find("INV-000004"));            // newest first
        CHECK(has(r.body, "href='/payment?no=INV-000004'") && has(r.body, "History (newest first)"));
        r = s.call("GET", "/customer", {{"id","2"}}, ck2);
        CHECK(has(r.body, "INV-000003") && has(r.body, "INV-000004") && has(r.body, "INV-000006") && has(r.body, "href='/vehicle?id=1'") && has(r.body, "&#8369;260.00"));   // 180 (old) + 80 owed, one view
        { double nena = 0; for (const auto& d : reports::receivablesByCustomer(in.invoices().all(), std::time(nullptr))) if (d.customer == "Aling Nena") nena = d.balance; CHECK(nena == 260.0); }
        r = s.call("GET", "/receivables", {}, ck2); CHECK(has(r.body, "&#8369;260.00") && has(r.body, "Brand New Free Text") && has(r.body, "href='/customer?id=1'"));
        r = s.call("GET", "/invoices", {}, ck2); CHECK(has(r.body, "<th>Vehicle</th>") && has(r.body, "href='/vehicle?id=1'>ABC123</a>") && has(r.body, "INV-000001") && has(r.body, "Pedro &lt;b&gt;Santos"));
        r = s.call("GET", "/customers", {}, ck2); CHECK(has(r.body, "&#8369;260.00"));
        // a payment on a linked invoice works and the customer's balance follows
        r = s.call("POST", "/payment", {{"csrf",csrf2},{"no","INV-000004"},{"amount","80"},{"method","Cash"},{"balance","80.00"}}, ck2); CHECK(has(r.headers["Location"], "kind=ok"));
        r = s.call("GET", "/customer", {{"id","2"}}, ck2); CHECK(has(r.body, "&#8369;180.00") && !has(r.body, "&#8369;260.00"));

        // archive / delete are Admin only; history is never deleted
        CHECK(s.call("POST", "/customer/archive", {{"csrf",csrf2},{"id","2"},{"on","1"}}, ck2).status == 403);
        CHECK(s.call("POST", "/customer/delete", {{"csrf",csrf2},{"id","2"}}, ck2).status == 403);
        CHECK(s.call("POST", "/vehicle/archive", {{"csrf",csrf2},{"id","1"},{"on","1"}}, ck2).status == 403);
        CHECK(s.call("POST", "/vehicle/delete", {{"csrf",csrf2},{"id","1"}}, ck2).status == 403);
        r = s.call("GET", "/customer", {{"id","2"}}, ck2); CHECK(!has(r.body, ">Archive<") && !has(r.body, ">Delete<"));
        r = s.call("GET", "/customer", {{"id","2"}}, ck); CHECK(has(r.body, ">Archive<") && !has(r.body, ">Delete<"));      // admin sees Archive; Delete is hidden because of the history
        r = s.call("POST", "/customer/delete", {{"csrf",csrf},{"id","2"}}, ck); CHECK(has(r.headers["Location"], "err") && has(r.headers["Location"], "archive") && in.invoices().find("INV-000004") && !s.call("GET", "/customer", {{"id","2"}}, ck).body.empty());
        r = s.call("POST", "/vehicle/delete", {{"csrf",csrf},{"id","1"}}, ck); CHECK(has(r.headers["Location"], "err") && has(r.headers["Location"], "archive"));
        r = s.call("GET", "/customers", {{"q","nena"}}, ck); CHECK(has(r.body, "Aling Nena"));
        r = s.call("POST", "/vehicle/archive", {{"csrf",csrf},{"id","1"},{"on","1"}}, ck); CHECK(has(r.headers["Location"], "kind=ok"));
        r = s.call("GET", "/vehicle", {{"id","1"}}, ck); CHECK(has(r.body, "Archived") && has(r.body, "INV-000004") && has(r.body, ">Restore<"));       // history of an archived vehicle is still there
        r = s.call("GET", "/customers", {{"q","abc123"}}, ck); CHECK(has(r.body, "Aling Nena"));                                                          // ...and its owner can still be found by that plate
        r = sale("Aling Nena", "ABC123", ""); CHECK(has(r.headers["Location"], "archived"));                                                                // not sellable while archived
        r = s.call("POST", "/vehicle/archive", {{"csrf",csrf},{"id","1"},{"on","0"}}, ck); CHECK(has(r.headers["Location"], "kind=ok"));
        r = s.call("POST", "/customer/archive", {{"csrf",csrf},{"id","2"},{"on","1"}}, ck); CHECK(has(r.headers["Location"], "kind=ok") && in.invoices().find("INV-000004")->customerId == 2);
        r = s.call("GET", "/customers", {{"q","nena"}}, ck2); CHECK(!has(r.body, "href='/customer?id=2'") && has(r.body, "archived customer(s) also match"));
        r = s.call("GET", "/customers", {{"q","nena"},{"archived","1"}}, ck2); CHECK(has(r.body, "Aling Nena") && has(r.body, "Archived"));
        r = s.call("GET", "/invoice", {}, ck2); CHECK(!has(r.body, "<option value='Aling Nena - 0918 222 3333'") && !has(r.body, "(Aling Nena)'"));     // gone from autocomplete
        r = sale("Aling Nena - 0918 222 3333", "", ""); CHECK(has(r.headers["Location"], "archived"));
        r = s.call("GET", "/invoice", {{"no","INV-000004"}}, ck2); CHECK(r.status == 200 && has(r.body, "Aling Nena") && has(r.body, "ABC123"));       // old receipts are untouched
        r = s.call("GET", "/receivables", {}, ck2); CHECK(has(r.body, "Aling Nena"));                                                                     // her utang is still collectable
        r = s.call("POST", "/customer/archive", {{"csrf",csrf},{"id","2"},{"on","0"}}, ck); CHECK(has(r.headers["Location"], "kind=ok"));
        // a customer with no history and no vehicles can be deleted by an Admin
        r = s.call("POST", "/customer/save", {{"csrf",csrf},{"id","0"},{"name","Temp Customer"}}, ck); CHECK(has(r.headers["Location"], "/customer?id=4"));
        r = s.call("GET", "/customer", {{"id","4"}}, ck); CHECK(has(r.body, ">Delete<"));
        r = s.call("POST", "/customer/delete", {{"csrf",csrf},{"id","4"}}, ck); CHECK(has(r.headers["Location"], "kind=ok") && has(s.call("GET", "/customer", {{"id","4"}}, ck).headers["Location"], "not%20found"));
        // edit keeps the plate unique and the owner
        r = s.call("POST", "/vehicle/save", {{"csrf",csrf2},{"id","1"},{"customer_id","1"},{"plate","abc123"},{"model","Bajaj RE 4S"},{"year","2020"}}, ck2);
        CHECK(has(r.headers["Location"], "kind=ok") && in.invoices().find("INV-000004")->vehicleId == 1);
        r = s.call("POST", "/vehicle/save", {{"csrf",csrf2},{"id","1"},{"customer_id","1"},{"plate","tr 99"}}, ck2); CHECK(has(r.headers["Location"], "err"));
        r = s.call("GET", "/vehicle", {{"id","1"}}, ck2); CHECK(has(r.body, "Aling Nena") && has(r.body, "Bajaj RE 4S") && !has(r.body, "Mang Tonyo&"));
        // the data is on disk in the specified layout and survives a restart; old invoices and receivables are intact
        {
            std::string cs, vs, is;
            { std::ifstream f(dir + "/customers.csv"); cs.assign(std::istreambuf_iterator<char>(f), {}); }
            { std::ifstream f(dir + "/vehicles.csv"); vs.assign(std::istreambuf_iterator<char>(f), {}); }
            { std::ifstream f(dir + "/invoices.csv"); is.assign(std::istreambuf_iterator<char>(f), {}); }
            CHECK(cs.rfind("id,name,phone,address,notes", 0) == 0 && has(cs, "Mang Tonyo") && has(cs, "Aling Nena"));
            CHECK(vs.rfind("id,customer_id,plate,model,year,color,notes", 0) == 0 && has(vs, "ABC123") && !has(vs, "abc"));
            CHECK(is.rfind("invoice_no,timestamp,customer,username,line_type,part_id,description,qty,unit_price,amount_paid,payment_status,customer_id,vehicle_id", 0) == 0 && has(is, "Pedro <b>Santos</b>"));
            inv::Inventory reopened(dir);
            CHECK(reopened.invoices().all().size() == oldInvoiceCount + 5 && reopened.invoices().find("INV-000001")->customerId == 0 && reopened.invoices().find("INV-000001")->total() == 1199.0);
            CHECK(reopened.invoices().find("INV-000006")->customerId == 2 && reopened.invoices().find("INV-000006")->vehicleId == 1);
        }
        { auto* e = &au.entries(); std::string ev; for (auto& x : *e) ev += x.event + ","; for (const char* n : {"CUSTOMER_ADDED", "CUSTOMER_UPDATED", "CUSTOMER_ARCHIVED", "CUSTOMER_RESTORED", "CUSTOMER_DELETED", "VEHICLE_ADDED", "VEHICLE_UPDATED", "VEHICLE_ARCHIVED", "VEHICLE_RESTORED"}) { if (!has(ev, n)) { std::cerr << "missing audit event " << n << "\n"; ++fails; } } }
    }

    // ===== Phase 4: job orders - the HTML page and the JSON API behind the real session gate =====
    {
        r = s.call("GET", "/jobs", {}, ck);
        CHECK(r.status == 200 && has(r.body, "Job orders") && has(r.body, ">Job orders<") && has(r.body, "data-csrf=\"") &&
              has(r.body, "src=\"/static/jobs.js\""));
        std::string acsrf;
        { auto at = r.body.find("data-csrf=\""); if (at != std::string::npos) acsrf = r.body.substr(at + 11, 64); }
        CHECK(acsrf.size() == 64);
        const std::map<std::string, std::string> tok{{"X-CSRF-Token", acsrf}};

        r = s.call("POST", "/api/jobs", {{"plate", "ABC123"}, {"complaint", "Engine knock"}}, ck, "", tok);
        CHECK(r.status == 201 && has(r.body, "\"job_no\":\"JOB-000001\"") && has(r.body, "\"status\":\"Open\""));
        const int stock103 = in.find(103)->quantity();
        r = s.call("POST", "/api/jobs/JOB-000001/lines", {}, ck, "{\"kind\":\"Part\",\"part_id\":103,\"quantity\":2}", tok);
        CHECK(r.status == 201 && in.find(103)->quantity() == stock103 - 2);          // stock goes at once
        r = s.call("POST", "/api/jobs/JOB-000001/lines", {}, ck, "{\"kind\":\"Labor\",\"service_id\":1}", tok);
        CHECK(r.status == 201 && has(r.body, "\"labor_fee\":300.00"));
        r = s.call("GET", "/api/jobs", {}, ck);
        CHECK(r.status == 200 && has(r.body, "\"grand_total\":660.00"));      // 2 x 180 + 300
        r = s.call("POST", "/api/jobs/JOB-000001/status", {}, ck, "{\"status\":\"In progress\"}", tok);
        CHECK(r.status == 200);
        r = s.call("DELETE", "/api/jobs/JOB-000001/lines/1", {}, ck, "", tok);
        CHECK(r.status == 200 && in.find(103)->quantity() == stock103);              // ...and comes straight back

        // Staff work with jobs, but the service catalogue is Admin-only
        std::string acsrf2;
        { std::string p2 = s.call("GET", "/jobs", {}, ck2).body; auto at = p2.find("data-csrf=\"");
          if (at != std::string::npos) acsrf2 = p2.substr(at + 11, 64); }
        const std::map<std::string, std::string> tok2{{"X-CSRF-Token", acsrf2}};
        CHECK(s.call("GET", "/jobs", {}, ck2).status == 200);
        CHECK(s.call("GET", "/api/services", {}, ck2).status == 200);
        CHECK(s.call("POST", "/api/services", {}, ck2, "{\"name\":\"Wash\"}", tok2).status == 403);

        // a cross-site post has no token; a signed-out caller gets 401
        CHECK(s.call("POST", "/api/jobs", {{"complaint", "x"}}, ck, "", {}).status == 403);   // no CSRF token
        CHECK(s.call("GET", "/api/jobs", {}, "").status == 401);
        CHECK(s.call("GET", "/jobs", {}, "").headers["Location"] == "/login");

        std::string jev;
        for (auto& e : au.entries()) jev += e.event + ",";
        for (const char* n : {"JOB_CREATED", "JOB_LINE_ADDED", "JOB_LINE_REMOVED", "JOB_STATUS"}) CHECK(has(jev, n));
        CHECK(fs::exists(dir + "/jobs.csv") && fs::exists(dir + "/job_lines.csv") && fs::exists(dir + "/services.csv"));
        { std::ifstream f(dir + "/jobs.csv"); std::string js; js.assign(std::istreambuf_iterator<char>(f), {});
          CHECK(js.rfind("job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes,invoice_no", 0) == 0); }
        { std::ifstream f(dir + "/services.csv"); std::string ss; ss.assign(std::istreambuf_iterator<char>(f), {});
          CHECK(has(ss, "Change Oil") && has(ss, "Brake Pad Replacement") && has(ss, "Engine Overhaul")); }
    }

    // admin pages
    r = s.call("GET", "/reports", {}, ck); CHECK(r.status == 200 && has(r.body, "Reorder list") && has(r.body, "Dead stock") && has(r.body, "Headlight Bulb") && has(r.body, "Top-selling"));
    r = s.call("GET", "/export/movements.csv", {}, ck); CHECK(r.body.rfind("\xEF\xBB\xBF", 0) == 0 && has(r.body, "SALE") && has(r.body, "ADJUSTMENT"));
    r = s.call("POST", "/import", {{"csrf",csrf},{"csv","id,name,system,price,quantity\n103,,,,99\n"}}, ck); CHECK(r.status == 200 && has(r.body, "1 updated"));
    r = s.call("POST", "/backup", {{"csrf",csrf}}, ck); CHECK(has(r.headers["Location"], "ok"));
    CHECK(fs::exists(dir + "/backups"));
    r = s.call("POST", "/delete", {{"csrf",csrf},{"id","500"}}, ck); CHECK(has(r.headers["Location"], "ok") && !in.find(500));
    r = s.call("POST", "/users/reset", {{"csrf",csrf},{"username","juan"},{"password","brandnew99"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    CHECK(s.call("GET", "/", {}, ck2).headers["Location"] == "/login");  // juan's session ended by the reset
    r = s.call("POST", "/logout", {{"csrf",csrf}}, ck); CHECK(r.status == 303);
    CHECK(s.call("GET", "/", {}, ck).headers["Location"] == "/login");
    // brute force lock
    for (int i = 0; i < 6; ++i) s.call("POST", "/login", {{"username","boss"},{"password","bad"}});
    r = s.call("POST", "/login", {{"username","boss"},{"password","bosspass1"}}); CHECK(has(r.headers["Location"], "Too%20many"));
    std::string events; for (auto& e : au.entries()) events += e.event + ",";
    for (const char* e : {"LOGIN", "LOGOUT", "LOGIN_FAILED", "PRICE_CHANGE", "RECORD_DELETED", "PASSWORD_RESET", "ACCESS_DENIED", "BACKUP", "IMPORT", "USER_CREATED", "CREDIT_SALE", "PAYMENT_RECORDED", "PART_MODELS_CHANGED"}) CHECK(has(events, e));
    fs::remove_all(dir);
    std::cout << (fails ? "E2E FAILED\n" : "WEB E2E PASSED\n"); return fails ? 1 : 0;
}
