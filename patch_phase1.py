#!/usr/bin/env python3
"""patch_phase1.py - Phase 1: Low Stock Alerts (banner, sidebar counter, red out-of-stock rows, 'Out of stock' tile, ?low=1 link).
Usage: python3 patch_phase1.py [path-to-Brothers_Motorshop]   (Windows: py patch_phase1.py [path])
Safe: edits by exact text anchors; if anything does not match, NOTHING is changed. Originals saved as *.p1.bak."""
import sys, os, shutil
root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
if not os.path.isfile(os.path.join(root, "src", "web.cpp")):
    sys.exit("ERROR: src/web.cpp not found in " + root + " - run this inside the project folder or pass its path.")

def rd(p):
    with open(os.path.join(root, p), "rb") as f: return f.read().decode("utf-8")
def wr(p, s):
    with open(os.path.join(root, p), "wb") as f: f.write(s.encode("utf-8"))

# CRLF-safe: work on LF text, restore the file's original line endings when writing.
def edit(path, pairs):
    raw = rd(path); crlf = "\r\n" in raw
    s = raw.replace("\r\n", "\n")
    if "alert-low" in s and path != "tests/web_e2e.cpp" or ("p1alert" in s and path == "tests/web_e2e.cpp"):
        print("skip (already patched):", path); return None
    for old, new in pairs:
        n = s.count(old)
        if n != 1: sys.exit("ERROR: anchor found %d times in %s (expected 1):\n  %s\nNothing was changed." % (n, path, old[:90]))
        s = s.replace(old, new, 1)
    return path, (s.replace("\n", "\r\n") if crlf else s)

CSS = r'''
/* ---- Phase 1: low-stock alerts ---- */
.nbadge{display:inline-block;min-width:1.35rem;padding:0 .4rem;margin-left:.45rem;border-radius:99px;background:#ef4444;color:#fff;font-size:.72rem;font-weight:700;line-height:1.35rem;text-align:center;animation:badge-pulse 1.8s ease-in-out infinite}
@keyframes badge-pulse{0%,100%{box-shadow:0 0 0 0 rgba(239,68,68,.55)}60%{box-shadow:0 0 0 7px rgba(239,68,68,0)}}
.stat.bad,body:not(.auth) .stat.bad{background:linear-gradient(180deg,#fff5f4,#fdd9d5);border-color:#f3b6b1}
.stat.bad b,body:not(.auth) .stat.bad b{color:#b42318}
tr.outrow td{background:#fdeceb}body:not(.auth) tbody tr.outrow:hover td{background:#fbd9d6}
.alert-low{display:flex;flex-wrap:wrap;align-items:center;gap:.6rem 1rem;padding:.85rem 1.15rem;margin:0 0 1rem;border-radius:16px;color:#6b4500;
  background:linear-gradient(90deg,#fff4d6,#ffe6a6);border:1px solid #f2d58a;box-shadow:0 14px 24px -16px rgba(138,90,0,.6)}
.alert-low.has-out{color:#7a1810;background:linear-gradient(90deg,#fdeceb,#fbcbc6);border-color:#f3b6b1;box-shadow:0 14px 24px -16px rgba(180,35,24,.6)}
.alert-low .alert-ic{flex:none;width:2.1rem;height:2.1rem;border-radius:50%;background:#f59e0b;color:#fff;font-weight:800;font-size:1.2rem;line-height:2.1rem;text-align:center;animation:badge-pulse 1.8s ease-in-out infinite}
.alert-low.has-out .alert-ic{background:#dc2626}
.alert-low .alert-txt{flex:1 1 240px}.alert-low .alert-txt b{font-size:1.05rem}
.alert-low .alert-acts{display:flex;gap:.5rem;flex-wrap:wrap}
@media (prefers-reduced-motion:reduce){.nbadge,.alert-low .alert-ic{animation:none}}
@media print{.alert-low,.nbadge{display:none!important}}
'''

web = edit("src/web.cpp", [
 # helpers
 ("    // ------------------------------------------------------------------ inventory dashboard\n",
  "    // ------------------------------------------------------------------ low-stock alert helpers (Phase 1)\n"
  "    int outOfStockCount() const { int n = 0; for (const inv::Part& p : inv.parts()) n += p.quantity() == 0; return n; }\n"
  "    int belowMinCount() const { int n = 0; for (const inv::Part& p : inv.parts()) n += p.isLowStock(); return n; }\n"
  "    int attentionCount() const { int n = 0; for (const inv::Part& p : inv.parts()) n += (p.isLowStock() || p.quantity() == 0); return n; }\n"
  "    std::string alertBadge() const {\n"
  "        const int n = attentionCount();\n"
  "        return n ? \" <span class='nbadge' title='Parts that are low or out of stock'>\" + num(n) + \"</span>\" : std::string();\n"
  "    }\n\n"
  "    // ------------------------------------------------------------------ inventory dashboard\n"),
 # sidebar counter
 ('link("/", "Inventory")', 'link("/", "Inventory" + alertBadge())'),
 # tiles + banner
 ('''        b += std::string("<div class='stat") + (low ? " warn" : "") + "'><span>Below minimum</span><b>" + num(low) + "</b></div></div>";''',
  '''        const int outN = outOfStockCount();
        b += std::string("<div class='stat") + (low ? " warn" : "") + "'><span>Below minimum</span><b>" + num(low) + "</b></div>"
             "<div class='stat" + (outN ? " bad" : "") + "'><span>Out of stock</span><b>" + num(outN) + "</b></div></div>";
        if (low || outN) {
            b += std::string("<div class='alert-low noprint") + (outN ? " has-out" : "") + "' role='alert'><span class='alert-ic'>!</span><div class='alert-txt'><b>" +
                 (low ? num(low) + (low == 1 ? " part is" : " parts are") + " below the reorder level" : std::string("Some parts are out of stock")) + "</b>" +
                 (outN ? " &middot; <b>" + num(outN) + " out of stock</b>" : std::string()) +
                 "<div class='mut'>Restock soon so you do not lose sales.</div></div><div class='alert-acts'><a class='btn sm' href='/?low=1'>Show low stock</a>" +
                 (auth::allowed(c.role, P::ViewReports) ? "<a class='btn sm sec' href='/reports'>Reorder list</a>" : std::string()) + "</div></div>";
        }'''),
 # out-of-stock row colour
 ('''b += "<tr" + std::string(p.isLowStock() ? " class='low'" : "") + " data-id='"''',
  '''b += "<tr" + std::string(p.quantity() == 0 ? " class='low outrow'" : (p.isLowStock() ? " class='low'" : "")) + " data-id='"'''),
])
css = edit("src/web_assets.h", [(')CSS";', CSS + ')CSS";')])
js = edit("src/web_assets.h", []) if False else None
test = edit("tests/web_e2e.cpp", [
 ('''{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","8"}}, ck); CHECK(has(r.headers["Location"], "ok"));''',
  '''{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","8"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    // p1alert: low-stock banner + sidebar counter appear when a part is below its minimum
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","99999"}}, ck); CHECK(has(r.headers["Location"], "ok"));
    r = s.call("GET", "/", {}, ck); CHECK(has(r.body, "alert-low") && has(r.body, "nbadge") && has(r.body, "Out of stock") && has(r.body, "href='/?low=1'"));
    r = s.call("POST", "/edit", {{"csrf",csrf},{"id","101"},{"name","Engine Oil 1L"},{"category","Engine"},{"price","499.5"},{"min","8"}}, ck); CHECK(has(r.headers["Location"], "ok"));'''),
])
# JS (same file as CSS): pre-tick "Low stock only" when opened with ?low=1
raw = None
res = [x for x in (web, css, test) if x]
if css:
    p, s = css
    old = "    low.addEventListener('change', apply);\n"
    if s.replace("\r\n", "\n").count(old) != 1: sys.exit("ERROR: JS anchor not found in web_assets.h. Nothing was changed.")
    crlf = "\r\n" in s
    t = s.replace("\r\n", "\n").replace(old, old + "    if (/[?&]low=1/.test(location.search)) { low.checked = true; apply(); }\n", 1)
    res = [x for x in res if x[0] != p] + [(p, t.replace("\n", "\r\n") if crlf else t)]
for p, s in res:
    if not os.path.isfile(os.path.join(root, p + ".p1.bak")): shutil.copy2(os.path.join(root, p), os.path.join(root, p + ".p1.bak"))
    wr(p, s); print("patched", p)
print("\nOK - Phase 1 installed. Next: build and run (Ctrl+F5 in the browser).")
