#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure, bounded decoding of the game's camera pipeline. The caller supplies
// guarded reads; this code never invokes a game method or changes game memory.
namespace edvr { namespace stationcam {

constexpr unsigned kStages = 96; // root and several nested eleven-child stacks
constexpr unsigned kActivities = 16;
constexpr unsigned kNameBytes = 48;
using Read = bool (*)(void*, uintptr_t, void*, size_t) noexcept;

struct ParkInput {
    bool active = false, gameplay = false, dockedKnown = false, docked = false;
    bool seatedKnown = false, seated = false, guiKnown = false;
    uint32_t gui = 0, statusSamples = 0;
    bool selected = false, menu = false, reset = false; // reset is metadata; current rows remain valid
    uint64_t frame = 0;
    uintptr_t depth = 0;
    float origin[3]{};
};
inline bool qualifiedScene(const ParkInput& in) noexcept {
    if (!in.active || !in.gameplay || !in.dockedKnown || !in.docked ||
        !in.seatedKnown || !in.seated || !in.guiKnown || in.gui != 0 ||
        !in.selected || in.menu || !in.depth) return false;
    for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(in.origin[i])) return false;
    return true;
}
struct ParkGate {
    uint64_t lastFrame = 0;
    uint32_t lastSample = 0, fresh = 0, stable = 0;
    bool sampled = false;
    const char* reason = "no accepted scene frame";
    bool step(const ParkInput& in) noexcept {
        const bool good = qualifiedScene(in);
        if (!good) {
            stable = fresh = 0; lastFrame = 0;
            lastSample = in.statusSamples; sampled = true;
            reason = !in.active ? "journal-watcher-inactive" : !in.gameplay ? "no-LoadGame-in-this-process" :
                !in.dockedKnown ? "docked-unknown" : !in.docked ? "not-docked" :
                !in.seatedKnown ? "seated-unknown" : !in.seated ? "not-seated" :
                !in.guiKnown ? "GuiFocus-unknown" : in.gui != 0 ? "GuiFocus-not-gameplay" :
                !in.selected ? "no-selected-scene" : in.menu ? "menu-scene-contract" :
                !in.depth ? "no-selected-depth" : "selected-origin-nonfinite";
            return false;
        }
        if (!sampled) { lastSample = in.statusSamples; sampled = true; }
        else if (in.statusSamples != lastSample) {
            lastSample = in.statusSamples;
            if (fresh < 2) ++fresh;
        }
        if (lastFrame + 1 == in.frame) ++stable;
        else stable = 1;
        lastFrame = in.frame;
        reason = fresh < 2 ? "waiting-for-two-fresh-Status-samples" :
            stable < 30 ? "waiting-for-30-qualified-scene-frames" : "qualified";
        return fresh >= 2 && stable >= 30;
    }
};

struct Activity {
    uintptr_t iface = 0, vtable = 0, concreteVtable = 0, weightFn = 0, stateFn = 0, state = 0;
    char name[kNameBytes]{};
    bool nameRead = false;
    float weight = 0;
    bool weightKnown = false, stateKnown = false, poseRead = false;
    float pose[16]{}; // three basis rows and positive origin
};
struct Stage {
    uintptr_t child = 0, stack = 0, predecessor = 0, nestedParent = 0, activityArray = 0;
    uintptr_t stackVtable = 0;
    uint64_t activityCount = 0;
    uint32_t mode = 0;
    uint32_t inactiveNestedOmitted = 0;
    int parent = -1; // -1 renderer source, otherwise a predecessor/nested edge
    char edge = 'R';
    char stackName[kNameBytes]{};
    bool nameRead = false, baseRead = false, outputRead = false;
    float basePose[16]{}, outputPose[16]{};
    Activity activities[kActivities]{};
};
struct Snapshot {
    uintptr_t owner = 0, source = 0, helper = 0, rootChild = 0;
    uint64_t callerRva = 0;
    float sourceBefore[4]{}, sourceAfter[4]{}, helperAfter[4]{};
    bool sourceBeforeRead = false, sourceAfterRead = false, helperAfterRead = false;
    bool validSource = false, consistent = true, complete = true;
    const char* failure = "none";
    unsigned stageCount = 0;
    Stage stages[kStages]{};
};

template<class T> inline bool get(Read read, void* context, uintptr_t at, T& value) noexcept {
    return at != 0 && read(context, at, &value, sizeof(value));
}
inline bool bytes(Read read, void* context, uintptr_t at, void* value, size_t count) noexcept {
    return at != 0 && read(context, at, value, count);
}
inline bool finitePose(const float pose[16]) noexcept {
    for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(pose[i])) return false;
    return true;
}
inline bool pose(Read read, void* context, uintptr_t state, float out[16]) noexcept {
    return state && bytes(read, context, state + 0x40, out, sizeof(float) * 16) && finitePose(out);
}
inline uintptr_t rva(uintptr_t address, uintptr_t base) noexcept {
    return address >= base && address - base < 0x6000000 ? address - base : 0;
}
inline bool childGuard(Read read, void* context, uintptr_t base, uintptr_t child) noexcept {
    uintptr_t v0 = 0, v1 = 0;
    return child && get(read, context, child, v0) && get(read, context, child + 0x10, v1) &&
        v0 == base + 0x5185F90 && v1 == base + 0x5185F98;
}
inline void fail(Snapshot& out, const char* why) noexcept {
    out.complete = false;
    if (std::strcmp(out.failure, "none") == 0) out.failure = why;
}
inline bool readName(Read read, void* context, uintptr_t stack, char out[kNameBytes]) noexcept {
    uintptr_t object = 0;
    uint64_t length = 0, capacity = 0;
    if (!get(read, context, stack + 0x20, object) || !object ||
        !get(read, context, object, length) || !get(read, context, object + 8, capacity) ||
        length >= kNameBytes || length > capacity || capacity > 4096) return false;
    if (!bytes(read, context, object + 0x14, out, static_cast<size_t>(length))) return false;
    out[length] = 0;
    for (unsigned i = 0; i < length; ++i) if (static_cast<unsigned char>(out[i]) < 0x20 ||
                                                static_cast<unsigned char>(out[i]) > 0x7e) return false;
    return true;
}
inline bool readCString(Read read, void* context, uintptr_t at, char out[kNameBytes]) noexcept {
    if (!at) return false;
    for (unsigned i = 0; i < kNameBytes - 1; ++i) {
        if (!get(read, context, at + i, out[i])) return false;
        if (!out[i]) return true;
        if (static_cast<unsigned char>(out[i]) < 0x20 || static_cast<unsigned char>(out[i]) > 0x7e) return false;
    }
    out[kNameBytes - 1] = 0;
    return false;
}

inline void activity(Read read, void* context, uintptr_t base, uintptr_t iface, Activity& out,
                     Snapshot& snapshot) noexcept {
    out.iface = iface;
    if (!iface || !get(read, context, iface, out.vtable) || !out.vtable ||
        !get(read, context, out.vtable + 0x20, out.weightFn) ||
        !get(read, context, out.vtable + 0x28, out.stateFn)) {
        fail(snapshot, "activity-vtable-read"); return;
    }
    if (out.weightFn == base + 0x8BE920) {
        uint8_t enabled = 0;
        if (iface < 4 || !get(read, context, iface - 4, enabled)) fail(snapshot, "leaf-weight-read");
        else { out.weightKnown = true; out.weight = enabled ? 1.0f : 0.0f; }
    } else if (out.weightFn == base + 0x108ADE0) {
        uintptr_t config = 0;
        if (iface < 0xA0 || !get(read, context, iface - 0xA0, config) ||
            !get(read, context, config + 0x34, out.weight) || !std::isfinite(out.weight))
            fail(snapshot, "nested-weight-read");
        else out.weightKnown = true;
    }
    if (out.stateFn == base + 0x5293E0) {
        out.state = iface + 0x10;
        out.stateKnown = true;
        uintptr_t name = 0;
        if (get(read, context, iface + 8, name)) out.nameRead = readCString(read, context, name, out.name);
        if (iface >= 0x20) get(read, context, iface - 0x20, out.concreteVtable);
    } else if (out.stateFn == base + 0x108A98C) {
        if (out.vtable != base + 0x51869B0) { fail(snapshot, "nested-activity-vtable"); return; }
        uintptr_t nestedChild = 0;
        if (iface < 0x98 || !get(read, context, iface - 0x98 + 0x78, nestedChild) ||
            !childGuard(read, context, base, nestedChild)) fail(snapshot, "nested-state-read");
        else { out.state = nestedChild + 0x70; out.stateKnown = true; }
    }
    if (out.stateKnown) {
        out.poseRead = pose(read, context, out.state, out.pose);
        if (!out.poseRead) fail(snapshot, "activity-pose-read");
    }
}

inline void walk(Read read, void* context, uintptr_t base, uintptr_t child, int parent,
                 char edge, Snapshot& out) noexcept {
    if (!childGuard(read, context, base, child)) { fail(out, "child-guard"); return; }
    for (unsigned i = 0; i < out.stageCount; ++i) if (out.stages[i].child == child) {
        for (int p = parent; p >= 0; p = out.stages[p].parent)
            if (p == static_cast<int>(i)) { fail(out, "cycle"); return; }
        return; // an inherited parent already present in the bounded roster
    }
    if (out.stageCount == kStages) { fail(out, "stage-bound"); return; }
    const unsigned index = out.stageCount++;
    Stage& stage = out.stages[index];
    stage.child = child; stage.parent = parent; stage.edge = edge;
    uintptr_t terminal = 0;
    if (!get(read, context, child + 0x30, stage.stack) || !stage.stack ||
        !get(read, context, stage.stack, stage.stackVtable) ||
        (stage.stackVtable != base + 0x5186290 && stage.stackVtable != base + 0x5186938) ||
        !get(read, context, stage.stack + 0x78, terminal) || !childGuard(read, context, base, terminal)) {
        fail(out, "stack-identity"); return;
    }
    unsigned membership = 0;
    for (unsigned j = 0; j < 11; ++j) {
        uintptr_t memberChild = 0;
        if (!get(read, context, stage.stack + 0x28 + 8 * j, memberChild)) {
            fail(out, "child-array-read"); return;
        }
        membership += memberChild == child ? 1u : 0u;
    }
    if (membership != 1) { fail(out, "child-membership"); return; }
    if (edge == 'R' || edge == 'N') {
        if (terminal != child) { fail(out, "provider-not-terminal"); return; }
    }
    stage.nameRead = readName(read, context, stage.stack, stage.stackName);
    if (!stage.nameRead) fail(out, "stack-name-read");
    if (!get(read, context, child + 0x40, stage.predecessor) ||
        !get(read, context, child + 0x58, stage.activityCount) ||
        !get(read, context, child + 0x60, stage.activityArray) ||
        !get(read, context, child + 0x68, stage.mode)) {
        fail(out, "stage-fields-read"); return;
    }
    stage.outputRead = pose(read, context, child + 0x70, stage.outputPose);
    stage.baseRead = pose(read, context, stage.predecessor, stage.basePose);
    if (!stage.outputRead || !stage.baseRead) fail(out, "stage-pose-read");
    if (stage.activityCount > kActivities) { fail(out, "activity-bound"); return; }
    if (stage.activityCount && !stage.activityArray) { fail(out, "activity-array-null"); return; }
    for (unsigned j = 0; j < stage.activityCount; ++j) {
        uintptr_t iface = 0, again = 0;
        if (!get(read, context, stage.activityArray + 8 * j, iface)) {
            fail(out, "activity-pointer-read"); continue;
        }
        activity(read, context, base, iface, stage.activities[j], out);
        if (!get(read, context, stage.activityArray + 8 * j, again) || again != iface) {
            out.consistent = false; fail(out, "activity-pointer-changed");
        }
    }
    uintptr_t stackAgain = 0, predecessorAgain = 0, arrayAgain = 0, terminalAgain = 0;
    uint64_t countAgain = 0; uint32_t modeAgain = 0;
    if (!get(read, context, child + 0x30, stackAgain) ||
        !get(read, context, child + 0x40, predecessorAgain) ||
        !get(read, context, child + 0x58, countAgain) ||
        !get(read, context, child + 0x60, arrayAgain) ||
        !get(read, context, child + 0x68, modeAgain) ||
        !get(read, context, stage.stack + 0x78, terminalAgain) ||
        stackAgain != stage.stack || predecessorAgain != stage.predecessor ||
        countAgain != stage.activityCount || arrayAgain != stage.activityArray ||
        modeAgain != stage.mode || terminalAgain != terminal) {
        out.consistent = false; fail(out, "stage-fields-changed");
    }
    // Nested activities can source a whole other eleven-child stack. Include
    // that selected child, then the inherited predecessor edge below.
    for (unsigned j = 0; j < stage.activityCount; ++j) {
        const Activity& a = stage.activities[j];
        if (a.stateFn == base + 0x108A98C && a.stateKnown) {
            if (a.weightKnown && a.weight <= 0.0f) ++stage.inactiveNestedOmitted;
            else walk(read, context, base, a.state - 0x70, static_cast<int>(index), 'N', out);
        }
    }
    if (stage.predecessor == stage.stack + 0xB8 && stage.stackVtable == base + 0x5186290) return;
    if (stage.predecessor >= 0x70 && childGuard(read, context, base, stage.predecessor - 0x70))
        walk(read, context, base, stage.predecessor - 0x70, static_cast<int>(index), 'P', out);
    else if (stage.stackVtable == base + 0x5186938) {
        uintptr_t parentChild = 0, parentBase = 0;
        if (!get(read, context, stage.stack + 0x1B8, parentChild) ||
            !childGuard(read, context, base, parentChild) ||
            !get(read, context, parentChild + 0x40, parentBase) ||
            parentBase != stage.predecessor) fail(out, "nested-parent-base");
        else {
            stage.nestedParent = parentChild;
            uintptr_t parentStack = 0, parentStackVtable = 0;
            if (!get(read, context, parentChild + 0x30, parentStack) ||
                !get(read, context, parentStack, parentStackVtable) ||
                parentStackVtable != base + 0x5186290 || parentBase != parentStack + 0xB8)
                fail(out, "nested-parent-baseline-unknown");
        }
    } else fail(out, "predecessor-unresolved");
}

inline void capture(Read read, void* context, uintptr_t base, Snapshot& out) noexcept {
    if (!out.source || out.source < 0x70) { fail(out, "source-null"); return; }
    out.rootChild = out.source - 0x70;
    if (!childGuard(read, context, base, out.rootChild)) { fail(out, "source-child-guard"); return; }
    out.validSource = true;
    walk(read, context, base, out.rootChild, -1, 'R', out);
}

} } // namespace edvr::stationcam
