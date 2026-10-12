# RE Parts Tracker
A Bajaj RE Components and Inventory Management System in C++ (console / CLI).

100% standard C++17, no external libraries.

## Build (Windows, MSYS2 UCRT64 g++)
    g++ -std=c++17 -Wall -Wextra main.cpp -o re_parts_tracker.exe

## Run
    .\re_parts_tracker.exe

Run it from the project folder so it finds `parts.csv`. If the file is missing, sample data is created.

## Features
- Create / Read / Update / Delete parts (ID, name, vehicle system, price, quantity, minimum stock)
- Case-insensitive search by ID, name, or vehicle system; browse by system
- Low-stock alert on launch and after every stock change
- Auto-save to `parts.csv` after every change; auto-load on startup
- Reads the older 4-column parts.csv format too
