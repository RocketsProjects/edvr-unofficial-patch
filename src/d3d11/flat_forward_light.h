#pragma once
#include "flat_deferred_camera.h"

namespace edvr { namespace forward_light {
constexpr uint64_t kVs=0x8289669D93A18C1Dull,kPs=0xC6E6E419DA9F6FADull;
constexpr uint64_t kDepthVs=0xDEF19B035D5EDEDCull,kDepthPs=0xCB95394B50D737D6ull;
constexpr unsigned kBytes=752,kDomains=8,kPublications=8;
enum class Reason:uint32_t {None,Never,Ownership,Uncertain,Overflow,UnknownWriter,MissingDonor,StaleDonor,
    Ambiguous,MissingProducer,PendingProducer,MutatedProducer,Lineage,Binding,Range,OriginalMissing,
    OriginalStale,OriginalChanged,Forward,Origin,View,Phase,Issue,Abandoned,Count};
inline const char* name(Reason r) {
    const char* const text[]={"accepted","never-called","upstream-ownership-required","uncertain","capacity-overflow",
        "unknown-depth-writer","missing-origin-donor","stale-donor","ambiguous-camera","missing-depth-producer",
        "producer-not-returned","producer-mutated","depth-lineage","actual-binding","constant-range",
        "original-unavailable","original-stale","original-changed","forward-mismatch","origin-mismatch",
        "used-view-mismatch","phase-mismatch","actual-issue-mismatch","abandoned-issue"};
    const auto i=static_cast<unsigned>(r);return i<static_cast<unsigned>(Reason::Count)?text[i]:"invalid";
}
// These roles certify only camera/depth coordinates. They add no motion-source support.
enum class Role:uint32_t {Unknown,Origin,ProjectionOnly};
inline Role typedRole(uint64_t vs,uint64_t ps) {
    if(vs==0x84F6596FAF22CCFAull&&!ps)return Role::ProjectionOnly;
    // Captured pool-family PS has only colour outputs; unregistered pixel
    // shaders are not generically certified by their shared vertex family.
    if(vs==0xEB5234DB6ADB491Dull&&ps==0xB7D50283329322C3ull)return Role::Origin;
    if((vs==0x989E043933A369ABull&&ps==0xBD43EB1E0AAEFC7Aull)||
       (vs==0x7B0DC42D383F694Cull&&ps==0x0DF03E64DF9DBEF1ull))return Role::Origin;
    return Role::Unknown;
}
struct Surface {
    uint64_t resource=0;uint32_t width=0,height=0,mip=0,slice=0,slices=0,samples=0;
    bool known=false;
};
inline bool plane(const Surface& a,const Surface& b) {
    return a.known&&b.known&&a.resource&&a.resource==b.resource&&a.width==b.width&&a.height==b.height&&
        a.mip==b.mip&&a.slice==b.slice&&a.slices==b.slices&&a.samples==b.samples;
}
struct Source {
    uint64_t buffer=0,generation=0,bank=0,writeGeneration=0,frame=0;
    uint32_t writeQ=0,first=0,count=0;
};
inline bool sourceIdentity(const Source& a,const Source& b) {
    return a.buffer&&a.buffer==b.buffer&&a.generation==b.generation&&a.bank==b.bank&&a.first==b.first&&a.count==b.count;
}
struct Domain {
    Surface raw{};Source source{};unsigned char camera[96]{};
    uint32_t origins=0,projections=0,versions=0,lastQ=0;
    Reason failure=Reason::None;bool hasProjection=false;
};
// Returned exact-shader CPU draw input/output association, not a receipt of
// GPU execution, full-texel refresh or linear-depth metric equivalence.
struct Publication {
    Surface raw{},linear{};uint64_t token=0,frame=0;uint32_t q=0;
    bool live=false,issued=false,returned=false;
};
struct Ledger {
    uint64_t frame=0,serial=0;Domain domains[kDomains]{};Publication publications[kPublications]{};
    uint32_t used=0;bool overflow=false;
    void reset(uint64_t f=0){for(auto& d:domains)d={};for(auto& p:publications)p={};used=0;overflow=false;frame=f;}
    void begin(uint64_t f){if(frame!=f)reset(f);}
    Domain* domain(const Surface& raw) {
        for(unsigned i=0;i<used;++i)if(domains[i].raw.resource==raw.resource){
            if(!plane(domains[i].raw,raw))domains[i].failure=Reason::Lineage;return &domains[i];}
        if(!raw.known||!raw.resource)return nullptr;
        if(used==kDomains){overflow=true;return nullptr;}auto& d=domains[used++];d.raw=raw;return &d;
    }
    void observe(const Surface& raw,Role role,bool depthWrite,bool current,const Source& s,
                 const unsigned char* camera,uint32_t q) {
        auto* d=domain(raw);if(!d)return;
        if(role==Role::Unknown){if(depthWrite)d->failure=Reason::UnknownWriter;return;}
        if(!current||!camera||!s.buffer||!s.generation||s.frame!=frame||!s.writeQ||s.writeQ>q){d->failure=Reason::StaleDonor;return;}
        if(d->hasProjection&&(!sourceIdentity(d->source,s)||std::memcmp(d->camera,camera,64)))d->failure=Reason::Ambiguous;
        if(d->origins&&role==Role::Origin&&std::memcmp(d->camera,camera,96))d->failure=Reason::Ambiguous;
        if(!d->hasProjection){std::memcpy(d->camera,camera,96);d->source=s;d->hasProjection=true;}
        if(role==Role::Origin){if(!d->origins)std::memcpy(d->camera,camera,96);++d->origins;}
        else ++d->projections;
        d->source=s;d->lastQ=q;if(++d->versions==UINT32_MAX){overflow=true;d->failure=Reason::Overflow;}
    }
    void mutation(uint64_t resource,bool forgetSources=true) {
        if(forgetSources)for(auto& d:domains)if(d.hasProjection&&(!resource||d.raw.resource==resource))d.failure=Reason::StaleDonor;
        for(auto& p:publications)if(!resource||p.raw.resource==resource||p.linear.resource==resource)p.live=false;
    }
    uint64_t publish(const Surface& raw,const Surface& linear,uint32_t q) {
        mutation(linear.resource);Publication* slot=nullptr;
        for(auto& p:publications)if(p.linear.resource==linear.resource){slot=&p;break;}
        if(!slot)for(auto& p:publications)if(!p.token){slot=&p;break;}
        if(!slot){overflow=true;return 0;}*slot={raw,linear,++serial,frame,q,true,false,false};return serial;
    }
    void issued(uint64_t token){for(auto& p:publications)if(p.token==token&&p.live)p.issued=true;}
    void returned(uint64_t token){for(auto& p:publications)if(p.token==token&&p.live&&p.issued)p.returned=true;}
    void abandon(uint64_t token){for(auto& p:publications)if(p.token==token)p.live=false;}
    const Publication* find(const Surface& raw,const Surface& linear,uint32_t q,Reason& why)const {
        if(overflow){why=Reason::Overflow;return nullptr;}
        for(const auto& p:publications)if(plane(p.raw,raw)&&plane(p.linear,linear)) {
            if(!p.live){why=Reason::MutatedProducer;return nullptr;}
            if(p.frame!=frame||!p.q||p.q>=q){why=Reason::Lineage;return nullptr;}
            if(!p.returned){why=Reason::PendingProducer;return nullptr;}why=Reason::None;return &p;
        }why=Reason::MissingProducer;return nullptr;
    }
};
inline bool phaseReceipt(bool upstream,float x,float y,uint64_t applied) {
    return upstream&&std::isfinite(x)&&std::isfinite(y)&&((x==0&&y==0)||applied!=0);
}
struct ConstantIdentity {
    uint64_t buffer=0,generation=0,mutation=0,writeGeneration=0,bank=0,writeFrame=0;
    uint32_t first=0,count=0,width=0,writeQ=0,bytes=0,hash=0;
};
inline Reason constantReady(const ConstantIdentity& id,bool tracked,bool shadow,bool mapped,bool pending,
                            uint64_t frame,uint32_t q,unsigned required) {
    if(!id.buffer)return Reason::Binding;
    if(!required||required%16||uint64_t(required/16)>id.count||
       (uint64_t(id.first)*16+required)>id.width)return Reason::Range;
    if(!tracked||!shadow||mapped||pending||!id.generation)return Reason::OriginalMissing;
    if(id.writeFrame!=frame||!id.writeQ||id.writeQ>=q)return Reason::OriginalStale;
    return Reason::None;
}
inline bool same(const ConstantIdentity& a,const ConstantIdentity& b) {
    return a.buffer&&a.buffer==b.buffer&&a.generation==b.generation&&a.mutation==b.mutation&&
        a.writeGeneration==b.writeGeneration&&a.bank==b.bank&&a.writeFrame==b.writeFrame&&
        a.first==b.first&&a.count==b.count&&a.width==b.width&&a.writeQ==b.writeQ&&a.bytes==b.bytes&&a.hash==b.hash;
}
struct ConverterRaster {
    uint32_t topology=0,fill=0,cull=0,width=0,height=0,scissors=0;
    int32_t bias=0,left=0,top=0,right=0,bottom=0;
    float clamp=0,slope=0;
    bool clip=false,scissor=false,multisample=false,antialiasedLine=false,depth=false,stencil=false;
};
inline bool converterRaster(const ConverterRaster& r) {
    // D3D11 triangle list/strip, solid raster; this certifies observed draw
    // state/resource lineage, never that every GPU texel was executed.
    return (r.topology==4||r.topology==5)&&r.fill==3&&(r.cull>=1&&r.cull<=3)&&r.width&&r.height&&
        !r.bias&&r.clamp==0&&r.slope==0&&r.clip&&!r.multisample&&!r.antialiasedLine&&!r.depth&&!r.stencil&&
        (!r.scissor||(r.scissors==1&&r.left<=0&&r.top<=0&&int64_t(r.right)>=r.width&&int64_t(r.bottom)>=r.height));
}
struct CloneReceipt {
    uint64_t context=0,owner=0,originalVs=0,originalPs=0,shadowVs=0,shadowPs=0,actualVs=0,actualPs=0,patchedVs=0,patchedPs=0;
    uint32_t vsGeneration=0,psGeneration=0,currentVsGeneration=0,currentPsGeneration=0;
};
inline bool cloneReceipt(const CloneReceipt& r) {
    return r.context&&r.context==r.owner&&r.originalVs&&r.originalVs==r.shadowVs&&r.originalPs==r.shadowPs&&
        r.vsGeneration==r.currentVsGeneration&&r.psGeneration==r.currentPsGeneration&&
        r.actualVs==(r.patchedVs?r.patchedVs:r.originalVs)&&r.actualPs==(r.patchedPs?r.patchedPs:r.originalPs);
}
struct Authority {
    uint64_t frame=0,hdr=0,raw=0,linear=0,token=0;bool live=false;
    bool affected(uint64_t current,uint64_t resource)const {
        return live&&frame==current&&(!resource||resource==raw||resource==linear);
    }
};
struct Issue {
    enum Kind:uint32_t {None,Converter,Light,Geometry};Kind kind=None;
    uint64_t token=0,frame=0,color=0,vs=0,ps=0;uint32_t q=0,linearSlot=0;
    Surface raw{},linear{};ConstantIdentity constants[2]{};
    float phaseX=0,phaseY=0;unsigned char camera[96]{};bool depthWrite=false,donated=false,issued=false,returned=false;
};
inline bool close(double a,double b,unsigned n=12) {
    return std::abs(a-b)<=flatDeferredFloatBound(n,std::abs(a)+std::abs(b));
}
// Used view2..4, forward10..13 only. Unused inverse-view6..9 supplies no authority.
// Scope: centered underlying projection; asymmetric/off-center views refuse.
// The original raster matrix must carry exactly this frame's upstream phase.
inline Reason match(const unsigned char* cameraBytes,const unsigned char* cbBytes,const FlatProjectionJitter& phase) {
    if(!cameraBytes||!cbBytes||!flat_projection_detail::finite(phase))return Reason::OriginalMissing;
    float camera[6][4],cb[47][4];std::memcpy(camera,cameraBytes,sizeof(camera));std::memcpy(cb,cbBytes,sizeof(cb));
    for(const auto& row:camera)for(float x:row)if(!std::isfinite(x))return Reason::OriginalMissing;
    for(unsigned r:{2u,3u,4u,10u,11u,12u,13u})for(float x:cb[r])if(!std::isfinite(x))return Reason::OriginalMissing;
    if(camera[0][2]!=0||camera[1][2]!=0||camera[2][2]!=0||camera[3][0]!=0||camera[3][1]!=0||
       camera[3][3]!=0||camera[3][2]<=0||camera[4][0]!=camera[0][3]||camera[4][1]!=camera[1][3]||camera[4][2]!=camera[2][3])return Reason::Forward;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)if(!close(cb[10+r][c],camera[c][r],8))return Reason::Forward;
    if(cb[12][3]!=camera[3][2])return Reason::Forward;
    for(unsigned r:{0u,1u,3u}) {
        double residual=cb[10+r][3],mag=std::abs(residual);
        for(unsigned c=0;c<3;++c){const double t=double(camera[c][r])*camera[5][c];residual+=t;mag+=std::abs(t);}
        if(std::abs(residual)>flatDeferredFloatBound(8,mag))return Reason::Origin;
    }
    for(unsigned r=0;r<3;++r) {
        double center=cb[2+r][3],mag=std::abs(center);
        for(unsigned c=0;c<3;++c){const double t=double(cb[2+r][c])*camera[5][c];center+=t;mag+=std::abs(t);}
        if(std::abs(center)>flatDeferredFloatBound(12,mag))return Reason::Origin;
        for(unsigned j=0;j<3;++j){double dot=0,terms=1;
            for(unsigned c=0;c<3;++c){double v=double(cb[2+r][c])*cb[2+j][c];dot+=v;terms+=std::abs(v);}
            if(std::abs(dot-(r==j?1:0))>flatDeferredFloatBound(16,terms))return Reason::View;}
        if(!close(cb[13][r],cb[4][r]))return Reason::View;
    }
    if(!close(cb[13][3],cb[4][3]))return Reason::View;
    const double determinant=double(cb[2][0])*(double(cb[3][1])*cb[4][2]-double(cb[3][2])*cb[4][1])-
        double(cb[2][1])*(double(cb[3][0])*cb[4][2]-double(cb[3][2])*cb[4][0])+
        double(cb[2][2])*(double(cb[3][0])*cb[4][1]-double(cb[3][1])*cb[4][0]);
    if(!close(determinant,1,32))return Reason::View;
    const double shift[]={phase.ndcX,phase.ndcY};
    for(unsigned r=0;r<2;++r) {
        double scale=0;
        for(unsigned c=0;c<3;++c)scale+=(double(cb[10+r][c])-shift[r]*cb[13][c])*cb[2+r][c];
        if(!std::isfinite(scale)||scale>std::numeric_limits<float>::max()||scale<std::numeric_limits<float>::min())return Reason::View;
        const float nativeScale=static_cast<float>(scale),reciprocal=1.0f/nativeScale;
        if(!(scale>0)||!std::isnormal(nativeScale)||!std::isnormal(reciprocal))return Reason::View;
        // An expected nonzero raster shift must be distinguishable from no
        // shift in the actual float matrix. Huge rotated bases can erase it.
        bool resolved=shift[r]==0;
        for(unsigned c=0;c<3;++c)if(std::abs(shift[r]*cb[13][c])>
            flatDeferredFloatBound(24,std::abs(cb[10+r][c])+std::abs(scale*cb[2+r][c])))resolved=true;
        if(!resolved)return Reason::Phase;
        for(unsigned c=0;c<4;++c){const double a=double(cb[10+r][c])-shift[r]*cb[13][c],b=scale*cb[2+r][c];
            const double mag=std::abs(cb[10+r][c])+std::abs(shift[r]*cb[13][c])+std::abs(b);
            if(std::abs(a-b)>flatDeferredFloatBound(24,mag))return Reason::Phase;}
    }
    return Reason::None;
}
} } // namespace edvr::forward_light
