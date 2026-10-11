#include "plugin_selection_admission.h"
#include "plugin_manifest.inc"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;
using namespace edvr::plugin_selection::admission_v1::test_seam;

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

const std::uint8_t testFingerprint[32] = {0x13, 0x37, 0x42};
constexpr std::uint32_t vrGeneratedGroups[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr std::uint32_t flatGeneratedGroups[] = {0, 7};
GroupV1 readyGroups[] = {{0, 1, 1}, {1, 1, 1}, {2, 2, 2}};
CatalogViewV1 readyCatalog{readyGroups, 3, 2, testFingerprint};

PluginSelectionSnapshotV1 snapshot(PluginSelectionProfileV1 profile = PluginSelectionProfileV1::Vr,
                                   std::uint32_t mask = 0) {
    PluginSelectionSnapshotV1 out{};
    out.structSize = sizeof(out);
    out.abiVersion = kPluginSelectionSnapshotVersionV1;
    out.recordState = static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported);
    out.installedProfile = out.recordProfile = static_cast<std::uint32_t>(profile);
    out.recordSchemaVersion = 1;
    out.recordCatalogSchemaVersion = out.compiledCatalogSchemaVersion = readyCatalog.schema;
    out.recordedPluginMask = mask;
    std::memcpy(out.catalogFingerprint, readyCatalog.fingerprint, 32);
    return out;
}

DecisionV1 decideFixture(const PluginSelectionSnapshotV1* value,
                         PluginSelectionProfileV1 profile = PluginSelectionProfileV1::Vr,
                         CatalogViewV1 catalog = readyCatalog) {
    return decideWithCatalog(S_OK, value, profile, 0x12345000, catalog);
}

void wireAndGeneratedCatalog() {
    const auto wire = contract();
    check(wire.structSize == 64 && wire.abiVersion == 1 &&
          wire.snapshotAbiVersion == kPluginSelectionSnapshotVersionV1 &&
          wire.selectionSchemaVersion == 1 &&
          wire.catalogSchemaVersion == edvr::plugins::kSelectionCatalogSchemaVersion &&
          wire.capabilities == kCapabilities && wire.reserved == 0,
          "contract has the fixed ABI and generated catalog schema");
    check(wire.supportedProfileMask == 0,
          "current generated catalog advertises no profile until every group is ready");
    check(std::memcmp(wire.catalogFingerprint, edvr::plugins::kSelectionCatalogFingerprint, 32) == 0,
          "contract fingerprints the generated catalog");
    auto current = PluginSelectionSnapshotV1{};
    current.structSize = sizeof(current);
    current.abiVersion = kPluginSelectionSnapshotVersionV1;
    current.recordState = static_cast<std::uint32_t>(PluginSelectionSnapshotStateV1::Supported);
    current.installedProfile = current.recordProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Vr);
    current.recordSchemaVersion = 1;
    current.recordCatalogSchemaVersion = current.compiledCatalogSchemaVersion =
        edvr::plugins::kSelectionCatalogSchemaVersion;
    std::memcpy(current.catalogFingerprint, edvr::plugins::kSelectionCatalogFingerprint, 32);
    const auto dormant = decide(S_OK, &current, PluginSelectionProfileV1::Vr, 0x12345000);
    check(dormant.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          dormant.reason == static_cast<std::uint32_t>(Reason::ProfileNotReady) &&
          dormant.selectedPluginMask == 0 && !selected(dormant, 0) &&
          ownerEnabled(dormant, 0),
          "generated all-unready catalog keeps an empty supported mask Legacy");

    const auto remainsLegacy = [&](PluginSelectionSnapshotV1 malformed,
                                   PluginSelectionProfileV1 active) {
        const auto fallback = decide(S_OK, &malformed, active, 0x12345000);
        check(fallback.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
              fallback.reason == static_cast<std::uint32_t>(Reason::InvalidSnapshot),
              "invalid source snapshot cannot disable a Legacy owner");
        const std::uint32_t* expected = active == PluginSelectionProfileV1::Vr ?
            vrGeneratedGroups : flatGeneratedGroups;
        const std::size_t count = active == PluginSelectionProfileV1::Vr ?
            sizeof(vrGeneratedGroups) / sizeof(vrGeneratedGroups[0]) :
            sizeof(flatGeneratedGroups) / sizeof(flatGeneratedGroups[0]);
        for (std::uint32_t i = 0; i < edvr::plugins::kPluginCount; ++i) {
            bool applicable = false;
            for (std::size_t j = 0; j < count; ++j) applicable |= expected[j] == i;
            check(ownerEnabled(fallback, i) == applicable,
                  "Legacy fallback preserves exactly the active profile's generated owners");
        }
    };
    auto malformed = current; malformed.recordedPluginMask = 0x80000000u;
    remainsLegacy(malformed, PluginSelectionProfileV1::Vr);
    malformed = current; malformed.reserved[0] = 1; remainsLegacy(malformed, PluginSelectionProfileV1::Vr);
    malformed = current; malformed.recordSchemaVersion++; remainsLegacy(malformed, PluginSelectionProfileV1::Vr);
    malformed = current; malformed.validationFlags = 1; remainsLegacy(malformed, PluginSelectionProfileV1::Vr);
    malformed = current; malformed.recordProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat);
    remainsLegacy(malformed, PluginSelectionProfileV1::Vr);

    auto flatCurrent = current;
    flatCurrent.installedProfile = flatCurrent.recordProfile =
        static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat);
    malformed = flatCurrent; malformed.recordCatalogSchemaVersion++;
    remainsLegacy(malformed, PluginSelectionProfileV1::Flat);

    const auto unknown = decide(S_OK, &current, static_cast<PluginSelectionProfileV1>(99), 0x12345000);
    check(unknown.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          !ownerEnabled(unknown, 0) && !ownerEnabled(unknown, 7),
          "unknown active profile keeps all profile-specific owners disabled");
    check(!ownerEnabled(dormant, 32) &&
          !ownerEnabled(dormant, (std::numeric_limits<std::uint32_t>::max)()),
          "out-of-range Legacy owner indices are disabled");

    const auto unavailable = decide(E_FAIL, &current, PluginSelectionProfileV1::Vr, 0);
    check(unavailable.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          unavailable.reason == static_cast<std::uint32_t>(Reason::SnapshotUnavailable) &&
          ownerEnabled(unavailable, 0),
          "capture failure preserves Legacy behavior for applicable owners");
}

void readinessAndSelection() {
    check(contractWithCatalog(readyCatalog).supportedProfileMask == 3,
          "all-ready fixture contract supports both complete profiles");
    const auto empty = snapshot();
    auto emptyDecision = decideFixture(&empty);
    check(emptyDecision.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection) &&
          emptyDecision.reason == static_cast<std::uint32_t>(Reason::None),
          "all-ready VR groups admit an empty recorded mask");
    check(!selectedWithCatalog(emptyDecision, 0, readyCatalog) &&
          !ownerEnabledWithCatalog(emptyDecision, 0, readyCatalog),
          "admitted empty mask disables an unselected applicable group");

    readyGroups[1].readyMask = 0;
    check(contractWithCatalog(readyCatalog).supportedProfileMask == 2,
          "contract retains Flat support when only a VR group is unready");
    const auto flatEmptySnapshot = snapshot(PluginSelectionProfileV1::Flat);
    const auto flatEmpty = decideWithCatalog(S_OK, &flatEmptySnapshot,
        PluginSelectionProfileV1::Flat, 0x12345000, readyCatalog);
    check(flatEmpty.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection) &&
          !ownerEnabledWithCatalog(flatEmpty, 2, readyCatalog) &&
          !ownerEnabledWithCatalog(flatEmpty, 0, readyCatalog) &&
          !ownerEnabledWithCatalog(flatEmpty, 1, readyCatalog),
          "Flat empty selection stays admitted while an applicable VR group is unready");
    const auto flatChosenSnapshot = snapshot(PluginSelectionProfileV1::Flat, 1u << 2);
    const auto flatChosen = decideWithCatalog(S_OK, &flatChosenSnapshot,
        PluginSelectionProfileV1::Flat, 0x12345000, readyCatalog);
    check(flatChosen.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection) &&
          selectedWithCatalog(flatChosen, 2, readyCatalog) &&
          ownerEnabledWithCatalog(flatChosen, 2, readyCatalog) &&
          !selectedWithCatalog(flatChosen, 0, readyCatalog) &&
          !selectedWithCatalog(flatChosen, 1, readyCatalog) &&
          !ownerEnabledWithCatalog(flatChosen, 0, readyCatalog) &&
          !ownerEnabledWithCatalog(flatChosen, 1, readyCatalog),
          "Flat selection enables its selected group and keeps VR-only groups absent");
    readyGroups[1].readyMask = 1;

    const auto chosen = snapshot(PluginSelectionProfileV1::Vr, 1u << 1);
    auto selectedDecision = decideFixture(&chosen);
    check(selectedDecision.mode == static_cast<std::uint32_t>(Mode::AdmittedSelection) &&
          !selectedWithCatalog(selectedDecision, 0, readyCatalog) &&
          selectedWithCatalog(selectedDecision, 1, readyCatalog) &&
          !selectedWithCatalog(selectedDecision, 2, readyCatalog) &&
          ownerEnabledWithCatalog(selectedDecision, 1, readyCatalog) &&
          !ownerEnabledWithCatalog(selectedDecision, 0, readyCatalog),
          "admitted mask enables only its selected generated group");
    check(!selectedWithCatalog(selectedDecision, 3, readyCatalog) &&
          !selectedWithCatalog(selectedDecision, 8, readyCatalog) &&
          !selectedWithCatalog(selectedDecision, 31, readyCatalog) &&
          !selectedWithCatalog(selectedDecision, 32, readyCatalog) &&
          !selectedWithCatalog(selectedDecision,
              (std::numeric_limits<std::uint32_t>::max)(), readyCatalog) &&
          !ownerEnabledWithCatalog(selectedDecision, 32, readyCatalog) &&
          !ownerEnabledWithCatalog(selectedDecision,
              (std::numeric_limits<std::uint32_t>::max)(), readyCatalog),
          "out-of-bounds generated indices remain disabled");

    readyGroups[1].readyMask = 0;
    auto notReady = decideFixture(&empty);
    check(notReady.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          notReady.reason == static_cast<std::uint32_t>(Reason::ProfileNotReady) &&
          ownerEnabledWithCatalog(notReady, 0, readyCatalog) &&
          ownerEnabledWithCatalog(notReady, 1, readyCatalog),
          "one unready applicable group keeps the whole profile Legacy");
    readyGroups[1].readyMask = 1;

    const auto providerMissing = decideWithCatalog(S_OK, &empty, PluginSelectionProfileV1::Vr,
                                                   0, readyCatalog);
    check(providerMissing.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          providerMissing.reason == static_cast<std::uint32_t>(Reason::ProviderRejected) &&
          ownerEnabledWithCatalog(providerMissing, 0, readyCatalog),
          "zero graphics provider cannot create unusable admission or suppress Legacy owner");

    readyGroups[1].index = 0;
    notReady = decideFixture(&empty);
    check(notReady.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          !selectedWithCatalog(selectedDecision, 1, readyCatalog),
          "misindexed catalog fails closed");
    readyGroups[1].index = 1;

    auto invalidDecision = selectedDecision;
    ++invalidDecision.reserved[0];
    check(!selectedWithCatalog(invalidDecision, 1, readyCatalog) &&
          !ownerEnabledWithCatalog(invalidDecision, 1, readyCatalog),
          "nonzero reserved decision data cannot select or enable an owner");
    invalidDecision = selectedDecision;
    invalidDecision.captureStatus = S_FALSE;
    check(!selectedWithCatalog(invalidDecision, 1, readyCatalog),
          "only exact S_OK capture may produce usable selection");
    invalidDecision = selectedDecision;
    invalidDecision.reason = static_cast<std::uint32_t>(Reason::ProfileNotReady);
    check(!selectedWithCatalog(invalidDecision, 1, readyCatalog),
          "admitted decisions require the no-error reason");
    invalidDecision = selectedDecision;
    invalidDecision.graphicsProviderBaseLow = 0;
    invalidDecision.graphicsProviderBaseHigh = 0;
    check(!selectedWithCatalog(invalidDecision, 1, readyCatalog),
          "admitted decisions require a graphics provider identity");
    invalidDecision = selectedDecision;
    invalidDecision.selectedPluginMask = 1u << 3;
    check(!selectedWithCatalog(invalidDecision, 1, readyCatalog),
          "decision mask bits outside the catalog cannot select");
    invalidDecision = selectedDecision;
    invalidDecision.selectedPluginMask = 1u << 2;
    check(!selectedWithCatalog(invalidDecision, 2, readyCatalog),
          "decision cannot select a group outside its active profile");
}

void snapshotValidation() {
    auto source = snapshot();
    auto bad = source;
    bad.installedProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat);
    check(decideFixture(&bad).reason == static_cast<std::uint32_t>(Reason::InvalidSnapshot),
          "installed profile mismatch is rejected");
    bad = source;
    bad.recordProfile = static_cast<std::uint32_t>(PluginSelectionProfileV1::Flat);
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "record profile mismatch is rejected");
    bad = source; ++bad.recordSchemaVersion;
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "selection schema mismatch is rejected");
    bad = source; ++bad.recordCatalogSchemaVersion;
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "record catalog schema mismatch is rejected");
    bad = source; ++bad.compiledCatalogSchemaVersion;
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "compiled catalog schema mismatch is rejected");
    bad = source; bad.catalogFingerprint[0] ^= 1;
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "catalog fingerprint mismatch is rejected");
    bad = source; bad.validationFlags = 1;
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "snapshot validation flags are rejected");
    bad = source; bad.reserved[1] = 1;
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "snapshot reserved fields are rejected");
    bad = source; bad.recordedPluginMask = 1u << 3;
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "out-of-catalog mask bit is rejected");
    bad = snapshot(PluginSelectionProfileV1::Vr, 1u << 2);
    check(decideFixture(&bad).mode == static_cast<std::uint32_t>(Mode::Legacy),
          "selected group outside active profile is rejected");

    bad = source; ++bad.recordSchemaVersion;
    const auto malformedLegacy = decideFixture(&bad);
    check(malformedLegacy.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          malformedLegacy.reason == static_cast<std::uint32_t>(Reason::InvalidSnapshot) &&
          ownerEnabledWithCatalog(malformedLegacy, 0, readyCatalog) &&
          ownerEnabledWithCatalog(malformedLegacy, 1, readyCatalog) &&
          !ownerEnabledWithCatalog(malformedLegacy, 2, readyCatalog),
          "malformed synthetic snapshot keeps all and only applicable Legacy owners");
    const auto unknown = decideWithCatalog(S_OK, &source,
        static_cast<PluginSelectionProfileV1>(99), 0x12345000, readyCatalog);
    check(unknown.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          !ownerEnabledWithCatalog(unknown, 0, readyCatalog) &&
          !ownerEnabledWithCatalog(unknown, 2, readyCatalog),
          "unknown synthetic profile cannot enable any owner");

    auto unavailable = decideWithCatalog(S_FALSE, &source, PluginSelectionProfileV1::Vr,
                                         0x12345000, readyCatalog);
    check(unavailable.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          unavailable.reason == static_cast<std::uint32_t>(Reason::SnapshotUnavailable) &&
          !selectedWithCatalog(unavailable, 0, readyCatalog),
          "non-S_OK snapshot capture cannot admit");
    unavailable = decideWithCatalog(S_OK, nullptr, PluginSelectionProfileV1::Vr,
                                    0x12345000, readyCatalog);
    check(unavailable.mode == static_cast<std::uint32_t>(Mode::Legacy) &&
          unavailable.reason == static_cast<std::uint32_t>(Reason::SnapshotUnavailable),
          "missing snapshot stays Legacy");
}
}

int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "--dry-run") != 0 &&
                      std::strcmp(argv[1], "--self-test") != 0)) {
        std::fprintf(stderr, "usage: plugin_selection_admission_test.exe --dry-run|--self-test\n");
        return 2;
    }
    const bool selfTest = std::strcmp(argv[1], "--self-test") == 0;
    wireAndGeneratedCatalog();
    readinessAndSelection();
    snapshotValidation();
    std::puts(selfTest ? "plugin selection admission rules self-test: OK" :
                         "plugin selection admission rules dry-run: OK (no files written)");
    return 0;
}
