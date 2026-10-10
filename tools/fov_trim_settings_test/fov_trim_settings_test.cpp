#include "../../src/common/config.h"
#include "../../src/plugins/performance/fov_trim_settings_policy.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using edvr::plugins::performance::FovTrimSettingsInputV1;
using edvr::plugins::performance::FovTrimSettingsNoteV1;
using edvr::plugins::performance::FovTrimSettingsOutputV1;
using edvr::plugins::performance::FovTrimSettingsPolicyOpsV1;

struct SeenNote {
    FovTrimSettingsNoteV1 kind{};
    uint32_t keyIndex = 0;
    std::string keyName, token, headsetKey, entries;
    uint32_t vertical = 0, outer = 0, nasal = 0;
};

struct Notes {
    std::vector<SeenNote> values;
};

static void note(void* context, FovTrimSettingsNoteV1 kind, uint32_t keyIndex,
                 const char* keyName, const char* token, const char* headsetKey,
                 uint32_t vertical, uint32_t outer, uint32_t nasal,
                 const char* entries) {
    auto& notes = *static_cast<Notes*>(context);
    notes.values.push_back({kind, keyIndex, keyName ? keyName : "",
                            token ? token : "", headsetKey ? headsetKey : "",
                            entries ? entries : "", vertical, outer, nasal});
}

static size_t count(const Notes& notes, FovTrimSettingsNoteV1 kind) {
    size_t result = 0;
    for (const SeenNote& value : notes.values) if (value.kind == kind) ++result;
    return result;
}

static FovTrimSettingsInputV1 input(const char* runtime, const char* system,
                                    bool haveHeadset = true) {
    FovTrimSettingsInputV1 value{};
    value.structSize = sizeof(value);
    value.version = edvr::plugins::performance::kFovTrimSettingsPolicyVersion1;
    value.haveHeadset = haveHeadset ? 1u : 0u;
    if (runtime) std::strncpy(value.runtimeToken, runtime, sizeof(value.runtimeToken) - 1);
    if (system) std::strncpy(value.systemToken, system, sizeof(value.systemToken) - 1);
    return value;
}

static void setTrim(const char* vertical, const char* outer, const char* nasal) {
    edvr::Config::get().set("experimental.fov_trim_vertical", vertical);
    edvr::Config::get().set("experimental.fov_trim_outer", outer);
    edvr::Config::get().set("experimental.fov_trim_nasal", nasal);
}

static bool resolve(const FovTrimSettingsPolicyOpsV1& ops, void* state,
                    Notes& notes, const FovTrimSettingsInputV1& request,
                    FovTrimSettingsOutputV1* output) {
    return ops.resolve(state, &notes, &note, &request, output);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("fov_trim_settings_test: dry-run (no Config init, resolver, callbacks, or writes)");
        return 0;
    }
    if (std::strcmp(argv[1], "--self-test")) return 2;

    unsigned checks = 0, failures = 0;
    const auto check = [&](bool condition, const char* description) {
        ++checks;
        if (!condition) {
            ++failures;
            std::printf("FAIL: %s\n", description);
        }
    };

    const auto* ops = edvr::plugins::performance::fovTrimSettingsPolicyOpsV1();
    check(ops && ops->structSize == sizeof(*ops) &&
              ops->version == edvr::plugins::performance::kFovTrimSettingsOpsVersion1 &&
              ops->create && ops->destroy && ops->resolve,
          "factory returns the fixed v1 create/destroy/resolve table");
    if (!ops || !ops->create || !ops->destroy || !ops->resolve) return 1;
    void* state = ops->create();
    check(state != nullptr, "selected test instance creates private policy state");
    if (!state) return 1;

    Notes notes;
    auto call = [&](const FovTrimSettingsInputV1& request) {
        FovTrimSettingsOutputV1 answer{sizeof(FovTrimSettingsOutputV1),
                                       edvr::plugins::performance::kFovTrimSettingsPolicyVersion1,
                                       {0xffffffffu, 0xffffffffu, 0xffffffffu}};
        const bool ok = resolve(*ops, state, notes, request, &answer);
        check(ok, "valid policy request resolves");
        check(answer.structSize == sizeof(answer) &&
                  answer.version == edvr::plugins::performance::kFovTrimSettingsPolicyVersion1,
              "resolver returns the exact v1 output shape");
        return answer;
    };

    // Config::set is memory-only. This rig intentionally never calls Config::init.
    setTrim(nullptr, nullptr, nullptr);
    auto answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(answer.degrees[0] == 0 && answer.degrees[1] == 0 && answer.degrees[2] == 0,
          "unset keys resolve to zero degrees");
    const size_t resolvedAfterUnset = count(notes, FovTrimSettingsNoteV1::ResolvedEntries);
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(count(notes, FovTrimSettingsNoteV1::ResolvedEntries) == resolvedAfterUnset,
          "same labels and same unset config reuse the cached result and note cadence");
    setTrim("", "", "");
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(answer.degrees[0] == 0 && answer.degrees[1] == 0 && answer.degrees[2] == 0,
          "explicitly empty keys match the zero-default behavior");

    setTrim("pimax-openxr:4,pimax-openxr/pimax-crystal-super:11",
            "pimax-openxr/pimax-other:12,pimax-openxr:7",
            "pimax-openxr/pimax-crystal-super:3");
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(answer.degrees[0] == 11 && answer.degrees[1] == 7 && answer.degrees[2] == 3,
          "exact runtime/system entries beat runtime-only entries and mismatched systems fall back only to runtime-only");
    check(!notes.values.empty() &&
              notes.values.back().kind == FovTrimSettingsNoteV1::ResolvedEntries &&
              notes.values.back().headsetKey == "pimax-openxr/pimax-crystal-super" &&
              notes.values.back().vertical == 11 && notes.values.back().outer == 7 &&
              notes.values.back().nasal == 3 &&
              notes.values.back().entries ==
                  "vertical pimax-openxr:4, pimax-openxr/pimax-crystal-super:11; "
                  "outer pimax-openxr/pimax-other:12, pimax-openxr:7; "
                  "nasal pimax-openxr/pimax-crystal-super:3",
          "resolved diagnostic preserves exact entry text, headset identity, and vertical/outer/nasal order");

    setTrim("pimax-openxr:9", "", "");
    answer = call(input("pimax-openxr", "meta-quest-3"));
    check(answer.degrees[0] == 9 && answer.degrees[1] == 0 && answer.degrees[2] == 0,
          "runtime-only entry matches a headset on the same runtime");
    setTrim("pimax-openxr/pimax-crystal-super:8", "", "");
    answer = call(input("pimax-openxr", "meta-quest-3"));
    check(answer.degrees[0] == 0,
          "runtime/system entry for another headset is not used as a runtime fallback");

    setTrim("pimax-openxr:6", "", "");
    const size_t noHeadsetBefore = count(notes, FovTrimSettingsNoteV1::NoHeadset);
    answer = call(input(nullptr, nullptr, false));
    check(answer.degrees[0] == 0 && answer.degrees[1] == 0 && answer.degrees[2] == 0 &&
              count(notes, FovTrimSettingsNoteV1::NoHeadset) == noHeadsetBefore + 1,
          "saved entries with no published headset apply no trim and note once");
    answer = call(input(nullptr, nullptr, false));
    check(count(notes, FovTrimSettingsNoteV1::NoHeadset) == noHeadsetBefore + 1,
          "same no-headset state does not repeat its diagnostic");
    setTrim("", "", "");
    (void)call(input(nullptr, nullptr, false));
    setTrim("pimax-openxr:6", "", "");
    (void)call(input(nullptr, nullptr, false));
    check(count(notes, FovTrimSettingsNoteV1::NoHeadset) == noHeadsetBefore + 1,
          "no-headset note cadence persists through empty and then nonempty lists");
    (void)call(input("pimax-openxr", "pimax-crystal-super"));
    answer = call(input(nullptr, nullptr, false));
    check(count(notes, FovTrimSettingsNoteV1::NoHeadset) == noHeadsetBefore + 2,
          "a known-headset note clears no-headset cadence for the next transition");

    setTrim("5, nope, pimax-openxr:31, :6, pimax-openxr:8", "", "");
    const size_t ignoredBefore = count(notes, FovTrimSettingsNoteV1::IgnoredEntry);
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(answer.degrees[0] == 8,
          "bare, malformed, and out-of-range tokens are ignored while the later valid entry applies");
    check(count(notes, FovTrimSettingsNoteV1::IgnoredEntry) == ignoredBefore + 4,
          "the four distinct invalid forms produce four ignored-entry notes");
    bool exactBadForms = true;
    const char* badForms[] = {"5", "nope", "pimax-openxr:31", ":6"};
    for (const char* bad : badForms) {
        bool found = false;
        for (const SeenNote& seen : notes.values) {
            if (seen.kind == FovTrimSettingsNoteV1::IgnoredEntry &&
                seen.keyName == "experimental.fov_trim_vertical" && seen.token == bad) found = true;
        }
        exactBadForms = exactBadForms && found;
    }
    check(exactBadForms, "ignored-entry callback names the exact key and trimmed token");

    setTrim("h0:1,h1:2,h2:3,h3:4,h4:5,h5:6,h6:7,h7:8,h8:9", "", "");
    const size_t ignoredAtCapacity = count(notes, FovTrimSettingsNoteV1::IgnoredEntry);
    answer = call(input("h8", ""));
    check(answer.degrees[0] == 0 &&
              count(notes, FovTrimSettingsNoteV1::IgnoredEntry) == ignoredAtCapacity + 1,
          "only eight valid entries are retained and the ninth valid token is diagnosed");
    bool ninthNamed = false;
    for (const SeenNote& seen : notes.values)
        if (seen.kind == FovTrimSettingsNoteV1::IgnoredEntry && seen.token == "h8:9")
            ninthNamed = true;
    check(ninthNamed,
          "the ninth over-capacity entry is the one named");

    std::string manyBad;
    for (unsigned i = 0; i < 27; ++i) {
        if (i) manyBad += ',';
        manyBad += "bad" + std::to_string(i);
    }
    setTrim(manyBad.c_str(), "", "");
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    const size_t cappedIgnored = count(notes, FovTrimSettingsNoteV1::IgnoredEntry);
    check(cappedIgnored == 32,
          "distinct malformed tokens stop at the process-state diagnostic cap of 32");
    setTrim("bad0,bad1,late-invalid", "", "");
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(count(notes, FovTrimSettingsNoteV1::IgnoredEntry) == 32,
          "existing malformed tokens deduplicate and diagnostics stop after the cap");

    // A changed setting and a changed headset both invalidate the cached triple.
    setTrim("pimax-openxr:6", "", "");
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(answer.degrees[0] == 6,
          "changed config text is resolved without restarting the policy");
    const size_t resolvedBeforeSameTriple = count(notes, FovTrimSettingsNoteV1::ResolvedEntries);
    setTrim("pimax-openxr:6,other-runtime:1", "", "");
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(answer.degrees[0] == 6 &&
              count(notes, FovTrimSettingsNoteV1::ResolvedEntries) == resolvedBeforeSameTriple,
          "changed config that preserves headset and resolved triple reparses without another resolved note");
    setTrim("pimax-openxr:6,other-runtime:2", "", "");
    answer = call(input("pimax-openxr", "pimax-crystal-super"));
    check(answer.degrees[0] == 6 &&
              count(notes, FovTrimSettingsNoteV1::ResolvedEntries) == resolvedBeforeSameTriple,
          "repeated changed text still preserves legacy note cadence when the resolved triple is unchanged");
    setTrim("pimax-openxr:6,virtualdesktopxr/meta-quest-3:5", "", "");
    answer = call(input("virtualdesktopxr", "meta-quest-3"));
    check(answer.degrees[0] == 5,
          "changed headset labels resolve that headset's entry");

    FovTrimSettingsInputV1 malformed = input("pimax-openxr", "pimax-crystal-super");
    malformed.structSize -= 1;
    FovTrimSettingsOutputV1 rejected{sizeof(FovTrimSettingsOutputV1),
                                     edvr::plugins::performance::kFovTrimSettingsPolicyVersion1,
                                     {71, 72, 73}};
    check(!resolve(*ops, state, notes, malformed, &rejected) &&
              rejected.structSize == sizeof(rejected) && rejected.version ==
                  edvr::plugins::performance::kFovTrimSettingsPolicyVersion1 &&
              rejected.degrees[0] == 0 && rejected.degrees[1] == 0 && rejected.degrees[2] == 0,
          "malformed input is rejected with a zeroed, versioned output");

    ops->destroy(state);
    std::printf("fov_trim_settings_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
