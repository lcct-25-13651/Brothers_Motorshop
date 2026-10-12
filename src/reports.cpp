#include "reports.h"

#include <algorithm>
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

}  // namespace reports
