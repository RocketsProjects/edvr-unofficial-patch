#pragma once
#include "../../src/d3d11/flat_origin_optical.h"
#include "../../src/common/runtime_profile.h"

// A narrow WARP fixture for the *passive* sidecar. The current rows move
// 80 -> 20 m in both cases. Only the prior depth changes: a true coordinate
// reanchor leaves a fixed world pattern in place, whereas inward travel moves
// that pattern by the raw camera projection. The renderer still receives the
// same source buffers it would without the sidecar.
inline int originOpticalGpuTests(ID3D11Device* device,ID3D11DeviceContext* context) {
    using namespace edvr;
    check(flatOriginOpticalCategory(60.f,20,80,false)==0,
        "origin optical: 80 to 20 m inward travel has reserved short-inward quota");
    unsigned spent[5]{};
    for(unsigned n=0;n<120;++n){const unsigned c=flatOriginOpticalCategory(5000.f,20,5020,false);if(spent[c]<6)++spent[c];}
    check(spent[0]==0 && spent[1]==6 && flatOriginOpticalCategory(60.f,20,80,false)==0,
        "origin optical: long periodic reanchors cannot spend inward-travel quota");
    check(!flatOriginOpticalPriorDomainSupported(true,false) && !flatOriginOpticalPriorDomainSupported(false,true) &&
          !flatOriginOpticalPriorDomainSupported(false,false,true) &&
          flatOriginOpticalPriorDomainSupported(false,false),
        "origin optical: foreground and mixed-camera previous depth refuse a score without prior ownership");
    {uint32_t score[40]{};score[7]=100;score[12]=90;
     check(std::strcmp(flatOriginOpticalSignal(score),"inconclusive")==0,
        "origin optical: a flat-depth tie is explicitly inconclusive");
     score[13]=20;score[14]=0;check(std::strcmp(flatOriginOpticalSignal(score),"raw-travel-depth-favored")==0,
        "origin optical: only shared-support exclusive depth fits can favor raw translation");
     score[13]=0;score[14]=20;check(std::strcmp(flatOriginOpticalSignal(score),"zero-translation-depth-favored")==0,
        "origin optical: only shared-support exclusive depth fits can favor zero translation");}

    const RuntimeProfile previousProfile=runtimeProfileValue();
    struct Outcome {std::vector<unsigned char> mask,output;std::vector<float> motion;std::string signal;bool score=false;};
    auto run=[&](bool reanchor,bool probe,bool gap=false,bool steady=true,bool foreground=false,bool priorForeground=false)->Outcome {
        flatMonoResolveReset();
        runtimeProfileSet(probe?RuntimeProfile::Flat:RuntimeProfile::LegacyVr);
        steadygpu::Rig r(device,context);
        r.start(FlatMonoResolveMode::Dlss);
        r.f.steadyDetail=steady;
        r.f.camera[0][0]=r.f.previousCamera[0][0]=1.f/64.f;
        r.f.camera[5][0]=r.f.previousCamera[5][0]=80.f;
        ComPtr<ID3D11Texture2D> foregroundMap;
        ComPtr<ID3D11ShaderResourceView> foregroundView;
        if(foreground || priorForeground) {
            std::vector<float> emptyMap(steadygpu::W*steadygpu::H*4,0.f);
            foregroundMap=texture(device,steadygpu::W,steadygpu::H,DXGI_FORMAT_R32G32B32A32_FLOAT,
                D3D11_BIND_SHADER_RESOURCE,emptyMap.data(),steadygpu::W*16);
            foregroundView=view(device,foregroundMap.Get());
        }
        if(priorForeground) {
            r.f.foregroundMotion=foregroundView.Get();r.f.foregroundQualified=true;r.f.foregroundFrame=r.f.frame;
        }
        r.fillZ(.01f);
        r.setZ(reanchor?6:0,0,10,steadygpu::H,.02f);
        r.run("origin optical: adjacent baseline");
        r.f.reset=false;
        if(priorForeground){r.f.foregroundMotion=nullptr;r.f.foregroundQualified=false;}
        if(gap)++r.f.frame;
        r.f.previousCamera[5][0]=80.f;r.f.camera[5][0]=20.f;
        r.fillZ(.01f);r.setZ(6,0,10,steadygpu::H,.02f);
        const uint64_t eventFrame=r.f.frame;
        if(foreground) {
            r.f.foregroundMotion=foregroundView.Get();r.f.foregroundQualified=true;r.f.foregroundFrame=eventFrame;
        }
        const size_t firstLine=opticalLines.size();
        ComPtr<ID3D11ShaderResourceView> output;
        steadygpu::Seen event=r.run("origin optical: 60 m inward candidate",true,&output);
        Outcome o{};o.mask=std::move(event.mask);o.motion=std::move(event.motion);
        if(output){ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> image;
            output->GetResource(resource.GetAddressOf());if(resource&&SUCCEEDED(resource.As(&image)))
                check(readWhole(context,image.Get(),o.output,4),"origin optical: final output readable");}
        r.f.previousCamera[5][0]=r.f.camera[5][0];
        for(unsigned attempt=0;attempt<12;++attempt) {
            for(size_t i=firstLine;i<opticalLines.size();++i) {
                const std::string marker="frame="+std::to_string(eventFrame)+" ";
                if(opticalLines[i].find("flat origin optical score:")==0 && opticalLines[i].find(marker)!=std::string::npos) {
                    o.score=true;o.signal=opticalLines[i];break;
                }
            }
            if(o.score || !probe || gap || !steady || foreground || priorForeground)break;
            context->Flush();Sleep(2);
            r.run("origin optical: readback drain frame");
        }
        if(probe && (gap || !steady || foreground || priorForeground)) {
            bool unavailable=false;
            for(size_t i=firstLine;i<opticalLines.size();++i)
                unavailable |= opticalLines[i].find(priorForeground?"unavailable=prior-domain-unverified-previous-foreground-or-untrusted":
                    foreground?"unavailable=prior-domain-unverified-foreground-or-untrusted":
                    "unavailable=no-adjacent-previous-depth")!=std::string::npos;
            check(unavailable,priorForeground?"origin optical: prior SDK foreground excludes current world-only score":
                foreground?"origin optical: qualified SDK foreground input refuses unverified prior domain":
                "origin optical: gap or missing depth names no adjacent previous depth");
        }
        return o;
    };

    const Outcome inward=run(false,true),inwardNoProbe=run(false,false);
    if(!inward.score || inward.signal.find("signal=raw-travel-depth-favored")==std::string::npos)
        std::printf("origin optical inward diagnostic: score=%u %s\n",inward.score?1u:0u,inward.signal.c_str());
    check(inward.score && inward.signal.find("signal=raw-travel-depth-favored")!=std::string::npos,
        "origin optical: 80 to 20 m inward travel favors unmodified raw translation on shared static depth samples");
    check(inward.mask==inwardNoProbe.mask && inward.motion==inwardNoProbe.motion && inward.output==inwardNoProbe.output,
        "origin optical: passive sidecar changes neither SDK inputs nor final color");
    const Outcome reanchor=run(true,true),reanchorNoProbe=run(true,false);
    check(reanchor.score && reanchor.signal.find("signal=zero-translation-depth-favored")!=std::string::npos,
        "origin optical: same raw camera jump with reexpressed world favors zero translation");
    check(reanchor.mask==reanchorNoProbe.mask && reanchor.motion==reanchorNoProbe.motion && reanchor.output==reanchorNoProbe.output,
        "origin optical: reanchor probe leaves motion, mask, and final color unchanged");
    run(false,true,true);  // frame gap, previous depth no longer adjacent
    run(false,true,false,false); // steady depth disabled, no prior image
    run(false,true,false,true,true); // bound SDK foreground: prior image has no ownership mask
    run(false,true,false,true,false,true); // same depth scale: previous foreground, current world-only
    runtimeProfileSet(previousProfile);
    flatMonoResolveReset();
    return 0;
}
