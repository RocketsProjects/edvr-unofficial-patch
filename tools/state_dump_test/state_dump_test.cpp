// Test rig for the state dump's pure half (src/d3d11/state_dump_core.h), v2: the reader table and its pinned prologue (a
// one-byte change refused), the capped byte diff with its offsets, the counter deltas (a reset is no event), the length
// parse (default, clamps, a non-number), the change text, the CSV row and header (equal field counts, 26 columns), and
// the start, event and dock lines. No DLL and no game.
#include "../../src/d3d11/state_dump_core.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace sd = edvr::statedump;

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

int countFields(const std::string& s) {
    int n = 1;
    for (char c : s)
        if (c == ',') ++n;
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    const bool selfTest = argc > 1 && std::strcmp(argv[1], "--self-test") == 0;
    if (!selfTest) {
        std::printf("usage: state_dump_test --self-test\n");
        return 2;
    }

    // --- The readers: four distinct RVAs, and the pinned prologue (push rbx; sub rsp,20h).
    check(sd::kReaders == 4, "four readers");
    check(sd::kReaderRva[0] == 0x12C3C20u && sd::kReaderRva[1] == 0x12CAF80u && sd::kReaderRva[2] == 0x12CADE0u &&
              sd::kReaderRva[3] == 0x12CB320u,
          "the reader RVAs are the four getters' callers");
    bool distinct = true;
    for (int i = 0; i < sd::kReaders; ++i)
        for (int j = i + 1; j < sd::kReaders; ++j)
            if (sd::kReaderRva[i] == sd::kReaderRva[j]) distinct = false;
    check(distinct, "reader RVAs are distinct");
    check(sd::kExpectedTimestamp == 1788384820u && sd::kExpectedImageSize == 104894464u, "build 332841 identity");
    {
        uint8_t got[sizeof(sd::kReaderPin)] = {};
        std::memcpy(got, sd::kReaderPin, sizeof(got));
        check(sd::pinMatches(got, sd::kReaderPin, sizeof(got)), "reader pin: the true bytes are accepted");
        bool allRefused = true;
        for (size_t i = 0; i < sizeof(got); ++i) {
            uint8_t changed[sizeof(got)];
            std::memcpy(changed, sd::kReaderPin, sizeof(got));
            changed[i] = static_cast<uint8_t>(changed[i] ^ 0x01);
            if (sd::pinMatches(changed, sd::kReaderPin, sizeof(got))) allRefused = false;
        }
        check(allRefused, "reader pin: a one-byte change is refused at every position");
        check(!sd::pinMatches(nullptr, sd::kReaderPin, sizeof(got)), "reader pin: a null read is refused");
    }
    check(sd::kMidOffset <= 0x330 && sd::kMidOffset + sd::kMidBytes >= 0x346, "the middle window covers +0x330..+0x345");
    check(sd::kObjBytes >= 0x40 && sd::kPtrOffset == 0x240, "the head window covers +0x18 and +0x30; the pointer is at +0x240");

    // --- The byte diff: offsets, the cap, the total.
    {
        uint8_t was[32], now[32];
        std::memset(was, 0x10, sizeof(was));
        std::memcpy(now, was, sizeof(was));
        sd::Change list[4];
        int total = -1;
        check(sd::diffBytes(was, now, sizeof(was), 0, list, 4, &total) == 0 && total == 0,
              "identical bytes differ nowhere");
        now[3] = 0x11;
        now[20] = 0x0F;
        now[31] = 0xFF;
        int kept = sd::diffBytes(was, now, sizeof(was), 0x300, list, 4, &total);
        check(kept == 3 && total == 3, "three changed bytes are kept and counted");
        check(list[0].offset == 0x303 && list[0].was == 0x10 && list[0].now == 0x11, "the first change's offset and values");
        check(list[1].offset == 0x314 && list[2].offset == 0x31F && list[2].now == 0xFF, "later changes' offsets");
        kept = sd::diffBytes(was, now, sizeof(was), 0, list, 2, &total);
        check(kept == 2 && total == 3, "the cap keeps the first changes and the total still counts all");
    }

    // --- The change text.
    {
        sd::Change list[2];
        list[0].offset = 0x10;
        list[0].was = 0x00;
        list[0].now = 0x01;
        list[1].offset = 0x2A;
        list[1].was = 0xFE;
        list[1].now = 0x7F;
        char buf[256];
        size_t at = sd::appendChanges(buf, sizeof(buf), 0, "A1.obj", list, 2, 2);
        buf[at] = '\0';
        check(std::string(buf) == "A1.obj+0x10:00>01 A1.obj+0x2A:FE>7F", "change text: name, offset and both values");
        at = sd::appendChanges(buf, sizeof(buf), 0, "A2.mid", list, 1, 5);
        buf[at] = '\0';
        check(std::string(buf) == "A2.mid+0x10:00>01 A2.mid+4_more", "change text: the rest is counted, not listed");
        char tiny[12];
        at = sd::appendChanges(tiny, sizeof(tiny), 0, "A1.obj", list, 2, 2);
        check(at < sizeof(tiny), "change text never writes past its buffer");
    }

    // --- The counter deltas: an increase counts, a reset does not.
    check(sd::deltaOf(3, 5) == 2, "a counter that rose by two is two events");
    check(sd::deltaOf(5, 5) == 0, "an unchanged counter is no event");
    check(sd::deltaOf(9, 2) == 0, "a counter that fell (a new journal session) is no event");

    // --- The length: default, clamps, a non-number.
    {
        uint32_t n = 0;
        check(sd::parseFrames("", &n) && n == 3000, "an empty length is the default, 3000 frames");
        check(sd::parseFrames("1200", &n) && n == 1200, "a whole number is the length");
        check(sd::parseFrames("99999", &n) && n == 6000, "a length above the cap is the cap, 6000");
        check(sd::parseFrames("5", &n) && n == 60, "a length below the floor is the floor, 60");
        check(!sd::parseFrames("12x", &n) && n == 3000, "a value that is not a whole number is the default and says so");
        check(!sd::parseFrames("abc", &n) && n == 3000, "a word is the default and says so");
    }

    // --- The CSV: 26 columns; the header and each row agree.
    {
        sd::FrameRow r;
        r.index = 7;
        r.frame = 4242;
        r.counters.docks = 3;
        r.counters.undocks = 2;
        r.counters.disembarks = 1;
        r.counters.embarks = 1;
        r.locKnown = 1;
        r.locDocked = 0;
        r.statusDocked = 1;
        r.flags = 0x0900000Du;
        r.flags2 = 0x00016009u;
        r.flagsKnown = true;
        r.flags2Known = true;
        r.code[0] = 2;
        r.code[1] = -1;
        r.code[2] = 0;
        r.code[3] = 1;
        r.calls[0] = 1;
        r.calls[2] = 3;
        r.object[0] = 0x1F4A0B20000ull;
        r.changes = "A0.obj+0x88:00>01";
        char line[1024];
        const int n = sd::formatFrameRow(line, sizeof(line), r);
        check(n > 0, "a CSV row formats");
        const std::string row = line;
        check(countFields(sd::csvHeader()) == 26, "the header has 26 columns");
        check(countFields(row) == 26, "the CSV row has 26 fields");
        check(row.rfind("7,4242,3,2,1,1,1,0,1,0x0900000D,0x00016009,1,1,2,-1,0,1,1,0,3,0,", 0) == 0,
              "the CSV row's fields are in header order (dock columns before the Status words)");
        check(row.find("0x1F4A0B20000") != std::string::npos && row.find(",A0.obj+0x88:00>01") != std::string::npos,
              "the CSV row carries the object and the change text");
        r.changes = "";
        sd::formatFrameRow(line, sizeof(line), r);
        check(std::string(line).back() == '-', "an empty change list is written as -");
    }

    // --- The start, event and dock lines.
    {
        sd::StartInfo s;
        s.firstFrame = 100;
        s.frames = 600;
        s.trigger = "hotkey.state_dump";
        s.detours = "r0=installed r1=installed r2=installed r3=installed";
        s.csvName = "state_dump_20261011_120000.csv";
        char line[480];
        sd::formatStartLine(line, sizeof(line), s);
        const std::string t = line;
        check(t.find("armed for frames 100..699 (600 frames") != std::string::npos,
              "the start line names the frames from the length");
        check(t.find("hotkey.state_dump") != std::string::npos && t.find("csv=state_dump_") != std::string::npos,
              "the start line names the trigger and the file");
        s.frames = 3000;
        sd::formatStartLine(line, sizeof(line), s);
        check(std::string(line).find("armed for frames 100..3099") != std::string::npos, "a 3000-frame capture ends at 3099");
        char ev[160];
        sd::formatEventLine(ev, sizeof(ev), 5, "docks", 2);
        check(std::string(ev) == "state dump: f=5 docks +2", "an event line with a count");
        sd::formatEventLine(ev, sizeof(ev), 5, "reader0 code 1", 0);
        check(std::string(ev) == "state dump: f=5 reader0 code 1", "an event line without a count");
        sd::formatDockLine(ev, sizeof(ev), 9, "location_docked", 1);
        check(std::string(ev) == "state dump: f=9 location_docked=1", "a dock line names the source and the value");
    }

    std::printf("state_dump_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("state dump self-test passed\n");
    return 0;
}
