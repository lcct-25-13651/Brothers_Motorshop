// reports.h - read-only analytics over parts, movements.csv and invoices.csv.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "inventory.h"

namespace reports {

// ---- Top-selling / fast-moving parts ----
struct TopSeller {
    int partId = 0;
    std::string name;
    long long unitsSold = 0;
    double revenue = 0;
    int saleCount = 0;       // number of SALE movements
    double unitsPerDay = 0;  // velocity over the window
};
// days = 0 -> all time. Sorted by units sold (then revenue), at most `limit` rows (0 = all).
std::vector<TopSeller> topSelling(const std::vector<inv::Movement>& moves, int days, size_t limit, std::time_t now);

// ---- Revenue & expenses ----
enum class Period { Daily, Weekly, Monthly };
struct PeriodTotals {
    std::string label;          // "2026-10-12", "Week of 2026-10-05", "2026-10"
    double partsRevenue = 0;    // SALE movements x selling price
    double serviceRevenue = 0;  // SERVICE lines on invoices
    double expenses = 0;        // RESTOCK movements x unit cost
    double revenue() const { return partsRevenue + serviceRevenue; }
    double net() const { return revenue() - expenses; }
};
// Oldest -> newest, always `count` buckets ending with the current period (empty ones are zero).
std::vector<PeriodTotals> revenueByPeriod(const std::vector<inv::Movement>& moves, const std::vector<inv::Invoice>& invoices,
                                          Period period, int count, std::time_t now);
int restocksWithoutCost(const std::vector<inv::Movement>& moves);  // excluded from expenses (cost unknown)

// ---- Dead stock ----
struct DeadStockItem {
    const inv::Part* part = nullptr;
    std::string lastMovement;  // timestamp of the last RESTOCK/SALE/ADJUSTMENT
    int daysIdle = 0;
    double stockValue = 0;     // quantity x price tied up
};
// Parts that still have stock but no movement for >= days. Most idle first.
std::vector<DeadStockItem> deadStock(const std::vector<inv::Part>& parts, const std::vector<inv::Movement>& moves, int days, std::time_t now);

// ---- Reorder list ----
struct ReorderItem {
    const inv::Part* part = nullptr;
    int suggestedQty = 0;  // minStock - quantity: brings the part back up to its minimum
};
std::vector<ReorderItem> reorderList(const std::vector<inv::Part>& parts);  // most short first

}  // namespace reports
