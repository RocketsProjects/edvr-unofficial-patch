#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace edvr::openxr {

// Wait lead and frame work per 30 s window: the pacing question of
// docs/render-thread-cpu-2026-10-10.md (H1 work tips the frame over, H2 the
// runtime starts the frame late). Pure arithmetic over microsecond instants the
// caller supplies, so tools\openxr_native_test drives it with a fake runtime
// clock. The host supplies the QPC-microsecond clock (edvrNativeTraceUs) and
// converts XrTime with xrConvertTimeToWin32PerformanceCounterKHR.
//
// Per game frame N, the frame a real xrWaitFrame returned for (not a loading
// frame, not a synthesized deferred frame):
//   lead_N   = predicted display time of N, on the QPC-us clock, minus the
//              instant that wait returned. Negative when the predicted time is
//              already past at the return.
//   work_N   = the start of N's xrEndFrame call minus the instant that wait
//              returned (game work, Submit, and the frame-end work before the call).
//   cycle_N  = the next game wait's return minus this wait's return.
//   late_N   = the next game wait's predicted display time skipped at least one
//              period past N's: round((pred_next - pred_N) / period) - 1 >= 1.
//              This is the arithmetic of FrameBoundary::noteReal (the existing
//              late_frames counter), attributed to the frame it is about. It
//              counts frames, where late_frames counts skipped periods.
// N is committed when the next game wait returns, since only then is late_N
// known. The newest frame stays pending and joins the next window.
//
// Classification, per window. W50 is the work p50 over the window's frames with
// a known work value. Lead is measured in us, period in us.
//   late_short_lead  late_N and lead_N < period_N - W50   ("the runtime gave too little time")
//   late_long_work   late_N and work_N > lead_N           ("the frame's own work overran its lead")
// The two are not exclusive: one late frame can count in both. A late frame
// whose lead could not be converted counts in late and in neither split.
//
// Long-cycle census (native_long_cycle_1x): cycles with
//   period_N + 0.5 ms < cycle_N < 2 * period_N
// with their count, the cycle p50, the work p50 of those frames, and the split
//   work_ge_period  work_N >= period_N   (the frame's own work spanned a period: H1)
//   work_lt_period  work_N <  period_N   (the cycle ran long with its own work
//                                        under a period: the time went to the
//                                        wait or the gap after xrEndFrame: H2)
// Cycles of 2 * period or more stay with native_long_cycle.
class WaitLeadStats final {
 public:
  static constexpr unsigned capacity = 8192; // about 90 s of 90 Hz frames; a fuller window counts `dropped`

  struct Report {
    uint64_t window = 0;
    unsigned frames = 0, dropped = 0;
    bool leadValid = false, workValid = false, cycleValid = false;
    double leadP5Ms = 0, leadP50Ms = 0, leadP95Ms = 0;
    double workP50Ms = 0, workP95Ms = 0, workP99Ms = 0;
    unsigned late = 0, lateShortLead = 0, lateLongWork = 0;
    double periodMs = 0;
    unsigned long1x = 0, long1xWorkGePeriod = 0, long1xWorkLtPeriod = 0;
    double long1xCycleP50Ms = 0, long1xWorkP50Ms = 0;
    bool long1xCycleValid = false, long1xWorkValid = false;
  };

  // A real game wait returned at returnUs. leadKnown is false when the predicted
  // time could not be converted; then leadUs is ignored. predictedNs and periodNs
  // are the frame's XrTime values (nanoseconds). Commits the previous frame.
  void noteWait(int64_t returnUs, bool leadKnown, int64_t leadUs, int64_t predictedNs, int64_t periodNs) {
    if (pending_.open) commit(returnUs, predictedNs, periodNs);
    pending_ = Pending{};
    pending_.open = true;
    pending_.returnUs = returnUs;
    pending_.leadUs = leadUs;
    pending_.leadValid = leadKnown;
    pending_.predictedNs = predictedNs;
    pending_.periodNs = periodNs;
  }

  // The xrEndFrame call of the open frame began at callUs. Only the first end
  // after its wait counts; later ends and ends with no open frame are ignored.
  void noteEndCall(int64_t callUs) {
    if (!pending_.open || pending_.workValid) return;
    pending_.workUs = callUs - pending_.returnUs;
    pending_.workValid = true;
  }

  // Fills out with the window's figures and clears the committed frames. The
  // pending frame is kept. out.window is the caller's; it is not set here.
  void takeWindow(Report& out) {
    out = Report{};
    out.frames = unsigned(records_.size());
    out.dropped = dropped_;
    std::vector<int64_t> leads, works, cycles, census, censusWork;
    for (const Record& r : records_) {
      if (r.leadValid) leads.push_back(r.leadUs);
      if (r.workValid) works.push_back(r.workUs);
    }
    if (!leads.empty()) {
      out.leadValid = true;
      out.leadP5Ms = ms(pick(leads, 5)); out.leadP50Ms = ms(pick(leads, 50)); out.leadP95Ms = ms(pick(leads, 95));
    }
    int64_t w50 = 0;
    if (!works.empty()) {
      out.workValid = true;
      w50 = pick(works, 50);
      out.workP50Ms = ms(w50); out.workP95Ms = ms(pick(works, 95)); out.workP99Ms = ms(pick(works, 99));
    }
    if (!records_.empty()) out.periodMs = ms(records_.back().periodUs);
    for (const Record& r : records_) {
      if (r.late) {
        ++out.late;
        if (r.leadValid && !works.empty() && r.leadUs < r.periodUs - w50) ++out.lateShortLead;
        if (r.leadValid && r.workValid && r.workUs > r.leadUs) ++out.lateLongWork;
      }
      if (r.cycleUs > r.periodUs + 500 && r.cycleUs < 2 * r.periodUs) {
        ++out.long1x;
        cycles.push_back(r.cycleUs);
        if (r.workValid) {
          censusWork.push_back(r.workUs);
          if (r.workUs >= r.periodUs) ++out.long1xWorkGePeriod; else ++out.long1xWorkLtPeriod;
        }
      }
    }
    if (!cycles.empty()) { out.cycleValid = true; out.long1xCycleP50Ms = ms(pick(cycles, 50)); }
    if (!censusWork.empty()) { out.long1xWorkValid = true; out.long1xWorkP50Ms = ms(pick(censusWork, 50)); }
    records_.clear();
    dropped_ = 0;
  }

  // Frames committed in the window so far (the pending one is not counted).
  size_t committed() const { return records_.size(); }

 private:
  struct Pending {
    bool open = false;
    int64_t returnUs = 0, leadUs = 0, predictedNs = 0, periodNs = 0, workUs = 0;
    bool leadValid = false, workValid = false;
  };
  struct Record {
    int64_t leadUs = 0, workUs = 0, cycleUs = 0, periodUs = 0;
    bool leadValid = false, workValid = false, late = false;
  };

  void commit(int64_t nextReturnUs, int64_t nextPredictedNs, int64_t nextPeriodNs) {
    Record r;
    r.leadUs = pending_.leadUs;
    r.leadValid = pending_.leadValid;
    r.workUs = pending_.workUs;
    r.workValid = pending_.workValid;
    r.cycleUs = nextReturnUs - pending_.returnUs;
    r.periodUs = pending_.periodNs / 1000;
    if (pending_.predictedNs > 0 && nextPredictedNs > 0 && nextPeriodNs > 0) {
      long long missed = std::llround(double(nextPredictedNs - pending_.predictedNs) / double(nextPeriodNs)) - 1;
      r.late = missed >= 1;
    }
    if (records_.size() < capacity) records_.push_back(r); else ++dropped_;
  }

  // Nearest rank, as FrameCycleStats::dist: v[(n*pct+99)/100 - 1]. Sorts v.
  static int64_t pick(std::vector<int64_t>& v, unsigned pct) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return v[(n * pct + 99) / 100 - 1];
  }
  static double ms(int64_t us) { return double(us) / 1000.0; }

  std::vector<Record> records_;
  Pending pending_;
  unsigned dropped_ = 0;
};

// native_wait_lead,window=N,lead_p5/p50/p95=a/b/c,work_p50/p95/p99=d/e/f,late=K,
// late_short_lead=A,late_long_work=B,period_ms=P,frames=F,dropped=D
// Values are ms with three decimals; "na" where the window has none. Returns the
// length written, or 0 when the buffer is too small.
inline int formatWaitLeadLine(const WaitLeadStats::Report& r, char* buf, size_t cap) {
  char l5[24], l50[24], l95[24], w50[24], w95[24], w99[24];
  auto put = [](char* dst, size_t n, bool valid, double v) {
    if (valid) std::snprintf(dst, n, "%.3f", v); else std::snprintf(dst, n, "na");
  };
  put(l5, sizeof l5, r.leadValid, r.leadP5Ms);
  put(l50, sizeof l50, r.leadValid, r.leadP50Ms);
  put(l95, sizeof l95, r.leadValid, r.leadP95Ms);
  put(w50, sizeof w50, r.workValid, r.workP50Ms);
  put(w95, sizeof w95, r.workValid, r.workP95Ms);
  put(w99, sizeof w99, r.workValid, r.workP99Ms);
  const int n = std::snprintf(buf, cap,
      "native_wait_lead,window=%llu,lead_p5/p50/p95=%s/%s/%s,work_p50/p95/p99=%s/%s/%s,late=%u,late_short_lead=%u,"
      "late_long_work=%u,period_ms=%.3f,frames=%u,dropped=%u\n",
      static_cast<unsigned long long>(r.window), l5, l50, l95, w50, w95, w99, r.late, r.lateShortLead,
      r.lateLongWork, r.periodMs, r.frames, r.dropped);
  return n > 0 && size_t(n) < cap ? n : 0;
}

// native_long_cycle_1x,window=N,period_ms=P,count=C,cycle_p50=X,work_p50=W,
// work_ge_period=A,work_lt_period=B
inline int formatLongCycle1xLine(const WaitLeadStats::Report& r, char* buf, size_t cap) {
  char c50[24], w50[24];
  auto put = [](char* dst, size_t n, bool valid, double v) {
    if (valid) std::snprintf(dst, n, "%.3f", v); else std::snprintf(dst, n, "na");
  };
  put(c50, sizeof c50, r.cycleValid, r.long1xCycleP50Ms);
  put(w50, sizeof w50, r.long1xWorkValid, r.long1xWorkP50Ms);
  const int n = std::snprintf(buf, cap,
      "native_long_cycle_1x,window=%llu,period_ms=%.3f,count=%u,cycle_p50=%s,work_p50=%s,work_ge_period=%u,work_lt_period=%u\n",
      static_cast<unsigned long long>(r.window), r.periodMs, r.long1x, c50, w50, r.long1xWorkGePeriod, r.long1xWorkLtPeriod);
  return n > 0 && size_t(n) < cap ? n : 0;
}

// The lead of a predicted display time over a wait return. convert maps an
// XrTime (ns) to the QPC-us clock and returns false when it cannot. Returns
// false, leaving leadUs alone, when the prediction is not positive or does not convert.
template<class Convert> bool measureWaitLeadUs(Convert convert, int64_t predictedNs, int64_t returnUs, int64_t& leadUs) {
  int64_t predictedUs = 0;
  if (predictedNs <= 0 || !convert(predictedNs, predictedUs)) return false;
  leadUs = predictedUs - returnUs;
  return true;
}

} // namespace edvr::openxr
