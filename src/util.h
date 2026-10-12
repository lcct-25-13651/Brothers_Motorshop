// util.h - small helpers shared by every module (strings, CSV, time, files).
#pragma once
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace util {

// ---- strings ----
std::string trim(const std::string& s);
std::string toLower(std::string s);
bool containsIgnoreCase(const std::string& hay, const std::string& needle);
bool equalsIgnoreCase(const std::string& a, const std::string& b);
std::string oneLine(std::string s);              // newlines -> spaces (1 record = 1 line)
std::string money(double v);                     // "1234.50" (CSV form)
std::string moneyFmt(double v);                  // "1,234.50" (display form)
std::string htmlEscape(const std::string& s);
std::string urlEncode(const std::string& s);

// Strict number parsing: whole string must be a number inside [lo, hi].
bool parseInt(const std::string& s, long long lo, long long hi, long long& out);
bool parseDouble(const std::string& s, double lo, double hi, double& out);

// ---- CSV (RFC-4180 style, one record per line) ----
std::string csvEscape(const std::string& field);
// Escape for *export*: additionally neutralises spreadsheet formula injection (=, +, -, @).
std::string csvSafe(const std::string& field);
std::vector<std::string> splitCsv(const std::string& line);
// Reads a whole CSV file into rows (BOM / CRLF tolerant, blank lines skipped).
std::vector<std::vector<std::string>> readCsvFile(const std::string& path);
std::vector<std::vector<std::string>> parseCsvText(const std::string& text);

// ---- time (local time, thread-safe) ----
std::tm localTime(std::time_t t);
std::string formatTime(std::time_t t, const char* fmt);
std::string nowStamp();                                  // "YYYY-MM-DD HH:MM:SS"
std::string nowCompact();                                // "YYYYMMDD_HHMMSS"
std::optional<std::time_t> parseStamp(const std::string& s);  // inverse of nowStamp()

// ---- files ----
bool writeFileAtomic(const std::string& path, const std::string& content);  // tmp + rename
bool appendLine(const std::string& path, const std::string& header, const std::string& line);

}  // namespace util
