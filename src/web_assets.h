// web_assets.h - the stylesheet and script served at /static/app.css and /static/app.js.
// Kept in their own header so web.cpp stays readable. No frameworks: plain CSS + vanilla JS.
#pragma once

namespace web {

inline constexpr const char* kCss = R"CSS(
:root{--bg:#f4f6f9;--card:#fff;--ink:#1b2430;--mut:#6b7686;--line:#e2e7ee;--pri:#0f5fd7;--pri2:#0a47a8;--ok:#127a3e;--okbg:#e5f6ec;--bad:#b42318;--badbg:#fdeceb;--warn:#8a5a00;--warnbg:#fff4d6}
*{box-sizing:border-box}
body{margin:0;font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--ink)}
header.top{background:#0d1b2e;color:#fff;display:flex;flex-wrap:wrap;align-items:center;gap:.25rem 1rem;padding:.6rem 1.2rem}
header.top .brand{font-weight:700;font-size:1.05rem;margin-right:.8rem;display:flex;align-items:center;gap:.6rem}
header.top .brand-logo{width:40px;height:40px;border-radius:10px;object-fit:cover;flex:none;box-shadow:0 0 0 1px rgba(255,255,255,.15)}
.doc-logo{display:block;width:84px;height:84px;margin:0 auto .5rem;border-radius:12px}
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
/* ---- Auth pages (login / first-time setup / reset password): centered card on a slate gradient ---- */
body.auth{--a-pri:#2563eb;--a-pri2:#1d4ed8;--a-ink:#0f172a;--a-mut:#64748b;--a-line:#e2e8f0;
min-height:100vh;display:flex;align-items:center;justify-content:center;padding:1.5rem;color:var(--a-ink);
position:relative;overflow:hidden;background:radial-gradient(1000px 500px at 15% -10%,rgba(37,99,235,.22),transparent 60%),linear-gradient(135deg,#0f172a 0%,#1e293b 100%) fixed}
body.auth .auth-wrap{width:100%;max-width:420px}
body.auth .auth-card{background:#fff;border:1px solid rgba(255,255,255,.12);border-radius:16px;padding:2rem 2rem 1.6rem;
box-shadow:0 20px 25px -5px rgba(0,0,0,.3),0 8px 10px -6px rgba(0,0,0,.2);animation:auth-in .35s ease-out both}
body.auth .auth-brand{text-align:center;margin-bottom:1.4rem}
body.auth .auth-logo{position:relative;width:96px;height:96px;margin:0 auto .9rem;border-radius:18px;display:flex;align-items:center;justify-content:center;
color:#fff;background:#0a0a0a;box-shadow:0 8px 18px -4px rgba(37,99,235,.6);animation:logo-float 3.2s ease-in-out infinite}
body.auth .auth-logo img{width:100%;height:100%;border-radius:18px;object-fit:cover;display:block}
body.auth .auth-logo::before,body.auth .auth-logo::after{content:"";position:absolute;inset:0;border-radius:18px;border:2px solid rgba(59,130,246,.55);animation:logo-ring 2.6s ease-out infinite}
body.auth .auth-logo::after{animation-delay:1.3s}
body.auth .auth-logo svg{width:44px;height:44px;overflow:visible}
body.auth .auth-logo .gear{transform-box:fill-box;transform-origin:center;animation:logo-spin 7s linear infinite}

body.auth .auth-brand h1{margin:0;font-size:1.45rem;line-height:1.2;letter-spacing:-.01em;color:var(--a-ink)}
body.auth .auth-sub{margin:.3rem 0 0;font-size:.82rem;color:var(--a-mut);text-transform:uppercase;letter-spacing:.08em}
body.auth h2{margin:0 0 .4rem;font-size:1.05rem;color:var(--a-ink)}
body.auth p{margin:.8rem 0 0}body.auth h2+p.mut{margin-top:0}
body.auth .mut{color:var(--a-mut);font-size:.9rem}
body.auth a{color:var(--a-pri);text-decoration:none;font-weight:500}body.auth a:hover{color:var(--a-pri2);text-decoration:underline}
body.auth label{margin:1rem 0 .35rem;font-size:.85rem;font-weight:600;color:#334155}
body.auth input{width:100%;padding:.7rem .85rem;font-size:1rem;color:var(--a-ink);background:#f8fafc;border:1px solid #cbd5e1;border-radius:10px;
transition:border-color .15s,box-shadow .15s,background-color .15s}
body.auth input::placeholder{color:#94a3b8}
body.auth input:hover{border-color:#94a3b8}
body.auth input:focus{outline:none;background:#fff;border-color:var(--a-pri);box-shadow:0 0 0 3px rgba(37,99,235,.3)}
body.auth button,body.auth .btn{display:block;width:100%;padding:.75rem 1rem;font-size:1rem;font-weight:600;text-align:center;border-radius:10px;
border:1px solid var(--a-pri);background:var(--a-pri);color:#fff;text-decoration:none;
transition:background-color .15s,box-shadow .15s,transform .05s,border-color .15s}
body.auth button:hover,body.auth .btn:hover{background:var(--a-pri2);border-color:var(--a-pri2);color:#fff;text-decoration:none}
body.auth button:active,body.auth .btn:active{transform:translateY(1px);box-shadow:inset 0 2px 4px rgba(0,0,0,.2)}
body.auth button:focus-visible,body.auth .btn:focus-visible{outline:none;box-shadow:0 0 0 3px rgba(37,99,235,.35)}
body.auth button.sec,body.auth .btn.sec{background:#fff;color:var(--a-pri);border-color:#cbd5e1}
body.auth button.sec:hover,body.auth .btn.sec:hover{background:#eff6ff;color:var(--a-pri2);border-color:var(--a-pri)}
body.auth .btn{margin-top:.6rem}
body.auth form>p:last-child.mut{text-align:center;margin-top:1.1rem}
body.auth .flash{display:flex;align-items:flex-start;gap:.55rem;margin:0 0 1rem;padding:.7rem .9rem;border-radius:10px;font-size:.92rem;line-height:1.4;border:1px solid transparent}
body.auth .flash::before{flex:none;width:1.25rem;height:1.25rem;border-radius:50%;font-size:.8rem;font-weight:700;line-height:1.25rem;text-align:center;color:#fff}
body.auth .flash.err{background:#fef2f2;border-color:#fecaca;color:#991b1b}body.auth .flash.err::before{content:"!";background:#dc2626}
body.auth .flash.ok{background:#f0fdf4;border-color:#bbf7d0;color:#166534}body.auth .flash.ok::before{content:"\2713";background:#16a34a}
body.auth .auth-foot{margin:1.1rem 0 0;text-align:center;font-size:.78rem;color:#94a3b8}
/* animated background: drifting glow orbs + faint dot grid + floating specks */
body.auth::before{content:"";position:fixed;inset:0;z-index:0;pointer-events:none;
background-image:radial-gradient(rgba(148,163,184,.14) 1px,transparent 1px);background-size:28px 28px;
-webkit-mask-image:radial-gradient(ellipse at center,#000 30%,transparent 80%);mask-image:radial-gradient(ellipse at center,#000 30%,transparent 80%)}
body.auth .bg-orb{position:fixed;z-index:0;pointer-events:none;border-radius:50%;filter:blur(70px);opacity:.55}
body.auth .bg-orb.o1{width:420px;height:420px;left:-120px;top:-100px;background:#2563eb;animation:orb-a 16s ease-in-out infinite alternate}
body.auth .bg-orb.o2{width:360px;height:360px;right:-100px;bottom:-80px;background:#7c3aed;animation:orb-b 19s ease-in-out infinite alternate}
body.auth .bg-orb.o3{width:260px;height:260px;right:22%;top:8%;background:#0ea5e9;opacity:.35;animation:orb-c 22s ease-in-out infinite alternate}
body.auth .bg-gear{position:fixed;z-index:0;pointer-events:none;color:rgba(148,163,184,.10);animation:logo-spin 40s linear infinite}
body.auth .bg-gear.g1{width:240px;height:240px;left:6%;bottom:-60px}
body.auth .bg-gear.g2{width:160px;height:160px;right:7%;top:10%;animation-duration:28s;animation-direction:reverse}
body.auth .bg-gear svg{width:100%;height:100%;display:block}
body.auth .auth-wrap{position:relative;z-index:1}
@keyframes logo-spin{to{transform:rotate(360deg)}}
@keyframes logo-float{0%,100%{transform:translateY(0)}50%{transform:translateY(-5px)}}
@keyframes logo-ring{0%{transform:scale(1);opacity:.8}100%{transform:scale(1.7);opacity:0}}
@keyframes orb-a{to{transform:translate(160px,120px) scale(1.15)}}
@keyframes orb-b{to{transform:translate(-180px,-90px) scale(1.2)}}
@keyframes orb-c{to{transform:translate(-120px,160px) scale(.85)}}
@keyframes auth-in{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
@media (max-width:480px){body.auth{padding:1rem;align-items:flex-start;padding-top:8vh}body.auth .auth-card{padding:1.5rem 1.25rem 1.25rem}}
@media (prefers-reduced-motion:reduce){body.auth .auth-card,body.auth .auth-logo,body.auth .auth-logo::before,body.auth .auth-logo::after,body.auth .gear,body.auth .gear-sm,body.auth .bg-orb,body.auth .bg-gear{animation:none!important}body.auth *{transition:none!important}}
/* ---- login: shop artwork as the background on the left, sign-in box on the right ---- */
body.auth{background:#3f8fd2 url(/static/login-bg.jpg) left center/cover no-repeat fixed;justify-content:flex-end;
padding:1.5rem clamp(1.2rem,6vw,7rem)}
body.auth::before,body.auth .bg-orb,body.auth .bg-gear{display:none}
body.auth .auth-card{background:rgba(255,255,255,.94);border:1px solid rgba(255,255,255,.9);border-radius:20px;
box-shadow:0 28px 56px -14px rgba(5,30,80,.65),0 8px 18px -6px rgba(5,30,80,.4);-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px)}
body.auth .auth-logo{box-shadow:0 8px 18px -4px rgba(23,64,127,.45)}
body.auth .auth-foot{color:#eaf3ff;text-shadow:0 1px 3px rgba(5,30,80,.7)}
@media (max-width:899px){body.auth{justify-content:center;background:linear-gradient(rgba(10,40,90,.2),rgba(10,40,90,.2)),#3f8fd2 url(/static/login-bg.jpg) 36% center/cover no-repeat fixed}}
/* ---- left sidebar navigation (wide screens); narrow screens keep the top bar ---- */
@media(min-width:900px){
header.top{position:fixed;top:0;left:0;bottom:0;width:230px;flex-direction:column;flex-wrap:nowrap;align-items:stretch;gap:.6rem;padding:1rem .8rem;overflow-y:auto;z-index:20}
header.top .brand{margin:0 0 .6rem;padding:.2rem .4rem;font-size:1.1rem;flex-direction:column;text-align:center}
header.top .brand-logo{width:96px;height:96px;border-radius:16px}
header.top nav{flex-direction:column;flex-wrap:nowrap;flex:1 1 auto;gap:.15rem}
header.top nav a{padding:.55rem .8rem}
header.top form{flex-direction:column;align-items:stretch;gap:.5rem}
header.top form a{display:block}header.top form .chip{display:block;text-align:center}
header.top form button{width:100%}
main{margin:1.2rem 0 1.2rem 230px;max-width:1400px;padding:0 1.6rem}
}
.an-head{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:baseline;gap:.4rem 1rem}.an-head h2{margin:1.2rem 0 .6rem}
.an-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(400px,1fr));gap:0 1rem}.an-grid h3{font-size:.95rem;margin:0 0 .5rem}.an-grid td{padding:.35rem .5rem}
.page-head{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:.4rem 1.2rem;margin-bottom:.6rem}.page-head h1{margin:.2rem 0}
.clock-card{display:flex;flex-direction:column;align-items:flex-end;text-align:right;padding:.65rem 1.4rem .7rem;border-radius:16px;color:#fff;line-height:1.1;
  background:linear-gradient(135deg,#0d1b2e 0%,#17407f 62%,#0f5fd7 100%);box-shadow:0 10px 24px -12px rgba(15,95,215,.8),inset 0 0 0 1px rgba(255,255,255,.08);
  font-family:Bahnschrift,"Segoe UI Variable Display","Segoe UI",system-ui,sans-serif}
.clk-date{font-size:1.05rem;font-weight:500;letter-spacing:.06em;text-transform:uppercase;color:#bcd2f5}
.clk-time{font-size:2.9rem;font-weight:700;letter-spacing:.02em;font-variant-numeric:tabular-nums;margin-top:.2rem;white-space:nowrap}
.clk-time small{font-size:1.15rem;font-weight:600;margin-left:.4rem;color:#9fc0f5;letter-spacing:.04em}
@media(max-width:600px){.clock-card{width:100%;align-items:center;text-align:center}.clk-time{font-size:2.4rem}}
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
      r.hay = ((d.pn || '') + ' ' + d.id + ' ' + d.name + ' ' + d.cat + ' ' + (d.models || '').split('|').join(' ')).toLowerCase();
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
    var numKeys = { id: 1, price: 1, qty: 1, min: 1, gap: 1 },
        sortSel = document.getElementById('sortsel'), original = rows.slice();
    function mark(key, dir) {
      Array.prototype.forEach.call(tbl.tHead.rows[0].cells, function (c) {
        c.classList.remove('asc', 'desc');
        if (key && c.getAttribute('data-sort') === key) c.classList.add(dir === 1 ? 'asc' : 'desc');
      });
    }
    function doSort(key, dir) {
      sortKey = key; sortDir = dir;
      mark(key, dir);
      var isNum = !!numKeys[key];
      rows.sort(function (a, b) {
        var x = a.dataset[key] || '', y = b.dataset[key] || '';
        if (isNum) return (parseFloat(x) - parseFloat(y)) * dir;
        return x.localeCompare(y, undefined, { numeric: true, sensitivity: 'base' }) * dir;   // AA101 < AA102 < AA1010, 99 < 100
      });
      rows.forEach(function (r) { body.appendChild(r); });
    }
    function sortBy(th) {
      var key = th.getAttribute('data-sort');
      doSort(key, sortKey === key ? -sortDir : 1);
      if (sortSel) {
        var v = key + ':' + sortDir;
        sortSel.value = Array.prototype.some.call(sortSel.options, function (o) { return o.value === v; }) ? v : '';
      }
    }
    if (sortSel) sortSel.addEventListener('change', function () {
      if (!sortSel.value) {           // back to the file order
        sortKey = null; mark(null, 1);
        rows = original.slice();
        rows.forEach(function (r) { body.appendChild(r); });
        return;
      }
      var p = sortSel.value.split(':');
      doSort(p[0], parseInt(p[1], 10));
    });
    q.addEventListener('input', apply);
    sys.addEventListener('change', apply);
    if (mdl) mdl.addEventListener('change', apply);
    low.addEventListener('change', apply);
    Array.prototype.forEach.call(tbl.querySelectorAll('th[data-sort]'), function (th) {
      th.addEventListener('click', function () { sortBy(th); });
    });
    apply();
  }
  // ---- live date & time (big clock on the Inventory page) ----
  var cd = document.getElementById('clkdate'), ct = document.getElementById('clktime');
  if (cd && ct) {
    var tick = function () {
      var d = new Date();
      cd.textContent = d.toLocaleDateString('en-US', {weekday: 'long', month: 'long', day: 'numeric', year: 'numeric'});
      var t = d.toLocaleTimeString('en-US', {hour: 'numeric', minute: '2-digit', second: '2-digit'}).split(' ');
      ct.textContent = t[0];
      var sm = document.createElement('small');
      sm.textContent = t[1] || '';
      ct.appendChild(sm);
    };
    tick();
    setInterval(tick, 1000);
  }
    // ---- Excel (.xlsx) / CSV import on the Add part page: parsed in the browser, then posted to /import ----
  function u16(d, o) { return d[o] | (d[o + 1] << 8); }
  function u32(d, o) { return (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0; }
  function inflateRaw(bytes) {
    var s = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Response(s).arrayBuffer().then(function (b) { return new Uint8Array(b); });
  }
  function unzip(buf) {
    var d = new Uint8Array(buf), eocd = -1, dec = new TextDecoder(), jobs = [], out = {};
    for (var i = d.length - 22; i >= Math.max(0, d.length - 65557); i--) if (u32(d, i) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error('This is not an .xlsx file (save it as Excel Workbook).');
    var n = u16(d, eocd + 10), p = u32(d, eocd + 16);
    for (var k = 0; k < n; k++) {
      if (u32(d, p) !== 0x02014b50) break;
      var method = u16(d, p + 10), csize = u32(d, p + 20), nl = u16(d, p + 28), el = u16(d, p + 30), cl = u16(d, p + 32), lo = u32(d, p + 42);
      var name = dec.decode(d.subarray(p + 46, p + 46 + nl));
      p += 46 + nl + el + cl;
      if (!/^xl\/(workbook\.xml|_rels\/workbook\.xml\.rels|sharedStrings\.xml|worksheets\/[^\/]+\.xml)$/.test(name)) continue;
      var start = lo + 30 + u16(d, lo + 26) + u16(d, lo + 28), raw = d.subarray(start, start + csize);
      (function (nm, m, r) {
        if (m === 0) out[nm] = r;
        else if (m === 8) jobs.push(inflateRaw(r).then(function (x) { out[nm] = x; }));
      })(name, method, raw);
    }
    return Promise.all(jobs).then(function () { return out; });
  }
  function parseXml(bytes) { return new DOMParser().parseFromString(new TextDecoder().decode(bytes), 'application/xml'); }
  function colIndex(ref) {
    var m = /^([A-Z]+)/.exec(ref || ''), n = 0;
    if (!m) return -1;
    for (var i = 0; i < m[1].length; i++) n = n * 26 + (m[1].charCodeAt(i) - 64);
    return n - 1;
  }
  function csvField(v) { return /[",\r\n]/.test(v) ? '"' + v.replace(/"/g, '""') + '"' : v; }
  function xlsxToCsv(buf) {
    return unzip(buf).then(function (f) {
      var shared = [], tags = function (el, name) { return Array.prototype.slice.call(el.getElementsByTagName(name)); };
      if (f['xl/sharedStrings.xml']) tags(parseXml(f['xl/sharedStrings.xml']), 'si').forEach(function (si) {
        var t = ''; tags(si, 't').forEach(function (x) { t += x.textContent; }); shared.push(t);
      });
      var path = 'xl/worksheets/sheet1.xml';
      if (f['xl/workbook.xml'] && f['xl/_rels/workbook.xml.rels']) {
        var sh = tags(parseXml(f['xl/workbook.xml']), 'sheet')[0];
        var rid = sh && (sh.getAttribute('r:id') || sh.getAttributeNS('http://schemas.openxmlformats.org/officeDocument/2006/relationships', 'id'));
        tags(parseXml(f['xl/_rels/workbook.xml.rels']), 'Relationship').forEach(function (r) {
          if (rid && r.getAttribute('Id') === rid) { var tg = r.getAttribute('Target') || ''; path = tg.charAt(0) === '/' ? tg.substr(1) : 'xl/' + tg; }
        });
      }
      if (!f[path]) path = Object.keys(f).filter(function (x) { return /^xl\/worksheets\//.test(x); }).sort()[0];
      if (!path || !f[path]) throw new Error('No worksheet found in the file.');
      var rows = [];
      tags(parseXml(f[path]), 'row').forEach(function (r) {
        var row = [], next = 0;
        tags(r, 'c').forEach(function (c) {
          var ci = colIndex(c.getAttribute('r')); if (ci < 0) ci = next; next = ci + 1;
          var t = c.getAttribute('t'), vEl = c.getElementsByTagName('v')[0], v = vEl ? vEl.textContent : '', s = '';
          if (t === 's') s = shared[parseInt(v, 10)] || '';
          else if (t === 'inlineStr') tags(c, 't').forEach(function (x) { s += x.textContent; });
          else if (t === 'str') s = v;
          else if (t === 'b') s = v === '1' ? 'TRUE' : 'FALSE';
          else if (v !== '') { var num = Number(v); s = isFinite(num) ? (Number.isInteger(num) ? String(num) : String(Math.round(num * 100) / 100)) : v; }
          while (row.length < ci) row.push('');
          row[ci] = s.replace(/[\r\n]+/g, ' ').trim();
        });
        if (row.some(function (x) { return x !== ''; })) rows.push(row);
      });
      var width = rows.reduce(function (m, r) { return Math.max(m, r.length); }, 0);
      var lines = rows.map(function (r) { while (r.length < width) r.push(''); return r.map(csvField).join(','); });
      return { csv: lines.join('\r\n') + '\r\n', rows: Math.max(0, rows.length - 1), header: rows.length ? rows[0].join(', ') : '' };
    });
  }
  var xf = document.getElementById('xlfile'), xc = document.getElementById('xlcsv');
  if (xf && xc) xf.addEventListener('change', function () {
    var file = xf.files[0];
    if (!file) return;
    var work = /\.csv$/i.test(file.name)
      ? file.text().then(function (t) { var ls = t.split(/\r?\n/).filter(Boolean); return { csv: t, rows: Math.max(0, ls.length - 1), header: ls[0] || '' }; })
      : file.arrayBuffer().then(xlsxToCsv);
    work.then(function (r) {
      if (!r.rows) { window.alert('No data rows found in ' + file.name + '.'); xf.value = ''; return; }
      if (window.confirm('Import ' + r.rows + ' row(s) from ' + file.name + '?\n\nColumns: ' + r.header + '\n\nNew part numbers are added; existing ones are updated (Quantity = the new stock level).')) { xc.value = r.csv; xc.form.submit(); }
      else xf.value = '';
    }).catch(function (e) { window.alert('Could not read the file: ' + e.message); xf.value = ''; });
  });
  // ---- CSV import: load the chosen file into the text box (it is then posted with the form) ----
  var f = document.getElementById('csvfile'), t = document.getElementById('csvtext');
  if (f && t) f.addEventListener('change', function () {
    if (f.files[0]) f.files[0].text().then(function (s) { t.value = s; if (window.confirm('Import ' + f.files[0].name + ' now?')) t.form.submit(); });
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
