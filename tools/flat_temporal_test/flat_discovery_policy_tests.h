#pragma once
#include "../../src/d3d11/flat_discovery_policy.h"
#include "../../src/d3d11/flat_offscreen_fast.h"
#include <fstream>
#include <iterator>
#include <string>

namespace flat_discovery_policy_tests {
// These checks execute the shared policy and offscreen live gate. Source checks
// below only connect those tested decisions to observer ordering/lifecycle;
// they do not execute the discovery collector or prove resolver/history parity.
template<class Check> void run(Check check) {
    using namespace edvr;
    unsigned combinations=0;
    for(unsigned observing=0;observing<2;++observing)
    for(unsigned manual=0;manual<2;++manual)
    for(unsigned candidate=0;candidate<2;++candidate)
    for(unsigned drawCapture=0;drawCapture<2;++drawCapture)
    for(unsigned packets=0;packets<2;++packets) {
        const bool demand=flatDiscoveryNeedsOriginalState(observing!=0,manual!=0,candidate!=0);
        check(demand==(observing && (manual || candidate)),"discovery policy: manual and independent candidate demand only while observing");
        FlatOffscreenLive live{};live.workFull=true;live.gpuFrameOpened=true;live.foregroundFrameReady=true;
        live.diagnostics=demand || drawCapture || packets;
        check(flatOffscreenLiveVerdict(live)==((demand || drawCapture || packets)?FlatOffscreenNo::Diagnostics:FlatOffscreenNo::None),
              "discovery policy: passive offscreen path stays eligible; manual/candidate/other capture keeps slow diagnostics");
        live.foreignWork=true;
        check(flatOffscreenLiveVerdict(live)==(live.diagnostics?FlatOffscreenNo::Diagnostics:FlatOffscreenNo::Foreign),
              "discovery policy: permitting passive capture cannot bypass foreign-work guard");
        ++combinations;
    }
    const auto read=[](const char* path) {std::ifstream f(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
    const auto body=[](const std::string& source,const std::string& signature) {
        const auto at=source.find(signature);if(at==std::string::npos)return std::string();
        const auto end=source.find("\n}",at);return end==std::string::npos?std::string():source.substr(at,end+2-at);
    };
    const auto before=[](const std::string& source,const char* a,const char* b) {
        const auto x=source.find(a),y=source.find(b);return x!=std::string::npos&&y!=std::string::npos&&x<y;
    };
    const auto temporal=read("src/d3d11/flat_temporal.cpp"),runtime=read("src/d3d11/flat_runtime.cpp"),hooks=read("src/d3d11/vscreen.cpp");
    check(!temporal.empty()&&!runtime.empty()&&!hooks.empty(),"discovery wiring: source inputs are readable");
    const auto draw=body(temporal,"void flatTemporalDraw("),dispatch=body(temporal,"void flatTemporalDispatch(");
    check(before(draw,"if (!flatTemporalCapturing()) return;","flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)")&&
          before(draw,"flatTemporalNeedsOriginalState()","flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)")&&
          before(draw,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)","flatComputeProbeDraw(")&&
          before(draw,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)","captureMenuCopy(ctx)"),
          "discovery wiring: draw preflight follows observer gate and precedes physical candidate/menu readers");
    check(before(dispatch,"if (!flatTemporalCapturing()) return;","flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch)")&&
          before(dispatch,"flatTemporalNeedsOriginalState()","flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch)")&&
          before(dispatch,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch)","flatComputeDispatch("),
          "discovery wiring: dispatch preflight follows observer gate and precedes physical candidate reader");
    std::string removed=draw;const auto restore=removed.find("flatRuntimeSubstitution(");
    if(restore!=std::string::npos)removed.erase(restore,removed.find(';',restore)+1-restore);
    check(!before(removed,"flatRuntimeSubstitution(ctx, FlatSubstEvent::kOtherDraw)","flatComputeProbeDraw("),
          "discovery wiring control: losing restore is detected");
    const auto arm=body(temporal,"void flatTemporalArm(");
    check(before(arm,"flatTemporalStart(device);","g_flatTemporalManual.store(true")&&
          arm.find("g.captureBudget = flatCaptureBudget(full ? FlatCaptureTier::Full : FlatCaptureTier::General);")!=std::string::npos&&
          arm.find("g.projectionManual = full;")!=std::string::npos,
          "discovery wiring: General and Full both request original state without changing their capture tiers");
    for(const char* function:{"void flatTemporalStart(","void flatTemporalStop(","void finishDiscovery("})
        check(body(temporal,function).find("g_flatTemporalManual.store(false")!=std::string::npos,
              "discovery wiring: startup, stop and completion clear manual demand");
    const char* hooksToCheck[]={"hookedDrawIndexedInstancedIndirect","hookedDrawInstancedIndirect","hookedDraw","hookedDrawAuto",
                               "hookedDrawIndexed","hookedDrawInstanced","hookedDrawIndexedInstanced"};
    for(const char* hook:hooksToCheck) {
        const auto part=body(hooks,std::string("void STDMETHODCALLTYPE ")+hook+"(");
        check(before(part,"flatTemporalDraw(","FlatRuntimeDrawScope flatDraw("),
              "discovery wiring: every flat draw observes before constructing slow/offscreen scope");
    }
    const auto live=body(runtime,"FlatOffscreenLive flatOffscreenLiveOf("),ctor=body(runtime,"FlatRuntimeDrawScope::FlatRuntimeDrawScope(");
    // Existing O5 tests also pin the source expressions and exercise reducer
    // parity on the committed corpus; this checks their shared new policy.
    check(live.find("flatTemporalNeedsOriginalState() || s.drawCapture.active() || s.drawPackets.armed()")!=std::string::npos&&
          ctor.find("flatTemporalNeedsOriginalState() || s.drawCapture.active() || s.drawPackets.armed()")!=std::string::npos,
          "discovery wiring: slow and offscreen constructors use identical physical-state demand");
    std::printf("  discovery policy: %u live-policy combinations; limited observer-order/lifecycle wiring checks\n",combinations);
}
} // namespace flat_discovery_policy_tests
