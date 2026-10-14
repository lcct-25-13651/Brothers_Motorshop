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
  web.h/.cpp        all HTTP routes + HTML views   (web_assets.h = CSS/JS served at /static)
  main.cpp          start-up: backup -> load -> serve
tests/              smoke_test.cpp (core logic), web_e2e.cpp + stub/ (web layer, no sockets)
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

Tests: `make test` (core logic) and `make e2e` (web layer vs. an in-process stand-in for httplib).

## Run
```
./re_parts_tracker [--port 8080] [--host 127.0.0.1] [--data <folder>] [--shop "Shop name"] [--no-backup]
```
Open http://localhost:8080. First start asks you to create the Admin account (an existing v2 `users.csv` is read as-is: its account becomes Admin).
Use `--host 0.0.0.0` so other PCs on the shop network can connect (the app speaks plain HTTP: keep it on a trusted network).

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
