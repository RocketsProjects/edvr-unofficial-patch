// Test rig for the builder probe's and the hatch-state probe's pure halves (src/d3d11/builder_probe_core.h and hatch_probe_core.h; TEMPORARY,
// Built v8). The entry agreement (rcx against the memory at r9 - 0x210 + 0x290, read or not), the two caller sites, the grouping by name,
// the summary and names lines, the CSV rows, the hatch change line (old -> new per field), the line budget and the table line.
// No DLL and no game.
#include "../../src/d3d11/builder_probe_core.h"
#include "../../src/d3d11/hatch_probe_core.h"
#include "../../src/d3d11/hatch_geometry_core.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bp = edvr::builder;
namespace hp = edvr::hatch;

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

bool contains(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }

// The number of fields in a CSV line (the rig's rows carry no commas inside a field).
int fieldCount(const char* line) {
    int n = 1;
    for (const char* p = line; *p; ++p)
        if (*p == ',') ++n;
    return n;
}

bp::Call makeCall(uint64_t rec, const char* name, bool seen, bool sphereOk, float radius, float dist, bool agree, bp::Caller caller) {
    bp::Call c;
    c.rec = rec;
    c.hasName = name != nullptr;
    if (name) c.name = name;
    c.storeSeen = seen;
    c.sphereOk = sphereOk;
    c.radius = radius;
    c.dist = dist;
    c.agree = agree;
    c.caller = caller;
    c.retRva = caller == bp::Caller::kA ? bp::kCallerA : (caller == bp::Caller::kB ? bp::kCallerB : 0x1234u);
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const bool selfTest = argc > 1 && std::strcmp(argv[1], "--self-test") == 0;
    if (!selfTest) {
        std::printf("usage: builder_probe_test --self-test\n");
        return 2;
    }

    // --- The entry: the record base is r9 - 0x210; rcx must equal the value at base + 0x290 (read, and equal).
    {
        check(bp::recordBase(0x10210u) == 0x10000u, "the record base is r9 - 0x210");
        check(bp::recordBase(0x100u) == 0, "a too-small r9 has no record base");
        check(bp::argsAgree(0x7000u, 0x10210u, true, 0x7000u), "rcx equal to the memory at base+0x290 agrees");
        check(!bp::argsAgree(0x7000u, 0x10210u, true, 0x7008u), "a different value at base+0x290 disagrees");
        check(!bp::argsAgree(0x7000u, 0x10210u, false, 0), "an unreadable base+0x290 counts as a disagreement");
        check(!bp::argsAgree(0x7000u, 0x100u, true, 0x7000u), "a too-small r9 counts as a disagreement");
        check(bp::callerOf(bp::kCallerA) == bp::Caller::kA, "0x43204F8 is caller A (the traversal)");
        check(bp::callerOf(bp::kCallerB) == bp::Caller::kB, "0x4321AE0 is caller B (the single-entry form)");
        check(bp::callerOf(0x43204F3u) == bp::Caller::kOther, "the call instruction itself is not a return address: other");
        check(std::strcmp(bp::callerName(bp::Caller::kOther), "other") == 0, "the other caller is named other");
    }

    // --- The grouping and the summary over three calls: two of record 0x1000 (named Alpha; the store saw one, not the other), one of 0x2000
    //     with no name and no sphere.
    std::vector<bp::Call> calls;
    calls.push_back(makeCall(0x1000, "Alpha", true, true, 10.0f, 5.0f, true, bp::Caller::kA));
    calls.push_back(makeCall(0x1000, "Alpha", false, true, 10.0f, 7.0f, true, bp::Caller::kB));
    calls.push_back(makeCall(0x2000, nullptr, true, false, 0.0f, -1.0f, false, bp::Caller::kOther));
    {
        const auto groups = bp::groupByName(calls);
        check(groups.size() == 2, "two groups: Alpha and (no name)");
        const bp::Group& a = groups.at("Alpha");
        check(a.count == 2 && a.seenYes == 1 && a.seenNo == 1, "Alpha: two calls, one seen, one not");
        check(a.radiusMin == 10.0f && a.radiusMax == 10.0f && a.distMin == 5.0f, "Alpha: radius 10 and the nearest distance 5");
        const bp::Group& n = groups.at("(no name)");
        check(n.count == 1 && n.seenYes == 1 && n.seenNo == 0 && !n.radiusSeen && n.distMin == -1.0f,
              "(no name): one call, with no radius and no distance measured");
        char row[256];
        bp::formatGroupRow(row, sizeof(row), n);
        check(std::string(row) == "(no name),1,1,0,-1.00,-1.00,-1.00", "a group with no radius writes -1 for radius and distance");
        check(fieldCount(bp::groupCsvHeader()) == 7 && fieldCount(row) == 7, "the group header and row have seven fields");
    }
    {
        const bp::Summary s = bp::summarise(calls);
        check(s.calls == 3 && s.distinctRecords == 2, "three calls over two distinct records");
        check(s.unseen == 1, "one call's record the store did not visit (H5 signature)");
        check(s.agree == 2 && s.disagree == 1, "two calls agree on the entry, one does not");
        check(s.callerA == 1 && s.callerB == 1 && s.callerOther == 1, "one call from each caller");
        check(s.topByCount.size() == 2 && s.topByCount[0].name == "Alpha", "the most called name comes first");
        char line[512];
        bp::formatSummary(line, sizeof(line), 7, s);
        check(std::string(line) == "builder probe: frame 7: calls 3, distinct records 2, not seen by the store this frame 1 (H5 "
                                   "signature), args agree 2, disagree 1, callers A/B/other 1/1/1",
              "the summary line is exact");
        char names[512];
        size_t written = 99;
        const int n = bp::formatNamesLine(names, sizeof(names), 7, s, 0, &written);
        check(n > 0 && written == 2 && contains(names, "Alpha x2 (unseen 1);") && contains(names, "(no name) x1 (unseen 0);"),
              "the names line lists both groups with their counts and unseen counts");
        check(std::strstr(names, "from 1") != nullptr, "the names line says where it starts");
        const bp::Summary empty = bp::summarise({});
        char zero[256];
        bp::formatSummary(zero, sizeof(zero), 8, empty);
        check(std::string(zero) == "builder probe: frame 8: calls 0, distinct records 0, not seen by the store this frame 0 (H5 "
                                   "signature), args agree 0, disagree 0, callers A/B/other 0/0/0",
              "a frame with no calls still writes its summary line, at zero");
        size_t none = 99;
        check(bp::formatNamesLine(zero, sizeof(zero), 8, empty, 0, &none) == 0 && none == 0, "no names: no names line");
    }
    {
        // Ten names with 1..10 calls: the names line writes eight, then two, then none.
        std::vector<bp::Call> many;
        for (int i = 0; i < 10; ++i)
            for (int k = 0; k <= i; ++k) many.push_back(makeCall(0x100u * (i + 1) + k, nullptr, true, false, 0, -1, true, bp::Caller::kA));
        for (size_t i = 0; i < many.size(); ++i) {
            char nm[16];
            std::snprintf(nm, sizeof(nm), "n%02d", static_cast<int>(many[i].rec / 0x100u) - 1);
            many[i].hasName = true;
            many[i].name = nm;
        }
        const bp::Summary s = bp::summarise(many);
        check(s.topByCount.size() == 10 && s.topByCount[0].name == "n09" && s.topByCount[0].count == 10,
              "ten names rank by count, most first");
        char line[512];
        size_t w1 = 0, w2 = 0, w3 = 0;
        bp::formatNamesLine(line, sizeof(line), 3, s, 0, &w1);
        bp::formatNamesLine(line, sizeof(line), 3, s, 8, &w2);
        bp::formatNamesLine(line, sizeof(line), 3, s, 10, &w3);
        check(w1 == 8 && w2 == 2 && w3 == 0, "the names go eight to a line: 8, then 2, then none");
    }

    // --- The CSV rows: the call row has thirteen fields, and a name that is not known is written as "(no name)".
    {
        bp::Call c = makeCall(0x1000, "Alpha", true, true, 10.0f, 5.0f, true, bp::Caller::kA);
        c.mask = 0x3F;
        c.admitted = 1;
        char row[512];
        bp::formatCallRow(row, sizeof(row), c);
        check(fieldCount(bp::callCsvHeader()) == 13 && fieldCount(row) == 13, "the call header and row have thirteen fields");
        check(contains(row, "0x1000,Alpha,0x3F,1,") && contains(row, ",1,1,A,0x43204F8"), "the call row carries the record, name, mask and caller");
        bp::Call unnamed = makeCall(0x2000, nullptr, false, false, 0, -1, false, bp::Caller::kOther);
        bp::formatCallRow(row, sizeof(row), unnamed);
        check(contains(row, "0x2000,(no name),") && fieldCount(row) == 13, "an unnamed call is written as (no name)");
    }

    // --- The hatch-state probe: change detection, the change line, the line budget, the table line.
    {
        hp::State a;
        a.valid = true;
        a.state3C8 = 2;
        a.owner = 0x2000;
        a.ownerCount = 1;
        hp::State b = a;
        check(!hp::differs(a, b), "identical reads do not differ");
        b.state3C8 = 1;
        check(hp::differs(a, b), "a changed state byte differs");
        b = a;
        b.name340[5] = 0xAB;
        check(hp::differs(a, b), "a changed name byte differs");
        b = a;
        b.ownerB2F = 1;
        check(hp::differs(a, b), "a changed owner byte differs");
        hp::State bad;
        check(hp::differs(a, bad), "a read that became unreadable differs");
        check(!hp::differs(bad, hp::State{}), "two unreadable reads do not differ");

        hp::State now = a;
        now.state3C8 = 1;
        now.ownerCount = 0;
        now.ownerB2F = 1;
        char line[512];
        hp::formatChange(line, sizeof(line), 0x1000, 5, 99, a, now, true, 7, 3, 0x05);
        check(contains(line, "hatch probe: object 1000 (readers r0,r2) frame 5 t=99 ms:"),
              "the change line names the object, the readers that supplied it (r0,r2), the frame and the time");
        check(contains(line, "3C8 2->1"), "the change line gives the state byte old -> new");
        check(contains(line, "owner_10 1->0"), "the change line gives the occupancy count old -> new");
        check(contains(line, "owner_2F 0->1"), "the change line gives owner+0x2F old -> new");
        check(contains(line, "owner 2000->2000"), "an unchanged field still shows old -> new");
        check(contains(line, "status flags=7 flags2=3"), "the change line carries the journal's Status words");
        hp::formatChange(line, sizeof(line), 0x1000, 5, 99, hp::State{}, now, false, 0, 0, 0x00);
        check(contains(line, "3C8 ?->1") && contains(line, "status flags unknown"),
              "an unreadable side is written as ? and unknown status says so");
        check(contains(line, "(readers none)"), "a line with no reader bit says so");

        hp::LineBudget budget;
        budget.cap = 3;
        bool took[5];
        for (bool& t : took) t = budget.take();
        check(took[0] && took[1] && took[2] && !took[3] && !took[4], "a line budget of three takes three and refuses the rest");
        check(budget.used == 3 && budget.suppressed == 2, "the budget counts the refused lines");

        uint64_t ptrs[hp::kTableCount] = {0x1, 0, 0, 0, 0, 0, 0, 0x7};
        char table[512];
        hp::formatTable(table, sizeof(table), 0x140000000u, ptrs, hp::kTableCount);
        check(contains(table, "image+0x5E78C40") && contains(table, "[0]=0x1") && contains(table, "[1]=0x0") &&
                  contains(table, "[7]=0x7"),
              "the table line gives the eight qwords at 0x5E78C40 in order");
        check(hp::kRemember == 32 && hp::kLineCap == 400, "thirty-two remembered objects and 400 lines a session");
        char who[32];
        hp::formatReaders(who, sizeof(who), 0x05);
        check(std::string(who) == "r0,r2", "a reader mask names the readers that supplied the object: r0,r2");
        hp::formatReaders(who, sizeof(who), 0x00);
        check(std::string(who) == "none", "no reader bit reads as none");
        hp::formatReaders(who, sizeof(who), 0x0F);
        check(std::string(who) == "r0,r1,r2,r3", "all four readers read as r0,r1,r2,r3");
    }
    {
        // The remember table (Built v8 fix 1 to 3): note, the readers' bits, clear on toggle-on, the cap once, refusals counted and named.
        hp::RememberTable t;
        check(t.note(0, 1) == hp::RememberTable::Outcome::kIgnored && t.count == 0, "a null object is ignored");
        check(t.note(0x1000, 1) == hp::RememberTable::Outcome::kNew, "a new object is remembered");
        check(t.note(0x1000, 4) == hp::RememberTable::Outcome::kKnown && t.readers[0] == 0x05,
              "a known object gets the second reader's bit: r0 and r2");
        check(t.count == 1, "a known object is not remembered twice");
        check(!t.takeCapLine(), "no cap line before the cap is hit");
        check(t.clear() == 1 && t.count == 0 && t.readers[0] == 0, "clear forgets the remembered objects and reports how many");
        check(t.note(0x1000, 1) == hp::RememberTable::Outcome::kNew, "after a clear the same object is new again");

        hp::RememberTable c;
        for (uint32_t i = 0; i < hp::kRemember; ++i) c.note(0x10000u + i * 0x100u, 2);
        check(c.count == hp::kRemember && !c.capHit, "thirty-two distinct objects fill the table with no refusal");
        check(c.note(0x90000, 1) == hp::RememberTable::Outcome::kRefused && c.capHit,
              "the thirty-third object is refused and the cap is hit");
        check(c.count == hp::kRemember, "the table does not grow past the cap");
        check(c.note(0x90000, 1) == hp::RememberTable::Outcome::kRefused && c.refusedDistinct == 1,
              "the same refused object again is not named twice");
        check(c.note(0x91000, 1) == hp::RememberTable::Outcome::kRefused && c.refusedDistinct == 2,
              "a second distinct refused object is named");
        check(c.takeCapLine() && !c.takeCapLine(), "the cap line is due once");
        for (uint32_t i = 0; i < hp::kRefusedCap - 2; ++i) c.note(0x200000u + i * 0x100u, 1);
        check(c.refusedDistinct == hp::kRefusedCap, "the named refusals stop at the tracked limit");
        check(c.note(0xA00000, 1) == hp::RememberTable::Outcome::kRefused && c.refusedOverflow == 1 && c.refusedDistinct == hp::kRefusedCap,
              "past the tracked limit a refusal is counted, not named");
        char cap[320];
        hp::formatCapLine(cap, sizeof(cap), 99, c);
        check(contains(cap, "cap (32) reached at frame 99: 32 distinct objects refused so far (1 more refusals not named)"),
              "the cap line says the cap, the frame, the refused count and the unnamed count");
        check(c.clear() == hp::kRemember && !c.capHit && c.refusedDistinct == 0 && !c.capLogged,
              "a toggle-on clear forgets the objects and re-arms the cap line");
        check(c.note(0x90000, 1) == hp::RememberTable::Outcome::kNew, "after the clear the table takes objects again");
        char clr[200];
        hp::formatClearLine(clr, sizeof(clr), 5, 12, "hotkey.hatch_probe");
        check(std::string(clr) == "hatch probe: on, cleared 5 remembered objects (frame 12, trigger hotkey.hatch_probe)",
              "the toggle-on line says how many objects were cleared");
    }

    // --- Built v8.1: the shared collection. The record set and the thread set in one thread; then the collector under a stress: four writers
    //     push calls and visits while a swapper takes the storage whole again and again. Every attempt must land in exactly one place: a taken
    //     storage's contents, or its drops. The same core code the engine runs (Collector<std::mutex>), driven by threads.
    {
        bp::RecordSet set;
        set.reset(3);
        check(set.insert(0x10) == bp::RecordSet::Result::kAdded, "a new record is added");
        check(set.insert(0x10) == bp::RecordSet::Result::kPresent, "a record already in the set is present, not added twice");
        check(set.insert(0) == bp::RecordSet::Result::kPresent && set.size() == 1, "key 0 is ignored");
        check(set.insert(0x20) == bp::RecordSet::Result::kAdded && set.insert(0x30) == bp::RecordSet::Result::kAdded,
              "the set takes up to its reserve");
        check(set.insert(0x40) == bp::RecordSet::Result::kFull && set.size() == 3, "past the reserve a record is refused, not stored");
        check(set.contains(0x10) && set.contains(0x30) && !set.contains(0x40), "contains finds stored records and not refused ones");

        bp::ThreadSet threads;
        for (uint32_t t = 1; t <= 17; ++t) threads.note(t);
        check(threads.count == bp::kThreadIds && threads.more, "a seventeenth thread sets more; the first sixteen are kept");
        threads.note(3);
        check(threads.count == bp::kThreadIds, "a repeated thread is not counted twice");

        bp::Collector<std::mutex> col;
        {
            bp::Storage s;
            s.reserve(4, 2);
            (void)col.exchange(std::move(s));
        }
        for (int i = 0; i < 5; ++i) {
            bp::Call c;
            c.rec = 0x1000u + static_cast<uint64_t>(i);
            col.addCall(std::move(c), 1);
        }
        for (uintptr_t r = 0x100; r < 0x104; ++r) col.noteStore(r, r == 0x100 ? 0x7000 : 0, 2);
        bp::Storage taken = col.exchange(bp::Storage{});
        check(taken.calls.size() == 4 && taken.callDrops == 1, "four calls fit the reserve; the fifth is a drop");
        check(taken.seen.size() == 2 && taken.seenDrops == 2, "two visits fit the reserve; the other two are drops");
        check(taken.ctx == 0x7000 && taken.storeThreads.count == 1 && taken.builderThreads.count == 1,
              "the storage keeps the context and the thread count of each detour");
        check(taken.seen.contains(0x100) && taken.seen.contains(0x101) && !taken.seen.contains(0x102),
              "a taken storage's visits are the ones that fit");
    }
    {
        constexpr int kWriters = 4;
        constexpr int kPerWriter = 20000;
        bp::Collector<std::mutex> col;
        {
            bp::Storage s;
            s.reserve(1 << 10, 1 << 11);
            (void)col.exchange(std::move(s));
        }
        std::atomic<bool> done{false};
        std::atomic<uint64_t> takenCalls{0}, takenCallDrops{0}, takenSeen{0}, takenSeenDrops{0};
        auto absorb = [&](bp::Storage& old) {
            takenCalls += old.calls.size();
            takenCallDrops += old.callDrops;
            takenSeen += old.seen.size();
            takenSeenDrops += old.seenDrops;
        };
        std::thread swapper([&] {
            while (!done.load()) {
                bp::Storage fresh;
                fresh.reserve(1 << 10, 1 << 11);
                bp::Storage old = col.exchange(std::move(fresh));
                absorb(old);
                std::this_thread::yield();
            }
        });
        std::vector<std::thread> writers;
        for (int w = 0; w < kWriters; ++w) {
            writers.emplace_back([&col, w, kPerWriter] {
                for (int i = 0; i < kPerWriter; ++i) {
                    bp::Call c;
                    c.rec = (static_cast<uint64_t>(w + 1) << 32) | static_cast<uint64_t>(i + 1);
                    col.addCall(std::move(c), static_cast<uint32_t>(w + 1));
                    col.noteStore(static_cast<uintptr_t>((static_cast<uint64_t>(w + 1) << 32) | static_cast<uint64_t>(i + 1)), 0,
                                  static_cast<uint32_t>(w + 1));
                }
            });
        }
        for (std::thread& t : writers) t.join();
        done = true;
        swapper.join();
        bp::Storage last = col.exchange(bp::Storage{});
        absorb(last);
        const uint64_t attempts = static_cast<uint64_t>(kWriters) * kPerWriter;
        check(takenCalls.load() + takenCallDrops.load() == attempts,
              "under the stress every builder call is in a taken storage or counted as a drop (no call lost, none twice)");
        check(takenSeen.load() + takenSeenDrops.load() == attempts,
              "under the stress every store visit is in a taken storage or counted as a drop (no visit lost, none twice)");
    }


    std::printf("builder_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("builder probe self-test passed\n");
    return 0;
}
