#include "util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace util {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool containsIgnoreCase(const std::string& hay, const std::string& needle) {
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) { return toLower(a) == toLower(b); }

std::string oneLine(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string money(double v) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << v;
    return os.str();
}

std::string moneyFmt(double v) {
    std::string s = money(v);
    const bool neg = !s.empty() && s[0] == '-';
    if (neg) s.erase(0, 1);
    const size_t dot = s.find('.');
    const std::string ip = s.substr(0, dot), frac = dot == std::string::npos ? "" : s.substr(dot);
    std::string out;
    int count = 0;
    for (size_t i = ip.size(); i-- > 0;) {
        out.insert(out.begin(), ip[i]);
        if (++count % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return (neg ? "-" : "") + out + frac;
}

std::string htmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

bool parseInt(const std::string& s, long long lo, long long hi, long long& out) {
    try {
        const std::string t = trim(s);
        size_t pos = 0;
        const long long v = std::stoll(t, &pos);
        if (t.empty() || pos != t.size() || v < lo || v > hi) return false;
        out = v;
        return true;
    } catch (const std::exception&) { return false; }
}

bool parseDouble(const std::string& s, double lo, double hi, double& out) {
    try {
        const std::string t = trim(s);
        size_t pos = 0;
        const double v = std::stod(t, &pos);
        if (t.empty() || pos != t.size() || !std::isfinite(v) || v < lo || v > hi) return false;
        out = v;
        return true;
    } catch (const std::exception&) { return false; }
}

// ---------------- CSV ----------------
std::string csvEscape(const std::string& f) {
    if (f.find_first_of(",\"\n\r") == std::string::npos) return f;
    std::string o = "\"";
    for (char c : f) { if (c == '"') o += "\"\""; else o += c; }
    return o + '"';
}

std::string csvSafe(const std::string& f) {
    if (!f.empty() && (f[0] == '=' || f[0] == '+' || f[0] == '-' || f[0] == '@' || f[0] == '\t'))
        return csvEscape("'" + f);
    return csvEscape(f);
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else q = false;
            } else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

std::vector<std::vector<std::string>> parseCsvText(const std::string& textIn) {
    std::string text = textIn;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    std::vector<std::vector<std::string>> rows;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        rows.push_back(splitCsv(line));
    }
    return rows;
}

std::vector<std::vector<std::string>> readCsvFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseCsvText(ss.str());
}

// ---------------- time ----------------
std::tm localTime(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::string formatTime(std::time_t t, const char* fmt) {
    const std::tm tmv = localTime(t);
    char buf[64] = "";
    std::strftime(buf, sizeof buf, fmt, &tmv);
    return buf;
}

std::string nowStamp() { return formatTime(std::time(nullptr), "%Y-%m-%d %H:%M:%S"); }
std::string nowCompact() { return formatTime(std::time(nullptr), "%Y%m%d_%H%M%S"); }

std::optional<std::time_t> parseStamp(const std::string& s) {
    std::tm tmv{};
    int y, mo, d, h, mi, se;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return std::nullopt;
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se; tmv.tm_isdst = -1;
    const std::time_t t = std::mktime(&tmv);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return t;
}

// ---------------- files ----------------
bool writeFileAtomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << content;
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {  // Windows cannot rename over an existing file on some toolchains
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

bool appendLine(const std::string& path, const std::string& header, const std::string& line) {
    std::error_code ec;
    const bool needHeader = !fs::exists(path, ec) || fs::file_size(path, ec) == 0;
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) return false;
    if (needHeader && !header.empty()) out << header << '\n';
    out << line << '\n';
    out.flush();
    return static_cast<bool>(out);
}

}  // namespace util
