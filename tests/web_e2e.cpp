#include <filesystem>
#include <iostream>
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
    for (const char* e : {"LOGIN", "LOGOUT", "LOGIN_FAILED", "PRICE_CHANGE", "RECORD_DELETED", "PASSWORD_RESET", "ACCESS_DENIED", "BACKUP", "IMPORT", "USER_CREATED"}) CHECK(has(events, e));
    fs::remove_all(dir);
    std::cout << (fails ? "E2E FAILED\n" : "WEB E2E PASSED\n"); return fails ? 1 : 0;
}
