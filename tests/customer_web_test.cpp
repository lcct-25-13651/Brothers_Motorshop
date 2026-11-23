// customer_web_test.cpp - customer form / list / sale-form behaviour (phone type, no digits in names, customer ID),
// driven through the in-process httplib stand-in like web_e2e.cpp.
#include <filesystem>
#include <iostream>
#include "httplib.h"
#include "web.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; ++fails; } } while (0)
static bool has(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }
static std::string csrfOf(const std::string& html) { auto p = html.find("name='csrf' value='"); return p == std::string::npos ? "" : html.substr(p + 19, 64); }
int main() {
    namespace fs = std::filesystem;
    auto dir = (fs::temp_directory_path() / "re_cust_web").string(); fs::remove_all(dir); fs::create_directories(dir);
    inv::Inventory in(dir); auth::Authenticator us(dir + "/users.csv"); auth::AuditLog au(dir + "/audit.csv");
    web::WebApp app(in, us, au, {});
    auto& s = *httplib::Server::last;
    auto r = s.call("POST", "/setup", {{"username","boss"},{"password","bosspass1"},{"password2","bosspass1"},{"question","First bike name?"},{"answer","thunderbolt1"}});
    r = s.call("POST", "/login", {{"username","boss"},{"password","bosspass1"}});
    std::string ck = r.headers["Set-Cookie"].substr(0, r.headers["Set-Cookie"].find(';'));
    r = s.call("GET", "/", {}, ck); const std::string csrf = csrfOf(r.body); CHECK(csrf.size() == 64);
    auto save = [&](const char* name, const char* phone) {
        return s.call("POST", "/customer/save", {{"csrf",csrf},{"id","0"},{"name",name},{"phone",phone}}, ck);
    };
    // form: name has the no-digits guard, phone has live detection hooks, ID note
    r = s.call("GET", "/customer/edit", {}, ck);
    CHECK(r.status == 200 && has(r.body, "data-person='1'") && has(r.body, "id='phone-hint'") && has(r.body, "data-phone='1'") && has(r.body, "unique customer ID"));
    // names: digits refused (also a phone number typed into the name box)
    r = save("Juan 2", "");                 CHECK(has(r.headers["Location"], "err") && has(r.headers["Location"], "numbers"));
    r = save("09171234567", "");            CHECK(has(r.headers["Location"], "err"));
    // phones: bad ones refused, good ones saved in canonical form
    r = save("Juan dela Cruz", "12345");    CHECK(has(r.headers["Location"], "err"));
    r = save("Juan dela Cruz", "0917-111-0001"); CHECK(has(r.headers["Location"], "kind=ok") && has(r.headers["Location"], "/customer?id=1"));
    r = save("Juan dela Cruz", "(02) 8123 4567");  CHECK(has(r.headers["Location"], "/customer?id=2"));   // namesake, landline: allowed, new ID
    r = save("Juan dela Cruz", "02-8123-4567");    CHECK(has(r.headers["Location"], "err"));              // same name + same phone, other spelling
    // list: IDs, phone type tags, search by ID
    r = s.call("GET", "/customers", {}, ck);
    CHECK(has(r.body, "CUST-0001") && has(r.body, "CUST-0002") && has(r.body, "0917 111 0001") && has(r.body, "(02) 8123 4567") && has(r.body, ">Mobile<") && has(r.body, ">Landline<"));
    r = s.call("GET", "/customers", {{"q","cust-0002"}}, ck); CHECK(has(r.body, "/customer?id=2") && !has(r.body, "/customer?id=1"));   // only that customer's row
    // detail page
    r = s.call("GET", "/customer", {{"id","2"}}, ck); CHECK(has(r.body, "Customer ID") && has(r.body, "CUST-0002") && has(r.body, ">Landline<"));
    // sale form: two Juans share a name; picking one by its label or its ID works, the bare name asks to pick
    r = s.call("GET", "/invoice", {}, ck); CHECK(has(r.body, "Juan dela Cruz - 0917 111 0001 [CUST-0001]") && has(r.body, "Juan dela Cruz - (02) 8123 4567 [CUST-0002]"));
    auto sale = [&](const char* cust) {
        return s.call("POST", "/invoice", {{"csrf",csrf},{"customer",cust},{"p_id_1","500"},{"p_qty_1","1"},{"amount_paid","0"}}, ck);
    };
    r = s.call("POST", "/add", {{"csrf",csrf},{"id","500"},{"name","Oil"},{"category","Engine"},{"price","10"},{"qty","30"},{"min","1"}}, ck);
    r = sale("Juan dela Cruz");             CHECK(has(r.headers["Location"], "More%20than%20one"));
    r = sale("Walk In 77");                 CHECK(has(r.headers["Location"], "numbers"));                  // free text with digits is refused
    r = sale("CUST-0009");                  CHECK(has(r.headers["Location"], "No%20customer"));
    r = sale("cust-0002");                  CHECK(has(r.headers["Location"], "INV-"));                      // typed ID resolves the right Juan
    const auto* inv1 = in.invoices().find("INV-000001");
    CHECK(inv1 && inv1->customerId == 2);
    if (fails == 0) std::cout << "ALL CUSTOMER WEB TESTS PASSED\n";
    return fails ? 1 : 0;
}
