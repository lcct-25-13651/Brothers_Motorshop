# RE Parts Tracker v3 (modular, web-only)

Bajaj RE parts & inventory web app. Standard C++17, [cpp-httplib](https://github.com/yhirose/cpp-httplib) for HTTP,
plain CSV files for storage. No Node, Python, database engine or JS framework.

## Layout
```
CMakeLists.txt  Makefile  README.md
third_party/httplib.h        <- you add this (single header, see below)
src/
  util.h/.cpp       CSV, time, HTML-escape, atomic file writes
  auth.h/.cpp       accounts, salted SHA-256 (20,000 rounds), roles + permissions, login throttle, audit.csv
  inventory.h/.cpp  parts.csv (incl. cost_price, compatible_models), movements.csv, invoices.csv + payments.csv (credit sales), CSV import/export, backups
  reports.h/.cpp    top sellers, daily/weekly/monthly revenue & expenses, dead stock, reorder list, receivables, daily cash report
  crm.h             Phase 3: customers.csv + vehicles.csv, search, vehicle/customer history (header-only)
  jobs.h/.cpp       Phase 4: jobs.csv + job_lines.csv + services.csv - status machine, lines, stock, totals
  api.h/.cpp        Phase 4: the JSON REST layer (/api/*) the Job orders page (or your own UI) calls
  jobs_ui.h         Phase 4: the Job orders page - server-rendered body + /static/jobs.js
  web.h/.cpp        all HTTP routes + HTML views   (web_assets.h = CSS/JS served at /static)
  main.cpp          start-up: backup -> load -> serve
tests/              smoke_test.cpp (core logic), web_e2e.cpp + stub/ (web layer, no sockets),
                    jobs_test.cpp (Phase 4 core), api_e2e.cpp (Phase 4 endpoints)
```

## Build
1. Download https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h and save it as `third_party/httplib.h`.
2. Build with either:
```
make                          # g++ -std=c++17 ...   (Windows MSYS2/MinGW: same command, links ws2_32)
# or
cmake -S . -B build && cmake --build build
```
Equivalent manual command (Linux/macOS): `g++ -std=c++17 -O2 -Ithird_party src/*.cpp -o re_parts_tracker -pthread`
Windows: `g++ -std=c++17 -O2 -Ithird_party -D_WIN32_WINNT=0x0A00 -DNOMINMAX src/*.cpp -o re_parts_tracker.exe -lws2_32`

Tests: `make check` runs all four suites - `test` (core logic), `jobstest` (Phase 4 core), `e2e` (web layer
vs. an in-process stand-in for httplib) and `apie2e` (Phase 4 endpoints).

## Run
```
./re_parts_tracker [--port 8080] [--host 127.0.0.1] [--data <folder>] [--shop "Shop name"] [--no-backup]
```
Open http://localhost:8080. First start asks you to create the Admin account (an existing v2 `users.csv` is read as-is: its account becomes Admin) (an existing v2 `users.csv` is read as-is: its account becomes Admin).
Use `--host 0.0.0.0` so other PCs on the shop network can connect (the app speaks plain HTTP: keep it on a trusted network).

If the app is shown **inside a frame on another site** (a shop portal, a kiosk screen, a hosted preview), start it with
`--allow-embedding`. Framing changes two things that would otherwise break the session: the `frame-ancestors` /
`X-Frame-Options` headers are relaxed, and the session cookie is sent as `SameSite=None; Secure; Partitioned`
(a browser drops `SameSite=Strict` in a cross-site frame; `Partitioned` - CHIPS - keeps it even when third-party
cookies are blocked). It looks exactly like "login does not work", but `audit.csv` shows a successful `LOGIN`.
Override the attributes yourself with `--cookie-samesite Strict|Lax|None`, `--secure-cookies` and
`--partitioned-cookies`. If a browser blocks third-party cookies *and* does not support CHIPS, open the framed app
in its own tab - as a top-level page the normal `SameSite=Strict` cookie works.

## Phase 1 additions
**Cost price.** `parts.csv` now has a 7th column, `cost_price` (what one unit costs the shop). Older 6-column files load fine (cost = 0) and are
upgraded on the first save; the start-up backup still keeps the original file. Admins set it under *Add part* / *Edit part* or via CSV import
(`cost_price` column; the older `UnitCost` column still means "what I paid for stock arriving in this import" and feeds the expense report).
The **Top Sellers** report shows `COGS = units sold x cost_price`, `Net profit = Revenue - COGS` and `Margin % = Net profit / Revenue`.
Parts with no cost price show "n/a" instead of a misleading 100 % margin. COGS uses the part's *current* cost_price.

**Void invoice (Admin only).** *Invoices -> open an invoice -> Void invoice* (reason required). Nothing is deleted: `invoices.csv` is untouched,
one `VOID` row per part line is appended to `movements.csv` (positive quantity, `ref` = invoice number) and the part quantities in `parts.csv`
go back up. A services-only invoice gets one marker row (part_id 0, quantity 0). An `INVOICE_VOIDED` event is written to `audit.csv`.
A voided invoice is shown as VOIDED, cannot be voided twice, and its sales/services no longer count in revenue or Top Sellers
(they drop out of their original period). If a part on the invoice has since been deleted, re-add it with the same ID first.

## Importing the client's receipt (tools/seed_data.cpp)
Standalone C++17 tool that adds the 75 parts from the supplier receipt to `parts.csv` (schema
`id,part_name,vehicle_system,selling_price,quantity,min_stock,cost_price,compatible_models`).
```
make seed        # or: g++ -std=c++17 -O2 -Wall -Wextra tools/seed_data.cpp -o seed_data && ./seed_data [path/to/parts.csv]
```
- Supplier numbers (e.g. `36AA4028`) are not numeric, so each part gets the next free internal ID (from 1001) and the supplier
  number is kept in the name as `[36AA4028]` - the search box finds it.
- `selling_price` = SRP from the receipt, `quantity` = received quantity, `min_stock` = 5 (or the quantity if lower).
- `cost_price` is an **estimate** (80 % of SRP, constant `kCostRatio` in the tool). Replace it with real costs.
- `vehicle_system` is inferred from the description (Engine, Braking, Electrical, Transmission, Chassis): check the odd ones.
- Re-running is safe: parts already present (matched by the `[supplier number]` tag) are not touched, so live stock is never reset.
  The previous file is copied to `parts.csv.bak`; an unreadable `parts.csv` is never overwritten.
- The app now keeps the `cost_price` and `compatible_models` columns when it saves.

## Phase 2 additions

### 1. Receivables / Utang (credit sales)
- **Payment status.** Every invoice is `Paid`, `Partial` or `Unpaid`. *New sale* has an **Amount paid now** box and a **Payment method**
  (Cash, GCash, Maya, Bank transfer, Card). Blank = paid in full; a smaller number (or `0`) makes it a credit sale. Credit sales need a
  customer name (walk-in customers must pay in full). Stock is deducted at the time of sale as before.
- **Receivables / Utang tab** (Staff + Admin): total outstanding, balance per customer (name variants like "Juan  Cruz"/"juan cruz" are merged),
  oldest unpaid date, and the open invoices with a **Record payment** button. The **Invoices** list shows paid / balance / status and has
  All / Unpaid / Partial / Paid filters. Receipts show the balance due and the payment history.
- **Record payment.** Partial or full instalments; the full balance is pre-filled. Overpayment is refused. The form carries the balance the
  cashier saw, so a double-click or a second cashier cannot apply the same payment twice.
- **Files.** `invoices.csv` gains two columns, `amount_paid` and `payment_status` (invoice-level, repeated on every line row, rewritten
  atomically on each payment). Old 9-column files are read as fully paid and upgraded on the first start (the start-up backup keeps the
  original; if damaged rows are found the file is also kept as `invoices.csv.bak`). New file `payments.csv`
  (`timestamp,invoice_no,username,amount,method,balance_after,note`) has one row per payment, including the part paid at the till.
  Invoices from before this version get one "opening balance" payment row so the daily report adds up.
- **Audit.** `audit.csv` gets `CREDIT_SALE` (a sale left with a balance) and `PAYMENT_RECORDED` (invoice, customer, amount, method, balance after, status, note).

### 2. Daily cash report (register close-out)
*Daily cash report* in the menu (Staff + Admin; pick any past date). Shows invoice counts (paid / partial / unpaid), gross sales
(parts + services), **net collected** (every payment received that day, split into payments on today's invoices vs. earlier utang),
credit given that day, all unpaid credit sales outstanding, a payment-method breakdown, expected cash in the drawer (Cash payments only) with
blank *Counted cash / Over-short* lines, the day's invoices and payments, and signature lines. *Print report* gives a clean print layout.
Reports for past dates show invoice statuses as they stood at the end of that day.

### 3. Vehicle compatibility
- `compatible_models` (comma / semicolon / pipe separated, e.g. `Bajaj RE 4S, Bajaj RE Compact, Bajaj RE Maxima`) can now be typed in
  *Add part* / *Edit part* (Admin) and imported with a `compatible_models` column.
- The inventory search box searches ID, name, system **and models**; several words must all match (`oil maxima`).
- New **All vehicle models** drop-down next to the system filter lists every distinct model; choosing one shows only parts that list exactly
  that model (parts with no model data are hidden while a model is selected). `/?model=Bajaj%20RE%20Maxima` opens the page pre-filtered.
  A new *Fits (vehicle models)* column shows each part's models. Changing a part's models is written to the audit trail (`PART_MODELS_CHANGED`).
- Note: `tools/seed_data.cpp` tags all 75 receipt parts with the generic `Bajaj RE`; fill in the specific models for the drop-down to be useful.

Permissions: Staff can use Receivables and the Daily cash report (new permissions `Receivables`, `CashReport` in `auth.cpp::allowed`; remove them from the Staff list there to make them Admin-only).

## Phase 3 additions - customer & vehicle records

### Files
- `customers.csv` - `id,name,phone,address,notes,archived`
- `vehicles.csv` - `id,customer_id,plate,model,year,color,notes,archived`
- `invoices.csv` gets two more columns at the end: `customer_id,vehicle_id` (13 columns now). **Blank on every old invoice** and on sales to
  walk-in / free-text customers. The first start on an 11-column file upgrades it in place (all original values unchanged; the start-up backup keeps the
  original file). The `customer` text on an invoice is still the name as printed, so receipts and the Receivables page work as before.
- `archived` (0/1) is the only column added to the spec: it is how a customer / vehicle is archived. Files with the 5 / 7 columns of the spec load fine.
- **Plates** are stored UPPERCASE with all spaces removed (`" abc  1234 "` -> `ABC1234`; letters, digits and `-`, 2-12 characters) and are **unique** - archived vehicles count too.
- All three files are covered by the normal start-up / *Backup now* copies (every `*.csv`). They hold personal data: they are in `.gitignore` - never commit them.

### Pages (menu: *Customers*, Staff + Admin)
- **Customers** (`/customers`): one search box for **name** (every word must match), **phone** (digits, at least 3) or **plate** (any spelling, at least 2 characters).
  Results show the plate that matched, the vehicles and the utang balance. *Show archived* reveals archived customers.
- **Customer detail** (`/customer?id=`): info, vehicles, **utang balance**, **invoice history** (newest first, with *Record payment* on open ones), Edit / Add vehicle / New sale.
- **Vehicle history** (`/vehicle?id=` or `/vehicle?plate=ABC 1234`): owner, totals, and **every invoice of that plate, newest first**.
  (Phase 4: job orders are to be merged into the same list - see `crm::HistoryRow` / `crm::vehicleHistory`.)
- **Add / Edit customer** and **Add / Edit vehicle** forms. A vehicle's owner cannot be changed by an edit.

### New sale
- *Customer* box: pick a saved customer from the suggestions (shows the phone) **or just type a name** - exactly as before. Free text that matches no record stays free text.
  A typed name that equals exactly one saved customer is linked to it; if two customers share the name you are asked to pick one.
- *Vehicle plate* box: only saved vehicles (the list narrows to the chosen customer; picking a plate fills in its owner). A plate that belongs to somebody else, an unknown plate, or an archived customer / vehicle is refused with a message and nothing is deducted from stock.
- Works without JavaScript too (the server resolves what was typed); `/static/app.js` only adds the narrowing / auto-fill.
- Receipts show the vehicle; the invoice list has a *Vehicle* column; names link to the customer record.

### Old invoices, utang and archive rules
- Old free-text invoices are never rewritten or linked behind your back. On a customer's page they appear marked **(by name)** and **count in the utang balance**
  when the customer's name matches (ignoring case / extra spaces) *and* only one customer record has that name - so the figure equals the Receivables page.
- Receivables / Utang is unchanged (still grouped by the name on the invoice); the customer name there links to the record when there is exactly one match.
- A customer or vehicle **with invoices cannot be deleted - only archived** (Admin). Archived records disappear from the sale form but stay in every history and can still be found
  (a plate of an archived vehicle still finds its owner); their old utang stays collectable. Only a record with no history (and a customer with no vehicles) can be deleted, by an Admin.
- New permissions in `auth.cpp::allowed`: `Customers` (view / add / edit - Staff + Admin) and `ArchiveCustomers` (archive / restore / delete - Admin only).
- Audit events: `CUSTOMER_ADDED/UPDATED/ARCHIVED/RESTORED/DELETED`, `VEHICLE_ADDED/UPDATED/ARCHIVED/RESTORED/DELETED`.

---

### Tests
`make test` (core: plate rules, uniqueness, search, archive/delete rules, persistence, history order, balances, upgrade of an 11-column `invoices.csv` with every value unchanged)
and `make e2e` (web: add/edit/search by plate, linked + free-text sales, history pages, Staff vs Admin, CSRF, escaping, archive/restore, files on disk).

## Phase 4 additions - job orders / repair tickets

```
src/jobs.h/.cpp    the model + every rule (status machine, lines, stock, totals)   <- no HTTP in here
src/api.h/.cpp     the JSON REST layer (/api/*)                                    <- no business rules in here
src/jobs_ui.h      the Job orders page: server-rendered body + /static/jobs.js (fetch -> /api/*)
tests/jobs_test.cpp   core rules: stock, totals, workflow, concurrency, crash recovery
tests/api_e2e.cpp     every endpoint, driven through the in-process httplib stand-in
```

### Files
- `jobs.csv` - `job_no,opened,customer_id,vehicle_id,odometer,complaint,mechanic,status,closed,notes,invoice_no`
- `job_lines.csv` - `job_no,line_no,kind,part_id,description,quantity,unit_price,labor_fee,service_id`
- `services.csv` - `id,name,default_labor_fee,interval_months,interval_km,active`

The two columns beyond the spec (`invoice_no`, `service_id`) are appended at the end and are optional on
load: they are what Phase 5 (Job <-> Invoice) and Phase 6 (reminders) need, so no migration is required later.
`services.csv` is created on first start with **Change Oil** (3 months / 3,000 km), **Brake Pad Replacement**
(12 months / 10,000 km) and **Engine Overhaul** (36 months / 40,000 km); an Admin edits the fees and intervals.
All three files are covered by the normal start-up / *Backup now* copies (every `*.csv`) and are in `.gitignore`
(job orders hold customer data - never commit them).

### Status workflow
```
Open -> In progress -> Done -> Released        Cancelled (from Open or In progress)
        In progress -> Open                    Done -> In progress (reopen)
```
`Released` and `Cancelled` are final. Lines and job details can only be changed while the job is **Open** or
**In progress** - once it is **Done** it must be reopened first, and once it is **Released** nothing may change
again (every endpoint answers `409 conflict`). Every change writes one row to `audit.csv`
(`JOB_CREATED`, `JOB_UPDATED`, `JOB_MECHANIC`, `JOB_STATUS`, `JOB_LINE_ADDED`, `JOB_LINE_REMOVED`) with the
job number first in `details`, which is what `GET /api/jobs/<no>/history` returns.

### Stock (the rule that must never break)
- Adding a **Part** line deducts stock **immediately**: `parts.csv` is rewritten and one `JOB_ISSUE` row is
  appended to `movements.csv` with `ref = job_no` and the running `resulting_stock`.
- Removing that line appends a `JOB_RETURN` row (same `ref`) and puts the units back.
- Cancelling a job returns **every** part line in one batch.
- `quantity > available stock` is refused (`409`) before anything is written.
- Labour lines never touch stock. Billing a job (Phase 5) will not deduct again.
- Both the stock change and the paperwork are written in one all-or-nothing step: if `parts.csv` or
  `job_lines.csv` cannot be written, nothing changes and the caller gets an error.

Two new movement types are used, `JOB_ISSUE` and `JOB_RETURN`, so "parts fitted on a job" stays out of the
sales revenue figures (the invoice created in Phase 5 is what counts as revenue).

**Crash recovery.** `job_lines.csv` is the source of truth. At start-up `JobBook` compares what each job
should have issued with the `JOB_ISSUE` / `JOB_RETURN` rows in `movements.csv` and applies the difference
(logged as `JOB_STOCK_RECONCILED`). A write that was interrupted by a power cut therefore heals itself
instead of silently leaking stock - and it is idempotent, so a second start does nothing.

### Concurrency
The web layer serialises every request on one mutex, and `JobBook` additionally locks its own mutex, so the
job store is safe to call from several threads directly. `tests/jobs_test.cpp` hammers it with 40 simultaneous
"requests": every unit on the shelf is issued exactly once, the rest are refused, and the ledger matches.
Lock order is always *request mutex -> JobBook mutex*, never the reverse.

### REST API (JSON)
Sessions, roles and CSRF are the same as for the HTML pages: send the session cookie, and for `POST`/`DELETE`
the CSRF token either in the `X-CSRF-Token` header, as a `csrf` field, or inside the JSON body.

| Method | Path | Permission | What it does |
|---|---|---|---|
| GET | `/api/jobs` | Jobs | list; filters `status`, `mechanic`, `date`, `from`/`to`, `plate`, `q`, `customer_id`, `vehicle_id`, `limit`, `order` |
| POST | `/api/jobs` | Jobs | open a job (`customer_id`, `vehicle_id` **or** `plate`, `odometer`, `complaint`, `mechanic`, `notes`) -> `201` |
| GET | `/api/jobs/<no>` | Jobs | job + lines + totals |
| POST | `/api/jobs/<no>` | Jobs | edit complaint / odometer / notes / mechanic |
| POST | `/api/jobs/<no>/status` | Jobs | `{"status":"Done","note":"..."}` - only allowed transitions |
| POST | `/api/jobs/<no>/mechanic` | Jobs | assign / clear the mechanic |
| POST | `/api/jobs/<no>/cancel` | Jobs | cancel with a reason (returns every part) |
| GET | `/api/jobs/<no>/lines` | Jobs | lines + running totals |
| POST | `/api/jobs/<no>/lines` | Jobs | add `{"kind":"Part","part_id":101,"quantity":2}` or `{"kind":"Labor","service_id":1}` |
| DELETE | `/api/jobs/<no>/lines/<n>` | Jobs | remove a line and return the stock (also `POST .../lines/<n>/delete`) |
| GET | `/api/jobs/<no>/card` | Jobs | everything the printed job card needs |
| GET | `/api/jobs/<no>/history` | Jobs | the job's audit trail |
| GET | `/api/services` | Jobs | the catalogue (`?all=1` includes inactive) |
| POST | `/api/services` | **Admin** | add (`id` omitted) or edit a service |
| POST | `/api/services/<id>/active` | **Admin** | activate / deactivate |
| GET | `/api/parts` | ViewInventory | `?q=&limit=&in_stock=1` - picker for part lines |
| GET | `/api/vehicles` | Customers | `?q=&limit=` - plates for the new-job form |

Failures are `{"ok":false,"error":"...","code":"not_found|conflict|bad_request|forbidden|unauthorized","status":404}`.
Money is a number with two decimals (`450.00`). New permissions in `auth.cpp::allowed`: `Jobs` (Staff + Admin)
and `ServiceCatalog` (Admin).

The **Job orders** page in the menu is a working reference client: server-rendered body, and
`/static/jobs.js` calls the endpoints above with `fetch`. Replace it with your own TypeScript front-end when
you like - while it is served from another port, start the app with
`--cors-origin http://localhost:5173` (and `--allow-embedding` only if you also frame it).

### Tests
`make jobstest` (core rules) and `make apie2e` (endpoints). `make check` runs all four suites:
`test` (smoke), `jobstest`, `e2e` (HTML layer) and `apie2e`.
