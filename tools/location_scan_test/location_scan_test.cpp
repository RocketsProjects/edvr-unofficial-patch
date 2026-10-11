// The journal's Location scanner (src\d3d11\journal_location_core.h, the reader of journal_watch.cpp): the Docked value of a Location
// line is read whatever the read boundaries are. Every split of a one-line journal, reads of one byte, a replay from the top, an
// overlapping re-read, and the refusals (a Docked event line, a null value, a line without the key) are checked. No game, no DLL.
#include "../../src/d3d11/journal_location_core.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// A Location line as the live journal writes it: the event, a long DistFromStarLS (about fifty bytes to the key), then the key.
std::string locationLine(const char* docked) {
    return std::string("{ \"timestamp\":\"2026-10-11T10:00:00Z\", \"event\":\"Location\", "
                       "\"DistFromStarLS\":12345678901234567890.123456789, \"Docked\":") +
           docked + ", \"StationName\":\"Example\" }\n";
}

// Feeds bytes [from, to) of text at their absolute positions.
void feedRange(edvr::journal_location::LocationScan& s, const std::string& text, size_t from, size_t to) {
    s.feedChunk(reinterpret_cast<const uint8_t*>(text.data() + from), static_cast<uint32_t>(to - from),
                static_cast<uint64_t>(from));
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    if (!selfTest) {
        std::printf("usage: location_scan_test --self-test\n");
        return 2;
    }
    using edvr::journal_location::LocationScan;

    const std::string docked = locationLine("true");
    const std::string undocked = locationLine("false");
    const size_t n = docked.size();

    // The whole line in one read.
    {
        LocationScan s;
        feedRange(s, docked, 0, n);
        check(s.known() && s.docked(), "one read: Docked true");
    }
    {
        LocationScan s;
        feedRange(s, undocked, 0, undocked.size());
        check(s.known() && !s.docked(), "one read: Docked false");
    }

    // Every split: the value is read whichever byte the first read ends on, the gap and the key included.
    bool everyTrue = true, everyFalse = true;
    for (size_t k = 0; k <= n; ++k) {
        LocationScan s;
        feedRange(s, docked, 0, k);
        feedRange(s, docked, k, n);
        if (!(s.known() && s.docked())) everyTrue = false;
        LocationScan u;
        feedRange(u, undocked, 0, k);
        feedRange(u, undocked, k, undocked.size());
        if (!(u.known() && !u.docked())) everyFalse = false;
    }
    check(everyTrue, "every split of a Docked-true line gives Docked true");
    check(everyFalse, "every split of a Docked-false line gives Docked false");

    // One byte per read.
    {
        LocationScan s;
        for (size_t k = 0; k < n; ++k) feedRange(s, docked, k, k + 1);
        check(s.known() && s.docked(), "one byte per read: Docked true");
    }

    // The key may have spaces after the colon, and the value may be null (not read: the earlier value stands).
    {
        LocationScan s;
        feedRange(s, locationLine(" true"), 0, locationLine(" true").size());
        check(s.known() && s.docked(), "a space after the colon is skipped");
    }
    {
        LocationScan s;
        feedRange(s, docked, 0, n);
        const std::string nul = locationLine("null");
        feedRange(s, nul, 0, nul.size());
        check(s.known() && s.docked(), "a null value leaves the earlier Docked value standing");
    }

    // A journal with several lines: the last Location line with a key decides; a Docked event and a Location without a key do not.
    {
        const std::string docEvent =
            "{ \"timestamp\":\"2026-10-11T10:00:01Z\", \"event\":\"Docked\", \"StationName\":\"Example\", \"Docked\":true }\n";
        const std::string noKey = "{ \"timestamp\":\"2026-10-11T10:00:02Z\", \"event\":\"Location\", \"Body\":\"Example\" }\n";
        const std::string text = docked + docEvent + undocked + noKey;
        LocationScan s;
        feedRange(s, text, 0, text.size());
        check(s.known() && !s.docked(), "the last Location with a key decides: Docked false");
        LocationScan t;
        feedRange(t, docEvent, 0, docEvent.size());
        check(!t.known(), "a Docked event alone is not a Location line");
        LocationScan v;
        feedRange(v, noKey, 0, noKey.size());
        check(!v.known(), "a Location line without a Docked key gives no value");
    }

    // Replays: a file read again from the top (reset), and a re-read of bytes already fed (dropped, not fed twice).
    {
        LocationScan s;
        feedRange(s, undocked, 0, undocked.size());
        feedRange(s, docked, 0, n);   // the same positions again: dropped
        check(s.known() && !s.docked(), "bytes below next() are dropped: the first value stands");
        s.reset();
        feedRange(s, docked, 0, n);
        check(s.known() && s.docked(), "after reset, a replay from the top reads its own value");
    }
    {
        LocationScan s;
        feedRange(s, docked, 0, n);
        const size_t fedAgain = s.feedChunk(reinterpret_cast<const uint8_t*>(docked.data()), static_cast<uint32_t>(n), 0);
        check(fedAgain == 0 && s.next() == n, "a full re-read feeds nothing");
        const size_t overlap = s.feedChunk(reinterpret_cast<const uint8_t*>(docked.data()), static_cast<uint32_t>(n), 0);
        check(overlap == 0, "a second full re-read feeds nothing");
    }
    {
        // An overlapping read: the first 40 bytes again, then the rest, gives the same answer as one read.
        LocationScan s;
        feedRange(s, docked, 0, 40);
        feedRange(s, docked, 0, n);
        check(s.known() && s.docked() && s.next() == n, "an overlapping re-read gives the whole line's value");
        LocationScan one;
        const size_t fed = one.feedChunk(reinterpret_cast<const uint8_t*>(docked.data()), static_cast<uint32_t>(n), 0);
        check(fed == n && one.docked(), "feedChunk counts every new byte");
    }

    // Chunks of every size from 2 to 60 bytes.
    bool allSizes = true;
    for (size_t size = 2; size <= 60; ++size) {
        LocationScan s;
        for (size_t at = 0; at < n; at += size) {
            const size_t end = at + size < n ? at + size : n;
            feedRange(s, docked, at, end);
        }
        if (!(s.known() && s.docked())) allSizes = false;
    }
    check(allSizes, "chunks of 2 to 60 bytes all give Docked true");

    std::printf("location scan test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
