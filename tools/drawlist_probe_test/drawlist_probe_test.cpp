// The draw-list probe's pure half (src\d3d11\drawlist_probe_core.h; TEMPORARY, Built v9): the origin classes, the carried-candidate key sets
// over three frames, the key cap, the CSV field count for every row kind, the frame line and the join statement. No DLL and no game.
#include "../../src/d3d11/drawlist_probe_core.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace dl = edvr::drawlist;

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

// Fields in a row: commas plus one. Names are sanitised, so this holds for any name.
int fieldCount(const std::string& s) {
    int n = 1;
    for (char ch : s)
        if (ch == ',') ++n;
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    (void)selfTest;

    // --- The origin classes: path A before path B, B before carried, carried before unattributed.
    check(dl::classifyOrigin(true, false, false) == dl::Origin::kAppendedA, "appended by A is appended_A");
    check(dl::classifyOrigin(true, true, true) == dl::Origin::kAppendedA, "A wins over B and carried");
    check(dl::classifyOrigin(false, true, true) == dl::Origin::kAppendedB, "B wins over carried");
    check(dl::classifyOrigin(false, false, true) == dl::Origin::kCarried, "appended earlier only is carried");
    check(dl::classifyOrigin(false, false, false) == dl::Origin::kUnattributed, "nothing seen is unattributed");
    check(std::strcmp(dl::originName(dl::Origin::kCarried), "carried") == 0, "the carried origin name");
    check(std::strcmp(dl::originName(dl::Origin::kUnattributed), "unattributed") == 0, "the unattributed origin name");

    // --- The key sets over three frames: keys 1,2 then 2,3 then 1.
    {
        dl::KeySet k;
        k.reset();
        k.beginFrame();                                  // frame 1
        check(k.noteAppended(1) && k.noteAppended(2), "frame 1: two keys are new");
        check(!k.noteAppended(2), "a repeat in the same frame is not new");
        check(k.distinctThisFrame() == 2, "frame 1: two distinct keys");
        check(k.carried() == 0, "frame 1: nothing earlier, nothing carried");
        k.beginFrame();                                  // frame 2
        check(k.noteAppended(2) && k.noteAppended(3), "frame 2: keys 2 and 3 are new this frame");
        check(k.seenEarlier(1) && !k.seenEarlier(3), "frame 2: key 1 is in an earlier frame, key 3 is not");
        check(k.carried() == 1, "frame 2: key 1 is carried (appended earlier, not this frame)");
        k.beginFrame();                                  // frame 3
        check(k.noteAppended(1), "frame 3: key 1 is new this frame");
        check(k.carried() == 2, "frame 3: keys 2 and 3 are carried");
        check(k.appendedThisFrame(1) && !k.appendedThisFrame(2), "frame 3: only key 1 is appended this frame");
    }
    // --- The key cap: the overflow is counted, not tracked, and the tracked keys stay exact.
    {
        dl::KeySet k;
        k.reset();
        k.beginFrame();
        for (uint64_t i = 1; i <= dl::kKeyCap + 5; ++i) k.noteAppended(i);
        check(k.distinctThisFrame() == dl::kKeyCap, "the cap tracks kKeyCap keys");
        check(k.overflow() == 5, "the five keys past the cap are counted as overflow");
    }

    // --- The CSV: the header and every row kind have the same field count, and a comma in a name does not split a row.
    {
        const int header = fieldCount(dl::csvHeader());
        check(header == dl::kColumns, "the header has kColumns fields");
        char buf[512];
        dl::CsvRow ev;
        ev.table = "event";
        ev.frame = 7;
        ev.kind = "builder";
        ev.record = 0x1234;
        ev.key = 0x5678;
        ev.name = "Hangar,Lobby";          // a comma: sanitised, so the row keeps its fields
        ev.origin = "appended_A";
        ev.mode = "all";
        dl::formatRow(buf, sizeof(buf), ev);
        check(fieldCount(buf) == dl::kColumns, "an event row has kColumns fields, with a comma in the name");
        check(std::strstr(buf, "Hangar;Lobby") != nullptr, "the comma in the name is written as a semicolon");
        check(std::strstr(buf, ",-,-,-,") != nullptr, "unknown numbers print as '-'");

        dl::CsvRow fam;
        fam.table = "family";
        fam.frame = 7;
        fam.kind = "family";
        fam.family = "Ship";
        fam.draws = 42;
        fam.origin = "unattributed";
        fam.mode = "off";
        dl::formatRow(buf, sizeof(buf), fam);
        check(fieldCount(buf) == dl::kColumns, "a family row has kColumns fields");
        check(std::strstr(buf, ",42,") != nullptr, "the family row carries its draw count");

        dl::CsvRow col;
        col.table = "collection";
        col.frame = 7;
        col.kind = "clear";
        col.owner = 0xABC;
        col.rebuilt = "yes";
        col.origin = "-";
        col.mode = "named";
        dl::formatRow(buf, sizeof(buf), col);
        check(fieldCount(buf) == dl::kColumns, "a collection row has kColumns fields");

        dl::CsvRow fr;
        fr.table = "frame";
        fr.frame = 7;
        fr.appended = 3;
        fr.draws = 9;
        fr.origin = "unattributed";
        fr.mode = "sphere";
        fr.rebuilt = "no";
        dl::formatRow(buf, sizeof(buf), fr);
        check(fieldCount(buf) == dl::kColumns, "a frame row has kColumns fields");

        // A long name is cut to the row buffer's name field without breaking the field count.
        std::string longName(200, 'x');
        dl::CsvRow lr = ev;
        lr.name = longName.c_str();
        dl::formatRow(buf, sizeof(buf), lr);
        check(fieldCount(buf) == dl::kColumns, "a name longer than the field keeps the field count");
    }

    // --- The frame line: the engine state, the bracket state, the overflow note and the names are all said.
    {
        dl::FrameCounts c;
        c.frame = 100;
        c.window = 2;
        c.mode = "all";
        c.engineLive = false;
        c.builderPathBracket = false;
        c.builderCalls = 4;
        c.emitCalls = 0;
        c.copyCalls = 1;
        c.copyRecords = 8;
        c.familyDraws = 0;
        c.appendedA = 2;
        c.carried = 5;
        c.rebuilt = 1;
        c.keysOverflow = 3;
        char line[1600];
        dl::formatFrameLine(line, sizeof(line), c, "names: A [x]");
        const std::string s = line;
        check(s.find("frame 100") != std::string::npos && s.find("window 2 of 3") != std::string::npos, "the frame line names the frame and window");
        check(s.find("mode all") != std::string::npos, "the frame line names the mode");
        check(s.find("engine motion NOT live") != std::string::npos, "a dead engine motion is said, not reported as zero");
        check(s.find("NOT the bracket path, so A is zero") != std::string::npos, "a non-bracket builder path is said");
        check(s.find("overflowed") != std::string::npos, "the key overflow is said");
        check(s.find("names: A [x]") != std::string::npos, "the caller's names are appended");
        dl::FrameCounts live = c;
        live.engineLive = true;
        live.builderPathBracket = true;
        live.keysOverflow = 0;
        dl::formatFrameLine(line, sizeof(line), live, "");
        const std::string t = line;
        check(t.find("engine motion live") != std::string::npos, "a live engine motion is said");
        check(t.find("overflowed") == std::string::npos, "no overflow note when nothing overflowed");
    }

    // --- The join statement: it says the join is not live, and that the scene draws are unattributed.
    check(std::strstr(dl::joinStatement(), "NOT LIVE") != nullptr, "the join is said to be NOT LIVE");
    check(std::strstr(dl::joinStatement(), "unattributed") != nullptr, "the scene draws are said to be unattributed");

    std::printf("drawlist_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
