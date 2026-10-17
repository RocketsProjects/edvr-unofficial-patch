#include "src/d3d11/flat_stationary_camera_core.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <unordered_map>

using namespace edvr::stationcam;

struct Memory {
    std::unordered_map<uintptr_t, uint8_t> cells;
    uintptr_t denied = 0;
    uintptr_t changeOnRead = 0, changeAddress = 0;
    uintptr_t changeValue = 0;
    unsigned watchedReads = 0, changeAfter = 0;
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
        if (at == m.changeOnRead && ++m.watchedReads == m.changeAfter)
            m.put(m.changeAddress, m.changeValue);
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
constexpr uintptr_t kPlacedRecord = 0x31000000, kProvider = 0x32000000;
constexpr uintptr_t kConfig = 0x33000000, kOverride = 0x34000000, kTarget = 0x35000000;

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

Memory placedFixture() {
    Memory m = fixture();
    const uintptr_t object = kActivity - 0x20;
    const uintptr_t placedVtable = kBase + 0x5189EA8;
    m.put(kActivity, placedVtable);
    m.put(object, kBase + 0x5189E38);
    m.put(placedVtable + 0x20, kBase + 0x8BE920);
    m.put(placedVtable + 0x28, kBase + 0x5293E0);
    m.put(object + 0x1E8, kPlacedRecord);
    m.put(kPlacedRecord + 8, kTarget);
    m.put(kPlacedRecord + 0x10, kProvider);
    m.put(kProvider, kBase + 0x530B340); // class is observed, semantics unknown
    m.put(kBase + 0x530B340 + 0x20, kBase + 0x5293E0);
    m.put(kTarget + 0xC0, int32_t{8});
    m.put(object + 0x10, kConfig);
    m.put(kConfig + 0x14, int32_t{1});
    m.put(kConfig + 0x18, int32_t{0});
    m.put(object + 0x188, int32_t{-1});
    m.put(object + 0x190, kOverride);
    m.put(kOverride, kBase + 0x5355318);
    m.put(kBase + 0x5355318 + 0x70, kBase + 0x123456);
    float matrix[16]{};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    matrix[12] = 10.0f;
    m.putBytes(kPlacedRecord + 0x20, matrix, sizeof(matrix));
    matrix[12] = 20.0f;
    m.putBytes(kPlacedRecord + 0x60, matrix, sizeof(matrix));
    matrix[12] = 21.0f;
    m.putBytes(object + 0x70, matrix, sizeof(matrix));
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

bool testPlaced() {
    Memory m = placedFixture();
    auto s = std::make_unique<Snapshot>(); s->source = kC + 0x70;
    capture(&Memory::read, &m, kBase, *s);
    if (!s->complete || s->placedSeen != 1 || s->placedCount != 1 || s->placedOverflow) return false;
    const PlacedDetail& d = s->placed[0];
    if (!d.identityComplete || !d.providerPresent || !d.providerKnown || !d.overridePresent ||
        d.provider != kProvider || d.providerVtable != kBase + 0x530B340 ||
        d.providerFn != kBase + 0x5293E0 || d.overrideFn != kBase + 0x123456 ||
        d.targetRef != kTarget || !d.targetStateRead || d.targetState != 8 ||
        !d.validatorKnown || !d.valid5 || !d.valid8 || !d.overrideEligible || !d.poseStable ||
        !d.localRead || !d.cacheRead || !d.finalRead ||
        d.local[12] != 10 || d.cache[12] != 20 || d.final[12] != 21) return false;

    Memory noProvider = placedFixture(); noProvider.put(kPlacedRecord + 0x10, uintptr_t{0});
    auto nullResult = std::make_unique<Snapshot>(); nullResult->source = kC + 0x70;
    capture(&Memory::read, &noProvider, kBase, *nullResult);
    if (nullResult->placedCount != 1 || !nullResult->placed[0].identityComplete ||
        nullResult->placed[0].providerPresent ||
        std::strcmp(nullResult->placed[0].reason, "provider-null") != 0) return false;

    Memory unknownProvider = placedFixture();
    unknownProvider.put(kProvider, kBase + 0x5310000);
    unknownProvider.put(kBase + 0x5310000 + 0x20, kBase + 0x100000);
    auto unknownProviderResult = std::make_unique<Snapshot>(); unknownProviderResult->source = kC + 0x70;
    capture(&Memory::read, &unknownProvider, kBase, *unknownProviderResult);
    if (!unknownProviderResult->placed[0].identityComplete ||
        !unknownProviderResult->placed[0].providerPresent ||
        unknownProviderResult->placed[0].providerKnown) return false;

    Memory noRecord = placedFixture(); noRecord.put(kActivity - 0x20 + 0x1E8, uintptr_t{0});
    auto noRecordResult = std::make_unique<Snapshot>(); noRecordResult->source = kC + 0x70;
    capture(&Memory::read, &noRecord, kBase, *noRecordResult);
    if (noRecordResult->placedCount != 1 || !noRecordResult->placed[0].identityComplete ||
        noRecordResult->placed[0].record || noRecordResult->placed[0].providerPresent ||
        noRecordResult->placed[0].localRead || noRecordResult->placed[0].cacheRead ||
        !noRecordResult->placed[0].finalRead || !noRecordResult->placed[0].eligibilityKnown ||
        noRecordResult->placed[0].overrideEligible ||
        std::strcmp(noRecordResult->placed[0].reason, "record-null") != 0) return false;

    Memory noTarget = placedFixture(); noTarget.put(kPlacedRecord + 8, uintptr_t{0});
    auto noTargetResult = std::make_unique<Snapshot>(); noTargetResult->source = kC + 0x70;
    capture(&Memory::read, &noTarget, kBase, *noTargetResult);
    if (!noTargetResult->placed[0].identityComplete || noTargetResult->placed[0].targetRef ||
        noTargetResult->placed[0].targetStateRead || !noTargetResult->placed[0].validatorKnown ||
        !noTargetResult->placed[0].eligibilityKnown ||
        noTargetResult->placed[0].valid5 || noTargetResult->placed[0].valid8 ||
        noTargetResult->placed[0].overrideEligible) return false;

    const int32_t states[] = {-1, 4, 5, 7, 8};
    for (const int32_t state : states) {
        Memory boundary = placedFixture(); boundary.put(kTarget + 0xC0, state);
        auto boundaryResult = std::make_unique<Snapshot>(); boundaryResult->source = kC + 0x70;
        capture(&Memory::read, &boundary, kBase, *boundaryResult);
        const auto& value = boundaryResult->placed[0];
        if (!value.identityComplete || !value.targetStateRead || value.targetState != state ||
            value.valid5 != (state >= 5) || value.valid8 != (state >= 8) ||
            !value.eligibilityKnown || value.overrideEligible != (state >= 8)) return false;
    }

    Memory unreadable = placedFixture(); unreadable.denied = kProvider;
    auto unreadableResult = std::make_unique<Snapshot>(); unreadableResult->source = kC + 0x70;
    capture(&Memory::read, &unreadable, kBase, *unreadableResult);
    if (unreadableResult->placedCount != 1 || unreadableResult->placed[0].identityComplete ||
        std::strcmp(unreadableResult->placed[0].reason, "provider-vtable-read") != 0) return false;

    Memory changed = placedFixture();
    changed.changeOnRead = kPlacedRecord + 0x10;
    changed.changeAddress = kPlacedRecord + 0x10;
    changed.changeValue = kProvider + 8;
    changed.changeAfter = 2;
    auto changedResult = std::make_unique<Snapshot>(); changedResult->source = kC + 0x70;
    capture(&Memory::read, &changed, kBase, *changedResult);
    if (changedResult->placedCount != 1 || changedResult->placed[0].identityComplete ||
        changedResult->placed[0].eligibilityKnown ||
        std::strcmp(changedResult->placed[0].reason, "identity-changed") != 0) return false;

    Memory movingPose = placedFixture();
    movingPose.changeOnRead = kPlacedRecord + 0x20;
    movingPose.changeAddress = kPlacedRecord + 0x20;
    movingPose.changeValue = uintptr_t{0};
    movingPose.changeAfter = 2;
    auto movingResult = std::make_unique<Snapshot>(); movingResult->source = kC + 0x70;
    capture(&Memory::read, &movingPose, kBase, *movingResult);
    if (movingResult->placedCount != 1 || !movingResult->placed[0].identityComplete ||
        !movingResult->placed[0].localRead || movingResult->placed[0].poseStable) return false;

    Memory targetUnreadable = placedFixture(); targetUnreadable.denied = kTarget + 0xC0;
    auto targetResult = std::make_unique<Snapshot>(); targetResult->source = kC + 0x70;
    capture(&Memory::read, &targetUnreadable, kBase, *targetResult);
    if (targetResult->placedCount != 1 || targetResult->placed[0].identityComplete ||
        targetResult->placed[0].validatorKnown || targetResult->placed[0].eligibilityKnown ||
        std::strcmp(targetResult->placed[0].reason, "target-state-read") != 0) return false;

    Memory targetUnreadableButQAbsent = placedFixture();
    targetUnreadableButQAbsent.denied = kTarget + 0xC0;
    targetUnreadableButQAbsent.put(kActivity - 0x20 + 0x190, uintptr_t{0});
    auto knownFalseResult = std::make_unique<Snapshot>(); knownFalseResult->source = kC + 0x70;
    capture(&Memory::read, &targetUnreadableButQAbsent, kBase, *knownFalseResult);
    if (!knownFalseResult->placed[0].eligibilityKnown || knownFalseResult->placed[0].overrideEligible ||
        knownFalseResult->placed[0].identityComplete) return false;

    Memory targetUnreadableButCfgZero = placedFixture();
    targetUnreadableButCfgZero.denied = kTarget + 0xC0;
    targetUnreadableButCfgZero.put(kConfig + 0x14, int32_t{0});
    auto cfgFalseResult = std::make_unique<Snapshot>(); cfgFalseResult->source = kC + 0x70;
    capture(&Memory::read, &targetUnreadableButCfgZero, kBase, *cfgFalseResult);
    if (!cfgFalseResult->placed[0].eligibilityKnown || cfgFalseResult->placed[0].overrideEligible ||
        cfgFalseResult->placed[0].identityComplete) return false;

    Memory cfg18 = placedFixture(); cfg18.put(kConfig + 0x18, int32_t{1});
    auto cfg18Result = std::make_unique<Snapshot>(); cfg18Result->source = kC + 0x70;
    capture(&Memory::read, &cfg18, kBase, *cfg18Result);
    if (!cfg18Result->placed[0].identityComplete || cfg18Result->placed[0].config18 != 1 ||
        !cfg18Result->placed[0].overrideEligible) return false;

    Memory rootGuard = placedFixture(); rootGuard.put(kActivity - 0x20, kBase + 0x12345);
    auto guardResult = std::make_unique<Snapshot>(); guardResult->source = kC + 0x70;
    capture(&Memory::read, &rootGuard, kBase, *guardResult);
    if (guardResult->placedSeen != 1 || guardResult->placed[0].identityComplete ||
        std::strcmp(guardResult->placed[0].reason, "placed-root-guard-refused") != 0) return false;

    Memory noOverride = placedFixture(); noOverride.put(kActivity - 0x20 + 0x190, uintptr_t{0});
    auto noOverrideResult = std::make_unique<Snapshot>(); noOverrideResult->source = kC + 0x70;
    capture(&Memory::read, &noOverride, kBase, *noOverrideResult);
    if (noOverrideResult->placedCount != 1 || !noOverrideResult->placed[0].identityComplete ||
        noOverrideResult->placed[0].overridePresent || !noOverrideResult->placed[0].eligibilityKnown ||
        noOverrideResult->placed[0].overrideEligible) return false;

    Memory noPlaced = fixture();
    auto absentResult = std::make_unique<Snapshot>(); absentResult->source = kC + 0x70;
    capture(&Memory::read, &noPlaced, kBase, *absentResult);
    if (!absentResult->complete || absentResult->placedSeen || absentResult->placedCount) return false;

    auto bounded = std::make_unique<Snapshot>();
    bounded->placedCount = kPlacedDetails;
    bounded->placedSeen = kPlacedDetails;
    placedDetail(&Memory::read, &m, kBase, s->stages[0].activities[0], 0, 0, *bounded);
    if (!bounded->placedOverflow || bounded->placedSeen != kPlacedDetails + 1 ||
        bounded->placedCount != kPlacedDetails) return false;
    return true;
}

bool testReportMarkers() {
    // The one-shot report must distinguish hook-never-ran and missing/partial
    // Placed detail from a successful renderer-source identity capture.
    std::FILE* file = std::fopen("src/d3d11/flat_stationary_camera_capture.cpp", "rb");
    if (!file) return false;
    std::string source;
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) source.append(buffer, count);
    const bool readOkay = std::ferror(file) == 0;
    std::fclose(file);
    return readOkay &&
        source.find("renderer-hook-never-ran") != std::string::npos &&
        source.find("Placed-not-observed") != std::string::npos &&
        source.find("Placed-bound") != std::string::npos &&
        source.find("Placed-identity-incomplete") != std::string::npos &&
        source.find("detail-complete=%u") != std::string::npos &&
        source.find("placed-overflow=%u") != std::string::npos &&
        source.find("flat stationary Placed-matrix[") != std::string::npos &&
        source.find("selected GPU donor UNPROVED") != std::string::npos;
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    if (!testGate() || !testCapture() || !testPlaced() || !testReportMarkers()) {
        std::fprintf(stderr, "flat stationary camera test failed\n"); return 1;
    }
    std::puts("flat stationary camera test passed");
    return 0;
}
