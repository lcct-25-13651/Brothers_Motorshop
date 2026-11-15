// jobs_ui.h - Phase 4: the Job orders page.
//
// A server-rendered body (kJobsBody, dropped into the normal page layout by web.cpp) plus a static
// script (kJobsScript, served at /static/jobs.js - the site's CSP allows no inline scripts) that talks
// to /api/* with fetch: the same calls a TypeScript front-end would make, kept here so the endpoints
// are exercised by something real. web.cpp replaces the /*CSRF*/ and /*SHOP*/ markers with the
// session's token and the shop name before the body is sent.
#pragma once

namespace web {

inline constexpr const char* kJobsBody = R"HTML(
<style>
.jobs-wrap{display:grid;grid-template-columns:minmax(280px,1fr) minmax(420px,1.6fr);gap:1rem;align-items:start}
@media(max-width:900px){.jobs-wrap{grid-template-columns:1fr}}
#joblist{max-height:70vh;overflow:auto}
#joblist tr{cursor:pointer}
#joblist tr.sel{background:#e8f0fe}
#joblist tr:hover{background:#f2f6fc}
.lines{width:100%}
.lines td{padding:.35rem .4rem}
.lines tr.tot td{font-weight:700;border-top:2px solid var(--line)}
.row{display:flex;gap:.5rem;flex-wrap:wrap;align-items:flex-end}
.row>div{display:flex;flex-direction:column;gap:.2rem}
.row label{font-size:.8rem;color:var(--mut)}
.hist{font-size:.85rem;max-height:220px;overflow:auto}
.hist div{padding:.15rem 0;border-bottom:1px solid var(--line)}
#toast{position:fixed;bottom:1rem;right:1rem;max-width:26rem;z-index:50}
.empty{color:var(--mut);padding:1rem 0}
h2{font-size:1.05rem;margin:.2rem 0 .6rem}
h3{font-size:.95rem;margin:1rem 0 .4rem}
@media print{
  header.top,.jobs-wrap>div:first-child,#toast,.noprint{display:none!important}
  .jobs-wrap{grid-template-columns:1fr}
}
</style>
<h1>Job orders</h1>
<div class="jobs-wrap">
  <div class="card">
    <div class="bar noprint">
      <input id="fq" placeholder="Search job, plate, customer" style="flex:2">
      <select id="fstatus"><option value="">All statuses</option><option>Open</option><option>In progress</option><option>Done</option><option>Released</option><option>Cancelled</option></select>
      <input id="fmech" placeholder="Mechanic" style="flex:1">
      <input id="fplate" placeholder="Plate" style="flex:1">
    </div>
    <table id="joblist"><tbody></tbody></table>
    <p><button class="btn noprint" id="newjob">+ New job order</button></p>
    <div id="newjobform" style="display:none" class="noprint">
      <h3>New job order</h3>
      <div class="row">
        <div style="flex:2"><label for="n_plate">Plate (any saved vehicle)</label><input id="n_plate" list="plates" placeholder="ABC 1234"></div>
        <div style="flex:1"><label for="n_odo">Odometer</label><input id="n_odo" type="number" min="0" placeholder="km"></div>
        <div style="flex:2"><label for="n_mech">Mechanic</label><input id="n_mech" list="mechs"></div>
      </div>
      <div class="row"><div style="flex:1"><label for="n_comp">Complaint / job requested</label><input id="n_comp"></div></div>
      <p><button class="btn" id="n_save">Open job</button> <button class="btn sec" id="n_cancel">Cancel</button></p>
    </div>
  </div>

  <div class="card" id="detail">
    <div class="empty">Pick a job on the left, or open a new one.</div>
  </div>
</div>
<datalist id="plates"></datalist>
<datalist id="mechs"></datalist>
<div id="toast"></div>
<div id="jobs-root" data-csrf="/*CSRF*/" data-shop="/*SHOP*/"></div>
<script src="/static/jobs.js"></script>)HTML";

inline constexpr const char* kJobsScript = R"JS(
(function () {
  'use strict';
  var root = document.getElementById('jobs-root');
  if (!root) return;
  var CSRF = root.getAttribute('data-csrf') || '';
  if (typeof SHOP === 'undefined') var SHOP = root.getAttribute('data-shop') || '';

  var api = function (path, options) {
    options = options || {};
    options.credentials = 'same-origin';
    options.headers = Object.assign({'Accept': 'application/json'}, options.headers || {});
    if (options.body && typeof options.body === 'string') options.headers['Content-Type'] = 'application/json';
    if (options.method && options.method !== 'GET') options.headers['X-CSRF-Token'] = CSRF;
    return fetch(path, options).then(function (r) {
      return r.text().then(function (t) {
        var j = null; try { j = JSON.parse(t); } catch (e) { j = null; }
        if (!r.ok) throw new Error((j && j.error) || ('HTTP ' + r.status));
        return j;
      });
    });
  };
  var esc = function (s) { return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
    return ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'})[c]; }); };
  var php = function (v) { return '&#8369;' + Number(v || 0).toFixed(2).replace(/\B(?=(\d{3})+(?!\d))/g, ','); };
  var toast = function (msg, bad) {
    var t = document.getElementById('toast');
    t.innerHTML = "<div class='flash " + (bad ? 'err' : 'ok') + "'><span>" + esc(msg) + "</span></div>";
    setTimeout(function () { t.innerHTML = ''; }, bad ? 7000 : 3500);
  };
  var selected = null, services = [], parts = [];

  var statusTag = function (s) {
    var cls = (s === 'Released' || s === 'Done') ? 'ok' : (s === 'Cancelled' ? 'out' : (s === 'In progress' ? 'low' : 'tag'));
    return "<span class='tag " + cls + "'>" + esc(s) + "</span>";
  };

  function loadFilters() {
    api('/api/services').then(function (j) { services = j.services; });
    api('/api/parts?limit=500').then(function (j) { parts = j.parts; });
    api('/api/vehicles?limit=500').then(function (j) {
      document.getElementById('plates').innerHTML = (j.vehicles || []).map(function (v) {
        return "<option value='" + esc(v.plate) + "'>" + esc(v.customer) + " - " + esc(v.model) + "</option>"; }).join('');
    }).catch(function () { /* the plate box still works, it just will not suggest */ });
  }

  function loadList() {
    var q = [];
    if (document.getElementById('fstatus').value) q.push('status=' + encodeURIComponent(document.getElementById('fstatus').value));
    if (document.getElementById('fmech').value) q.push('mechanic=' + encodeURIComponent(document.getElementById('fmech').value));
    if (document.getElementById('fplate').value) q.push('plate=' + encodeURIComponent(document.getElementById('fplate').value));
    if (document.getElementById('fq').value) q.push('q=' + encodeURIComponent(document.getElementById('fq').value));
    return api('/api/jobs' + (q.length ? '?' + q.join('&') : '')).then(function (j) {
      var rows = j.jobs.map(function (x) {
        return "<tr data-no='" + esc(x.job_no) + "' class='" + (selected === x.job_no ? 'sel' : '') + "'>" +
          "<td><b>" + esc(x.job_no) + "</b><br><span class='mut'>" + esc(x.opened_date) + " &middot; " + esc(x.plate || 'no plate') + "</span></td>" +
          "<td>" + statusTag(x.status) + "<br><span class='mut'>" + esc(x.mechanic || 'unassigned') + "</span></td>" +
          "<td class='n'>" + php(x.grand_total) + "</td></tr>";
      }).join('');
      document.querySelector('#joblist tbody').innerHTML = rows || "<tr><td class='empty'>No job orders match.</td></tr>";
      Array.prototype.forEach.call(document.querySelectorAll('#joblist tr[data-no]'), function (tr) {
        tr.onclick = function () { open(tr.getAttribute('data-no')); };
      });
      var mechs = {};
      j.jobs.forEach(function (x) { if (x.mechanic) mechs[x.mechanic] = 1; });
      document.getElementById('mechs').innerHTML = Object.keys(mechs).map(function (m) {
        return "<option value='" + esc(m) + "'>"; }).join('');
    }).catch(function (e) { toast(e.message, true); });
  }

  function open(no) {
    selected = no;
    return api('/api/jobs/' + encodeURIComponent(no)).then(function (j) {
      render(j);
      loadList();
    }).catch(function (e) { toast(e.message, true); });
  }

  function render(j) {
    var x = j.job, locked = x.locked;
    document.title = SHOP + ' - ' + x.job_no;          // what a printed / saved PDF is called
    var lines = j.lines.map(function (l) {
      return "<tr><td>" + esc(l.line_no) + "</td><td>" + esc(l.kind) + "</td><td>" + esc(l.description) +
        (l.kind === 'Part' ? " <span class='mut'>(stock " + l.available_stock + ")</span>" : '') + "</td>" +
        "<td class='n'>" + (l.kind === 'Part' ? l.quantity : '') + "</td>" +
        "<td class='n'>" + (l.kind === 'Part' ? php(l.unit_price) : '') + "</td>" +
        "<td class='n'>" + php(l.labor_fee) + "</td><td class='n'>" + php(l.line_total) + "</td>" +
        "<td class='n'>" + (locked ? '' : "<button class='sm danger' data-del='" + l.line_no + "'>Remove</button>") + "</td></tr>";
    }).join('');
    var next = (x.allowed_next || []).map(function (s) {
      return "<button class='btn " + (s === 'Cancelled' ? 'danger' : '') + "' data-status='" + esc(s) + "'>" + esc(s) + "</button>"; }).join(' ');
    var partOpts = parts.map(function (p) {
      return "<option value='" + p.id + "'>" + esc(p.name) + " (" + p.stock + " in stock, " + php(p.price) + ")</option>"; }).join('');
    var svcOpts = services.map(function (s) {
      return "<option value='" + s.id + "'>" + esc(s.name) + " - " + php(s.default_labor_fee) + "</option>"; }).join('');

    document.getElementById('detail').innerHTML =
      "<div class='bar'><h2 style='margin:0;flex:1'>" + esc(x.job_no) + " " + statusTag(x.status) + "</h2>" +
        "<button class='btn sec' id='print'>Print job card</button></div>" +
      "<p class='mut'>" + esc(x.customer || 'Walk-in customer') + (x.plate ? " &middot; " + esc(x.plate) : '') +
        (x.phone ? " &middot; " + esc(x.phone) : '') + " &middot; opened " + esc(x.opened) +
        (x.closed ? " &middot; closed " + esc(x.closed) : '') + "</p>" +
      "<div class='bar noprint'>" + (next || "<span class='mut'>This job is finished - no further changes.</span>") + "</div>" +
      "<h3>Job</h3>" +
      "<div class='row'>" +
        "<div style='flex:2'><label for='d_comp'>Complaint</label><input id='d_comp' value='" + esc(x.complaint) + "'" + (locked ? ' disabled' : '') + "></div>" +
        "<div style='flex:1'><label for='d_odo'>Odometer (km)</label><input id='d_odo' type='number' value='" + x.odometer + "'" + (locked ? ' disabled' : '') + "></div>" +
        "<div style='flex:1'><label for='d_mech'>Mechanic</label><input id='d_mech' list='mechs' value='" + esc(x.mechanic) + "'" + (locked ? ' disabled' : '') + "></div>" +
        "<div class='noprint' style='flex:0'><button class='btn' id='save'" + (locked ? ' disabled' : '') + ">Save</button></div>" +
      "</div>" +
      "<h3>Lines</h3>" +
      "<table class='lines'><thead><tr><th>#</th><th>Kind</th><th>Description</th><th class='n'>Qty</th><th class='n'>Unit</th><th class='n'>Labour</th><th class='n'>Amount</th><th></th></tr></thead>" +
      "<tbody>" + (lines || "<tr><td colspan='8' class='empty'>No lines yet.</td></tr>") + "</tbody>" +
      "<tfoot><tr class='tot'><td colspan='6'>Parts</td><td class='n'>" + php(j.totals.parts_total) + "</td><td></td></tr>" +
      "<tr class='tot'><td colspan='6'>Labour</td><td class='n'>" + php(j.totals.labor_total) + "</td><td></td></tr>" +
      "<tr class='tot'><td colspan='6'>Grand total</td><td class='n'>" + php(j.totals.grand_total) + "</td><td></td></tr></tfoot></table>" +
      (locked ? "<p class='mut'>This job is " + esc(x.status) + " - it can no longer be edited.</p>" :
       "<h3>Add a line</h3><div class='row'>" +
        "<div style='flex:2'><label for='a_part'>Part</label><select id='a_part'>" + partOpts + "</select></div>" +
        "<div style='flex:0 4rem'><label for='a_qty'>Qty</label><input id='a_qty' type='number' min='1' value='1' style='width:4.5rem'></div>" +
        "<div class='noprint' style='flex:0'><button class='btn' id='addpart'>Add part</button></div></div>" +
        "<div class='row'>" +
        "<div style='flex:2'><label for='a_svc'>Labour</label><select id='a_svc'>" + svcOpts + "</select></div>" +
        "<div style='flex:1'><label for='a_fee'>Fee</label><input id='a_fee' type='number' step='0.01' placeholder='default'></div>" +
        "<div class='noprint' style='flex:0'><button class='btn' id='addlab'>Add labour</button></div></div>") +
      "<h3>History</h3><div class='hist' id='hist'></div>";

    document.getElementById('print').onclick = function () { window.print(); };
    var save = document.getElementById('save');
    if (save) save.onclick = function () {
      api('/api/jobs/' + encodeURIComponent(x.job_no), {method: 'POST', body: JSON.stringify({
        complaint: document.getElementById('d_comp').value,
        odometer: document.getElementById('d_odo').value,
        mechanic: document.getElementById('d_mech').value
      })}).then(function (r) { toast(r.message); open(x.job_no); }).catch(function (e) { toast(e.message, true); });
    };
    Array.prototype.forEach.call(document.querySelectorAll('[data-status]'), function (b) {
      b.onclick = function () {
        var st = b.getAttribute('data-status');
        var note = (st === 'Cancelled') ? prompt('Why is this job cancelled?') : '';
        if (note === null) return;
        api('/api/jobs/' + encodeURIComponent(x.job_no) + '/status', {method: 'POST', body: JSON.stringify({status: st, note: note})})
          .then(function (r) { toast(r.message); open(x.job_no); }).catch(function (e) { toast(e.message, true); });
      };
    });
    Array.prototype.forEach.call(document.querySelectorAll('[data-del]'), function (b) {
      b.onclick = function () {
        api('/api/jobs/' + encodeURIComponent(x.job_no) + '/lines/' + b.getAttribute('data-del'), {method: 'DELETE'})
          .then(function (r) { toast(r.message); open(x.job_no); }).catch(function (e) { toast(e.message, true); });
      };
    });
    var addpart = document.getElementById('addpart');
    if (addpart) addpart.onclick = function () {
      api('/api/jobs/' + encodeURIComponent(x.job_no) + '/lines', {method: 'POST', body: JSON.stringify({
        kind: 'Part', part_id: Number(document.getElementById('a_part').value), quantity: Number(document.getElementById('a_qty').value)
      })}).then(function (r) { toast(r.message); open(x.job_no); }).catch(function (e) { toast(e.message, true); });
    };
    var addlab = document.getElementById('addlab');
    if (addlab) addlab.onclick = function () {
      var body = {kind: 'Labor', service_id: Number(document.getElementById('a_svc').value)};
      var fee = document.getElementById('a_fee').value;
      if (fee !== '') body.labor_fee = Number(fee);
      api('/api/jobs/' + encodeURIComponent(x.job_no) + '/lines', {method: 'POST', body: JSON.stringify(body)})
        .then(function (r) { toast(r.message); open(x.job_no); }).catch(function (e) { toast(e.message, true); });
    };
    api('/api/jobs/' + encodeURIComponent(x.job_no) + '/history').then(function (h) {
      document.getElementById('hist').innerHTML = h.events.slice().reverse().map(function (e) {
        return "<div><b>" + esc(e.timestamp) + "</b> " + esc(e.user) + " &middot; " + esc(e.event.replace('JOB_', '').toLowerCase()) +
               "<br><span class='mut'>" + esc(e.details) + "</span></div>"; }).join('');
    }).catch(function () {});
  }

  document.getElementById('newjob').onclick = function () {
    var f = document.getElementById('newjobform');
    f.style.display = f.style.display === 'none' ? '' : 'none';
    if (f.style.display !== 'none') document.getElementById('n_plate').focus();
  };
  document.getElementById('n_cancel').onclick = function () { document.getElementById('newjobform').style.display = 'none'; };
  document.getElementById('n_save').onclick = function () {
    var body = {
      plate: document.getElementById('n_plate').value,
      odometer: Number(document.getElementById('n_odo').value || 0),
      complaint: document.getElementById('n_comp').value,
      mechanic: document.getElementById('n_mech').value
    };
    api('/api/jobs', {method: 'POST', body: JSON.stringify(body)}).then(function (j) {
      toast(j.message);
      document.getElementById('newjobform').style.display = 'none';
      ['n_plate', 'n_odo', 'n_comp', 'n_mech'].forEach(function (i) { document.getElementById(i).value = ''; });
      open(j.job.job_no);
    }).catch(function (e) { toast(e.message, true); });
  };
  ['fq', 'fstatus', 'fmech', 'fplate'].forEach(function (i) {
    var el = document.getElementById(i);
    el.onchange = loadList;
    if (i === 'fq') el.oninput = function () { clearTimeout(el._t); el._t = setTimeout(loadList, 250); };
  });

  loadFilters();
  loadList();
})();
)JS";

}  // namespace web
