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
  inventory.h/.cpp  parts.csv (incl. cost_price), movements.csv, invoices.csv, invoice voids, CSV import/export, backups
  reports.h/.cpp    top sellers (COGS, net profit, margin %), daily/weekly/monthly revenue & expenses, dead stock, reorder list
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
