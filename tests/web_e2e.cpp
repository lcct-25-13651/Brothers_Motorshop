#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include "httplib.h"
#include "web.h"
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
    // price edit -> audit
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","8"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    // Staff session
    auto lj = s.call("POST", "/login", {{"username","juan"},{"password","juanpass1"}});
    std::string ck2 = lj.headers["Set-Cookie"].substr(0, lj.headers["Set-Cookie"].find(';'));
    r = s.call("GET", "/", {}, ck2); CHECK(r.status == 200 && !has(r.body, "btn sm danger") && !has(r.body, ">Reports<") && has(r.body, ">Sell<"));
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
