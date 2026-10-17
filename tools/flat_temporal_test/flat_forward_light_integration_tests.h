#pragma once
#include "flat_laser_scene_tone_tests.h"
#include "../../src/d3d11/flat_forward_light.h"
#include <fstream>
#include <iterator>
namespace forward_light_integration_test {
using namespace edvr;
inline unsigned run() {
    unsigned checks=0,failures=0;auto expect=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: forward integration %s\n",label);}};
    using namespace forward_light;using namespace laser_scene_tone_test;
    ConstantIdentity id{0x200,17,300,400,2,900,16,47,1008,12,752,0x321};
    expect(constantReady(id,true,true,false,false,900,13,752)==Reason::None,"complete original nonzero range accepted");
    for(unsigned n=0;n<12;++n){auto bad=id;bool tracked=true,shadow=true,mapped=false,pending=false;unsigned required=752;
        if(n==0)bad.buffer=0;if(n==1)bad.first=17;if(n==2)bad.count=46;if(n==3)bad.width=1007;
        if(n==4)tracked=false;if(n==5)shadow=false;if(n==6)mapped=true;if(n==7)pending=true;
        if(n==8)--bad.writeFrame;if(n==9)bad.writeQ=0;if(n==10)bad.writeQ=13;if(n==11)required=784;
        expect(constantReady(bad,tracked,shadow,mapped,pending,900,13,required)!=Reason::None,"missing/stale/mapped/pending/truncated original refuses");}
    ConverterRaster raster;raster.topology=5;raster.fill=3;raster.cull=1;raster.width=3840;raster.height=2160;raster.clip=true;
    expect(converterRaster(raster),"observed triangle solid/full-viewport converter state");
    for(unsigned n=0;n<12;++n){auto bad=raster;
        if(n==0)bad.topology=2;if(n==1)bad.fill=2;if(n==2)bad.cull=4;if(n==3)bad.bias=1;if(n==4)bad.clamp=1;if(n==5)bad.slope=1;
        if(n==6)bad.clip=false;if(n==7)bad.multisample=true;if(n==8)bad.antialiasedLine=true;if(n==9)bad.depth=true;if(n==10)bad.stencil=true;
        if(n==11){bad.scissor=true;bad.scissors=1;bad.right=3839;bad.bottom=2160;}
        expect(!converterRaster(bad),"partial/unknown raster or depth-stencil converter refuses");}
    for(unsigned cull:{1u,2u,3u}){raster.cull=cull;expect(converterRaster(raster),"conventional culling permits observed association, not pixel coverage");}
    raster.scissor=true;raster.scissors=1;raster.left=-1;raster.right=3840;raster.bottom=2160;
    expect(converterRaster(raster),"full-covering enabled scissor remains a valid state");
    CloneReceipt clone{1,1,2,3,2,3,4,5,4,5,7,8,7,8};
    expect(cloneReceipt(clone),"exact current context/pair/generation shader clone receipt");
    for(unsigned n=0;n<10;++n){auto bad=clone;
        if(n==0)bad.owner=9;if(n==1)bad.originalVs=9;if(n==2)bad.originalPs=9;if(n==3)bad.shadowVs=9;if(n==4)bad.shadowPs=9;
        if(n==5)bad.actualVs=9;if(n==6)bad.actualPs=9;if(n==7)bad.currentVsGeneration=9;if(n==8)bad.currentPsGeneration=9;if(n==9)bad.context=0;
        expect(!cloneReceipt(bad),"wrong-context/stale/mixed-stage clone receipt refuses");}
    expect(typedRole(0xEB5234DB6ADB491Dull,0xB7D50283329322C3ull)==Role::Origin&&
        typedRole(0xEB5234DB6ADB491Dull,0xBAD)==Role::Unknown,"exact observed colour-only pool PS certified; other family PS unknown");
    Authority authority{900,4,2,3,5,true};
    expect(authority.affected(900,2)&&authority.affected(900,3)&&authority.affected(900,0),"post-donation raw/linear/unknown mutation revokes");
    expect(!authority.affected(901,2)&&!authority.affected(900,4)&&!authority.affected(900,99),"new frame/H accumulation/unrelated MRT preserve authority");
    authority.live=false;expect(!authority.affected(900,2),"revoked authority cannot revive itself");
    // Same runtime attestation -> reducer -> inherited H -> image handoff. Its
    // b1 stays null: donor provenance is separate from actual light bindings.
    auto replay=[&](unsigned negative){auto prefix=std::make_unique<FlatRuntimePrefix>();auto& p=*prefix;p.frame=900;
        auto src=draw(token(0x1000),23,token(0x2000),token(0x2001),0xBFE51414CC3024B4ull,0xDB79AE788E049DFDull);
        src.key.kind=kFlatContractPool;src.supported=true;laser_scene_tone_test::camera(src,900,1);flatRuntimeObserve(p,src);
        if(negative==1){auto conflict=src;conflict.camera[80]^=1;conflict.key.cameraHash=flatCameraHash(conflict.camera);flatRuntimeObserve(p,conflict);}
        auto gap=draw(token(0x9000),28,nullptr,nullptr,1,2);flatRuntimeObserve(p,gap);
        auto h=draw(token(0x4000),26,token(0x2000),token(0x2001),kVs,kPs);
        h.forwardHdrVerified=h.forwardUpstreamPhase=true;std::memcpy(h.camera,src.camera,96);h.key.cameraHash=flatCameraHash(h.camera);
        auto& cb=h.key.projection[1];cb.resource=token(0x6000);cb.width=784;cb.copied=752;cb.hash=123;
        cb.status=FlatProjectionStatus::Available;cb.writeEpoch=900;cb.writeSeq=2;
        if(negative==2)cb.copied=112;if(negative==3)cb.copied=751;if(negative==4)cb.hash=0;
        if(negative==5)--cb.writeEpoch;if(negative==6)cb.writeSeq=p.sequence+1;if(negative==7)cb.resource=nullptr;
        if(negative==8)h.forwardHdrVerified=false;if(negative==9)h.forwardUpstreamPhase=false;
        if(negative==10){h.forwardHdrVerified=h.forwardUpstreamPhase=false;h.deferredHdrVerified=true;} // old trace bit cannot authorize752
        if(negative==11){h.key.camera=h.camera;h.key.b1=src.key.b1;}
        if(negative==12)h.key.dsv=token(0xBAD);if(negative==13)h.key.vs=0x963B52C73B4143ACull;
        if(negative==14)h.camera[80]^=1;if(negative==15)p.uncertain=true;
        auto event=flatTraceEventFromDraw(h,false);auto decoded=flatTraceEventToDraw(event);
        expect(event.key.camera==nullptr&&event.key.projection[1].bytes==nullptr,"trace does not serialize pointer authority");
        if(!negative)expect((event.flags&kFlatTraceForwardHdrVerified)&&(event.flags&kFlatTraceForwardUpstreamPhase)&&!(event.flags&kFlatTraceHasCamera),"new typed flags distinct from actual b1 camera flag");
        flatRuntimeObserve(p,decoded);auto* target=flatRuntimeTarget(p,h.key.color);
        const bool admitted=target&&target->hdrCamera&&target->tone.forwardLightCamera;
        if(negative==11)return !admitted&&p.uncertain;
        if(negative)return !admitted;
        expect(admitted&&target->tone.key.b1==nullptr&&target->tone.sceneCameraB1==src.key.b1,"inherited H names actual donor without forging bound b1");
        expect(target->tone.key.projection[1].resource==cb.resource&&target->tone.key.projection[1].copied==752&&
            target->tone.firstWriteSeq==2&&target->tone.sceneCameraWriteSeq==1&&flat_mono_detail::cameraCurrent(target->tone,900),"selected camera retains original light and donor write ordinals");
        auto image=draw(token(0x5000),9,token(0x2000),token(0x2001),0xCFA91824129ECBBCull,0xFCFAD73924BF45B9ull);
        image.imageSourceCameraIndependentVerified=true;flatRuntimeObserve(p,image);
        auto copy=draw(h.key.color,26,nullptr,nullptr,0xCFA91824129ECBBCull,0xDFCBA0EC70B03C9Bull);
        copy.key.depthWidth=copy.key.depthHeight=0;copy.key.srvResource[0]=image.key.color;copy.key.srvView[0]=token(0x5002);copy.hdrCopyVerified=true;flatRuntimeObserve(p,copy);
        expect(p.imageCopiesAccepted==1,"existing verified image-copy policy consumes new typed H");
        p.uncertain=true; // exact replay effect of an actual-scope MarkUncertain cancellation
        expect(!flatRuntimeForwardDonor(p,h.key,p.sequence+1),"cancelled actual issue cannot retain reducer authorization");return admitted;
    };
    expect(replay(0),"typed light survives trace roundtrip and image handoff");
    for(unsigned n=1;n<=15;++n)expect(replay(n),"malformed/stale/mixed/old-bit/later-pair attestation cannot establish H");
    // Pin the integration seams whose accidental placement behind diagnostic
    // arms or actual-call early returns would make the pure proof unreachable.
    auto read=[](const char* path){std::ifstream f(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
    const auto runtime=read("src/d3d11/flat_runtime.cpp"),adapter=read("src/d3d11/flat_forward_light_runtime.inl"),header=read("src/d3d11/flat_runtime.h");
    const auto qualify=runtime.find("const auto lightIssue=forwardQualify(s,d);"),reduce=runtime.find("FlatMonoFrame selected =",qualify);
    const auto recertify=runtime.find("!forwardIssueValid(s,forwardIssue)",qualify);
    expect(qualify!=std::string::npos&&recertify<reduce,"original binding receipt precedes reducer");
    expect(runtime.find("forwardBeginIssue(state(),forwardIssue,indirectArgs!=nullptr)")<runtime.find("if(!weaponFootprintStarted)return;"),"actual begin is not hidden behind diagnostic footprint");
    expect(runtime.find("forwardReturn(state(),forwardIssue)")!=std::string::npos&&runtime.find("forwardAbandon(state(),forwardIssue)")!=std::string::npos&&
        header.find("return forwardIssue.kind!=forward_light::Issue::None")!=std::string::npos,"actual return/cancel callbacks are scheduled with probes off");
    expect(adapter.find("projectionFrames")==std::string::npos&&adapter.find("flatOriginCapture")==std::string::npos&&adapter.find("lightLive(")==std::string::npos,
        "production qualification never depends on Full/probe flags");
    expect(runtime.find("forwardReset(s);finishLightAudit")!=std::string::npos&&runtime.find("forwardReset(s); finishLightAudit")!=std::string::npos&&
        runtime.find("forwardMutation(s,reinterpret_cast<uintptr_t>(res))")!=std::string::npos&&
        runtime.find("forwardMutation(s,reinterpret_cast<uintptr_t>(u.Get()))")!=std::string::npos,"work/resize/API writes/dispatches expire lineage");
    expect(adapter.find("OMGetRenderTargetsAndUnorderedAccessViews")!=std::string::npos&&adapter.find("RenderTargetWriteMask")!=std::string::npos&&
        adapter.find("forwardMutation(s,reinterpret_cast<uintptr_t>(resource.Get()),false)")!=std::string::npos,"all writable MRT identities invalidate conversion");
    expect(adapter.find("forwardRefusal(s,Reason::Abandoned,reinterpret_cast<void*>(issue.color))")!=std::string::npos&&
        adapter.find("flatTraceMark(s.traceRing,kFlatTraceEventMarkUncertain,nullptr)")!=std::string::npos,"abandoned typed light marks replay uncertain");
    expect(adapter.find("stage&&consumer==3?784:752")!=std::string::npos&&adapter.find("consumer==3?1:0")!=std::string::npos&&
        adapter.find("if((!first&&!typed)||(consumer!=1&&!typed))return issue;")!=std::string::npos,"later exact recipes revalidate actual declared span/input but never establish camera");
    const auto motion=read("src/d3d11/engine_velocity.cpp");
    expect(motion.find("bool engineVelocityFlatSourceShaders(")!=std::string::npos&&
        motion.find("r.currentVsGeneration=bindingGeneration(BindSlot::Vs)")!=std::string::npos&&
        motion.find("r.currentPsGeneration=bindingGeneration(BindSlot::Ps)")!=std::string::npos&&
        motion.find("marker.context.Get()==ctx")!=std::string::npos,"actual clone receipt requires held original/replacement generation and context");
    expect(adapter.find("a.affected(s.prefix.frame,resource)")!=std::string::npos&&
        adapter.find("target.hdrBad=true;target.hdrCamera=false")!=std::string::npos&&
        adapter.find("forwardRevoke(s,0,forward_light::Reason::Ownership)")!=std::string::npos&&
        runtime.find("if(forwardIssue.donated)forwardAttest(s,forwardIssue)")!=std::string::npos,"typed H dependencies revoke without waiting for another light");
    const auto phaseRuntime=read("src/d3d11/flat_phase_failure_runtime.inl");
    const auto observation=adapter.find("forwardPhaseObserve(s,d,*domain,*publication,constants,phase,reason)");
    expect(observation!=std::string::npos&&observation<adapter.find("if(reason!=Reason::None)return refuse(reason);",observation),"first phase failure bytes snapshotted before refusal mutates state");
    expect(adapter.find("forwardReset(s,s.prefix.frame,f.ownerKnown&&f.upstream!=upstream)")!=std::string::npos&&
        adapter.find("if(captureLifecycle)forwardPhaseLifecycle(s)")!=std::string::npos,"normal frame reset preserves process capture while real ownership/lifecycle invalidates cohorts");
    expect(runtime.find("forwardPhaseSelected(s,selected);",runtime.find("if (!flatMonoResolve("))!=std::string::npos&&
        phaseRuntime.find("phase_failure::selectedCohort(capture.lastSelection,r)")!=std::string::npos&&
        phaseRuntime.find("domain.source.buffer!=reinterpret_cast<uintptr_t>(selected.sceneConstants)")!=std::string::npos,"successful selected source cohort gates one-pair capture");
    expect(phaseRuntime.find("copyConstants(")==std::string::npos&&phaseRuntime.find("->Map(")==std::string::npos&&
        phaseRuntime.find("projectionFrames")==std::string::npos&&phaseRuntime.find("flatOriginCapture")==std::string::npos,"phase receipt only copies previously qualified bytes without GPU reads or capture flags");
    const auto phaseEmission=phaseRuntime.find("void forwardPhaseReport(");
    expect(phaseEmission!=std::string::npos&&phaseRuntime.find("flat phase receipt COMPLETE:",phaseEmission)<phaseRuntime.find("c.emitted=true",phaseEmission)&&
        phaseRuntime.find("c.emitted=true;forwardPhaseRelease(s)",phaseEmission)!=std::string::npos,"receipt completion follows every metadata/payload record and releases retained objects");
    std::printf("forward light integration: %u checks, %u failed\n",checks,failures);return failures;
}
}
