# RE Parts Tracker
A Bajaj RE Components and Inventory Management System in C++.

100% standard C++17, no external libraries. Works as a console (CLI) app **and** as a web dashboard in Chrome.

## Build (Windows, MSYS2 UCRT64 g++)
    g++ -std=c++17 -Wall -Wextra main.cpp -o re_parts_tracker.exe -lws2_32

## Run
    .\re_parts_tracker.exe          (console menu; option 10 opens Chrome)
    .\re_parts_tracker.exe web      (starts the dashboard and opens Chrome directly)

The dashboard runs at http://localhost:8080 (local computer only). Keep the terminal open; press Ctrl+C to stop.
Run from the project folder so `parts.csv` is found. If it is missing, sample data is created.

## Features
- Create / Read / Update / Delete parts (ID, name, vehicle system, price, quantity, minimum stock)
- Case-insensitive search by ID, name, or vehicle system; filter by system
- Low-stock alert on launch and after every stock change
- Auto-save to `parts.csv` after every change; auto-load on startup
