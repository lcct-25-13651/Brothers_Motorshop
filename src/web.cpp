#include "web.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "httplib.h"
#include "reports.h"
#include "util.h"
#include "web_assets.h"

namespace web {

using util::htmlEscape;
namespace {

std::string h(const std::string& s) { return htmlEscape(s); }
std::string php(double v) { return "&#8369;" + util::moneyFmt(v); }  // peso sign
std::string num(long long v) { return std::to_string(v); }

struct Session {
    std::string user, csrf;
    std::chrono::steady_clock::time_point expires;
};

// Everything a handler needs about the current request.
struct Ctx {
    const httplib::Request& req;
    httplib::Response& res;
    Session* sess = nullptr;                // null on public pages
    auth::Role role = auth::Role::Staff;
    std::string ip;
    Ctx(const httplib::Request& rq, httplib::Response& rs) : req(rq), res(rs) {}
    std::string param(const std::string& k) const { return req.get_param_value(k); }
    const std::string& user() const { static const std::string none; return sess ? sess->user : none; }
};

std::string cookieValue(const httplib::Request& req, const std::string& name) {
    const std::string c = req.get_header_value("Cookie");
    size_t pos = 0;
    while (pos < c.size()) {
        size_t end = c.find(';', pos);
        if (end == std::string::npos) end = c.size();
        const std::string item = util::trim(c.substr(pos, end - pos));
        if (item.rfind(name + "=", 0) == 0) return item.substr(name.size() + 1);
        pos = end + 1;
    }
    return "";
}

std::string msgUrl(const std::string& path, const std::string& msg, bool ok) {
    return path + (path.find('?') == std::string::npos ? "?" : "&") + "kind=" + (ok ? "ok" : "err") + "&msg=" + util::urlEncode(msg);
}

}  // namespace

// ============================================================================ Impl
struct WebApp::Impl {
    inv::Inventory& inv;
    auth::Authenticator& users;
    auth::AuditLog& audit;
    Config cfg;
    httplib::Server svr;
    std::mutex mtx;  // httplib is multi-threaded; the stores are not
    std::map<std::string, Session> sessions;
    auth::LoginThrottle throttle;
    static constexpr std::chrono::minutes kIdle{30};

    Impl(inv::Inventory& i, auth::Authenticator& u, auth::AuditLog& a, Config c) : inv(i), users(u), audit(a), cfg(std::move(c)) {
        svr.set_payload_max_length(8 * 1024 * 1024);
        svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            res.status = 500;
            res.set_content("<h1>Something went wrong</h1><p>The request could not be completed.</p>", "text/html; charset=utf-8");
        });
        routes();
    }

    // ------------------------------------------------------------------ sessions
    Session* sessionFor(const httplib::Request& req, std::string* tokenOut = nullptr) {
        const std::string t = cookieValue(req, "sid");
        auto it = sessions.find(t);
        if (t.empty() || it == sessions.end()) return nullptr;
        if (std::chrono::steady_clock::now() > it->second.expires) { sessions.erase(it); return nullptr; }
        it->second.expires = std::chrono::steady_clock::now() + kIdle;
        if (tokenOut) *tokenOut = t;
        return &it->second;
    }
    std::string startSession(const std::string& user) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = sessions.begin(); it != sessions.end();) it = now > it->second.expires ? sessions.erase(it) : std::next(it);
        const std::string t = auth::newToken();
        sessions[t] = {user, auth::newToken(), now + kIdle};
        return t;
    }
    void dropUserSessions(const std::string& user, const std::string& keep = "") {
        for (auto it = sessions.begin(); it != sessions.end();)
            it = (util::equalsIgnoreCase(it->second.user, user) && it->first != keep) ? sessions.erase(it) : std::next(it);
    }

    // ------------------------------------------------------------------ routing + gatekeeping
    using Fn = void (Impl::*)(Ctx&);

    // perm == nullopt  -> any logged-in user.   pub == true -> no login needed.
    void add(const char* method, const std::string& path, Fn fn, std::optional<auth::Permission> perm = std::nullopt, bool pub = false) {
        auto handler = [this, fn, perm, pub, path](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(mtx);
            res.set_header("X-Content-Type-Options", "nosniff");
            res.set_header("X-Frame-Options", "DENY");
            res.set_header("Referrer-Policy", "same-origin");
            res.set_header("Cache-Control", "no-store");
            res.set_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'; form-action 'self'");
            Ctx c{req, res};
            c.ip = req.remote_addr;
            if (!users.isConfigured()) {  // brand-new install: only the setup page works
                if (path != "/setup" && path.rfind("/static/", 0) != 0) return redirect(c, "/setup");
            } else if (!pub) {
                std::string token;
                c.sess = sessionFor(req, &token);
                const auto role = c.sess ? users.roleOf(c.sess->user) : std::nullopt;
                if (!c.sess || !role) {
                    if (c.sess) sessions.erase(token);  // account was deleted meanwhile
                    return redirect(c, "/login");
                }
                c.role = *role;
                if (perm && !auth::allowed(c.role, *perm)) {
                    audit.record(c.user(), "ACCESS_DENIED", std::string(req.method) + " " + req.path, c.ip);
                    return forbidden(c, "Your role (" + std::string(auth::roleName(c.role)) + ") is not allowed to do that.");
                }
                if (req.method == "POST" && !auth::constantTimeEquals(c.param("csrf"), c.sess->csrf)) {
                    return forbidden(c, "Security token missing or expired. Go back, reload the page and try again.");
                }
            }
            (this->*fn)(c);
        };
        if (std::string(method) == "GET") svr.Get(path, handler); else svr.Post(path, handler);
    }

    void routes() {
        using P = auth::Permission;
        add("GET", "/static/app.css", &Impl::cssFile, std::nullopt, true);
        add("GET", "/static/app.js", &Impl::jsFile, std::nullopt, true);
        add("GET", "/login", &Impl::loginPage, std::nullopt, true);
        add("POST", "/login", &Impl::doLogin, std::nullopt, true);
        add("GET", "/setup", &Impl::setupPage, std::nullopt, true);
        add("POST", "/setup", &Impl::doSetup, std::nullopt, true);
        add("GET", "/forgot", &Impl::forgotPage, std::nullopt, true);
        add("POST", "/forgot", &Impl::doForgot, std::nullopt, true);
        add("POST", "/logout", &Impl::doLogout);
        add("GET", "/", &Impl::dashboard, P::ViewInventory);
        add("GET", "/stock", &Impl::stockPage, P::AdjustStock);
        add("POST", "/stock", &Impl::doStock, P::AdjustStock);
        add("GET", "/add", &Impl::addPage, P::AddPart);
        add("POST", "/add", &Impl::doAdd, P::AddPart);
        add("GET", "/edit", &Impl::editPage, P::EditPart);
        add("POST", "/edit", &Impl::doEdit, P::EditPart);
        add("GET", "/delete", &Impl::deletePage, P::DeletePart);
        add("POST", "/delete", &Impl::doDelete, P::DeletePart);
        add("GET", "/invoice", &Impl::invoicePage, P::Sell);
        add("POST", "/invoice", &Impl::doInvoice, P::Sell);
        add("GET", "/invoices", &Impl::invoiceList, P::Sell);
        add("GET", "/receivables", &Impl::receivablesPage, P::Receivables);
        add("GET", "/payment", &Impl::paymentPage, P::Receivables);
        add("POST", "/payment", &Impl::doPayment, P::Receivables);
        add("GET", "/cash-report", &Impl::cashReportPage, P::CashReport);
        add("GET", "/reports", &Impl::reportsPage, P::ViewReports);
        add("GET", "/movements", &Impl::movementsPage, P::ViewReports);
        add("GET", "/audit", &Impl::auditPage, P::ViewAudit);
        add("GET", "/data", &Impl::dataPage, P::ExportData);
        add("GET", "/export/inventory.csv", &Impl::exportInventory, P::ExportData);
        add("GET", "/export/movements.csv", &Impl::exportMovements, P::ExportData);
        add("POST", "/import", &Impl::doImport, P::ImportData);
        add("POST", "/backup", &Impl::doBackup, P::RunBackup);
        add("GET", "/users", &Impl::usersPage, P::ManageUsers);
        add("POST", "/users/add", &Impl::doUserAdd, P::ManageUsers);
        add("POST", "/users/role", &Impl::doUserRole, P::ManageUsers);
        add("POST", "/users/reset", &Impl::doUserReset, P::ManageUsers);
        add("POST", "/users/delete", &Impl::doUserDelete, P::ManageUsers);
        add("GET", "/account", &Impl::accountPage);
        add("POST", "/account/password", &Impl::doAccountPassword);
    }

    // ------------------------------------------------------------------ response helpers
    static void redirect(Ctx& c, const std::string& url) { c.res.set_redirect(url, 303); }
    static void html(Ctx& c, const std::string& body, int status = 200) { c.res.status = status; c.res.set_content(body, "text/html; charset=utf-8"); }
    void forbidden(Ctx& c, const std::string& why) { html(c, layout(c, "Not allowed", "<div class='flash err'>" + h(why) + "</div><p><a class='btn sec' href='/'>Back to inventory</a></p>"), 403); }
    static void csv(Ctx& c, const std::string& name, const std::string& body) {
        c.res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
        c.res.set_content(body, "text/csv; charset=utf-8");
    }
    void cssFile(Ctx& c) { c.res.set_header("Cache-Control", "max-age=3600"); c.res.set_content(kCss, "text/css; charset=utf-8"); }
    void jsFile(Ctx& c) { c.res.set_header("Cache-Control", "max-age=3600"); c.res.set_content(kJs, "application/javascript; charset=utf-8"); }

    static std::string tokenField(const Ctx& c) { return "<input type='hidden' name='csrf' value='" + h(c.sess ? c.sess->csrf : "") + "'>"; }
    static std::string flash(Ctx& c) {
        const std::string m = c.param("msg");
        if (m.empty()) return "";
        return std::string("<div class='flash ") + (c.param("kind") == "ok" ? "ok" : "err") + "'>" + h(m) + "</div>";
    }
    int intParam(Ctx& c, const char* name, int lo, int hi, int dflt) {
        long long v;
        return util::parseInt(c.param(name), lo, hi, v) ? static_cast<int>(v) : dflt;
    }

    // Money typed by a person: allows "1,200.50" and surrounding spaces. Blank -> false.
    static bool parseMoneyText(const std::string& in, double& out) {
        std::string s;
        for (char ch : util::trim(in)) if (ch != ',') s += ch;
        return !s.empty() && util::parseDouble(s, 0, inv::limits::kMaxPrice, out);
    }
    static std::string statusTag(inv::PayStatus s) {
        const char* cls = s == inv::PayStatus::Paid ? "ok" : s == inv::PayStatus::Partial ? "low" : "out";
        return std::string("<span class='tag ") + cls + "'>" + inv::payStatusName(s) + "</span>";
    }
    static std::string methodSelect(const std::string& chosen, const char* name = "method") {
        std::string o = std::string("<select name='") + name + "'>";
        const std::string pick = inv::canonicalMethod(chosen).empty() ? "Cash" : inv::canonicalMethod(chosen);
        for (const std::string& m : inv::paymentMethods()) o += "<option" + std::string(m == pick ? " selected" : "") + ">" + h(m) + "</option>";
        return o + "</select>";
    }
    std::string customersDatalist() const {
        std::set<std::string> seen;
        std::string o = "<datalist id='custs'>";
        for (const inv::Invoice& i : inv.invoices().all())
            if (i.customer != "Walk-in customer" && seen.insert(reports::customerKey(i.customer)).second && seen.size() <= 500) o += "<option value='" + h(i.customer) + "'>";
        return o + "</datalist>";
    }

    // ------------------------------------------------------------------ layout
    std::string head(const std::string& title) const {
        return "<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>" + h(title) + " - " + h(cfg.shopName) + "</title><link rel='stylesheet' href='/static/app.css'></head><body>";
    }
    std::string layout(Ctx& c, const std::string& title, const std::string& body) {
        std::string o = head(title);
        if (c.sess) {
            using P = auth::Permission;
            const std::string path = c.req.path;
            auto link = [&](const std::string& href, const std::string& text) {
                return "<a href='" + href + "'" + (path == href ? " class='on'" : "") + ">" + text + "</a>";
            };
            o += "<header class='top'><span class='brand'>&#9881; " + h(cfg.shopName) + "</span><nav>" + link("/", "Inventory");
            if (auth::allowed(c.role, P::Sell)) o += link("/invoice", "New sale") + link("/invoices", "Invoices");
            if (auth::allowed(c.role, P::Receivables)) o += link("/receivables", "Receivables / Utang");
            if (auth::allowed(c.role, P::CashReport)) o += link("/cash-report", "Daily cash report");
            if (auth::allowed(c.role, P::AddPart)) o += link("/add", "Add part");
            if (auth::allowed(c.role, P::ViewReports)) o += link("/reports", "Reports") + link("/movements", "Movements");
            if (auth::allowed(c.role, P::ExportData)) o += link("/data", "Data &amp; backups");
            if (auth::allowed(c.role, P::ViewAudit)) o += link("/audit", "Audit");
            if (auth::allowed(c.role, P::ManageUsers)) o += link("/users", "Users");
            o += "</nav><form method='post' action='/logout'>" + tokenField(c) + "<a href='/account'><span class='chip'>" + h(c.user()) + " &middot; " +
                 auth::roleName(c.role) + "</span></a><button class='sm sec' type='submit'>Log out</button></form></header>";
        }
        o += "<main>" + flash(c);
        if (!inv.lastWarning().empty()) o += "<div class='flash err'>" + h(inv.lastWarning()) + "</div>";
        o += body + "</main><script src='/static/app.js'></script></body></html>";
        return o;
    }
    // Pages shown before login (no nav bar).
    std::string bare(Ctx& c, const std::string& title, const std::string& body) {
        return head(title) + "<div class='login'><h1>&#9881; " + h(cfg.shopName) + "</h1>" + flash(c) + "<div class='card'>" + body + "</div></div></body></html>";
    }

    // ------------------------------------------------------------------ login / setup / recovery
    void loginPage(Ctx& c) {
        if (sessionFor(c.req)) return redirect(c, "/");
        html(c, bare(c, "Sign in",
                     "<h2>Sign in</h2><form method='post' action='/login'><label>Username</label><input name='username' autofocus required autocomplete='username'>"
                     "<label>Password</label><input name='password' type='password' required autocomplete='current-password'><p><button type='submit'>Sign in</button></p>"
                     "<p class='mut'><a href='/forgot'>Forgot password?</a></p></form>"));
    }
    void doLogin(Ctx& c) {
        const std::string name = c.param("username"), key = c.ip + "|" + util::toLower(name);
        if (throttle.blocked(key) || throttle.blocked(c.ip + "|*")) return redirect(c, msgUrl("/login", "Too many failed attempts. Wait a few minutes and try again.", false));
        auto who = users.login(name, c.param("password"));
        if (!who) {
            throttle.fail(key);
            throttle.fail(c.ip + "|*");
            audit.record(name, "LOGIN_FAILED", "bad username or password", c.ip);
            return redirect(c, msgUrl("/login", "Wrong username or password.", false));
        }
        throttle.ok(key);
        const std::string token = startSession(who->first);
        audit.record(who->first, "LOGIN", std::string("role ") + auth::roleName(who->second), c.ip);
        c.res.set_header("Set-Cookie", "sid=" + token + "; Path=/; HttpOnly; SameSite=Strict");
        redirect(c, "/");
    }
    void doLogout(Ctx& c) {
        std::string token;
        sessionFor(c.req, &token);
        audit.record(c.user(), "LOGOUT", "", c.ip);
        sessions.erase(token);
        c.res.set_header("Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        redirect(c, msgUrl("/login", "You have been signed out.", true));
    }
    void setupPage(Ctx& c) {
        if (users.isConfigured()) return redirect(c, "/login");
        html(c, bare(c, "First-time setup",
                     "<h2>First-time setup</h2><p class='mut'>Create the Admin account. Passwords are stored only as salted hashes.</p>"
                     "<form method='post' action='/setup'><label>Admin username</label><input name='username' value='admin' required>"
                     "<label>Password (min 8 characters)</label><input name='password' type='password' required autocomplete='new-password'>"
                     "<label>Repeat password</label><input name='password2' type='password' required autocomplete='new-password'>"
                     "<label>Recovery question</label><input name='question' required placeholder='e.g. Name of my first motorcycle?'>"
                     "<label>Recovery answer (min 8 characters)</label><input name='answer' required>"
                     "<p><button type='submit'>Create account</button></p></form>"));
    }
    void doSetup(Ctx& c) {
        if (users.isConfigured()) return redirect(c, "/login");
        if (c.param("password") != c.param("password2")) return redirect(c, msgUrl("/setup", "The two passwords do not match.", false));
        const std::string err = users.initialSetup(c.param("username"), c.param("password"), c.param("question"), c.param("answer"));
        if (!err.empty()) return redirect(c, msgUrl("/setup", err, false));
        audit.record(c.param("username"), "USER_CREATED", "first Admin account", c.ip);
        redirect(c, msgUrl("/login", "Account created. Please sign in.", true));
    }
    void forgotPage(Ctx& c) {
        const std::string u = c.param("u"), q = users.recoveryQuestion(u);
        std::string b = "<h2>Reset password</h2><form method='post' action='/forgot'><label>Username</label><input name='username' value='" + h(u) + "' required>";
        if (!u.empty() && !q.empty()) b += "<p><b>Recovery question:</b> " + h(q) + "</p>";
        else if (!u.empty()) b += "<p class='mut'>No recovery question is set for that account. Ask an Admin to reset your password.</p>";
        b += "<label>Recovery answer</label><input name='answer' required><label>New password (min 8 characters)</label>"
             "<input name='password' type='password' required autocomplete='new-password'><p><button type='submit'>Reset password</button> "
             "<a class='btn sec' href='/login'>Cancel</a></p></form>";
        if (u.empty()) b += "<form method='get' action='/forgot'><p class='mut'>Not sure of your question? Enter your username and show it:</p>"
                            "<input name='u' placeholder='username'> <button class='sec' type='submit'>Show question</button></form>";
        html(c, bare(c, "Reset password", b));
    }
    void doForgot(Ctx& c) {
        const std::string name = c.param("username"), key = c.ip + "|recover";
        if (throttle.blocked(key)) return redirect(c, msgUrl("/login", "Too many attempts. Wait a few minutes and try again.", false));
        const std::string err = users.recoverPassword(name, c.param("answer"), c.param("password"));
        if (!err.empty()) {
            throttle.fail(key);
            audit.record(name, "LOGIN_FAILED", "password recovery attempt failed", c.ip);
            return redirect(c, msgUrl("/forgot?u=" + util::urlEncode(name), err, false));
        }
        throttle.ok(key);
        const std::string canon = users.canonicalName(name);
        dropUserSessions(canon);
        audit.record(canon, "PASSWORD_RESET", "via recovery answer", c.ip);
        redirect(c, msgUrl("/login", "Password changed. Please sign in.", true));
    }

    // ------------------------------------------------------------------ inventory dashboard
    static std::string statusTag(const inv::Part& p) {
        if (p.quantity() == 0) return "<span class='tag out'>Out of stock</span>";
        if (p.isLowStock()) return "<span class='tag low'>Low</span>";
        return "<span class='tag ok'>OK</span>";
    }

    static std::string modelsText(const inv::Part& p) {   // "Bajaj RE 4S, Bajaj RE Compact"
        std::string o;
        for (const std::string& m : p.modelList()) o += (o.empty() ? "" : ", ") + m;
        return o;
    }
    static std::string modelsKey(const inv::Part& p) {    // "bajaj re 4s|bajaj re compact" (matched exactly by the filter)
        std::string o;
        for (const std::string& m : p.modelList()) o += (o.empty() ? "" : "|") + m;
        return o;
    }

    void dashboard(Ctx& c) {
        using P = auth::Permission;
        const auto& parts = inv.parts();
        long long units = 0;
        double value = 0;
        int low = 0;
        std::set<std::string> systems(inv::standardSystems().begin(), inv::standardSystems().end());
        for (const inv::Part& p : parts) { units += p.quantity(); value += p.stockValue(); low += p.isLowStock(); systems.insert(p.category()); }

        const std::vector<std::string> models = inv.allModels();
        const std::string wantModel = util::trim(c.param("model"));
        std::string b = "<h1>Inventory</h1><div class='stats'><div class='stat'><span>Parts</span><b>" + num(static_cast<long long>(parts.size())) + "</b></div>"
                        "<div class='stat'><span>Units in stock</span><b>" + num(units) + "</b></div>";
        if (auth::allowed(c.role, P::ViewReports)) b += "<div class='stat'><span>Stock value</span><b>" + php(value) + "</b></div>";
        b += std::string("<div class='stat") + (low ? " warn" : "") + "'><span>Below minimum</span><b>" + num(low) + "</b></div></div>";

        b += "<div class='card'><div class='bar'><input id='q' type='search' placeholder='Search by ID, name, system or vehicle model...' autocomplete='off'>"
             "<select id='sys'><option value=''>All vehicle systems</option>";
        for (const std::string& s : systems) b += "<option>" + h(s) + "</option>";
        b += "</select><select id='model' title='Show only parts that fit this vehicle'><option value=''>All vehicle models</option>";
        for (const std::string& m : models) b += "<option" + std::string(util::equalsIgnoreCase(m, wantModel) ? " selected" : "") + ">" + h(m) + "</option>";
        b += "</select><label class='inline mut'><input id='lowonly' type='checkbox'> Low stock only</label><span id='count' class='mut grow'></span>";
        if (auth::allowed(c.role, P::AddPart)) b += "<a class='btn' href='/add'>+ Add part</a>";
        b += "</div><table id='inv'><thead><tr><th data-sort='id' data-type='num'>ID</th><th data-sort='name'>Part</th><th data-sort='cat'>System</th><th>Fits (vehicle models)</th>"
             "<th class='n' data-sort='price' data-type='num'>Price</th><th class='n' data-sort='qty' data-type='num'>Stock</th><th class='n'>Min</th><th>Status</th><th></th></tr></thead><tbody>";
        for (const inv::Part& p : parts) {
            b += "<tr" + std::string(p.isLowStock() ? " class='low'" : "") + " data-id='" + num(p.id()) + "' data-name='" + h(util::toLower(p.name())) +
                 "' data-cat='" + h(util::toLower(p.category())) + "' data-models='" + h(util::toLower(modelsKey(p))) + "' data-price='" + util::money(p.price()) + "' data-qty='" + num(p.quantity()) +
                 "' data-low='" + (p.isLowStock() ? "1" : "0") + "'><td>" + num(p.id()) + "</td><td>" + h(p.name()) + "</td><td>" + h(p.category()) +
                 "</td><td class='fits'>" + (p.modelList().empty() ? std::string("<span class='mut'>-</span>") : h(modelsText(p))) +
                 "</td><td class='n'>" + php(p.price()) + "</td><td class='n'>" + num(p.quantity()) + "</td><td class='n'>" + num(p.minStock()) + "</td><td>" +
                 statusTag(p) + "</td><td class='right'>";
            const std::string id = "?id=" + num(p.id());
            if (auth::allowed(c.role, P::Sell)) b += "<a class='btn sm' href='/invoice?part=" + num(p.id()) + "'>Sell</a> ";
            if (auth::allowed(c.role, P::AdjustStock)) b += "<a class='btn sm sec' href='/stock" + id + "'>Stock</a> ";
            if (auth::allowed(c.role, P::EditPart)) b += "<a class='btn sm sec' href='/edit" + id + "'>Edit</a> ";
            if (auth::allowed(c.role, P::DeletePart)) b += "<a class='btn sm danger' href='/delete" + id + "'>Delete</a>";
            b += "</td></tr>";
        }
        b += "</tbody></table></div>";
        html(c, layout(c, "Inventory", b));
    }

    // ------------------------------------------------------------------ stock: restock / adjust (Staff + Admin)
    void stockPage(Ctx& c) {
        const inv::Part* p = inv.find(intParam(c, "id", 1, inv::limits::kMaxId, 0));
        if (!p) return redirect(c, msgUrl("/", "Part not found.", false));
        const std::string id = num(p->id());
        std::string b = "<h1>Stock: " + h(p->name()) + "</h1><p class='mut'>Part " + id + " &middot; " + h(p->category()) + " &middot; currently <b>" + num(p->quantity()) +
                        "</b> in stock (minimum " + num(p->minStock()) + ")</p><div class='grid'><div class='card'><h2>Restock (stock arrives)</h2>"
                        "<form method='post' action='/stock'>" + tokenField(c) + "<input type='hidden' name='id' value='" + id + "'><input type='hidden' name='action' value='restock'>"
                        "<label>Quantity received</label><input name='qty' type='number' min='1' required>"
                        "<label>Unit cost paid (&#8369;, optional - used for expense reports)</label><input name='cost' type='number' step='0.01' min='0'>"
                        "<label>Note (optional)</label><input name='note' maxlength='80'><p><button type='submit'>Add stock</button></p></form></div>"
                        "<div class='card'><h2>Adjust (count correction, damage, loss)</h2><form method='post' action='/stock'>" + tokenField(c) +
                        "<input type='hidden' name='id' value='" + id + "'><input type='hidden' name='action' value='adjust'>"
                        "<label>Change (use a minus sign to remove, e.g. -2)</label><input name='qty' type='number' required>"
                        "<label>Reason</label><input name='note' maxlength='80' required><p><button type='submit'>Apply adjustment</button></p></form></div></div>"
                        "<p><a class='btn sec' href='/'>Back</a></p>";
        html(c, layout(c, "Stock", b));
    }
    void doStock(Ctx& c) {
        const int id = intParam(c, "id", 1, inv::limits::kMaxId, 0);
        long long qty;
        if (!util::parseInt(c.param("qty"), -inv::limits::kMaxQty, inv::limits::kMaxQty, qty)) return redirect(c, msgUrl("/stock?id=" + num(id), "Enter a whole number.", false));
        const std::string note = util::trim(c.param("note"));
        inv::Result r;
        if (c.param("action") == "restock") {
            double cost = 0;
            if (!util::trim(c.param("cost")).empty() && !util::parseDouble(c.param("cost"), 0, inv::limits::kMaxPrice, cost))
                return redirect(c, msgUrl("/stock?id=" + num(id), "Unit cost is not a valid amount.", false));
            r = inv.restock(id, static_cast<int>(qty), cost, c.user(), note);
        } else {
            r = inv.adjust(id, qty, c.user(), note);
        }
        redirect(c, r.ok ? msgUrl("/", r.message, true) : msgUrl("/stock?id=" + num(id), r.message, false));
    }

    // ------------------------------------------------------------------ add / edit / delete part (Admin)
    std::string systemsDatalist() const {
        std::set<std::string> s(inv::standardSystems().begin(), inv::standardSystems().end());
        for (const inv::Part& p : inv.parts()) s.insert(p.category());
        std::string o = "<datalist id='systems'>";
        for (const std::string& x : s) o += "<option value='" + h(x) + "'>";
        return o + "</datalist>";
    }
    int nextFreeId() const {
        int m = 100;
        for (const inv::Part& p : inv.parts()) m = std::max(m, p.id());
        return m + 1;
    }
    void addPage(Ctx& c) {
        std::string b = "<h1>Add part</h1><div class='card'><form method='post' action='/add'>" + tokenField(c) + "<div class='grid'>"
                        "<div><label>Part ID</label><input name='id' type='number' min='1' value='" + num(nextFreeId()) + "' required></div>"
                        "<div><label>Name</label><input name='name' maxlength='80' required></div>"
                        "<div><label>Vehicle system</label><input name='category' list='systems' required></div>"
                        "<div><label>Price (&#8369;)</label><input name='price' type='number' step='0.01' min='0' required></div>"
                        "<div><label>Opening stock</label><input name='qty' type='number' min='0' value='0' required></div>"
                        "<div><label>Minimum stock</label><input name='min' type='number' min='0' value='5' required></div></div>"
                        "<label>Compatible vehicle models <span class='mut'>(optional, separate with commas)</span></label>"
                        "<input name='models' maxlength='300' style='width:100%' placeholder='Bajaj RE 4S, Bajaj RE Compact, Bajaj RE Maxima'>" + systemsDatalist() +
                        "<p><button type='submit'>Save part</button> <a class='btn sec' href='/'>Cancel</a></p></form></div>";
        html(c, layout(c, "Add part", b));
    }
    // Parses the shared part fields; returns "" or an error.
    std::string readPart(Ctx& c, long long& id, std::string& name, std::string& cat, double& price, long long& qty, long long& mn, bool needIdQty) {
        name = util::trim(c.param("name")); cat = util::trim(c.param("category"));
        if (needIdQty && (!util::parseInt(c.param("id"), 1, inv::limits::kMaxId, id) || !util::parseInt(c.param("qty"), 0, inv::limits::kMaxQty, qty)))
            return "ID and opening stock must be whole numbers in range.";
        if (!util::parseDouble(c.param("price"), 0, inv::limits::kMaxPrice, price)) return "Price must be a valid amount.";
        if (!util::parseInt(c.param("min"), 0, inv::limits::kMaxQty, mn)) return "Minimum stock must be a whole number.";
        return "";
    }
    void doAdd(Ctx& c) {
        long long id = 0, qty = 0, mn = 0;
        double price = 0;
        std::string name, cat;
        std::string err = readPart(c, id, name, cat, price, qty, mn, true);
        if (err.empty()) {
            inv::Part part(static_cast<int>(id), name, cat, price, static_cast<int>(qty), static_cast<int>(mn));
            part.setCompatibleModels(util::oneLine(util::trim(c.param("models"))));
            const inv::Result r = inv.addPart(part, c.user());
            if (r.ok) return redirect(c, msgUrl("/", "Part " + num(id) + " added.", true));
            err = r.message;
        }
        redirect(c, msgUrl("/add", err, false));
    }
    void editPage(Ctx& c) {
        const inv::Part* p = inv.find(intParam(c, "id", 1, inv::limits::kMaxId, 0));
        if (!p) return redirect(c, msgUrl("/", "Part not found.", false));
        std::string b = "<h1>Edit part " + num(p->id()) + "</h1><div class='card'><form method='post' action='/edit'>" + tokenField(c) +
                        "<input type='hidden' name='id' value='" + num(p->id()) + "'><div class='grid'><div><label>Name</label><input name='name' maxlength='80' value='" + h(p->name()) + "' required></div>"
                        "<div><label>Vehicle system</label><input name='category' list='systems' value='" + h(p->category()) + "' required></div>"
                        "<div><label>Price (&#8369;)</label><input name='price' type='number' step='0.01' min='0' value='" + util::money(p->price()) + "' required></div>"
                        "<div><label>Minimum stock</label><input name='min' type='number' min='0' value='" + num(p->minStock()) + "' required></div></div>"
                        "<label>Compatible vehicle models <span class='mut'>(separate with commas)</span></label>"
                        "<input name='models' maxlength='300' style='width:100%' value='" + h(p->compatibleModels()) + "' placeholder='Bajaj RE 4S, Bajaj RE Compact, Bajaj RE Maxima'>" + systemsDatalist() +
                        "<p class='mut'>Current stock is " + num(p->quantity()) + ". Change it with <a href='/stock?id=" + num(p->id()) + "'>Restock / Adjust</a> so the movement is logged.</p>"
                        "<p><button type='submit'>Save changes</button> <a class='btn sec' href='/'>Cancel</a></p></form></div>";
        html(c, layout(c, "Edit part", b));
    }
    void doEdit(Ctx& c) {
        const int id = intParam(c, "id", 1, inv::limits::kMaxId, 0);
        const inv::Part* cur = inv.find(id);
        if (!cur) return redirect(c, msgUrl("/", "Part not found.", false));
        const double oldPrice = cur->price();
        const std::string oldName = cur->name(), oldModels = cur->compatibleModels();
        long long dummy = 0, mn = 0;
        double price = 0;
        std::string name, cat;
        std::string err = readPart(c, dummy, name, cat, price, dummy, mn, false);
        if (err.empty()) {
            std::optional<std::string> models;   // a request without the field (old form) leaves the models alone
            if (c.req.has_param("models")) models = c.param("models");
            const inv::Result r = inv.updatePart(id, name, cat, price, static_cast<int>(mn), c.user(), models);
            if (r.ok) {
                if (models && inv.find(id) && inv.find(id)->compatibleModels() != oldModels)
                    audit.record(c.user(), "PART_MODELS_CHANGED", "Part " + num(id) + " " + oldName + ": [" + oldModels + "] -> [" + inv.find(id)->compatibleModels() + "]", c.ip);
                if (price != oldPrice)
                    audit.record(c.user(), "PRICE_CHANGE", "Part " + num(id) + " " + oldName + ": " + util::money(oldPrice) + " -> " + util::money(price), c.ip);
                return redirect(c, msgUrl("/", "Part " + num(id) + " updated.", true));
            }
            err = r.message;
        }
        redirect(c, msgUrl("/edit?id=" + num(id), err, false));
    }
    void deletePage(Ctx& c) {
        const inv::Part* p = inv.find(intParam(c, "id", 1, inv::limits::kMaxId, 0));
        if (!p) return redirect(c, msgUrl("/", "Part not found.", false));
        std::string b = "<h1>Delete part?</h1><div class='card'><p>You are about to delete <b>" + h(p->name()) + "</b> (ID " + num(p->id()) + ") with <b>" + num(p->quantity()) +
                        "</b> units in stock. The removal is recorded in the movement log and audit trail.</p><form method='post' action='/delete'>" + tokenField(c) +
                        "<input type='hidden' name='id' value='" + num(p->id()) + "'><button class='danger' type='submit'>Yes, delete</button> <a class='btn sec' href='/'>Cancel</a></form></div>";
        html(c, layout(c, "Delete part", b));
    }
    void doDelete(Ctx& c) {
        const int id = intParam(c, "id", 1, inv::limits::kMaxId, 0);
        const inv::Part* p = inv.find(id);
        if (!p) return redirect(c, msgUrl("/", "Part not found.", false));
        const std::string desc = "Part " + num(id) + " " + p->name() + " (stock " + num(p->quantity()) + ")";
        const inv::Result r = inv.removePart(id, c.user());
        if (r.ok) audit.record(c.user(), "RECORD_DELETED", desc, c.ip);
        redirect(c, msgUrl("/", r.ok ? "Part deleted." : r.message, r.ok));
    }

    // ------------------------------------------------------------------ invoices / receipts
    static constexpr int kPartRows = 8, kSvcRows = 4;

    void invoicePage(Ctx& c) {
        const std::string no = c.param("no");
        if (!no.empty()) return receipt(c, no);
        std::string b = "<h1>New sale / invoice</h1><div class='card'><form method='post' action='/invoice'>" + tokenField(c) +
                        "<label>Customer name <span class='mut'>(required for credit / utang)</span></label><input name='customer' list='custs' maxlength='80' placeholder='Walk-in customer' value='" + h(c.param("customer")) + "'>" + customersDatalist() +
                        "<h2>Parts <span class='mut'>(type or pick a part ID - the catalogue price is applied)</span></h2><table><thead><tr><th>Part ID</th><th>Quantity</th></tr></thead><tbody>";
        const std::string pre = c.param("part");
        for (int i = 1; i <= kPartRows; ++i) {
            std::string id = c.param("p_id_" + num(i)), qty = c.param("p_qty_" + num(i));
            if (i == 1 && id.empty() && !pre.empty()) { id = pre; qty = "1"; }
            b += "<tr><td><input name='p_id_" + num(i) + "' list='plist' inputmode='numeric' value='" + h(id) + "'></td><td><input name='p_qty_" + num(i) + "' type='number' min='1' value='" + h(qty) + "'></td></tr>";
        }
        b += "</tbody></table><datalist id='plist'>";
        for (const inv::Part& p : inv.parts())
            b += "<option value='" + num(p.id()) + "'>" + h(p.name()) + " - &#8369;" + util::moneyFmt(p.price()) + " (" + num(p.quantity()) + " in stock)</option>";
        b += "</datalist><h2>Services <span class='mut'>(labour, repairs - optional)</span></h2><table><thead><tr><th>Description</th><th>Qty</th><th>Price (&#8369;)</th></tr></thead><tbody>";
        for (int i = 1; i <= kSvcRows; ++i)
            b += "<tr><td><input name='s_desc_" + num(i) + "' maxlength='100' value='" + h(c.param("s_desc_" + num(i))) + "'></td><td><input name='s_qty_" + num(i) +
                 "' type='number' min='1' value='" + h(c.param("s_qty_" + num(i))) + "'></td><td><input name='s_price_" + num(i) + "' type='number' step='0.01' min='0' value='" + h(c.param("s_price_" + num(i))) + "'></td></tr>";
        b += "</tbody></table><h2>Payment</h2><div class='grid'><div><label>Amount paid now (&#8369;) - leave blank if paid in full</label>"
             "<input name='amount_paid' inputmode='decimal' autocomplete='off' placeholder='full amount' value='" + h(c.param("amount_paid")) + "'></div>"
             "<div><label>Payment method</label>" + methodSelect(c.param("method")) + "</div></div>"
             "<p class='mut'>Credit sale (utang): type what the customer pays now (0 if nothing). The rest is tracked under <b>Receivables / Utang</b> and needs a customer name.</p>"
             "<p><button type='submit'>Record sale &amp; create receipt</button> <a class='btn sec' href='/'>Cancel</a></p></form></div>";
        html(c, layout(c, "New sale", b));
    }

    void doInvoice(Ctx& c) {
        auto back = [&](const std::string& msg) {  // keep what was typed
            std::string q = "/invoice?customer=" + util::urlEncode(c.param("customer")) + "&amount_paid=" + util::urlEncode(c.param("amount_paid")) +
                            "&method=" + util::urlEncode(c.param("method"));
            for (int i = 1; i <= kPartRows; ++i) q += "&p_id_" + num(i) + "=" + util::urlEncode(c.param("p_id_" + num(i))) + "&p_qty_" + num(i) + "=" + util::urlEncode(c.param("p_qty_" + num(i)));
            for (int i = 1; i <= kSvcRows; ++i)
                q += "&s_desc_" + num(i) + "=" + util::urlEncode(c.param("s_desc_" + num(i))) + "&s_qty_" + num(i) + "=" + util::urlEncode(c.param("s_qty_" + num(i))) +
                     "&s_price_" + num(i) + "=" + util::urlEncode(c.param("s_price_" + num(i)));
            redirect(c, msgUrl(q, msg, false));
        };
        std::vector<inv::InvoiceLine> lines;
        for (int i = 1; i <= kPartRows; ++i) {
            const std::string ids = util::trim(c.param("p_id_" + num(i)));
            if (ids.empty()) continue;
            long long id, qty;
            if (!util::parseInt(ids, 1, inv::limits::kMaxId, id) || !util::parseInt(c.param("p_qty_" + num(i)), 1, inv::limits::kMaxQty, qty))
                return back("Part row " + num(i) + ": enter a valid part ID and a quantity of at least 1.");
            inv::InvoiceLine l;
            l.kind = inv::InvoiceLine::Kind::Part; l.partId = static_cast<int>(id); l.qty = static_cast<int>(qty);
            lines.push_back(l);
        }
        for (int i = 1; i <= kSvcRows; ++i) {
            const std::string desc = util::trim(c.param("s_desc_" + num(i))), pr = util::trim(c.param("s_price_" + num(i)));
            if (desc.empty() && pr.empty()) continue;
            long long qty = 1;
            double price;
            if (!util::trim(c.param("s_qty_" + num(i))).empty() && !util::parseInt(c.param("s_qty_" + num(i)), 1, inv::limits::kMaxQty, qty))
                return back("Service row " + num(i) + ": quantity must be at least 1.");
            if (!util::parseDouble(pr, 0, inv::limits::kMaxPrice, price)) return back("Service row " + num(i) + ": enter a valid price.");
            inv::InvoiceLine l;
            l.kind = inv::InvoiceLine::Kind::Service; l.description = desc; l.qty = static_cast<int>(qty); l.unitPrice = price;
            lines.push_back(l);
        }
        double paid = -1.0;   // blank = paid in full
        if (!util::trim(c.param("amount_paid")).empty() && !parseMoneyText(c.param("amount_paid"), paid)) return back("Amount paid must be a valid amount (or leave it blank).");
        const inv::CheckoutResult r = inv.checkout(c.param("customer"), lines, c.user(), paid, c.param("method").empty() ? "Cash" : c.param("method"));
        if (!r.ok) return back(r.message);
        if (r.balance > 0) {
            const inv::Invoice* v = inv.invoices().find(r.invoiceNumber);
            audit.record(c.user(), "CREDIT_SALE", r.invoiceNumber + " " + (v ? v->customer : std::string()) + ": total " + util::money(r.paid + r.balance) + ", paid " + util::money(r.paid) +
                                                   ", balance " + util::money(r.balance), c.ip);
        }
        redirect(c, "/invoice?no=" + util::urlEncode(r.invoiceNumber));
    }

    void receipt(Ctx& c, const std::string& no) {
        const inv::Invoice* v = inv.invoices().find(no);
        if (!v) return redirect(c, msgUrl("/invoices", "Invoice not found.", false));
        std::string b = "<p class='noprint'><button data-print>&#128424; Print receipt</button> <a class='btn sec' href='/invoice'>New sale</a> <a class='btn sec' href='/invoices'>All invoices</a>" +
                        (v->balance() > 0 && auth::allowed(c.role, auth::Permission::Receivables) ? " <a class='btn' href='/payment?no=" + util::urlEncode(v->number) + "'>Record payment</a>" : std::string()) + "</p>"
                        "<div class='receipt'><h1>" + h(cfg.shopName) + "</h1><div class='sub'>Official receipt</div><div class='meta'><div><b>Invoice:</b> " + h(v->number) +
                        "<br><b>Customer:</b> " + h(v->customer) + "</div><div class='right'><b>Date:</b> " + h(v->timestamp.substr(0, 10)) + "<br><b>Time:</b> " + h(v->timestamp.size() > 11 ? v->timestamp.substr(11) : "") +
                        "<br><b>Served by:</b> " + h(v->user) + "</div></div><table><thead><tr><th>Item</th><th>Type</th><th class='n'>Qty</th><th class='n'>Unit price</th><th class='n'>Amount</th></tr></thead><tbody>";
        for (const inv::InvoiceLine& l : v->lines)
            b += "<tr><td>" + h(l.description) + (l.kind == inv::InvoiceLine::Kind::Part ? " <span class='mut'>#" + num(l.partId) + "</span>" : "") + "</td><td>" +
                 (l.kind == inv::InvoiceLine::Kind::Part ? "Part" : "Service") + "</td><td class='n'>" + num(l.qty) + "</td><td class='n'>" + php(l.unitPrice) + "</td><td class='n'>" + php(l.total()) + "</td></tr>";
        b += "</tbody><tfoot><tr><td colspan='4' class='n'>TOTAL</td><td class='n'>" + php(v->total()) + "</td></tr>";
        if (v->status() != inv::PayStatus::Paid)
            b += "<tr class='sub'><td colspan='4' class='n'>Paid to date</td><td class='n'>" + php(v->amountPaid) + "</td></tr>"
                 "<tr><td colspan='4' class='n'>BALANCE DUE</td><td class='n minus'>" + php(v->balance()) + "</td></tr>";
        b += "</tfoot></table><p>Payment status: " + statusTag(v->status()) + "</p>";
        const auto pays = inv.payments().forInvoice(v->number);
        if (!pays.empty()) {
            b += "<h2>Payments received</h2><table><thead><tr><th>Date</th><th>Method</th><th>Received by</th><th>Note</th><th class='n'>Amount</th><th class='n'>Balance after</th></tr></thead><tbody>";
            for (const inv::Payment* p : pays)
                b += "<tr><td>" + h(p->timestamp) + "</td><td>" + h(p->method) + "</td><td>" + h(p->user) + "</td><td>" + h(p->note) + "</td><td class='n'>" + php(p->amount) + "</td><td class='n'>" + php(p->balanceAfter) + "</td></tr>";
            b += "</tbody></table>";
        }
        b += "<p class='thanks'>Thank you for your business!</p></div>";
        html(c, layout(c, "Receipt " + no, b));
    }

    void invoiceList(Ctx& c) {
        const std::string want = util::toLower(c.param("status"));   // "", paid, partial, unpaid
        auto tab = [&](const std::string& key, const std::string& text) {
            return "<a class='btn sm" + std::string(want == key ? "" : " sec") + "' href='/invoices" + (key.empty() ? "" : "?status=" + key) + "'>" + text + "</a> ";
        };
        std::string b = "<h1>Invoices</h1><p>" + tab("", "All") + tab("unpaid", "Unpaid") + tab("partial", "Partial") + tab("paid", "Paid") + "</p>"
                        "<div class='card'><table><thead><tr><th>Invoice</th><th>Date</th><th>Customer</th><th>Served by</th><th class='n'>Total</th><th class='n'>Paid</th><th class='n'>Balance</th><th>Status</th><th></th></tr></thead><tbody>";
        const auto& all = inv.invoices().all();
        int shown = 0;
        for (size_t i = all.size(); i-- > 0 && shown < 300;) {
            const inv::Invoice& v = all[i];
            if (!want.empty() && util::toLower(inv::payStatusName(v.status())) != want) continue;
            ++shown;
            b += "<tr><td>" + h(v.number) + "</td><td>" + h(v.timestamp) + "</td><td>" + h(v.customer) + "</td><td>" + h(v.user) + "</td><td class='n'>" + php(v.total()) +
                 "</td><td class='n'>" + php(v.amountPaid) + "</td><td class='n'>" + (v.balance() > 0 ? "<b class='minus'>" + php(v.balance()) + "</b>" : php(0)) + "</td><td>" + statusTag(v.status()) +
                 "</td><td class='right'>" + (v.balance() > 0 ? "<a class='btn sm' href='/payment?no=" + util::urlEncode(v.number) + "'>Record payment</a> " : std::string()) +
                 "<a class='btn sm sec' href='/invoice?no=" + util::urlEncode(v.number) + "'>View / print</a></td></tr>";
        }
        if (!shown) b += "<tr><td colspan='9' class='mut'>" + std::string(all.empty() ? "No invoices yet." : "No invoices with that status.") + "</td></tr>";
        html(c, layout(c, "Invoices", b + "</tbody></table></div>"));
    }

    // ------------------------------------------------------------------ receivables / utang (Staff + Admin)
    void receivablesPage(Ctx& c) {
        const std::time_t now = std::time(nullptr);
        const auto& all = inv.invoices().all();
        const auto debtors = reports::receivablesByCustomer(all, now);
        const std::string only = reports::customerKey(c.param("customer"));
        std::string shownName;
        for (const auto& d : debtors) if (reports::customerKey(d.customer) == only) shownName = d.customer;

        std::string b = "<h1>Receivables / Utang</h1><div class='stats'><div class='stat" + std::string(debtors.empty() ? "" : " warn") + "'><span>Total outstanding</span><b>" + php(reports::totalReceivable(all)) + "</b></div>"
                        "<div class='stat'><span>Customers with a balance</span><b>" + num(static_cast<long long>(debtors.size())) + "</b></div>"
                        "<div class='stat'><span>Open invoices</span><b>" + num(reports::openInvoiceCount(all)) + "</b></div></div>";

        b += "<h2>Customer balances</h2><div class='card'><table><thead><tr><th>Customer</th><th class='n'>Open invoices</th><th class='n'>Billed</th><th class='n'>Paid</th><th class='n'>Balance</th><th>Oldest unpaid</th><th></th></tr></thead><tbody>";
        for (const auto& d : debtors)
            b += "<tr><td><b>" + h(d.customer) + "</b></td><td class='n'>" + num(d.openInvoices) + "</td><td class='n'>" + php(d.billed) + "</td><td class='n'>" + php(d.paid) +
                 "</td><td class='n'><b class='minus'>" + php(d.balance) + "</b></td><td>" + h(d.oldestDate) + " <span class='mut'>(" + num(d.oldestDays) + " day" + (d.oldestDays == 1 ? "" : "s") + ")</span></td>"
                 "<td class='right'><a class='btn sm sec' href='/receivables?customer=" + util::urlEncode(d.customer) + "'>View invoices</a></td></tr>";
        if (debtors.empty()) b += "<tr><td colspan='7' class='mut'>Nobody owes the shop anything right now.</td></tr>";
        b += "</tbody></table></div>";

        b += "<h2>" + (shownName.empty() ? std::string("Open invoices") : "Open invoices - " + h(shownName)) + "</h2>";
        if (!shownName.empty()) b += "<p><a class='btn sm sec' href='/receivables'>Show all customers</a></p>";
        b += "<div class='card'><table><thead><tr><th>Invoice</th><th>Date</th><th>Customer</th><th class='n'>Total</th><th class='n'>Paid</th><th class='n'>Balance</th><th>Status</th><th></th></tr></thead><tbody>";
        int shown = 0;
        for (size_t i = all.size(); i-- > 0 && shown < 300;) {   // newest first
            const inv::Invoice& v = all[i];
            if (v.balance() <= 0 || (!shownName.empty() && reports::customerKey(v.customer) != only)) continue;
            ++shown;
            b += "<tr><td>" + h(v.number) + "</td><td>" + h(v.timestamp.substr(0, 10)) + "</td><td>" + h(v.customer) + "</td><td class='n'>" + php(v.total()) + "</td><td class='n'>" + php(v.amountPaid) +
                 "</td><td class='n'><b class='minus'>" + php(v.balance()) + "</b></td><td>" + statusTag(v.status()) + "</td><td class='right'><a class='btn sm' href='/payment?no=" + util::urlEncode(v.number) +
                 "'>Record payment</a> <a class='btn sm sec' href='/invoice?no=" + util::urlEncode(v.number) + "'>View</a></td></tr>";
        }
        if (!shown) b += "<tr><td colspan='8' class='mut'>No open invoices.</td></tr>";
        html(c, layout(c, "Receivables", b + "</tbody></table></div>"));
    }

    void paymentPage(Ctx& c) {
        const std::string no = c.param("no");
        const inv::Invoice* v = inv.invoices().find(no);
        if (!v) return redirect(c, msgUrl("/receivables", "Invoice not found.", false));
        if (v->balance() <= 0) return redirect(c, msgUrl("/invoice?no=" + util::urlEncode(no), "That invoice is already fully paid.", true));
        std::string b = "<h1>Record payment - " + h(v->number) + "</h1><div class='grid'><div class='card'><h2>" + h(v->customer) + "</h2>"
                        "<table class='kv'><tr><td>Invoice date</td><td>" + h(v->timestamp.substr(0, 10)) + "</td></tr><tr><td>Invoice total</td><td class='n'>" + php(v->total()) + "</td></tr>"
                        "<tr><td>Paid so far</td><td class='n'>" + php(v->amountPaid) + "</td></tr><tr><td><b>Balance owed</b></td><td class='n'><b class='minus'>" + php(v->balance()) + "</b></td></tr>"
                        "<tr><td>Status</td><td class='n'>" + statusTag(v->status()) + "</td></tr></table>"
                        "<p><a href='/invoice?no=" + util::urlEncode(v->number) + "'>View invoice</a></p></div>"
                        "<div class='card'><h2>Payment received</h2><form method='post' action='/payment'>" + tokenField(c) + "<input type='hidden' name='no' value='" + h(v->number) + "'>"
                        "<input type='hidden' name='balance' value='" + util::money(v->balance()) + "'>"
                        "<label>Amount (&#8369;) - the full balance is pre-filled; type less for a partial / installment payment</label>"
                        "<input name='amount' inputmode='decimal' autocomplete='off' value='" + util::money(v->balance()) + "' required>"
                        "<label>Payment method</label>" + methodSelect(c.param("method")) +
                        "<label>Note (optional, e.g. reference no.)</label><input name='note' maxlength='80'>"
                        "<p><button type='submit'>Save payment</button> <a class='btn sec' href='/receivables'>Cancel</a></p></form></div></div>";
        const auto pays = inv.payments().forInvoice(v->number);
        if (!pays.empty()) {
            b += "<h2>Payments so far</h2><div class='card'><table><thead><tr><th>Date</th><th>Method</th><th>Received by</th><th>Note</th><th class='n'>Amount</th></tr></thead><tbody>";
            for (const inv::Payment* p : pays)
                b += "<tr><td>" + h(p->timestamp) + "</td><td>" + h(p->method) + "</td><td>" + h(p->user) + "</td><td>" + h(p->note) + "</td><td class='n'>" + php(p->amount) + "</td></tr>";
            b += "</tbody></table></div>";
        }
        html(c, layout(c, "Record payment", b));
    }

    void doPayment(Ctx& c) {
        const std::string no = c.param("no");
        const std::string back = "/payment?no=" + util::urlEncode(no);
        double amount = 0, expected = -1.0;
        if (!parseMoneyText(c.param("amount"), amount)) return redirect(c, msgUrl(back, "Enter the payment amount as a number.", false));
        if (!c.param("balance").empty() && !util::parseDouble(c.param("balance"), 0, inv::limits::kMaxPrice, expected)) expected = -1.0;
        const inv::PaymentResult r = inv.recordPayment(no, amount, c.param("method"), c.param("note"), c.user(), expected);
        if (!r.ok) return redirect(c, msgUrl(inv.invoices().find(no) && inv.invoices().find(no)->balance() > 0 ? back : "/receivables", r.message, false));
        const std::string note = util::trim(c.param("note"));
        audit.record(c.user(), "PAYMENT_RECORDED", no + " " + r.customer + ": " + util::money(r.applied) + " via " + inv::canonicalMethod(c.param("method")) + ", balance " + util::money(r.balance) + " (" +
                                                       inv::payStatusName(r.status) + ")" + (note.empty() ? "" : " - " + util::oneLine(note)), c.ip);
        redirect(c, msgUrl("/invoice?no=" + util::urlEncode(no), r.message, true));
    }

    // ------------------------------------------------------------------ end-of-day cashier summary (Staff + Admin)
    void cashReportPage(Ctx& c) {
        const std::string today = util::formatTime(std::time(nullptr), "%Y-%m-%d");
        std::string date = util::trim(c.param("date")), warn;
        if (date.empty()) date = today;
        else if (!reports::validDate(date)) { warn = "<div class='flash err noprint'>That is not a valid date - showing today.</div>"; date = today; }
        const reports::DailyCashReport r = reports::dailyCashReport(inv.invoices().all(), inv.payments().all(), date);

        double cash = 0;
        for (const auto& m : r.byMethod) if (m.method == "Cash") cash = m.amount;
        auto row = [](const std::string& label, const std::string& value, bool bold = false, bool indent = false) {
            return "<tr><td" + std::string(indent ? " class='indent'" : "") + ">" + (bold ? "<b>" + label + "</b>" : label) + "</td><td class='n'>" + (bold ? "<b>" + value + "</b>" : value) + "</td></tr>";
        };

        std::string b = warn + "<form method='get' action='/cash-report' class='bar noprint'><label class='inline'>Report date</label><input type='date' name='date' value='" + h(date) + "' max='" + h(today) + "'>"
                        "<button class='sm sec' type='submit'>Show</button>" + (date != today ? " <a class='btn sm sec' href='/cash-report'>Today</a>" : std::string()) +
                        " <button type='button' data-print>&#128424; Print report</button></form>"
                        "<div class='report'><h1>" + h(cfg.shopName) + "</h1><div class='sub'>Daily cash report &middot; register close-out</div>"
                        "<div class='meta'><div><b>Date:</b> " + h(date) + "</div><div class='right'><b>Printed:</b> " + h(util::nowStamp()) + "<br><b>By:</b> " + h(c.user()) + "</div></div>"
                        "<div class='cols'><div><h2>Sales</h2><table class='kv'>" +
                        row("Invoices issued", num(r.invoiceCount)) +
                        row("Paid in full", num(r.paidCount), false, true) + row("Partial payment", num(r.partialCount), false, true) + row("Unpaid (all credit)", num(r.unpaidCount), false, true) +
                        row("Parts sales", php(r.partsSales), false, true) + row("Service / labour sales", php(r.serviceSales), false, true) +
                        row("Gross sales", php(r.grossSales), true) + "</table></div>"
                        "<div><h2>Money collected</h2><table class='kv'>" +
                        row("Net collected today (all methods)", php(r.collected), true) +
                        row("on today's invoices", php(r.collectedOnToday), false, true) + row("on earlier credit invoices (utang payments)", php(r.collectedOnOlder), false, true) +
                        row("Credit given today (unpaid at close)", php(r.creditExtended)) +
                        row("All unpaid credit sales, every customer", php(r.outstandingNow), true) +
                        row(num(r.customersOwing) + " customer(s) still owe the shop (as of now)", "", false, true) + "</table></div></div>"
                        "<h2>Payment method breakdown</h2><table><thead><tr><th>Method</th><th class='n'>Payments</th><th class='n'>Amount</th></tr></thead><tbody>";
        int count = 0;
        for (const auto& m : r.byMethod) { b += "<tr><td>" + h(m.method) + "</td><td class='n'>" + num(m.count) + "</td><td class='n'>" + php(m.amount) + "</td></tr>"; count += m.count; }
        if (r.byMethod.empty()) b += "<tr><td colspan='3' class='mut'>No payments were received on this date.</td></tr>";
        else b += "<tr class='tot'><td>Total</td><td class='n'>" + num(count) + "</td><td class='n'>" + php(r.collected) + "</td></tr>";
        b += "</tbody></table>"
             "<h2>Cash drawer</h2><table class='kv'>" + row("Expected cash in drawer (Cash payments only)", php(cash), true) +
             "<tr><td>Counted cash</td><td class='n line'>&nbsp;</td></tr><tr><td>Over / short</td><td class='n line'>&nbsp;</td></tr></table>"
             "<h2>Invoices issued</h2><table><thead><tr><th>Invoice</th><th>Time</th><th>Customer</th><th class='n'>Total</th><th class='n'>Balance now</th><th>Status now</th></tr></thead><tbody>";
        for (const inv::Invoice* v : r.invoices)
            b += "<tr><td>" + h(v->number) + "</td><td>" + h(v->timestamp.size() > 11 ? v->timestamp.substr(11) : "") + "</td><td>" + h(v->customer) + "</td><td class='n'>" + php(v->total()) + "</td><td class='n'>" +
                 php(v->balance()) + "</td><td>" + statusTag(v->status()) + "</td></tr>";
        if (r.invoices.empty()) b += "<tr><td colspan='6' class='mut'>No invoices on this date.</td></tr>";
        b += "</tbody></table><h2>Payments received</h2><table><thead><tr><th>Time</th><th>Invoice</th><th>Method</th><th>Received by</th><th class='n'>Amount</th></tr></thead><tbody>";
        for (const inv::Payment* p : r.payments)
            b += "<tr><td>" + h(p->timestamp.size() > 11 ? p->timestamp.substr(11) : "") + "</td><td>" + h(p->invoiceNo) + "</td><td>" + h(p->method) + "</td><td>" + h(p->user) + "</td><td class='n'>" + php(p->amount) + "</td></tr>";
        if (r.payments.empty()) b += "<tr><td colspan='5' class='mut'>No payments on this date.</td></tr>";
        b += "</tbody></table><div class='sigs'><div>Prepared by (cashier)</div><div>Checked by (owner / manager)</div></div></div>";
        html(c, layout(c, "Daily cash report", b));
    }

    // ------------------------------------------------------------------ reports (Admin)
    static std::string periodTable(const std::string& title, const std::vector<reports::PeriodTotals>& rows) {
        double mx = 1;
        for (const auto& r : rows) mx = std::max(mx, r.revenue());
        std::string b = "<h2>" + h(title) + "</h2><div class='card'><table><thead><tr><th>Period</th><th class='n'>Parts</th><th class='n'>Services</th><th class='n'>Revenue</th><th></th><th class='n'>Expenses</th><th class='n'>Net</th></tr></thead><tbody>";
        for (size_t i = rows.size(); i-- > 0;) {  // newest first
            const auto& r = rows[i];
            b += "<tr><td>" + h(r.label) + "</td><td class='n'>" + php(r.partsRevenue) + "</td><td class='n'>" + php(r.serviceRevenue) + "</td><td class='n'><b>" + php(r.revenue()) +
                 "</b></td><td style='width:90px'><div class='meter'><i style='width:" + util::money(r.revenue() / mx * 100) + "%'></i></div></td><td class='n'>" + php(r.expenses) +
                 "</td><td class='n " + (r.net() < 0 ? "minus" : "plus") + "'>" + php(r.net()) + "</td></tr>";
        }
        return b + "</tbody></table></div>";
    }

    void reportsPage(Ctx& c) {
        const std::time_t now = std::time(nullptr);
        const int days = intParam(c, "days", 0, 3650, 30), idle = intParam(c, "idle", 1, 3650, 30);
        const auto& moves = inv.movements().all();
        std::string b = "<h1>Business reports</h1><p class='mut'>Revenue = SALE movements at the selling price + service lines. Expenses = RESTOCK movements at the unit cost entered when stock arrived"
                        " (restocks with no cost entered count as 0: " + num(reports::restocksWithoutCost(moves)) + " so far).</p>";
        b += periodTable("Daily (last 14 days)", reports::revenueByPeriod(moves, inv.invoices().all(), reports::Period::Daily, 14, now));
        b += periodTable("Weekly (last 8 weeks, Monday start)", reports::revenueByPeriod(moves, inv.invoices().all(), reports::Period::Weekly, 8, now));
        b += periodTable("Monthly (last 12 months)", reports::revenueByPeriod(moves, inv.invoices().all(), reports::Period::Monthly, 12, now));

        b += "<h2>Top-selling / fast-moving parts</h2><div class='card'><form method='get' action='/reports' class='bar'><input type='hidden' name='idle' value='" + num(idle) + "'><label class='inline'>Window</label><select name='days'>";
        for (int d : {7, 30, 90, 365, 0}) b += "<option value='" + num(d) + "'" + (d == days ? " selected" : "") + ">" + (d ? "Last " + num(d) + " days" : std::string("All time")) + "</option>";
        b += "</select><button class='sm sec' type='submit'>Update</button></form><table><thead><tr><th>#</th><th>ID</th><th>Part</th><th class='n'>Units sold</th><th class='n'>Units/day</th><th class='n'>Sales</th><th class='n'>Revenue</th></tr></thead><tbody>";
        int rank = 0;
        for (const auto& t : reports::topSelling(moves, days, 15, now))
            b += "<tr><td>" + num(++rank) + "</td><td>" + num(t.partId) + "</td><td>" + h(t.name) + "</td><td class='n'>" + num(t.unitsSold) + "</td><td class='n'>" + (days ? util::money(t.unitsPerDay) : "-") +
                 "</td><td class='n'>" + num(t.saleCount) + "</td><td class='n'>" + php(t.revenue) + "</td></tr>";
        if (!rank) b += "<tr><td colspan='7' class='mut'>No sales recorded in this window.</td></tr>";
        b += "</tbody></table></div>";

        b += "<h2>Dead stock</h2><div class='card'><form method='get' action='/reports' class='bar'><input type='hidden' name='days' value='" + num(days) + "'><label class='inline'>No movement for at least</label>"
             "<input name='idle' type='number' min='1' value='" + num(idle) + "' style='width:90px'> days <button class='sm sec' type='submit'>Update</button></form><table><thead><tr><th>ID</th><th>Part</th><th>System</th><th class='n'>In stock</th><th class='n'>Value tied up</th><th>Last movement</th><th class='n'>Days idle</th></tr></thead><tbody>";
        double tied = 0;
        const auto dead = reports::deadStock(inv.parts(), moves, idle, now);
        for (const auto& d : dead) {
            tied += d.stockValue;
            b += "<tr><td>" + num(d.part->id()) + "</td><td>" + h(d.part->name()) + "</td><td>" + h(d.part->category()) + "</td><td class='n'>" + num(d.part->quantity()) + "</td><td class='n'>" + php(d.stockValue) +
                 "</td><td>" + h(d.lastMovement) + "</td><td class='n'>" + num(d.daysIdle) + "</td></tr>";
        }
        if (dead.empty()) b += "<tr><td colspan='7' class='mut'>Nothing has been idle that long. (Idle time is counted from each part's last logged movement.)</td></tr>";
        else b += "<tr><td colspan='4'><b>Total tied up</b></td><td class='n'><b>" + php(tied) + "</b></td><td colspan='2'></td></tr>";
        b += "</tbody></table></div>";

        b += "<h2>Reorder list</h2><div class='card'><p class='noprint'><button class='sm' data-print>&#128424; Print</button></p><table><thead><tr><th>ID</th><th>Part</th><th>System</th><th class='n'>In stock</th><th class='n'>Minimum</th><th class='n'>Suggested order</th></tr></thead><tbody>";
        const auto re = reports::reorderList(inv.parts());
        for (const auto& r : re)
            b += "<tr><td>" + num(r.part->id()) + "</td><td>" + h(r.part->name()) + "</td><td>" + h(r.part->category()) + "</td><td class='n'>" + num(r.part->quantity()) + "</td><td class='n'>" +
                 num(r.part->minStock()) + "</td><td class='n'><b>" + num(r.suggestedQty) + "</b></td></tr>";
        if (re.empty()) b += "<tr><td colspan='6' class='mut'>Every part is at or above its minimum.</td></tr>";
        b += "</tbody></table><p class='mut'>Suggested order = minimum stock &minus; current stock (brings the part back up to its minimum).</p></div>";
        html(c, layout(c, "Reports", b));
    }

    void movementsPage(Ctx& c) {
        std::string b = "<h1>Stock movement log</h1><p><a class='btn sec' href='/export/movements.csv'>Download CSV</a> <span class='mut'>Newest 500 shown.</span></p><div class='card'><table><thead><tr><th>Time</th><th>User</th><th>Part</th><th>Action</th>"
                        "<th class='n'>Change</th><th class='n'>Stock after</th><th class='n'>Unit price/cost</th><th>Ref</th></tr></thead><tbody>";
        for (const inv::Movement* m : inv.movements().recent(500))
            b += "<tr><td>" + h(m->timestamp) + "</td><td>" + h(m->user) + "</td><td>" + num(m->partId) + " " + h(m->partName) + "</td><td>" + inv::moveName(m->type) + "</td><td class='n " +
                 (m->qtyChanged < 0 ? "minus'>" : "plus'>+") + num(m->qtyChanged) + "</td><td class='n'>" + num(m->resultingStock) + "</td><td class='n'>" + util::money(m->unitPrice) + "</td><td>" + h(m->ref) + "</td></tr>";
        html(c, layout(c, "Movements", b + "</tbody></table></div>"));
    }

    void auditPage(Ctx& c) {
        std::string b = "<h1>Audit trail</h1><div class='card'><table><thead><tr><th>Time</th><th>User</th><th>Event</th><th>Details</th><th>IP</th></tr></thead><tbody>";
        for (const auth::AuditEntry* e : audit.recent(500))
            b += "<tr><td>" + h(e->timestamp) + "</td><td>" + h(e->user) + "</td><td>" + h(e->event) + "</td><td>" + h(e->details) + "</td><td>" + h(e->ip) + "</td></tr>";
        html(c, layout(c, "Audit", b + "</tbody></table></div>"));
    }

    // ------------------------------------------------------------------ import / export / backup
    void renderData(Ctx& c, const std::string& resultHtml) {
        std::string b = "<h1>Data &amp; backups</h1>" + resultHtml +
                        "<div class='grid'><div class='card'><h2>Export (opens in Excel)</h2><p><a class='btn' href='/export/inventory.csv'>Download inventory CSV</a></p><p><a class='btn' href='/export/movements.csv'>Download movement log CSV</a></p></div>"
                        "<div class='card'><h2>Backup</h2><p class='mut'>Copies every CSV file into the <code>backups</code> folder with a timestamp. This also runs automatically each time the program starts.</p>"
                        "<form method='post' action='/backup'>" + tokenField(c) + "<button type='submit'>Back up now</button></form></div></div>"
                        "<div class='card'><h2>Import / update stock from CSV</h2><p class='mut'>Columns: <code>ID, Name, System, Price, Quantity, MinStock</code> plus an optional <code>UnitCost</code> (header row recommended; the inventory export works as a template). "
                        "<b>Quantity is the new stock level.</b> Known IDs are updated (blank cells keep the current value); unknown IDs are added and need a name, system and price.</p>"
                        "<form method='post' action='/import'>" + tokenField(c) + "<p><input id='csvfile' type='file' accept='.csv,text/csv'></p>"
                        "<textarea id='csvtext' name='csv' rows='10' style='width:100%' placeholder='...or paste CSV text here' required></textarea><p><button type='submit'>Import</button></p></form></div>";
        html(c, layout(c, "Data & backups", b));
    }
    void dataPage(Ctx& c) { renderData(c, ""); }

    void exportInventory(Ctx& c) { csv(c, "inventory_" + util::formatTime(std::time(nullptr), "%Y%m%d") + ".csv", inv.partsCsv()); }
    void exportMovements(Ctx& c) { csv(c, "movements_" + util::formatTime(std::time(nullptr), "%Y%m%d") + ".csv", inv.movementsCsv()); }

    void doImport(Ctx& c) {
        const inv::ImportSummary s = inv.importCsv(c.param("csv"), c.user());
        for (const inv::PriceChange& p : s.priceChanges)
            audit.record(c.user(), "PRICE_CHANGE", "Part " + num(p.partId) + " " + p.name + ": " + util::money(p.oldPrice) + " -> " + util::money(p.newPrice) + " (csv import)", c.ip);
        audit.record(c.user(), "IMPORT", "added " + num(s.added) + ", updated " + num(s.updated) + ", skipped " + num(s.skipped), c.ip);
        std::string r = std::string("<div class='flash ") + (s.skipped == 0 && s.problems.empty() ? "ok" : "err") + "'><b>Import finished:</b> " + num(s.added) + " added, " + num(s.updated) + " updated, " +
                        num(s.unchanged) + " unchanged, " + num(s.skipped) + " skipped.";
        if (!s.problems.empty()) { r += "<ul>"; for (const auto& p : s.problems) r += "<li>" + h(p) + "</li>"; r += "</ul>"; }
        renderData(c, r + "</div>");
    }
    void doBackup(Ctx& c) {
        const inv::BackupSummary b = inv.backupAll();
        audit.record(c.user(), "BACKUP", num(b.copied) + " file(s) copied to " + b.directory, c.ip);
        redirect(c, b.problems.empty() ? msgUrl("/data", num(b.copied) + " file(s) copied to the backups folder.", true) : msgUrl("/data", "Backup problem: " + b.problems.front(), false));
    }

    // ------------------------------------------------------------------ user management (Admin) + own account
    void usersPage(Ctx& c) {
        std::string b = "<h1>Users</h1><div class='card'><table><thead><tr><th>Username</th><th>Role</th><th>Recovery</th><th>Change role</th><th>Reset password</th><th></th></tr></thead><tbody>";
        for (const auth::UserInfo& u : users.users()) {
            const bool self = util::equalsIgnoreCase(u.username, c.user());
            const std::string nm = "<input type='hidden' name='username' value='" + h(u.username) + "'>";
            b += "<tr><td>" + h(u.username) + (self ? " <span class='mut'>(you)</span>" : "") + "</td><td><span class='tag " + (u.role == auth::Role::Admin ? "adm" : "stf") + "'>" + auth::roleName(u.role) + "</span></td><td>" +
                 (u.hasRecovery ? "set" : "-") + "</td><td>";
            if (!self) b += "<form method='post' action='/users/role'>" + tokenField(c) + nm + "<select name='role'><option>Staff</option><option" + std::string(u.role == auth::Role::Admin ? " selected" : "") +
                            ">Admin</option></select> <button class='sm sec' type='submit'>Set</button></form>";
            b += "</td><td><form method='post' action='/users/reset'>" + tokenField(c) + nm + "<input name='password' type='password' placeholder='new password' minlength='8' required> <button class='sm sec' type='submit'>Reset</button></form></td><td>";
            if (!self) b += "<form method='post' action='/users/delete' data-confirm='Delete user " + h(u.username) + "?'>" + tokenField(c) + nm + "<button class='sm danger' type='submit'>Delete</button></form>";
            b += "</td></tr>";
        }
        b += "</tbody></table></div><h2>Add user</h2><div class='card'><form method='post' action='/users/add'>" + tokenField(c) + "<div class='grid'><div><label>Username</label><input name='username' required></div>"
             "<div><label>Password (min 8)</label><input name='password' type='password' minlength='8' required></div><div><label>Role</label><select name='role'><option>Staff</option><option>Admin</option></select></div></div>"
             "<p class='mut'>Staff: sell, view inventory, adjust stock. Admin: everything, including prices, deletion, reports, backups and users.</p><p><button type='submit'>Create user</button></p></form></div>";
        html(c, layout(c, "Users", b));
    }
    void doUserAdd(Ctx& c) {
        const auto role = auth::parseRole(c.param("role"));
        if (!role) return redirect(c, msgUrl("/users", "Choose a role.", false));
        const std::string err = users.addUser(util::trim(c.param("username")), c.param("password"), *role);
        if (err.empty()) audit.record(c.user(), "USER_CREATED", util::trim(c.param("username")) + " as " + auth::roleName(*role), c.ip);
        redirect(c, msgUrl("/users", err.empty() ? "User created." : err, err.empty()));
    }
    void doUserRole(Ctx& c) {
        const std::string name = users.canonicalName(c.param("username"));
        const auto role = auth::parseRole(c.param("role"));
        if (name.empty() || !role || util::equalsIgnoreCase(name, c.user())) return redirect(c, msgUrl("/users", "You cannot change that account's role.", false));
        const std::string err = users.setRole(name, *role);
        if (err.empty()) audit.record(c.user(), "ROLE_CHANGED", name + " -> " + auth::roleName(*role), c.ip);
        redirect(c, msgUrl("/users", err.empty() ? "Role updated." : err, err.empty()));
    }
    void doUserReset(Ctx& c) {
        const std::string name = users.canonicalName(c.param("username"));
        std::string err = name.empty() ? "User not found." : users.resetPassword(name, c.param("password"));
        if (err.empty()) {
            std::string token;
            sessionFor(c.req, &token);
            dropUserSessions(name, util::equalsIgnoreCase(name, c.user()) ? token : std::string());  // keep my own session
            audit.record(c.user(), "PASSWORD_RESET", "password reset for " + name, c.ip);
        }
        redirect(c, msgUrl("/users", err.empty() ? "Password reset for " + name + "." : err, err.empty()));
    }
    void doUserDelete(Ctx& c) {
        const std::string name = users.canonicalName(c.param("username"));
        if (name.empty() || util::equalsIgnoreCase(name, c.user())) return redirect(c, msgUrl("/users", "You cannot delete that account.", false));
        const std::string err = users.removeUser(name);
        if (err.empty()) { dropUserSessions(name); audit.record(c.user(), "USER_DELETED", name, c.ip); }
        redirect(c, msgUrl("/users", err.empty() ? "User deleted." : err, err.empty()));
    }
    void accountPage(Ctx& c) {
        std::string b = "<h1>My account</h1><div class='card'><p>Signed in as <b>" + h(c.user()) + "</b> (" + auth::roleName(c.role) + ").</p><form method='post' action='/account/password'>" + tokenField(c) +
                        "<label>Current password</label><input name='current' type='password' required autocomplete='current-password'><label>New password (min 8)</label><input name='password' type='password' minlength='8' required autocomplete='new-password'>"
                        "<p><button type='submit'>Change password</button></p></form></div>";
        html(c, layout(c, "My account", b));
    }
    void doAccountPassword(Ctx& c) {
        const std::string err = users.changePassword(c.user(), c.param("current"), c.param("password"));
        if (err.empty()) {
            std::string token;
            sessionFor(c.req, &token);
            dropUserSessions(c.user(), token);  // sign out other devices
            audit.record(c.user(), "PASSWORD_CHANGED", "own password", c.ip);
        }
        redirect(c, msgUrl("/account", err.empty() ? "Password changed." : err, err.empty()));
    }
};

// ============================================================================ WebApp
WebApp::WebApp(inv::Inventory& i, auth::Authenticator& u, auth::AuditLog& a, Config cfg) : impl_(new Impl(i, u, a, std::move(cfg))) {}
WebApp::~WebApp() = default;

bool WebApp::run() {
    std::cout << "RE Parts Tracker running at http://" << (impl_->cfg.host == "0.0.0.0" ? "localhost" : impl_->cfg.host) << ':' << impl_->cfg.port << "  (Ctrl+C to stop)\n";
    return impl_->svr.listen(impl_->cfg.host.c_str(), impl_->cfg.port);
}

}  // namespace web
