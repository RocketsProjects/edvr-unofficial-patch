// Test rig for the state dump's pure half (src/d3d11/state_dump_core.h): the reader table and
// its pinned prologue (a one-byte change refused), the capped byte diff with its offsets, the
// counter deltas (a reset is no event), the change text, the CSV row and header (equal field
// counts), the start and event lines. No DLL and no game.
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

    // --- The readers: four distinct RVAs in the exe, and the pinned prologue (push rbx; sub rsp,20h).
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

    // Pin: the true bytes accepted, every one-byte change refused, a null read refused.
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
        int kept = sd::diffBytes(was, now, sizeof(was), 0x100, list, 4, &total);
        check(kept == 3 && total == 3, "three changed bytes are kept and counted");
        check(list[0].offset == 0x103 && list[0].was == 0x10 && list[0].now == 0x11, "the first change's offset and values");
        check(list[1].offset == 0x114 && list[2].offset == 0x11F && list[2].now == 0xFF, "later changes' offsets");
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
        at = sd::appendChanges(buf, sizeof(buf), 0, "A2.sub", list, 1, 5);
        buf[at] = '\0';
        check(std::string(buf) == "A2.sub+0x10:00>01 A2.sub+4_more", "change text: the rest is counted, not listed");
        char tiny[12];
        at = sd::appendChanges(tiny, sizeof(tiny), 0, "A1.obj", list, 2, 2);
        check(at < sizeof(tiny), "change text never writes past its buffer");
    }

    // --- The counter deltas: an increase counts, a reset (a smaller value) does not.
    check(sd::deltaOf(3, 5) == 2, "a counter that rose by two is two events");
    check(sd::deltaOf(5, 5) == 0, "an unchanged counter is no event");
    check(sd::deltaOf(9, 2) == 0, "a counter that fell (a new journal session) is no event");

    // --- The CSV: the header and each row have equal field counts; the row's fields are in order.
    {
        sd::FrameRow r;
        r.index = 7;
        r.frame = 4242;
        r.counters.docks = 3;
        r.counters.undocks = 2;
        r.counters.disembarks = 1;
        r.counters.embarks = 1;
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
        check(countFields(sd::csvHeader()) == countFields(row), "the CSV row has the header's field count");
        check(row.rfind("7,4242,3,2,1,1,0x0900000D,0x00016009,1,1,2,-1,0,1,1,0,3,0,", 0) == 0,
              "the CSV row starts with the index, frame, counters, Status words, codes and calls");
        check(row.find("0x1F4A0B20000") != std::string::npos && row.find(",A0.obj+0x88:00>01") != std::string::npos,
              "the CSV row carries the object and the change text");
        r.changes = "";
        sd::formatFrameRow(line, sizeof(line), r);
        check(std::string(line).size() > 0 && std::string(line).back() == '-', "an empty change list is written as -");
        check(countFields(sd::csvHeader()) == 23, "the header has 23 columns");
    }

    // --- The start line and the event lines.
    {
        sd::StartInfo s;
        s.firstFrame = 100;
        s.trigger = "hotkey.state_dump";
        s.detours = "r0=installed r1=installed r2=installed r3=installed";
        s.csvName = "state_dump_20261011_120000.csv";
        char line[400];
        sd::formatStartLine(line, sizeof(line), s);
        const std::string t = line;
        check(t.find("armed for frames 100..699") != std::string::npos, "the start line names the 600 frames");
        check(t.find("hotkey.state_dump") != std::string::npos && t.find("csv=state_dump_") != std::string::npos,
              "the start line names the trigger and the file");
        char ev[120];
        sd::formatEventLine(ev, sizeof(ev), 5, "docks", 2);
        check(std::string(ev) == "state dump: f=5 docks +2", "an event line with a count");
        sd::formatEventLine(ev, sizeof(ev), 5, "reader0 code 1", 0);
        check(std::string(ev) == "state dump: f=5 reader0 code 1", "an event line without a count");
    }

    check(sd::kFrames == 600, "the capture is 600 frames");

    std::printf("state_dump_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("state dump self-test passed\n");
    return 0;
}
