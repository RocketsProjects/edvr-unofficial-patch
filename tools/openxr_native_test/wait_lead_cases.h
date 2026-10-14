#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include "../../src/openxr/wait_lead_stats.h"

// Cases for src/openxr/wait_lead_stats.h (native_wait_lead and native_long_cycle_1x). The runtime clock is fake: an
// XrTime is the QPC-us instant times 1000 plus an offset that is not the QPC base, and the conversion inverts it, as
// xrConvertTimeToWin32PerformanceCounterKHR does for the host.
namespace edvr::openxr::test {
namespace waitlead_detail {
constexpr int64_t kPeriodUs = 11111;
constexpr int64_t kOffsetNs = 5000000000LL;
constexpr int64_t kBase = 1000000; // us, the fake QPC origin of the first wait

inline bool fakeConvert(int64_t ns, int64_t& us) {
  us = (ns - kOffsetNs) / 1000;
  return us > 0;
}
// A game wait returning at returnUs, predicting a display at returnUs + leadUs, and an xrEndFrame call at
// returnUs + workUs (workUs < 0: no end this frame).
inline void game(WaitLeadStats& s, int64_t returnUs, int64_t leadUs, int64_t workUs) {
  const int64_t predictedNs = (returnUs + leadUs) * 1000 + kOffsetNs;
  int64_t lead = 0;
  const bool known = measureWaitLeadUs(fakeConvert, predictedNs, returnUs, lead);
  s.noteWait(returnUs, known, lead, predictedNs, kPeriodUs * 1000);
  if (workUs >= 0) s.noteEndCall(returnUs + workUs);
}
// A wait whose predicted display time did not convert: the lead is unknown, the prediction still drives late.
inline void gameNoLead(WaitLeadStats& s, int64_t returnUs, int64_t predictedUs, int64_t workUs) {
  s.noteWait(returnUs, false, 0, predictedUs * 1000 + kOffsetNs, kPeriodUs * 1000);
  if (workUs >= 0) s.noteEndCall(returnUs + workUs);
}
inline WaitLeadStats::Report takeReport(WaitLeadStats& s, uint64_t window) {
  WaitLeadStats::Report r{};
  s.takeWindow(r);
  r.window = window;
  return r;
}
// key=value fields of one trace line, keyed by name; the first comma field is kept as "_name".
inline std::map<std::string, std::string> parseFields(const char* line) {
  std::map<std::string, std::string> out;
  std::string s(line);
  if (!s.empty() && s.back() == '\n') s.pop_back();
  size_t pos = 0;
  bool first = true;
  while (pos <= s.size()) {
    size_t comma = s.find(',', pos);
    if (comma == std::string::npos) comma = s.size();
    const std::string token = s.substr(pos, comma - pos);
    if (first) { out["_name"] = token; first = false; }
    else {
      const size_t eq = token.find('=');
      if (eq != std::string::npos) out[token.substr(0, eq)] = token.substr(eq + 1);
    }
    pos = comma + 1;
  }
  return out;
}
// A value "a/b/c" with three numbers, all parsing as numbers.
inline bool numericTriple(const std::string& v) {
  int slashes = 0;
  for (char c : v) if (c == '/') ++slashes;
  if (slashes != 2) return false;
  size_t start = 0;
  for (int i = 0; i < 3; ++i) {
    const size_t slash = v.find('/', start);
    const std::string part = v.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    char* end = nullptr;
    std::strtod(part.c_str(), &end);
    if (part.empty() || end == part.c_str() || *end != '\0') return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}
} // namespace waitlead_detail

template<class Check> void runWaitLeadCases(Check check) {
  using namespace waitlead_detail;
  using S = WaitLeadStats;
  const int64_t P = kPeriodUs;

  // Steady: on-time predictions, lead 8 ms, work 1 ms.
  S steady;
  for (int64_t k = 0; k < 40; ++k) game(steady, kBase + k * P, 8000, 1000);
  const S::Report steadyR = takeReport(steady, 7);
  check(steadyR.frames == 39 && steadyR.late == 0 && steadyR.lateShortLead == 0 && steadyR.lateLongWork == 0,
    "wait-lead steady: 39 frames commit (the newest waits pending), none late");
  check(steadyR.leadValid && std::fabs(steadyR.leadP50Ms - 8.0) < 1e-9 && std::fabs(steadyR.leadP5Ms - 8.0) < 1e-9 &&
    std::fabs(steadyR.leadP95Ms - 8.0) < 1e-9, "wait-lead steady: lead read through the fake runtime clock");
  check(steadyR.workValid && std::fabs(steadyR.workP50Ms - 1.0) < 1e-9, "wait-lead steady: work is wait return to xrEndFrame call");
  check(std::fabs(steadyR.periodMs - 11.111) < 1e-9, "wait-lead steady: period_ms from the frame's own predicted period");
  check(!steadyR.cycleValid && steadyR.long1x == 0, "wait-lead steady: no census cycle at one period");
  check(takeReport(steady, 8).frames == 0, "wait-lead: a window taken clears the committed frames");

  // A short-lead, short-work frame that is late: lead 1 ms < period - W50 (10.111 ms), work 0.5 ms <= lead.
  S shortLate;
  for (int64_t k = 0; k < 30; ++k) game(shortLate, kBase + k * P, 8000, 1000);
  game(shortLate, kBase + 30 * P, 1000, 500);     // A
  game(shortLate, kBase + 31 * P, 8000, 1000);    // predicted display jumps 1.63 periods after A's: A is late
  game(shortLate, kBase + 32 * P, 8000, 1000);
  game(shortLate, kBase + 33 * P, 8000, 1000);
  const S::Report shortR = takeReport(shortLate, 9);
  check(shortR.late == 1 && shortR.lateShortLead == 1 && shortR.lateLongWork == 0,
    "wait-lead: a late frame with short lead and short work counts as late_short_lead, not late_long_work");

  // A long-lead, long-work frame that is late: lead 10.5 ms (not short), work 11 ms > lead.
  S longLate;
  for (int64_t k = 0; k < 30; ++k) game(longLate, kBase + k * P, 8000, 1000);
  game(longLate, kBase + 30 * P, 10500, 11000);   // B
  game(longLate, kBase + 31 * P, 16100, 1000);    // B's successor predicts 1.50 periods after: late
  game(longLate, kBase + 32 * P, 16100, 1000);
  const S::Report longR = takeReport(longLate, 9);
  check(longR.late == 1 && longR.lateLongWork == 1 && longR.lateShortLead == 0,
    "wait-lead: a late frame with long lead and long work counts as late_long_work, not late_short_lead");

  // Jitter: a 1.2-period predicted step is not a miss; a 1.6-period step is one, attributed to the frame before it.
  S jitter;
  game(jitter, kBase, 8000, 1000);
  game(jitter, kBase + P, 10222, 1000);           // predicted step 1.2 periods
  game(jitter, kBase + 2 * P, 16889, 1000);       // predicted step 1.6 periods: frame 1 is late
  game(jitter, kBase + 3 * P, 16889, 1000);
  const S::Report jitterR = takeReport(jitter, 1);
  check(jitterR.frames == 3 && jitterR.late == 1, "wait-lead: sub-period jitter is not late, a 1.6-period step is one miss");

  // Census: cycles strictly between period+0.5 ms and 2 periods; the split of their work is by one period.
  S census;
  const int64_t cycles[] = {P, P + 400, P + 1000, P + 1389, 2 * P + 2778, P + 500};
  const int64_t works[] = {1000, 1000, 1000, 12000, 1000, 1000};
  int64_t at = kBase;
  for (unsigned k = 0; k < 6; ++k) { game(census, at, 8000, works[k]); at += cycles[k]; }
  game(census, at, 8000, 1000);
  const S::Report censusR = takeReport(census, 3);
  check(censusR.long1x == 2 && censusR.long1xWorkGePeriod == 1 && censusR.long1xWorkLtPeriod == 1,
    "wait-lead census: cycles in (1x+0.5 ms, 2x) counted, split by work against one period");
  check(censusR.cycleValid && std::fabs(censusR.long1xCycleP50Ms - 12.111) < 1e-9 && censusR.workValid && censusR.long1xWorkValid &&
    std::fabs(censusR.long1xWorkP50Ms - 1.0) < 1e-9, "wait-lead census: cycle and work p50 of the counted cycles");

  // Pending frame: one wait commits nothing; no open frame: an end is ignored; no end: work is n/a.
  S pending;
  pending.noteEndCall(5);
  pending.noteWait(kBase, true, 8000, (kBase + 8000) * 1000 + kOffsetNs, kPeriodUs * 1000);
  check(takeReport(pending, 1).frames == 0, "wait-lead: the first wait commits nothing (its late is not yet known)");
  game(pending, kBase + P, 8000, -1);
  const S::Report noEnd = takeReport(pending, 2);
  check(noEnd.frames == 1 && !noEnd.workValid && noEnd.leadValid, "wait-lead: an end before any wait is ignored; a frame with no end has no work");

  // Lead unknown (no convert): late still counts, lead is n/a, no split is claimed.
  S unknown;
  for (int64_t k = 0; k < 20; ++k) gameNoLead(unknown, kBase + k * P, kBase + k * P + 8000, 1000);
  gameNoLead(unknown, kBase + 20 * P, kBase + 20 * P + 8000 + 2 * P, 1000);
  gameNoLead(unknown, kBase + 21 * P, kBase + 21 * P + 8000, 1000);
  const S::Report unknownR = takeReport(unknown, 4);
  check(unknownR.late == 1 && !unknownR.leadValid && unknownR.lateShortLead == 0 && unknownR.lateLongWork == 0,
    "wait-lead: an unconverted lead still counts late, and claims no lead split");

  // Capacity: 8300 waits commit 8299 frames; 8192 are kept and 107 counted as dropped.
  S full;
  for (int64_t k = 0; k < 8300; ++k) game(full, kBase + k * P, 8000, 1000);
  const S::Report fullR = takeReport(full, 5);
  check(fullR.frames == 8192 && fullR.dropped == 107, "wait-lead: a full window keeps its capacity and counts the rest as dropped");

  // Empty window: every value n/a.
  S empty;
  const S::Report emptyR = takeReport(empty, 6);
  char emptyLine[256];
  const int emptyLen = formatWaitLeadLine(emptyR, emptyLine, sizeof emptyLine);
  check(emptyLen > 0 && std::string(emptyLine).find("lead_p5/p50/p95=na/na/na,work_p50/p95/p99=na/na/na,late=0,") != std::string::npos,
    "wait-lead: an empty window prints na for each percentile");

  // Format: the steady window's line parses, with every field named and numeric where it should be.
  char line[256];
  check(formatWaitLeadLine(steadyR, line, sizeof line) > 0, "wait-lead: line fits its buffer");
  const auto f = parseFields(line);
  check(f.count("_name") && f.at("_name") == "native_wait_lead" && f.at("window") == "7",
    "wait-lead line: name and window parse");
  check(f.count("lead_p5/p50/p95") && numericTriple(f.at("lead_p5/p50/p95")) && f.at("lead_p5/p50/p95") == "8.000/8.000/8.000",
    "wait-lead line: lead triple parses as three numbers");
  check(f.count("work_p50/p95/p99") && numericTriple(f.at("work_p50/p95/p99")), "wait-lead line: work triple parses as three numbers");
  check(f.count("late") && f.at("late") == "0" && f.at("late_short_lead") == "0" && f.at("late_long_work") == "0" &&
    f.count("period_ms") && f.at("period_ms") == "11.111" && f.at("frames") == "39" && f.at("dropped") == "0",
    "wait-lead line: counts and period parse");
  char censusLine[256];
  check(formatLongCycle1xLine(censusR, censusLine, sizeof censusLine) > 0, "wait-lead census: line fits its buffer");
  const auto c = parseFields(censusLine);
  check(c.count("_name") && c.at("_name") == "native_long_cycle_1x" && c.at("window") == "3" && c.at("count") == "2" &&
    c.at("work_ge_period") == "1" && c.at("work_lt_period") == "1" && c.at("cycle_p50") == "12.111" && c.at("work_p50") == "1.000",
    "wait-lead census line: fields parse");
  const auto e = parseFields(emptyLine);
  check(e.count("lead_p5/p50/p95") && e.at("lead_p5/p50/p95") == "na/na/na", "wait-lead line: an empty window's na triple parses");
}
} // namespace edvr::openxr::test
