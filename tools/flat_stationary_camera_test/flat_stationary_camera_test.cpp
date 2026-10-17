#include "src/d3d11/flat_stationary_camera_core.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <unordered_map>

using namespace edvr::stationcam;

struct Memory {
    std::unordered_map<uintptr_t, uint8_t> cells;
    uintptr_t denied = 0;
    template<class T> void put(uintptr_t at, const T& value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) cells[at + i] = bytes[i];
    }
    void putBytes(uintptr_t at, const void* value, size_t count) {
        const auto* bytes = static_cast<const uint8_t*>(value);
        for (size_t i = 0; i < count; ++i) cells[at + i] = bytes[i];
    }
    static bool read(void* opaque, uintptr_t at, void* output, size_t count) noexcept {
        auto& m = *static_cast<Memory*>(opaque);
        auto* bytes = static_cast<uint8_t*>(output);
        for (size_t i = 0; i < count; ++i) {
            if (at + i == m.denied) return false;
            const auto found = m.cells.find(at + i);
            if (found == m.cells.end()) return false;
            bytes[i] = found->second;
        }
        return true;
    }
};

constexpr uintptr_t kBase = 0x10000000, kB = 0x20000000, kC = 0x21000000;
constexpr uintptr_t kName = 0x22000000, kRoster = 0x23000000;
constexpr uintptr_t kActivity = 0x24000000, kVtable = 0x25000000, kActivityName = 0x26000000;
constexpr uintptr_t kNestedStack = 0x27000000, kNestedConfig = 0x28000000;
constexpr uintptr_t kNestedName = 0x29000000, kNestedFirst = 0x30000000;

Memory fixture() {
    Memory m;
    m.put(kC, kBase + 0x5185F90);
    m.put(kC + 0x10, kBase + 0x5185F98);
    m.put(kC + 0x30, kB);
    m.put(kC + 0x40, kB + 0xB8);
    m.put(kC + 0x58, uint64_t{1});
    m.put(kC + 0x60, kRoster);
    m.put(kC + 0x68, uint32_t{0});
    m.put(kB, kBase + 0x5186290);
    m.put(kB + 0x20, kName);
    for (unsigned i = 0; i < 11; ++i) m.put(kB + 0x28 + 8*i, i == 10 ? kC : uintptr_t{0});
    m.put(kName, uint64_t{8});
    m.put(kName + 8, uint64_t{16});
    m.putBytes(kName + 0x14, "MainView", 8);
    float poseData[16]{};
    poseData[0] = poseData[5] = poseData[10] = poseData[15] = 1.0f;
    m.putBytes(kB + 0xB8 + 0x40, poseData, sizeof(poseData));
    poseData[12] = 42.0f;
    m.putBytes(kC + 0x70 + 0x40, poseData, sizeof(poseData));
    m.put(kRoster, kActivity);
    m.put(kActivity, kVtable);
    m.put(kVtable + 0x20, kBase + 0x8BE920);
    m.put(kVtable + 0x28, kBase + 0x5293E0);
    m.put(kActivity - 4, uint8_t{1});
    m.put(kActivity + 8, kActivityName);
    m.put(kActivity - 0x20, kBase + 0x12345);
    m.putBytes(kActivityName, "Headlook\0", 9);
    poseData[12] = 42.0f;
    m.putBytes(kActivity + 0x10 + 0x40, poseData, sizeof(poseData));
    return m;
}

Memory nestedFixture(float weight) {
    Memory m = fixture();
    const uintptr_t nestedActivity = kNestedStack + 0x98;
    m.put(kRoster, nestedActivity);
    m.put(nestedActivity, kBase + 0x51869B0);
    m.put(kBase + 0x51869B0 + 0x20, kBase + 0x108ADE0);
    m.put(kBase + 0x51869B0 + 0x28, kBase + 0x108A98C);
    m.put(nestedActivity - 0xA0, kNestedConfig);
    m.put(kNestedConfig + 0x34, weight);
    m.put(kNestedStack, kBase + 0x5186938);
    m.put(kNestedStack + 0x20, kNestedName);
    m.put(kNestedStack + 0x1B8, kC);
    m.put(kNestedName, uint64_t{10});
    m.put(kNestedName + 8, uint64_t{16});
    m.putBytes(kNestedName + 0x14, "NestedView", 10);
    float poseData[16]{};
    poseData[0] = poseData[5] = poseData[10] = poseData[15] = 1.0f;
    for (unsigned i = 0; i < 11; ++i) {
        const uintptr_t child = kNestedFirst + 0x1000 * i;
        m.put(kNestedStack + 0x28 + 8 * i, child);
        m.put(child, kBase + 0x5185F90);
        m.put(child + 0x10, kBase + 0x5185F98);
        m.put(child + 0x30, kNestedStack);
        m.put(child + 0x40, i ? kNestedFirst + 0x1000 * (i - 1) + 0x70 : kB + 0xB8);
        m.put(child + 0x58, uint64_t{0});
        m.put(child + 0x60, uintptr_t{0});
        m.put(child + 0x68, uint32_t{0});
        poseData[12] = static_cast<float>(i + 1);
        m.putBytes(child + 0x70 + 0x40, poseData, sizeof(poseData));
    }
    return m;
}

bool testGate() {
    ParkGate gate;
    ParkInput in;
    in.active = in.gameplay = in.dockedKnown = in.docked = in.seatedKnown = in.seated = in.guiKnown = true;
    in.selected = true; in.depth = 0xABC; in.origin[0] = 100;
    // A menu can keep stale docked/seated flags for hundreds of frames. It
    // cannot spend the freshness/stability budget.
    in.menu = true;
    for (unsigned frame = 1; frame <= 500; ++frame) {
        in.frame = frame; in.statusSamples = frame / 60;
        if (gate.step(in) || gate.stable || gate.fresh) return false;
    }
    in.menu = false;
    for (unsigned frame = 501; frame <= 560; ++frame) {
        in.frame = frame;
        in.depth = frame & 1 ? 0xABC : 0xDEF; // alternating valid render resources
        in.origin[0] = 100.0f + static_cast<float>(frame) * 0.1f; // no kinematic stillness requirement
        in.reset = (frame & 1) != 0; // previous-history reset does not invalidate current rows
        in.statusSamples = 8 + (frame >= 520 ? 1u : 0u) + (frame >= 540 ? 1u : 0u);
        const bool ready = gate.step(in);
        if (ready != (frame >= 540)) return false;
    }
    in.gui = 6; in.frame = 561;
    if (gate.step(in) || gate.stable || gate.fresh) return false;
    in.gui = 0; in.frame = 562;
    if (gate.step(in) || gate.fresh) return false; // stale status sample cannot re-arm
    in.frame = 563; in.origin[0] = std::numeric_limits<float>::quiet_NaN();
    if (gate.step(in) || gate.stable || gate.fresh) return false;
    return true;
}

bool testCapture() {
    Memory m = fixture();
    auto s = std::make_unique<Snapshot>(); s->source = kC + 0x70;
    capture(&Memory::read, &m, kBase, *s);
    if (!s->complete || !s->consistent || !s->validSource || s->stageCount != 1) return false;
    const Stage& stage = s->stages[0];
    if (!stage.nameRead || std::strcmp(stage.stackName, "MainView") != 0 ||
        stage.activityCount != 1 || !stage.baseRead || !stage.outputRead || stage.outputPose[12] != 42.0f) return false;
    const Activity& a = stage.activities[0];
    if (!a.weightKnown || a.weight != 1 || !a.stateKnown || !a.poseRead ||
        !a.nameRead || std::strcmp(a.name, "Headlook") != 0) return false;

    Memory cycle = fixture(); cycle.put(kC + 0x40, kC + 0x70);
    auto cycleResult = std::make_unique<Snapshot>(); cycleResult->source = kC + 0x70;
    capture(&Memory::read, &cycle, kBase, *cycleResult);
    if (cycleResult->complete || std::strcmp(cycleResult->failure, "cycle") != 0) return false;

    Memory overflow = fixture(); overflow.put(kC + 0x58, uint64_t{kActivities + 1});
    auto overflowResult = std::make_unique<Snapshot>(); overflowResult->source = kC + 0x70;
    capture(&Memory::read, &overflow, kBase, *overflowResult);
    if (overflowResult->complete || std::strcmp(overflowResult->failure, "activity-bound") != 0) return false;

    Memory partial = fixture(); partial.denied = kC + 0x68;
    auto partialResult = std::make_unique<Snapshot>(); partialResult->source = kC + 0x70;
    capture(&Memory::read, &partial, kBase, *partialResult);
    if (partialResult->complete || std::strcmp(partialResult->failure, "stage-fields-read") != 0) return false;

    Memory changing = fixture(); changing.put(kRoster, kActivity + 8);
    auto changingResult = std::make_unique<Snapshot>(); changingResult->source = kC + 0x70;
    capture(&Memory::read, &changing, kBase, *changingResult);
    if (changingResult->complete || std::strcmp(changingResult->failure, "activity-vtable-read") != 0) return false;

    Memory inactive = nestedFixture(0.0f);
    auto inactiveResult = std::make_unique<Snapshot>(); inactiveResult->source = kC + 0x70;
    capture(&Memory::read, &inactive, kBase, *inactiveResult);
    if (!inactiveResult->complete || inactiveResult->stageCount != 1 ||
        inactiveResult->stages[0].inactiveNestedOmitted != 1 ||
        inactiveResult->stages[0].activityCount != 1) return false;

    Memory active = nestedFixture(1.0f);
    auto activeResult = std::make_unique<Snapshot>(); activeResult->source = kC + 0x70;
    capture(&Memory::read, &active, kBase, *activeResult);
    if (!activeResult->complete || activeResult->stageCount != 12 ||
        activeResult->stages[0].inactiveNestedOmitted != 0) return false;

    Memory unknown = nestedFixture(1.0f);
    unknown.put(kBase + 0x51869B0 + 0x20, kBase + 0x123456);
    auto unknownResult = std::make_unique<Snapshot>(); unknownResult->source = kC + 0x70;
    capture(&Memory::read, &unknown, kBase, *unknownResult);
    if (!unknownResult->complete || unknownResult->stageCount != 12 ||
        unknownResult->stages[0].activities[0].weightKnown) return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    if (!testGate() || !testCapture()) {
        std::fprintf(stderr, "flat stationary camera test failed\n"); return 1;
    }
    std::puts("flat stationary camera test passed");
    return 0;
}
