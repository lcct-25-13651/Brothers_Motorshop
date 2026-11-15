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

// ---- Receivables ("utang") ----
struct CustomerBalance {
    std::string customer;
    int openInvoices = 0;
    double billed = 0, paid = 0, balance = 0;   // over the customer's OPEN invoices only
    std::string oldestDate;                      // date of the oldest open invoice (YYYY-MM-DD)
    int oldestDays = 0;                          // its age in days
};
// Customers who owe money, largest balance first. Names are grouped ignoring case/extra spaces.
std::vector<CustomerBalance> receivablesByCustomer(const std::vector<inv::Invoice>& invoices, std::time_t now);
double totalReceivable(const std::vector<inv::Invoice>& invoices);
int openInvoiceCount(const std::vector<inv::Invoice>& invoices);
std::string customerKey(const std::string& name);   // grouping key (lower-case, single spaces)

// ---- End-of-day cashier summary ----
struct MethodTotal { std::string method; int count = 0; double amount = 0; };
struct DailyCashReport {
    std::string date;                       // YYYY-MM-DD
    int invoiceCount = 0;                   // invoices created that day
    int paidCount = 0, partialCount = 0, unpaidCount = 0;   // their status at the END of that day
    double grossSales = 0;                  // total of those invoices (parts + services)
    double partsSales = 0, serviceSales = 0;
    double collected = 0;                   // every payment received that day = net cash collected
    double collectedOnToday = 0;            // ...of which on invoices made that same day
    double collectedOnOlder = 0;            // ...of which on earlier credit invoices (utang payments)
    double creditExtended = 0;              // unpaid part of that day's invoices at close of day
    double outstandingNow = 0;              // all customers' open balances right now (not date dependent)
    int customersOwing = 0;
    std::vector<MethodTotal> byMethod;      // only methods that were used, biggest first
    std::vector<const inv::Invoice*> invoices;   // that day's invoices, oldest first
    std::vector<const inv::Payment*> payments;   // that day's payments, oldest first
};
// Valid "YYYY-MM-DD" -> true.
bool validDate(const std::string& date);
DailyCashReport dailyCashReport(const std::vector<inv::Invoice>& invoices, const std::vector<inv::Payment>& payments, const std::string& date);

}  // namespace reports
