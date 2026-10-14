#include "reports.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "util.h"

namespace reports {

namespace {
constexpr int kDay = 86400;

// Start of the bucket containing t, as a local-time tm.
std::tm bucketStart(std::time_t t, Period p) {
    std::tm tmv = util::localTime(t);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    if (p == Period::Weekly) tmv.tm_mday -= (tmv.tm_wday + 6) % 7;  // Monday
    if (p == Period::Monthly) tmv.tm_mday = 1;
    tmv.tm_isdst = -1;
    std::mktime(&tmv);  // normalise
    return tmv;
}

std::string label(const std::tm& tmv, Period p) {
    char b[32];
    std::strftime(b, sizeof b, p == Period::Monthly ? "%Y-%m" : "%Y-%m-%d", &tmv);
    return p == Period::Weekly ? std::string("Week of ") + b : std::string(b);
}

std::string keyFor(std::time_t t, Period p) { return label(bucketStart(t, p), p); }
}  // namespace

std::vector<TopSeller> topSelling(const std::vector<inv::Movement>& moves, int days, size_t limit, std::time_t now) {
    const std::time_t from = days > 0 ? now - static_cast<std::time_t>(days) * kDay : 0;
    std::map<int, TopSeller> by;
    for (const inv::Movement& m : moves) {
        if (m.type != inv::MoveType::Sale) continue;
        const auto t = util::parseStamp(m.timestamp);
        if (!t || *t < from) continue;
        TopSeller& s = by[m.partId];
        s.partId = m.partId;
        s.name = m.partName;  // latest name wins
        s.unitsSold += -m.qtyChanged;
        s.revenue += -m.qtyChanged * m.unitPrice;
        ++s.saleCount;
    }
    std::vector<TopSeller> out;
    for (auto& kv : by) {
        if (days > 0) kv.second.unitsPerDay = static_cast<double>(kv.second.unitsSold) / days;
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const TopSeller& a, const TopSeller& b) {
        if (a.unitsSold != b.unitsSold) return a.unitsSold > b.unitsSold;
        if (a.revenue != b.revenue) return a.revenue > b.revenue;
        return a.partId < b.partId;
    });
    if (limit > 0 && out.size() > limit) out.resize(limit);
    return out;
}

std::vector<PeriodTotals> revenueByPeriod(const std::vector<inv::Movement>& moves, const std::vector<inv::Invoice>& invoices, Period period,
                                          int count, std::time_t now) {
    std::vector<PeriodTotals> out;
    std::map<std::string, size_t> index;
    std::tm cur = bucketStart(now, period);
    for (int i = count - 1; i >= 0; --i) {  // build oldest -> newest
        std::tm t = cur;
        if (period == Period::Monthly) t.tm_mon -= i;
        else t.tm_mday -= i * (period == Period::Weekly ? 7 : 1);
        t.tm_isdst = -1;
        std::mktime(&t);
        PeriodTotals pt;
        pt.label = label(t, period);
        index[pt.label] = out.size();
        out.push_back(pt);
    }
    auto bucket = [&](const std::string& stamp) -> PeriodTotals* {
        const auto t = util::parseStamp(stamp);
        if (!t) return nullptr;
        auto it = index.find(keyFor(*t, period));
        return it == index.end() ? nullptr : &out[it->second];
    };
    for (const inv::Movement& m : moves) {
        if (m.type != inv::MoveType::Sale && m.type != inv::MoveType::Restock) continue;
        PeriodTotals* b = bucket(m.timestamp);
        if (!b) continue;
        if (m.type == inv::MoveType::Sale) b->partsRevenue += -m.qtyChanged * m.unitPrice;
        else b->expenses += m.qtyChanged * m.unitPrice;
    }
    for (const inv::Invoice& inv : invoices) {
        PeriodTotals* b = bucket(inv.timestamp);
        if (!b) continue;
        for (const inv::InvoiceLine& l : inv.lines)
            if (l.kind == inv::InvoiceLine::Kind::Service) b->serviceRevenue += l.total();
    }
    return out;
}

int restocksWithoutCost(const std::vector<inv::Movement>& moves) {
    int n = 0;
    for (const inv::Movement& m : moves)
        if (m.type == inv::MoveType::Restock && m.unitPrice == 0 && m.ref != "opening balance") ++n;
    return n;
}

std::vector<DeadStockItem> deadStock(const std::vector<inv::Part>& parts, const std::vector<inv::Movement>& moves, int days, std::time_t now) {
    std::map<int, std::time_t> last;
    for (const inv::Movement& m : moves) {
        if (m.type == inv::MoveType::Delete) continue;
        const auto t = util::parseStamp(m.timestamp);
        if (!t) continue;
        std::time_t& l = last[m.partId];
        l = std::max(l, *t);
    }
    std::vector<DeadStockItem> out;
    for (const inv::Part& p : parts) {
        if (p.quantity() <= 0) continue;
        auto it = last.find(p.id());
        if (it == last.end()) continue;  // cannot happen after start-up reconciliation; skip rather than guess
        const int idle = static_cast<int>((now - it->second) / kDay);
        if (idle < days) continue;
        out.push_back({&p, util::formatTime(it->second, "%Y-%m-%d %H:%M:%S"), idle, p.stockValue()});
    }
    std::sort(out.begin(), out.end(), [](const DeadStockItem& a, const DeadStockItem& b) { return a.daysIdle > b.daysIdle; });
    return out;
}

std::vector<ReorderItem> reorderList(const std::vector<inv::Part>& parts) {
    std::vector<ReorderItem> out;
    for (const inv::Part& p : parts)
        if (p.isLowStock()) out.push_back({&p, p.minStock() - p.quantity()});
    std::sort(out.begin(), out.end(), [](const ReorderItem& a, const ReorderItem& b) { return a.suggestedQty > b.suggestedQty; });
    return out;
}

// ================================================================ receivables
std::string customerKey(const std::string& name) {
    std::string out;
    bool space = false;
    for (unsigned char ch : name) {
        if (std::isspace(ch)) { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false;
        out += static_cast<char>(std::tolower(ch));
    }
    return out;
}

std::vector<CustomerBalance> receivablesByCustomer(const std::vector<inv::Invoice>& invoices, std::time_t now) {
    std::map<std::string, CustomerBalance> by;
    for (const inv::Invoice& i : invoices) {
        if (i.balance() <= 0) continue;
        CustomerBalance& c = by[customerKey(i.customer)];
        if (c.customer.empty()) c.customer = i.customer;
        ++c.openInvoices;
        c.billed = util::round2(c.billed + i.total());
        c.paid = util::round2(c.paid + i.amountPaid);
        c.balance = util::round2(c.balance + i.balance());
        const std::string day = i.timestamp.substr(0, 10);
        if (c.oldestDate.empty() || day < c.oldestDate) {
            c.oldestDate = day;
            const auto t = util::parseStamp(day + " 00:00:00");
            c.oldestDays = t ? std::max(0, static_cast<int>((now - *t) / kDay)) : 0;
        }
    }
    std::vector<CustomerBalance> out;
    for (auto& kv : by) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const CustomerBalance& a, const CustomerBalance& b) {
        if (a.balance != b.balance) return a.balance > b.balance;
        return customerKey(a.customer) < customerKey(b.customer);
    });
    return out;
}

double totalReceivable(const std::vector<inv::Invoice>& invoices) {
    double t = 0;
    for (const inv::Invoice& i : invoices) t = util::round2(t + i.balance());
    return t;
}

int openInvoiceCount(const std::vector<inv::Invoice>& invoices) {
    int n = 0;
    for (const inv::Invoice& i : invoices) if (i.balance() > 0) ++n;
    return n;
}

// ================================================================ daily cash report
bool validDate(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) if (!std::isdigit(static_cast<unsigned char>(date[i]))) return false;
    const int y = std::atoi(date.substr(0, 4).c_str()), m = std::atoi(date.substr(5, 2).c_str()), d = std::atoi(date.substr(8, 2).c_str());
    if (y < 1970 || m < 1 || m > 12 || d < 1) return false;   // (mktime would silently roll 2026-02-30 into March)
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= days[m - 1] + (m == 2 && leap ? 1 : 0);
}

DailyCashReport dailyCashReport(const std::vector<inv::Invoice>& invoices, const std::vector<inv::Payment>& payments, const std::string& date) {
    DailyCashReport r;
    r.date = date;
    auto onDay = [&](const std::string& stamp) { return stamp.compare(0, 10, date) == 0; };
    auto upToDay = [&](const std::string& stamp) { return stamp.substr(0, 10) <= date; };

    std::map<std::string, const inv::Invoice*> todays;
    for (const inv::Invoice& i : invoices) {
        if (!onDay(i.timestamp)) continue;
        todays[i.number] = &i;
        r.invoices.push_back(&i);
        ++r.invoiceCount;
        r.grossSales = util::round2(r.grossSales + i.total());
        for (const inv::InvoiceLine& l : i.lines) {
            if (l.kind == inv::InvoiceLine::Kind::Service) r.serviceSales = util::round2(r.serviceSales + l.total());
            else r.partsSales = util::round2(r.partsSales + l.total());
        }
    }

    std::map<std::string, MethodTotal> methods;
    std::map<std::string, double> paidByEndOfDay;   // for today's invoices
    for (const inv::Payment& p : payments) {
        if (todays.count(p.invoiceNo) && upToDay(p.timestamp)) paidByEndOfDay[p.invoiceNo] = util::round2(paidByEndOfDay[p.invoiceNo] + p.amount);
        if (!onDay(p.timestamp)) continue;
        r.payments.push_back(&p);
        r.collected = util::round2(r.collected + p.amount);
        if (todays.count(p.invoiceNo)) r.collectedOnToday = util::round2(r.collectedOnToday + p.amount);
        else r.collectedOnOlder = util::round2(r.collectedOnOlder + p.amount);
        MethodTotal& m = methods[p.method];
        m.method = p.method;
        ++m.count;
        m.amount = util::round2(m.amount + p.amount);
    }
    for (const inv::Invoice* i : r.invoices) {
        const double total = util::round2(i->total()), paid = std::min(total, paidByEndOfDay[i->number]);
        const double owing = util::round2(total - paid);
        r.creditExtended = util::round2(r.creditExtended + owing);
        if (owing <= 0) ++r.paidCount; else if (paid <= 0) ++r.unpaidCount; else ++r.partialCount;
    }
    for (auto& kv : methods) r.byMethod.push_back(kv.second);
    std::sort(r.byMethod.begin(), r.byMethod.end(), [](const MethodTotal& a, const MethodTotal& b) {
        if (a.amount != b.amount) return a.amount > b.amount;
        return a.method < b.method;
    });
    std::stable_sort(r.payments.begin(), r.payments.end(), [](const inv::Payment* a, const inv::Payment* b) { return a->timestamp < b->timestamp; });
    std::stable_sort(r.invoices.begin(), r.invoices.end(), [](const inv::Invoice* a, const inv::Invoice* b) { return a->timestamp < b->timestamp; });
    r.outstandingNow = totalReceivable(invoices);
    r.customersOwing = static_cast<int>(receivablesByCustomer(invoices, std::time(nullptr)).size());
    return r;
}

}  // namespace reports
