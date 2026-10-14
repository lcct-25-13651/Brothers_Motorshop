// web_assets.h - the stylesheet and script served at /static/app.css and /static/app.js.
// Kept in their own header so web.cpp stays readable. No frameworks: plain CSS + vanilla JS.
#pragma once

namespace web {

inline constexpr const char* kCss = R"CSS(
:root{--bg:#f4f6f9;--card:#fff;--ink:#1b2430;--mut:#6b7686;--line:#e2e7ee;--pri:#0f5fd7;--pri2:#0a47a8;--ok:#127a3e;--okbg:#e5f6ec;--bad:#b42318;--badbg:#fdeceb;--warn:#8a5a00;--warnbg:#fff4d6}
*{box-sizing:border-box}
body{margin:0;font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--ink)}
header.top{background:#0d1b2e;color:#fff;display:flex;flex-wrap:wrap;align-items:center;gap:.25rem 1rem;padding:.6rem 1.2rem}
header.top .brand{font-weight:700;font-size:1.05rem;margin-right:.8rem}
header.top nav{display:flex;flex-wrap:wrap;gap:.2rem;flex:1}
header.top a{color:#cfd9e8;text-decoration:none;padding:.35rem .7rem;border-radius:6px}
header.top a:hover,header.top a.on{background:#1d3250;color:#fff}
header.top form{margin:0;display:flex;gap:.5rem;align-items:center}
.chip{background:#1d3250;border-radius:99px;padding:.25rem .7rem;font-size:.85rem}
main{max-width:1180px;margin:1.2rem auto;padding:0 1rem}
h1{font-size:1.4rem;margin:.2rem 0 1rem}h2{font-size:1.1rem;margin:1.6rem 0 .6rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:1rem 1.2rem;margin-bottom:1rem;overflow-x:auto}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:.8rem;margin-bottom:1rem}
.stat{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:.8rem 1rem}
.stat span{display:block;color:var(--mut);font-size:.8rem;text-transform:uppercase;letter-spacing:.04em}.stat b{font-size:1.4rem}
.stat.warn{background:var(--warnbg);border-color:#f2d58a}
table{border-collapse:collapse;width:100%}th,td{padding:.5rem .6rem;border-bottom:1px solid var(--line);text-align:left;vertical-align:middle}
th{font-size:.78rem;text-transform:uppercase;letter-spacing:.04em;color:var(--mut);white-space:nowrap}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}
th[data-sort]{cursor:pointer;user-select:none}th[data-sort]:hover{color:var(--pri)}
th.asc::after{content:" \25B2"}th.desc::after{content:" \25BC"}
tr.low td{background:#fffaf0}tr[hidden]{display:none}
.tag{display:inline-block;padding:.1rem .55rem;border-radius:99px;font-size:.78rem;font-weight:600}
.tag.ok{background:var(--okbg);color:var(--ok)}.tag.low{background:var(--warnbg);color:var(--warn)}.tag.out{background:var(--badbg);color:var(--bad)}
.tag.adm{background:#e8eefc;color:var(--pri2)}.tag.stf{background:#eef0f3;color:#445}
.plus{color:var(--ok);font-weight:600}.minus{color:var(--bad);font-weight:600}
.btn,button{font:inherit;cursor:pointer;border:1px solid var(--pri);background:var(--pri);color:#fff;border-radius:7px;padding:.4rem .85rem;text-decoration:none;display:inline-block}
.btn:hover,button:hover{background:var(--pri2)}
.btn.sec,button.sec{background:#fff;color:var(--pri)}.btn.sec:hover,button.sec:hover{background:#eef4ff}
.btn.danger,button.danger{background:var(--bad);border-color:var(--bad)}.btn.sm,button.sm{padding:.2rem .55rem;font-size:.85rem}
input,select,textarea{font:inherit;padding:.4rem .55rem;border:1px solid #c7cfda;border-radius:7px;background:#fff;max-width:100%}
input:focus,select:focus,textarea:focus{outline:2px solid #9ec0f7;border-color:var(--pri)}
label{display:block;font-size:.85rem;color:var(--mut);margin:.7rem 0 .2rem}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:0 1rem}
.bar{display:flex;flex-wrap:wrap;gap:.6rem;align-items:center;margin-bottom:.8rem}.bar input[type=search]{min-width:260px}.grow{flex:1}
.flash{padding:.6rem .9rem;border-radius:8px;margin-bottom:1rem}.flash.ok{background:var(--okbg);color:var(--ok)}.flash.err{background:var(--badbg);color:var(--bad)}
.mut{color:var(--mut)}.right{text-align:right}.inline{display:inline}.inline form{display:inline}
.meter{background:#e8eefc;border-radius:4px;height:8px;min-width:60px}.meter i{display:block;height:100%;background:var(--pri);border-radius:4px}
.login{max-width:420px;margin:8vh auto}.login .card{padding:1.4rem}
.receipt{max-width:720px;margin:0 auto;background:#fff;padding:2rem;border:1px solid var(--line)}
.receipt h1{text-align:center;margin:0}.receipt .sub{text-align:center;color:var(--mut);margin-bottom:1.2rem}
.receipt .meta{display:flex;justify-content:space-between;gap:1rem;margin-bottom:1rem}.receipt tfoot td{font-weight:700;font-size:1.1rem;border-top:2px solid var(--ink);border-bottom:0}
.receipt .thanks{text-align:center;color:var(--mut);margin-top:1.5rem}
.receipt tfoot tr.sub td{border-top:0;font-weight:400;font-size:1rem}
td.fits{font-size:.85rem;color:var(--mut);max-width:230px}
.kv td{padding:.3rem .6rem}.kv td.indent{padding-left:1.6rem;color:var(--mut)}.kv td.n{white-space:nowrap}
td.line{border-bottom:1px solid var(--ink);min-width:160px}
tr.tot td{font-weight:700;border-top:2px solid var(--ink)}
.report{max-width:900px;margin:0 auto;background:#fff;padding:1.6rem 2rem;border:1px solid var(--line)}
.report h1{text-align:center;margin:0}.report .sub{text-align:center;color:var(--mut);margin-bottom:1rem}
.report .meta{display:flex;justify-content:space-between;gap:1rem;margin-bottom:.6rem}
.report h2{margin:1.1rem 0 .4rem;font-size:1rem;border-bottom:1px solid var(--line);padding-bottom:.2rem}
.report .cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:0 1.5rem}
.report .sigs{display:flex;justify-content:space-between;gap:3rem;margin-top:3.2rem}
.report .sigs div{flex:1;border-top:1px solid var(--ink);padding-top:.25rem;text-align:center;font-size:.85rem}
@media print{@page{margin:12mm}body{background:#fff;font-size:12px}header.top,.noprint,.flash{display:none!important}main{margin:0;max-width:none;padding:0}
.receipt{border:0;padding:0}.report{border:0;padding:0;max-width:none}.card{border:0;padding:0}.stat{border:1px solid #ccc}
tr{page-break-inside:avoid}.report h2{page-break-after:avoid}.report .cols{grid-template-columns:1fr 1fr}.tag{border:1px solid #999;background:#fff!important;color:#000!important}}
)CSS";

inline constexpr const char* kJs = R"JS(
(function () {
  'use strict';
  // ---- Inventory table: live search, system filter, low-stock filter, column sorting ----
  var tbl = document.getElementById('inv');
  if (tbl) {
    var q = document.getElementById('q'), sys = document.getElementById('sys'), mdl = document.getElementById('model'),
        low = document.getElementById('lowonly'), cnt = document.getElementById('count'),
        body = tbl.tBodies[0], rows = Array.prototype.slice.call(body.rows),
        sortKey = null, sortDir = 1;
    // Search text matches ID, name, system AND compatible vehicle models; every word typed must match somewhere.
    rows.forEach(function (r) {
      var d = r.dataset;
      r.hay = (d.id + ' ' + d.name + ' ' + d.cat + ' ' + (d.models || '').split('|').join(' ')).toLowerCase();
    });
    function apply() {
      var words = q.value.trim().toLowerCase().split(/\s+/).filter(Boolean),
          s = sys.value.toLowerCase(), m = mdl ? mdl.value.toLowerCase() : '', n = 0;
      rows.forEach(function (r) {
        var d = r.dataset;
        var ok = words.every(function (w) { return r.hay.indexOf(w) !== -1; }) &&
                 (!s || d.cat === s) &&
                 (!m || (d.models ? d.models.split('|') : []).indexOf(m) !== -1) &&   // exact model, parts with no model data are hidden
                 (!low.checked || d.low === '1');
        r.hidden = !ok;
        if (ok) n++;
      });
      cnt.textContent = n + ' of ' + rows.length + ' parts';
    }
    function sortBy(th) {
      var key = th.getAttribute('data-sort'), num = th.getAttribute('data-type') === 'num';
      sortDir = (sortKey === key) ? -sortDir : 1;
      sortKey = key;
      Array.prototype.forEach.call(tbl.tHead.rows[0].cells, function (c) { c.classList.remove('asc', 'desc'); });
      th.classList.add(sortDir === 1 ? 'asc' : 'desc');
      rows.sort(function (a, b) {
        var x = a.dataset[key], y = b.dataset[key];
        if (num) { x = parseFloat(x); y = parseFloat(y); return (x - y) * sortDir; }
        return x < y ? -sortDir : x > y ? sortDir : 0;
      });
      rows.forEach(function (r) { body.appendChild(r); });
    }
    q.addEventListener('input', apply);
    sys.addEventListener('change', apply);
    if (mdl) mdl.addEventListener('change', apply);
    low.addEventListener('change', apply);
    Array.prototype.forEach.call(tbl.querySelectorAll('th[data-sort]'), function (th) {
      th.addEventListener('click', function () { sortBy(th); });
    });
    apply();
  }
  // ---- CSV import: load the chosen file into the text box (it is then posted with the form) ----
  var f = document.getElementById('csvfile'), t = document.getElementById('csvtext');
  if (f && t) f.addEventListener('change', function () {
    if (f.files[0]) f.files[0].text().then(function (s) { t.value = s; });
  });
  // ---- print buttons and confirm dialogs (no inline handlers: the CSP forbids them) ----
  Array.prototype.forEach.call(document.querySelectorAll('[data-print]'), function (b) {
    b.addEventListener('click', function () { window.print(); });
  });
  Array.prototype.forEach.call(document.querySelectorAll('form[data-confirm]'), function (fm) {
    fm.addEventListener('submit', function (e) { if (!window.confirm(fm.getAttribute('data-confirm'))) e.preventDefault(); });
  });
})();
)JS";

}  // namespace web
