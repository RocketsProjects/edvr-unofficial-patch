#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "../../src/d3d11/fss_panel.h"
#include "../../src/d3d11/fss_reveal.h"
#include "../../src/plugins/comfort/scanners/scanner_claim.h"

namespace {

unsigned g_checks = 0;
unsigned g_failures = 0;
std::string g_order;
unsigned g_panelCalls = 0;
unsigned g_panelObservedCalls = 0;
unsigned g_revealCalls = 0;
unsigned g_revealObservedCalls = 0;
bool g_panelResult = false;
bool g_revealResult = false;
bool g_revealFaultAfterHash = false;
bool g_throwLatch = false;
bool g_latchResult = false;
char g_lastKind = 0;
std::uint32_t g_lastCount = 0;
std::uint32_t g_lastInstances = 0;

void check(bool ok, const char* message) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", message);
    }
}

template <class T>
bool wasRead(const edvr::FssRead<T>& value, T expected) {
    return value.reached && value.known && value.value == expected;
}

template <class T>
bool wasNotRead(const edvr::FssRead<T>& value) {
    return !value.reached && !value.known;
}

void reset() {
    g_order.clear();
    g_panelCalls = 0;
    g_panelObservedCalls = 0;
    g_revealCalls = 0;
    g_revealObservedCalls = 0;
    g_panelResult = false;
    g_revealResult = false;
    g_revealFaultAfterHash = false;
    g_throwLatch = false;
    g_latchResult = false;
    g_lastKind = 0;
    g_lastCount = 0;
    g_lastInstances = 0;
    edvr::detail::g_fssPanelEnabled = false;
    edvr::detail::g_fssRevealSteady = false;
    edvr::detail::g_fssRevealLockstep = false;
}

void resetCase(std::uint32_t& body, std::uint32_t& frame,
               std::uint32_t& jump, std::uint32_t& jumpFrame) {
    reset();
    // ScannerClaimHostView borrows these scalars; resetting only the mock
    // globals would let one case inherit another case's expired window.
    body = 10;
    frame = 12;
    jump = 0;
    jumpFrame = 12;
}

std::string readSource(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string sourceBlock(const std::string& source, const char* begin,
                        const char* end) {
    const std::size_t first = source.find(begin);
    if (first == std::string::npos) return {};
    const std::size_t last = source.find(end, first + std::char_traits<char>::length(begin));
    if (last == std::string::npos) return {};
    return source.substr(first, last - first);
}

std::size_t occurrences(const std::string& source, const char* needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += std::char_traits<char>::length(needle);
    }
    return count;
}

std::string withoutWhitespace(const std::string& source) {
    std::string compact;
    compact.reserve(source.size());
    for (const char ch : source) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            compact.push_back(ch);
    }
    return compact;
}

void forwardingSourcePins() {
    const std::string ladder = readSource("src/d3d11/draw_ladder.h");
    const std::string screen = readSource("src/d3d11/vscreen.cpp");
    const std::string reveal = readSource("src/plugins/comfort/scanners/fss_reveal.cpp");
    check(!ladder.empty() && !screen.empty() && !reveal.empty(),
          "production source pins can read the repository files");
    if (ladder.empty() || screen.empty() || reveal.empty()) return;

    const std::string panelSite =
        "InterestGated<SiteId::kFssPanelClaim,SiteKind::Claim,draw_interest::InterestId::FssPanel>";
    const std::string revealSite =
        "InterestGated<SiteId::kFssRevealClaim,SiteKind::Claim,draw_interest::InterestId::FssReveal>";
    const std::string compactLadder = withoutWhitespace(ladder);
    const std::size_t panelInterest = compactLadder.find(panelSite);
    const std::size_t revealInterest = compactLadder.find(revealSite);
    check(panelInterest != std::string::npos &&
              revealInterest != std::string::npos && panelInterest < revealInterest,
          "panel and reveal remain ordered interest-gated ladder sites");

    const std::string panelBlock = sourceBlock(
        screen, "} else if constexpr (id == SiteId::kFssPanelClaim) {",
        "} else if constexpr (id == SiteId::kFssRevealClaim) {");
    const std::string revealBlock = sourceBlock(
        screen, "} else if constexpr (id == SiteId::kFssRevealClaim) {",
        "} else if constexpr (id == SiteId::kSunglareSkip) {");
    const std::string compactPanel = withoutWhitespace(panelBlock);
    const std::string compactReveal = withoutWhitespace(revealBlock);
    const char* hostView =
        "ScannerClaimHostViewhost{s->fssBodyFrame,s->frameNo,s->fssJumpFrame,s->frameNo};";
    check(occurrences(compactPanel, hostView) == 1 &&
              occurrences(compactReveal, hostView) == 1,
          "both selector sites pass the actual body, frame, jump, frame host fields");
    check(occurrences(compactPanel, "fact.kind=FssTraceFactKind::kPanel;") == 1 &&
              occurrences(compactPanel, "fact.handlerInvoked=true;") == 1 &&
              occurrences(compactPanel,
                  "scannerPanelClaim(self,kind,count,instances,host,&fact.panel)") == 1 &&
              occurrences(compactPanel,
                  "scannerPanelClaim(self,kind,count,instances,host,nullptr)") == 1 &&
              occurrences(compactPanel, "trace.fssFact(fact);") == 1 &&
              occurrences(compactPanel,
                  "if(matches)returnclaimed(id,DrawVerdict::kFssPanel);") == 1 &&
              occurrences(compactPanel,
                  "returnclaimed(id,DrawVerdict::kFssPanel);") == 2,
          "site 57 maps trace and normal evaluator calls to the panel fact and verdict");
    const std::size_t panelCall = compactPanel.find(
        "scannerPanelClaim(self,kind,count,instances,host,&fact.panel)");
    const std::size_t panelFact = compactPanel.find("trace.fssFact(fact);");
    const std::size_t panelReturn = compactPanel.find(
        "if(matches)returnclaimed(id,DrawVerdict::kFssPanel);");
    check(panelCall != std::string::npos && panelFact != std::string::npos &&
              panelReturn != std::string::npos &&
              panelCall < panelFact && panelFact < panelReturn,
          "panel trace fact is emitted after evaluation and before guarded claim");
    check(occurrences(compactReveal, "fact.kind=FssTraceFactKind::kReveal;") == 1 &&
              occurrences(compactReveal, "fact.handlerInvoked=true;") == 1 &&
              occurrences(compactReveal, "auto&observed=fact.reveal;") == 1 &&
              occurrences(compactReveal,
                  "scannerRevealClaim(self,kind,count,instances,host,&observed)") == 1 &&
              occurrences(compactReveal,
                  "scannerRevealClaim(self,kind,count,instances,host,nullptr)") == 1 &&
              occurrences(compactReveal, "trace.fssFact(fact);") == 1 &&
              occurrences(compactReveal,
                  "if(matches)returnclaimed(id,DrawVerdict::kFssReveal);") == 1 &&
              occurrences(compactReveal,
                  "returnclaimed(id,DrawVerdict::kFssReveal);") == 2,
          "site 58 maps trace and normal evaluator calls to the reveal fact and verdict");
    const std::size_t observedCall = compactReveal.find(
        "scannerRevealClaim(self,kind,count,instances,host,&observed)");
    const std::size_t arrival = compactReveal.find("++s->fssArrivalRecogs;");
    const std::size_t traceFact = compactReveal.find("trace.fssFact(fact);");
    const std::size_t revealReturn = compactReveal.find(
        "returnclaimed(id,DrawVerdict::kFssReveal);");
    check(observedCall != std::string::npos && arrival != std::string::npos &&
              traceFact != std::string::npos && revealReturn != std::string::npos &&
              observedCall < arrival && arrival < traceFact && traceFact < revealReturn,
          "reveal recognition mutation stays between trace evaluator and fact/claim");

    const std::string beginBlock = sourceBlock(
        screen, "__declspec(noinline) void forwardVerdictBegin(",
        "__declspec(noinline) void forwardVerdictEnd(");
    const std::string endBlock = sourceBlock(
        screen, "__declspec(noinline) void forwardVerdictEnd(",
        "// The census's name for the fix");
    const std::size_t panelBegin = beginBlock.find("fssPanelBegin(self)");
    const std::size_t revealBegin = beginBlock.find("fssRevealBegin(self)");
    const std::size_t revealEnd = endBlock.find("fssRevealEnd(self)");
    const std::size_t panelEnd = endBlock.find("fssPanelEnd(self)");
    check(panelBegin != std::string::npos && revealBegin != std::string::npos &&
              panelBegin < revealBegin,
          "FSS Begin order remains panel then reveal");
    check(revealEnd != std::string::npos && panelEnd != std::string::npos &&
              revealEnd < panelEnd,
          "FSS End order remains reveal then panel");

    const std::string forward = sourceBlock(
        screen, "void forwardWithVerdict(", "// The copy thunks.");
    const std::size_t begin = forward.find("forwardVerdictBegin(self, v);");
    const std::size_t observedDefinition = forward.find("auto observedDraw = [&](AlteredDraw altered) {");
    const std::size_t originalInLambda = forward.find("const bool issued = draw(altered);");
    const std::size_t originalInvocation = forward.find("const bool originalIssued=observedDraw(");
    const std::size_t earlyEnd = forward.find("forwardVerdictEnd(self, v);", originalInvocation);
    const std::size_t normalEnd = forward.find("forwardVerdictEnd(self, v);", earlyEnd + 1);
    check(observedDefinition != std::string::npos &&
              originalInLambda != std::string::npos &&
              originalInvocation != std::string::npos &&
              earlyEnd != std::string::npos && normalEnd != std::string::npos &&
              occurrences(forward, "const bool originalIssued=observedDraw(") == 1 &&
              occurrences(forward, "const bool issued = draw(altered);") == 1 &&
              observedDefinition < originalInLambda && originalInLambda < begin &&
              begin < originalInvocation && originalInvocation < earlyEnd &&
              earlyEnd < normalEnd,
          "verdict Begin precedes the actual original-draw invocation and both End paths");
    const std::size_t restoreSrvs = reveal.find("PSSetShaderResources(0, 4, g_lsDisplaced)");
    const std::size_t restoreCb = reveal.find("PSSetConstantBuffers(1, 1, &g_displaced)");
    check(restoreSrvs != std::string::npos && restoreCb != std::string::npos &&
              restoreSrvs < restoreCb,
          "reveal restores displaced SRVs before displaced scene constants");
}

using edvr::plugins::comfort::scanners::ScannerClaimHostView;

void panelCases() {
    std::uint32_t body = 0, frame = 0, jump = 0, jumpFrame = 0;
    ScannerClaimHostView host{body, frame, jump, jumpFrame};
    edvr::FssPanelObservation observation{};
    const auto context = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{1});
    resetCase(body, frame, jump, jumpFrame);

    check(!edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'X', 6, 1, host, &observation),
          "disabled panel declined");
    check(wasRead(observation.outerEnabled, false) &&
              wasNotRead(observation.bodyFrame) && g_panelObservedCalls == 0,
          "disabled panel short-circuits before host fields and helper");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssPanelEnabled = true;
    body = 0;
    observation = {};
    check(!edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'X', 6, 1, host, &observation),
          "zero body frame declined");
    check(wasRead(observation.bodyFrame, std::uint32_t{0}) &&
              wasNotRead(observation.frameNo) && g_panelObservedCalls == 0,
          "zero body frame short-circuits frame read and helper");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssPanelEnabled = true;
    body = 0xffffffffu;
    frame = 1;
    g_panelResult = true;
    observation = {};
    check(edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'X', 6, 1, host, &observation),
          "panel unsigned wrap age two claims when helper accepts");
    check(wasRead(observation.bodyFrame, body) &&
              wasRead(observation.frameNo, frame) &&
              g_panelObservedCalls == 1 && g_panelCalls == 0 &&
              g_lastKind == 'X' && g_lastCount == 6 && g_lastInstances == 1,
          "panel trace route calls observed helper exactly once with original draw facts");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssPanelEnabled = true;
    body = 10;
    frame = 13;
    observation = {};
    check(!edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'X', 6, 1, host, &observation),
          "panel age three declines");
    check(wasRead(observation.frameNo, frame) && g_panelObservedCalls == 0,
          "panel age three short-circuits helper");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssPanelEnabled = true;
    body = 10;
    frame = 12;
    g_panelResult = true;
    check(edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'N', 5, 2, host, nullptr),
          "unobserved helper result is returned unchanged");
    check(g_panelCalls == 1 && g_panelObservedCalls == 0 &&
              g_lastKind == 'N' && g_lastCount == 5 && g_lastInstances == 2,
          "normal route uses only the existing unobserved helper");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssPanelEnabled = true;
    body = 10;
    frame = 12;
    observation = {};
    check(!edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'D', 4, 1, host, &observation) &&
              g_panelObservedCalls == 1 && g_panelCalls == 0 &&
              wasRead(observation.helper.lookupCompleted, true),
          "observed panel helper miss returns false with one fact-bearing call");
    check(!edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'D', 4, 1, host, nullptr) &&
              g_panelCalls == 1 && g_panelObservedCalls == 1,
          "normal panel helper miss returns false through the unobserved route");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssPanelEnabled = true;
    body = 10;
    frame = 12;
    g_panelResult = true;
    observation = {};
    check(edvr::plugins::comfort::scanners::scannerPanelClaim(
              context, 'X', 6, 1, host, &observation),
          "panel preserves recognized result after helper reports caught release fault");
    check(g_panelObservedCalls == 1 &&
              wasRead(observation.helper.lookupCompleted, true) &&
              wasRead(observation.helper.releaseReached, true) &&
              wasRead(observation.helper.releaseCompleted, false) &&
              wasRead(observation.helper.guardReturned, false) &&
              wasRead(observation.helper.hashAfterGuard,
                      std::uint64_t{0xA888D51024D9798Eull}),
          "panel forwards the observed helper fault facts without re-evaluation");
}

void revealCases() {
    std::uint32_t body = 0, frame = 0, jump = 0, jumpFrame = 0;
    ScannerClaimHostView host{body, frame, jump, jumpFrame};
    edvr::FssRevealObservation observation{};
    const auto context = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{1});
    resetCase(body, frame, jump, jumpFrame);

    check(!edvr::plugins::comfort::scanners::scannerRevealClaim(
              context, 'N', 6, 1, host, &observation),
          "reveal modes off decline");
    check(wasRead(observation.outerSteady, false) &&
              wasRead(observation.outerLockstep, false) &&
              wasNotRead(observation.bodyFrame) && g_order.empty() &&
              g_revealObservedCalls == 0,
          "reveal OR-off short-circuits host fields, latch, and helper");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssRevealSteady = true;
    body = 10;
    frame = 12;
    g_revealResult = true;
    observation = {};
    check(edvr::plugins::comfort::scanners::scannerRevealClaim(
              context, 'N', 6, 1, host, &observation),
          "fresh body reveal claim returns helper result");
    check(wasRead(observation.outerSteady, true) &&
              wasNotRead(observation.outerLockstep) &&
              wasRead(observation.bodyFrame, body) &&
              wasRead(observation.bodyFrameNo, frame) &&
              wasNotRead(observation.jumpFrame) && g_order == "R" &&
              g_revealObservedCalls == 1,
          "steady body path skips lockstep, jump, and latch in order");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssRevealLockstep = true;
    body = 10;
    frame = 13;
    jump = 100;
    jumpFrame = 700;
    g_latchResult = false;
    observation = {};
    check(!edvr::plugins::comfort::scanners::scannerRevealClaim(
              context, 'N', 6, 1, host, &observation),
          "eligible jump path declines when mode latch is false");
    check(wasRead(observation.jumpFrame, jump) &&
              wasRead(observation.jumpFrameNo, jumpFrame) &&
              wasRead(observation.modeLatch, false) && g_order == "L" &&
              g_revealObservedCalls == 0,
          "jump path queries latch after age check and skips helper on latch miss");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssRevealLockstep = true;
    body = 10;
    frame = 13;
    jump = 100;
    jumpFrame = 700;
    g_latchResult = true;
    g_revealResult = false;
    observation = {};
    check(!edvr::plugins::comfort::scanners::scannerRevealClaim(
              context, 'N', 5, 1, host, &observation),
          "jump path returns helper miss");
    check(g_order == "LR" && g_revealObservedCalls == 1 &&
              wasRead(observation.modeLatch, true),
          "eligible jump calls latch then observed helper once");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssRevealSteady = true;
    g_revealResult = true;
    g_revealFaultAfterHash = true;
    observation = {};
    check(edvr::plugins::comfort::scanners::scannerRevealClaim(
              context, 'N', 6, 1, host, &observation),
          "reveal preserves recognized result after caught helper fault");
    check(g_revealObservedCalls == 1 &&
              wasRead(observation.helper.lookupCompleted, true) &&
              wasRead(observation.helper.releaseReached, true) &&
              wasRead(observation.helper.releaseCompleted, false) &&
              wasRead(observation.helper.guardReturned, false) &&
              wasRead(observation.helper.hashAfterGuard,
                      std::uint64_t{0x953C8123AD8DC13Bull}),
          "reveal forwards helper fault facts without running helper twice");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssRevealSteady = true;
    body = 10;
    frame = 12;
    g_revealResult = true;
    check(edvr::plugins::comfort::scanners::scannerRevealClaim(
              context, 'N', 6, 1, host, nullptr),
          "normal reveal route returns unobserved helper result");
    check(g_order == "R" && g_revealCalls == 1 &&
              g_revealObservedCalls == 0,
          "normal reveal route uses only existing unobserved helper");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssRevealSteady = true;
    body = 10;
    frame = 12;
    check(!edvr::plugins::comfort::scanners::scannerRevealClaim(
              context, 'D', 4, 1, host, nullptr) &&
              g_order == "R" && g_revealCalls == 1 &&
              g_revealObservedCalls == 0,
          "normal reveal helper miss returns false through the unobserved route");

    resetCase(body, frame, jump, jumpFrame);
    edvr::detail::g_fssRevealSteady = true;
    body = 0;
    jump = 100;
    jumpFrame = 700;
    g_throwLatch = true;
    observation = {};
    bool caught = false;
    try {
        (void)edvr::plugins::comfort::scanners::scannerRevealClaim(
            context, 'N', 6, 1, host, &observation);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    check(caught && g_order == "L" && g_revealObservedCalls == 0,
          "latch fault propagates and prevents later helper work");
}

}  // namespace

namespace edvr {
namespace detail {
bool g_fssPanelEnabled = false;
bool g_fssRevealSteady = false;
bool g_fssRevealLockstep = false;
}  // namespace detail

bool fssPanelOnEyeDraw(ID3D11DeviceContext*, char kind, std::uint32_t count,
                       std::uint32_t instances) {
    ++g_panelCalls;
    g_order.push_back('P');
    g_lastKind = kind;
    g_lastCount = count;
    g_lastInstances = instances;
    return g_panelResult;
}

bool fssPanelOnEyeDrawObserved(ID3D11DeviceContext*, char kind,
                               std::uint32_t count, std::uint32_t instances,
                               FssPanelObservation& observation) {
    ++g_panelObservedCalls;
    g_order.push_back('P');
    g_lastKind = kind;
    g_lastCount = count;
    g_lastInstances = instances;
    observation.helper.enabled = {true, true, detail::g_fssPanelEnabled};
    observation.helper.contextNonNull = {true, true, true};
    observation.helper.guardCallReached = {true, true, true};
    observation.helper.callbackEntered = {true, true, true};
    observation.helper.vsGetShaderCompleted = {true, true, true};
    observation.helper.lookupReached = {true, true, true};
    observation.helper.lookupCompleted = {true, true, true};
    observation.helper.assignedHash = {true, true, 0xA888D51024D9798Eull};
    observation.helper.shaderNonNull = {true, true, true};
    observation.helper.releaseReached = {true, true, true};
    observation.helper.releaseCompleted = {true, true, false};
    observation.helper.callbackCompleted = {true, true, false};
    observation.helper.guardReturned = {true, true, false};
    observation.helper.hashAfterGuard = {true, true, 0xA888D51024D9798Eull};
    observation.matchedHashBefore = {true, true, 0};
    observation.matchedHashAfter = {true, true, 0xA888D51024D9798Eull};
    return g_panelResult;
}

bool fssRevealOnEyeDraw(ID3D11DeviceContext*, char kind, std::uint32_t count,
                        std::uint32_t instances) {
    ++g_revealCalls;
    g_order.push_back('R');
    g_lastKind = kind;
    g_lastCount = count;
    g_lastInstances = instances;
    return g_revealResult;
}

bool fssRevealOnEyeDrawObserved(ID3D11DeviceContext*, char kind,
                                std::uint32_t count, std::uint32_t instances,
                                FssRevealObservation& observation) {
    ++g_revealObservedCalls;
    g_order.push_back('R');
    g_lastKind = kind;
    g_lastCount = count;
    g_lastInstances = instances;
    observation.helper.steady = {true, true, detail::g_fssRevealSteady};
    if (!detail::g_fssRevealSteady)
        observation.helper.lockstep = {true, true, detail::g_fssRevealLockstep};
    observation.helper.contextNonNull = {true, true, true};
    observation.helper.guardCallReached = {true, true, true};
    observation.helper.callbackEntered = {true, true, true};
    observation.helper.vsGetShaderCompleted = {true, true, true};
    observation.helper.lookupReached = {true, true, true};
    observation.helper.lookupCompleted = {true, true, true};
    observation.helper.assignedHash = {true, true, 0x953C8123AD8DC13Bull};
    observation.helper.shaderNonNull = {true, true, true};
    observation.helper.releaseReached = {true, true, true};
    observation.helper.releaseCompleted = {true, true, !g_revealFaultAfterHash};
    observation.helper.callbackCompleted = {true, true, !g_revealFaultAfterHash};
    observation.helper.guardReturned = {true, true, !g_revealFaultAfterHash};
    observation.helper.hashAfterGuard = {true, true, 0x953C8123AD8DC13Bull};
    return g_revealResult;
}

bool deviceHookFssModeLatch() {
    g_order.push_back('L');
    if (g_throwLatch) throw std::runtime_error("controlled mode-latch fault");
    return g_latchResult;
}
}  // namespace edvr

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("scanner-claim actual-source rig: dry run (no writes)");
        return 0;
    }
    if (argc != 1 && !(argc == 2 && std::strcmp(argv[1], "--self-test") == 0)) {
        std::fputs("usage: scanner_claim_test.exe [--self-test|--dry-run]\n", stderr);
        return 2;
    }
    panelCases();
    revealCases();
    forwardingSourcePins();
    std::printf("scanner-claim actual-source rig: %u checks, %u failures\n",
                g_checks, g_failures);
    return g_failures ? 1 : 0;
}
