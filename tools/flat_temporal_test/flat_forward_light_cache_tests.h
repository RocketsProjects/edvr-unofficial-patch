#pragma once
#include "../../src/d3d11/flat_forward_light.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace forward_light_cache_test {
// A COM-like handle with visible AddRef/Release counts. Keeping one reference
// to a view is what makes pointer identity safe across a later allocator reuse.
struct Object { int refs=0; };
struct Hold {
    Object* p=nullptr;
    Hold()=default;
    explicit Hold(Object* q):p(q){if(p)++p->refs;}
    Hold(const Hold& h):Hold(h.p){}
    Hold& operator=(const Hold& h){
        if(this!=&h){Object* next=h.p;if(next)++next->refs;Reset();p=next;}
        return *this;
    }
    ~Hold(){Reset();}
    Object* Get()const{return p;}
    void Reset(){if(p){--p->refs;p=nullptr;}}
};
struct Description { int resource=0, flags=0; };

inline int run() {
    unsigned checks=0;int failures=0;
    const auto expect=[&](bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL: forward cache %s\n",why);}};
    using Cache=edvr::forward_light::ImmutableDescriptionCache<Hold,Description>;
    Object a{},b{},depth{};
    Hold first(&a),second(&b),depthState(&depth),missing;
    Cache dsv,depthDesc;
    Description scratch{};bool hit=true;int loads=0;
    const auto loadFirst=[&](Description& value){++loads;value={11,0};return true;};
    expect(!dsv.describe(missing,scratch,hit,loadFirst)&&!hit&&!loads,
           "null binding cannot use a cache entry or invoke the metadata loader");
    const auto* initial=dsv.describe(first,scratch,hit,loadFirst);
    expect(initial&&initial->resource==11&&!hit&&loads==1&&a.refs==2,
           "first description load retains the actual view identity");
    for(int i=0;i<20;++i) {
        const auto* again=dsv.describe(first,scratch,hit,loadFirst);
        expect(again&&again->resource==11&&hit,"repeated actual view uses retained immutable description");
    }
    expect(loads==1,"production-used helper collapses repeated metadata loads");
    first.Reset();
    expect(a.refs==1&&dsv.held.Get()==&a,"cached view remains alive after getter's temporary reference is released");
    Hold firstAgain(&a);
    const auto* same=dsv.describe(firstAgain,scratch,hit,loadFirst);
    expect(same&&hit&&loads==1,"new getter reference to the same live view still hits");
    const auto loadSecond=[&](Description& value){++loads;value={22,1};return true;};
    const auto* switched=dsv.describe(second,scratch,hit,loadSecond);
    expect(switched&&switched->flags==1&&!hit&&loads==2&&a.refs==1&&b.refs==2,
           "different read-only view misses and evicts prior retained identity");
    firstAgain.Reset();
    expect(a.refs==0&&b.refs==2,"evicted address can become reusable only after all references leave");
    Hold firstReturned(&a);
    const auto* reloaded=dsv.describe(firstReturned,scratch,hit,loadFirst);
    expect(reloaded&&reloaded->flags==0&&!hit&&loads==3&&b.refs==1,
           "one-entry eviction forces a fresh description on return to the old identity");
    dsv.clear();
    const auto badLoad=[&](Description& value){++loads;value={0,9};return false;};
    for(int i=0;i<2;++i) {
        const auto* bad=dsv.describe(second,scratch,hit,badLoad);
        expect(bad==&scratch&&bad->resource==0&&!hit&&!dsv.valid,
               "failed metadata remains an uncached observation and retries on next use");
    }
    expect(loads==5&&b.refs==1,"failed observations invoke loader again without retaining the view");
    int depthLoads=0;
    const auto depthLoad=[&](Description& value){++depthLoads;value={1,0};return true;};
    Description depthScratch{};
    expect(depthDesc.describe(depthState,depthScratch,hit,depthLoad)&&!hit&&depthLoads==1&&depth.refs==2,
           "independent depth-state description cache retains its state identity");
    expect(depthDesc.describe(depthState,depthScratch,hit,depthLoad)&&hit&&depthLoads==1,
           "repeated depth state skips its GetDesc loader");
    expect(!depthDesc.describe(missing,depthScratch,hit,depthLoad)&&!hit&&depthLoads==1,
           "null depth state cannot alias a retained non-null description");
    dsv.clear();depthDesc.clear();
    expect(!dsv.find(firstReturned)&&!depthDesc.find(depthState)&&a.refs==1&&depth.refs==1,
           "work/frame reset releases both cached COM identities and descriptions");

    // The pure cache cannot prove that the production adapter asks D3D for
    // current mutable state. Pin that independent boundary at the call site.
    std::ifstream input("src/d3d11/flat_forward_light_runtime.inl",std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)),{});
    const auto begin=source.find("bool forwardDsv(State& s,");
    const auto end=source.find("bool forwardViewport(",begin);
    const std::string body=begin!=std::string::npos&&end!=std::string::npos?source.substr(begin,end-begin):std::string{};
    const auto inOrder=[&](const char* a,const char* b){const auto x=body.find(a),y=body.find(b);return x!=std::string::npos&&y!=std::string::npos&&x<y;};
    expect(inOrder("OMGetRenderTargets(0,nullptr,&dsv)","cache.describe(dsv,fresh,dsvHit")&&
           inOrder("cache.describe(dsv,fresh,dsvHit","dsv->GetResource(&value.resource)")&&
           body.find("return value.resource&&value.surface.known;")!=std::string::npos,
           "actual OM DSV query precedes production helper and failed metadata is not retained");
    expect(inOrder("OMGetDepthStencilState(&state,&ref)","depthCache.describe(state,freshDepth,depthHit")&&
           body.find("else {counts.add(flatcpu::kCopyRawDepthDefault);s.forwardLight.depthDescription.clear();}")!=std::string::npos&&
           body.find("writes=z.DepthEnable&&z.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL&&!(v.Flags&D3D11_DSV_READ_ONLY_DEPTH)")!=std::string::npos,
           "actual depth state is queried each draw; null/default and read-only write proof remain live");
    const auto reset=source.find("void forwardReset(State& s,");
    expect(reset!=std::string::npos&&source.find("f.dsvDescription.clear();f.depthDescription.clear();",reset)!=std::string::npos,
           "forward lifecycle reset releases both caches");
    std::printf("forward light cache: %u checks, %d failed\n",checks,failures);
    return failures;
}
} // namespace forward_light_cache_test
