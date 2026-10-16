#pragma once
// The claim check (docs\flat-cz-fps-2026-10-11.md, round 5 results): a clear or a merge that finds claims live but none in the list
// it walks is handed back without g_mutex, from an immutable snapshot of the live claim addresses. Every check's text starts
// "claim check C<n>:" and mutants.py names the case each mutation of the check must break.
//   C1  zero-hit calls under a live claim take no mutex and allocate nothing (and the same calls with the check off do both)
//   C2  the clear: hit, miss and refused shapes leave exactly the state the pre-change walk leaves
//   C3  the merge: hit, miss and refused lists leave exactly the state the pre-change merge leaves (reference copy below)
//   C4  more than four claims: every call takes the mutex, as before
//   C5  a plan in flight: every call takes the mutex, as before
//   C6  the snapshot equals the containers after every entry point, and is rewritten only when it changes
//   C7  a claim published while a call is checking is never lost and never wrongly moved: a torn read falls back to the mutex,
//       a claim published after the check is ordered after the call, and four threads of churn never lose or move a claim
#include "primary_copy_observer_tests.h"
#include "allocation_counter.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace claim_check_tests {
namespace copy=edvr::engine_velocity_primary_copy;
namespace emit=edvr::engine_velocity_emit;
namespace obs=primary_copy_observer_tests;
using obs::Check;
using obs::Destination;
using obs::Tree;
using obs::addressOf;
using obs::kFrame;
using obs::published;
using primary_copy_tests::Record;
using primary_copy_tests::claim;
using primary_copy_tests::record;

// Under the mutex: the rig reads the claim table while other threads churn it.
inline bool heldLocked(uintptr_t address) {copy::ClaimLock lock;return copy::g_emissions.count(address)==1;}

// The merge exactly as it was before the claim check and the shared walk: always the mutex, the whole body inline.
inline void* referenceBeginMerge(uintptr_t destination,uintptr_t source,uint32_t frame) noexcept {
    copy::ClaimLock lock(copy::Caller::MergeBegin);
    ++copy::g_stats.mergeCalls;
    if(copy::g_emissions.empty()){++copy::g_stats.mergeWithoutClaims;return copy::mergeNoClaimsToken();}
    try {
        uintptr_t dstLinks[2]{},srcLinks[2]{};
        if(!destination || !source || destination==source ||
           !emit::read(destination,dstLinks,sizeof(dstLinks)) || !emit::read(source,srcLinks,sizeof(srcLinks))) {copy::invalidateClaims();return nullptr;}
        auto plan=std::make_unique<copy::MergePlan>();plan->epoch=copy::g_emissionEpoch;plan->frame=frame;
        if(copy::g_mergePlans.size()>=copy::kMaxPools){copy::invalidateClaims();return nullptr;}
        std::vector<uintptr_t> clear;
        copy::AddressSet visited;
        uintptr_t tail=dstLinks[1],node=srcLinks[0],previous=source;
        uint64_t tailCount=0;
        if(dstLinks[0]==destination) {
            if(tail!=destination){copy::invalidateClaims();return nullptr;}
        } else {
            uintptr_t next=0;
            if(!tail || !emit::read(tail,&next,sizeof(next)) || next!=destination ||
               !emit::read(tail+emit::kNodeCount,&tailCount,8) || tailCount>8) {copy::invalidateClaims();return nullptr;}
        }
        while(node!=source) {
            uint64_t header[4]{};
            if(!node || node==destination || node==tail || node>UINTPTR_MAX-(emit::kNodeRecords+8*copy::kBytes) ||
               visited.size()>=copy::kMaxEmissions/8 || !visited.insert(node).second ||
               !emit::read(node,header,sizeof(header)) || header[1]!=previous || header[3]==0 || header[3]>8) {copy::invalidateClaims();return nullptr;}
            ++copy::g_stats.mergeNodes;
            const uint32_t count=uint32_t(header[3]);
            uint32_t fill=0;
            if(count!=8 && tail!=destination && tailCount!=8)fill=std::min(count,uint32_t(8-tailCount));
            for(uint32_t i=0;i<8;++i)clear.push_back(node+emit::kNodeRecords+uintptr_t(i)*copy::kBytes);
            for(uint32_t i=0;i<fill;++i)clear.push_back(tail+emit::kNodeRecords+uintptr_t(tailCount+i)*copy::kBytes);
            for(uint32_t i=0;i<count;++i) {
                const uintptr_t old=node+emit::kNodeRecords+uintptr_t(i)*copy::kBytes;
                const auto claimed=copy::g_emissions.find(old);
                if(claimed==copy::g_emissions.end() || claimed->second.frame!=frame)continue;
                std::array<uint32_t,copy::kWords> native{};
                if(!emit::read(old,native.data(),copy::kBytes)){copy::invalidateClaims();return nullptr;}
                if(native!=claimed->second.patch.native)continue;
                const uintptr_t target=i<fill?tail+emit::kNodeRecords+uintptr_t(tailCount+i)*copy::kBytes:
                    node+emit::kNodeRecords+uintptr_t(i-fill)*copy::kBytes;
                plan->transfers.push_back({target,claimed->second});
            }
            tailCount+=fill;
            if(count==8){if(tail==destination){tail=node;tailCount=8;}}
            else if(count>fill){tail=node;tailCount=count-fill;}
            previous=node;node=uintptr_t(header[0]);
        }
        if(srcLinks[1]!=previous){copy::invalidateClaims();return nullptr;}
        for(uintptr_t address:clear)copy::g_emissions.erase(address);
        plan->addresses=std::move(clear);copy::g_mergePlans.insert(plan.get());
        ++copy::g_stats.mergePlans;
        return plan.release();
    } catch(...) {copy::invalidateClaims();return nullptr;}
}

struct ClearResult {
    std::vector<uintptr_t> keys;
    uint64_t clearedClaims=0,invalidated=0,clearFailed=0,sourceResets=0,declined=0,epochDelta=0;
    bool operator==(const ClearResult& o) const {
        return keys==o.keys && clearedClaims==o.clearedClaims && invalidated==o.invalidated && clearFailed==o.clearFailed &&
               sourceResets==o.sourceResets && declined==o.declined && epochDelta==o.epochDelta;
    }
};
inline ClearResult clearResult(uint64_t epoch) {
    ClearResult r;for(const auto& e:copy::g_emissions)r.keys.push_back(e.first);std::sort(r.keys.begin(),r.keys.end());
    const auto s=copy::stats();
    r.clearedClaims=s.clearedClaims;r.invalidated=s.invalidated;r.clearFailed=s.clearFailed;r.sourceResets=s.sourceResets;r.declined=s.declined;
    r.epochDelta=copy::g_emissionEpoch-epoch;return r;
}

struct Claim { char where;unsigned node,slot; };   // 's': a source node, 't': the destination's tail node
struct MergeCase {
    const char* name;
    unsigned srcNodes;unsigned srcCount[3];
    int tail;                 // the destination tail node's count, or -1 for an empty destination
    Claim claims[3];unsigned nclaims;
    int corrupt;              // 0 none; 1 a node's previous link; 2 a node count 0; 3 a self-linked node; 4 the list's last link;
                              // 5 a tail count of 9; 6 a tail whose next is not the destination
    bool hit;                 // a claim is in a slot the merge clears or overwrites
};
struct MergeResult {
    int kind=0;                                  // 0 refused (nullptr), 1 no transfers (token or an empty plan), 2 transfers staged
    std::vector<uintptr_t> targets,keysBegin,keysEnd;
    uint64_t epochDelta=0,mergeFailed=0,declined=0,sourceResets=0;
    bool operator==(const MergeResult& o) const {
        return kind==o.kind && targets==o.targets && keysBegin==o.keysBegin && keysEnd==o.keysEnd && epochDelta==o.epochDelta &&
               mergeFailed==o.mergeFailed && declined==o.declined && sourceResets==o.sourceResets;
    }
};
inline std::vector<uintptr_t> keysNow() {
    std::vector<uintptr_t> k;for(const auto& e:copy::g_emissions)k.push_back(e.first);std::sort(k.begin(),k.end());return k;
}

// Publishes a claim from inside a reader's window (rig hooks); one shot.
inline Record* g_hookRecord=nullptr;
inline void publishHookClaim() noexcept {
    copy::g_claimMidRead=nullptr;copy::g_claimAfterWalk=nullptr;
    claim(*g_hookRecord,kFrame);
}

inline void run(ID3D11Device* device,Check check) {
    (void)device;
    // ---- C1: zero-hit calls under a live claim take no mutex and allocate nothing -----------------------------------------
    for(unsigned live=1;live<=copy::kClaimViewMax;++live) {
        Tree eight(1,8),source(1,2);Destination dst;std::vector<Record> claims(live,record(7));
        for(unsigned i=0;i<live;++i)claims[i]=record(float(7+i));
        constexpr unsigned N=1000;
        auto traffic=[&]{for(unsigned i=0;i<N;++i){copy::invalidateDictionary(eight.ptr());copy::endMergeOpaque(copy::beginMergeOpaque(dst.ptr(),source.anchor(0),kFrame),true);}};
        copy::reset();for(auto& r:claims)claim(r,kFrame);
        rig_allocations::start();traffic();const auto allocations=rig_allocations::stop();
        const auto s=copy::stats();const auto lockedWith=copy::g_stats.clearCalls+copy::g_stats.mergeCalls;
        const std::string suffix=" ("+std::to_string(live)+" live)";
        check(copy::g_stats.clearCalls==0 && copy::g_stats.mergeCalls==0 && s.clearPrechecked==N && s.mergePrechecked==N && s.clearCalls==N && s.mergeCalls==N &&
              s.clearLockFree==N && s.mergeLockFree==N && s.clearNoClaims==0 && s.mergeWithoutClaims==0 && s.precheckDeclined==0,
              ("claim check C1: clears and merges under live claims that they do not touch take the mutex 0 times, and are counted as lock-free calls"+suffix).c_str());
        check(allocations==0 && copy::g_mergePlans.empty() && copy::g_emissions.size()==live && published(),
              ("claim check C1: and allocate nothing, register no plan and leave the claims and the published state exact"+suffix).c_str());
        // The same calls with the check off (the code as it was): the instrument is live.
        copy::reset();for(auto& r:claims)claim(r,kFrame);copy::g_claimPrecheck=false;
        rig_allocations::start();traffic();const auto offAllocations=rig_allocations::stop();
        copy::g_claimPrecheck=true;
        std::printf("claim check C1 (%u live claims, %u clears and %u merges): with the check: %llu mutex acquisitions, %llu heap allocations; check off (the code as it was): %llu acquisitions, %llu allocations\n",
                    live,N,N,static_cast<unsigned long long>(lockedWith),static_cast<unsigned long long>(allocations),static_cast<unsigned long long>(copy::g_stats.clearCalls+copy::g_stats.mergeCalls),static_cast<unsigned long long>(offAllocations));
        check(copy::g_stats.clearCalls==N && copy::g_stats.mergeCalls==N && offAllocations>0 && copy::stats().clearPrechecked==0,
              ("claim check C1: with the check off the same calls take the mutex "+std::to_string(N)+" times each and the merge allocates (the counters are live)"+suffix).c_str());
        copy::reset();
    }
    // ---- C2: the clear -------------------------------------------------------------------------------------------------------
    {
        struct Shape { const char* name;unsigned entries,nodes;void (*corrupt)(Tree&);unsigned claimNode[3],claimSlot[3];unsigned claims;bool refused; };
        const Shape shapes[]={
            {"1 node",1,1,nullptr,{0,0,0},{0,3,0},2,false},
            {"1 node, one claim in its last slot",1,1,nullptr,{0,0,0},{7,0,0},1,false},
            {"1 node, one claim in its first slot",1,1,nullptr,{0,0,0},{0,0,0},1,false},
            {"8 nodes",1,8,nullptr,{0,3,7},{2,2,7},3,false},
            {"9 nodes",1,9,nullptr,{8,0,0},{7,0,0},2,false},
            {"70 nodes",1,70,nullptr,{0,64,69},{0,3,7},3,false},
            {"70 entries",70,1,nullptr,{0,0,0},{0,4,7},3,false},
            {"an empty entry",1,0,nullptr,{0,0,0},{0,0,0},0,false},
            {"a duplicated node",1,3,obs::cycleThroughEarlierNode,{0,2,0},{0,1,0},2,true},
            {"a self-linked node",1,2,obs::selfLoop,{0,0,0},{0,0,0},1,true},
            {"a node count of 9",1,3,obs::countNine,{0,0,0},{0,0,0},1,true},
            {"an unreadable node",1,2,obs::unreadableNode,{0,0,0},{0,0,0},1,true},
            {"a bucket count over the bound",1,1,obs::bucketsOverBound,{0,0,0},{0,0,0},1,true},
            {"a second bucket aliasing the first entry",1,1,obs::aliasedBucket,{0,0,0},{0,0,0},1,true},
            {"2048 nodes",1,2048,nullptr,{0,1000,2047},{0,5,7},3,false},
            {"2049 nodes",1,2049,nullptr,{0,0,0},{0,0,0},1,true},
        };
        for(const auto& shape:shapes) {
            Tree tree(shape.entries,shape.nodes);if(shape.corrupt)shape.corrupt(tree);
            Record other=record(900);
            for(int inTree=1;inTree>=0;--inTree) {
                ClearResult result[2];uint64_t prechecked=0;
                for(int reference=1;reference>=0;--reference) {
                    copy::reset();const auto epoch=copy::g_emissionEpoch;
                    if(inTree)for(unsigned k=0;k<shape.claims;++k){Record& r=tree.item(0,shape.claimNode[k],shape.claimSlot[k]);r=record(float(10+k));claim(r,kFrame);}
                    claim(other,kFrame);
                    if(reference)obs::referenceClear(tree.ptr());else copy::invalidateDictionary(tree.ptr());
                    check(published(),"claim check C6: the claim snapshot and both flags are exact after a clear, hit or miss");
                    result[reference]=clearResult(epoch);if(!reference)prechecked=copy::stats().clearPrechecked;
                }
                const std::string label=std::string("claim check C2: clear (")+shape.name+(inTree?", claims in the freed nodes":", a claim elsewhere only")+"): ";
                check(result[0]==result[1],(label+"same claims, counters, failure and epoch as the pre-change walk").c_str());
                if(inTree && !shape.refused && shape.claims)check(result[0].clearedClaims==shape.claims && prechecked==0,(label+"every claim in the nodes is revoked under the mutex").c_str());
                if(inTree && shape.refused)check(result[0].clearFailed==1 && result[0].keys.empty() && prechecked==0,(label+"a refused walk clears every claim, under the mutex").c_str());
                if(!inTree && !shape.refused)check(prechecked==1 && result[0].clearedClaims==0 && result[0].keys.size()==1,(label+"handed back without the mutex, the claim kept").c_str());
                if(!inTree && shape.refused)check(prechecked==0 && result[0].clearFailed==1 && result[0].keys.empty(),(label+"a refused walk still reaches the mutex and clears the claim").c_str());
            }
        }
    }
    // ---- C3: the merge -------------------------------------------------------------------------------------------------------
    {
        const MergeCase cases[]={
            {"one full node into an empty destination, claim in slot 3",1,{8,0,0},-1,{{'s',0,3}},1,0,true},
            {"a partial node (5), claim in slot 6 past its count",1,{5,0,0},-1,{{'s',0,6}},1,0,true},
            {"two nodes (8, 3), claim in the second",2,{8,3,0},-1,{{'s',1,2}},1,0,true},
            {"full node into a partial tail (5), claim in the tail's old slot 2",1,{8,0,0},5,{{'t',0,2}},1,0,false},
            {"3 records into a tail of 5, claim in tail slot 6 (filled)",1,{3,0,0},5,{{'t',0,6}},1,0,true},
            {"3 records into a tail of 5, claim in tail slot 5 (the first filled)",1,{3,0,0},5,{{'t',0,5}},1,0,true},
            {"3 records into a tail of 5, claim in tail slot 4 (below the count)",1,{3,0,0},5,{{'t',0,4}},1,0,false},
            {"3 records into a tail of 5, claim in tail slot 7 (the last filled)",1,{3,0,0},5,{{'t',0,7}},1,0,true},
            {"6 records into a tail of 7, claim in tail slot 7 (the one filled)",1,{6,0,0},7,{{'t',0,7}},1,0,true},
            {"6 records into a tail of 7, claim in tail slot 6 (kept)",1,{6,0,0},7,{{'t',0,6}},1,0,false},
            {"3 records into a full tail, claim in tail slot 7 (kept)",1,{3,0,0},8,{{'t',0,7}},1,0,false},
            {"3 records into an empty tail (0), claim in tail slot 1 (filled)",1,{3,0,0},0,{{'t',0,1}},1,0,true},
            {"nodes (8, 8, 2) into a tail of 4, claim in tail slot 5 (filled)",3,{8,8,2},4,{{'t',0,5}},1,0,true},
            {"nodes (8, 8, 2) into a tail of 4, claim in tail slot 3 (kept)",3,{8,8,2},4,{{'t',0,3}},1,0,false},
            {"a claim in the source and one in the filled tail slots",1,{3,0,0},5,{{'s',0,1},{'t',0,5}},2,0,true},
            {"a previous link that is wrong, claim in the source",2,{8,3,0},-1,{{'s',0,0}},1,1,true},
            {"a node count of 0, claim in the source",1,{8,0,0},-1,{{'s',0,0}},1,2,true},
            {"a self-linked node, claim in the source",2,{8,3,0},-1,{{'s',0,0}},1,3,true},
            {"a list whose last link is wrong, claim in the source",1,{8,0,0},-1,{{'s',0,0}},1,4,true},
            {"a tail count of 9, claim in the source",1,{3,0,0},9,{{'s',0,0}},1,5,true},
            {"a tail whose next is not the destination, claim in the source",1,{3,0,0},5,{{'s',0,0}},1,6,true},
        };
        for(const auto& c:cases) {
            std::unique_ptr<Tree> src(new Tree(1,c.srcNodes)),dstTree;
            for(unsigned n=0;n<c.srcNodes;++n)src->setCount(0,n,c.srcCount[n]);
            Destination emptyDst;
            if(c.tail>=0){dstTree.reset(new Tree(1,1));dstTree->setCount(0,0,uint64_t(c.tail));}
            switch(c.corrupt) {
            case 1:src->setWord(reinterpret_cast<uintptr_t>(src->node(0,c.srcNodes>1?1:0).data)+8,0x10);break;
            case 2:src->setCount(0,0,0);break;
            case 3:src->setWord(reinterpret_cast<uintptr_t>(src->node(0,c.srcNodes>1?1:0).data),src->node(0,c.srcNodes>1?1:0).ptr());break;
            case 4:src->setWord(src->entryAddress(0)+0x30,0x10);break;
            case 5:dstTree->setCount(0,0,9);break;
            case 6:dstTree->setWord(reinterpret_cast<uintptr_t>(dstTree->node(0,0).data),0x10);break;
            default:break;
            }
            const uintptr_t destination=dstTree?dstTree->anchor(0):emptyDst.ptr();
            Record other=record(901);
            for(int atFrame=0;atFrame<2;++atFrame) for(int inLists=1;inLists>=0;--inLists) {
                MergeResult result[2];bool token=false;uint64_t locked=0,prechecked=0;
                for(int reference=1;reference>=0;--reference) {
                    copy::reset();const auto epoch=copy::g_emissionEpoch;
                    if(inLists)for(unsigned k=0;k<c.nclaims;++k) {
                        Tree& t=c.claims[k].where=='s'?*src:*dstTree;
                        Record& r=t.item(0,c.claims[k].node,c.claims[k].slot);r=record(float(10+k));claim(r,kFrame);
                    }
                    claim(other,kFrame);
                    const uint32_t frame=atFrame?kFrame+1:kFrame;
                    void* plan=reference?referenceBeginMerge(destination,src->anchor(0),frame):copy::beginMergeOpaque(destination,src->anchor(0),frame);
                    MergeResult& r=result[reference];
                    r.kind=!plan?0:(plan==copy::mergeNoClaimsToken() || static_cast<copy::MergePlan*>(plan)->transfers.empty())?1:2;
                    if(plan && plan!=copy::mergeNoClaimsToken())for(const auto& t:static_cast<copy::MergePlan*>(plan)->transfers)r.targets.push_back(t.target);
                    std::sort(r.targets.begin(),r.targets.end());r.keysBegin=keysNow();
                    if(!reference){token=plan==copy::mergeNoClaimsToken();locked=copy::g_stats.mergeCalls;prechecked=copy::stats().mergePrechecked;}
                    check(published(),"claim check C6: the claim snapshot and both flags are exact after a merge begins, hit or miss");
                    copy::endMergeOpaque(plan,true);
                    check(published(),"claim check C6: and after it ends");
                    r.keysEnd=keysNow();const auto s=copy::stats();
                    r.epochDelta=copy::g_emissionEpoch-epoch;r.mergeFailed=s.mergeFailed;r.declined=s.declined;r.sourceResets=s.sourceResets;
                }
                const std::string label=std::string("claim check C3: merge (")+c.name+(atFrame?", another frame":"")+(inLists?", claims in the lists":", a claim elsewhere only")+"): ";
                check(result[0]==result[1],(label+"same claims at begin and at end, same transfers, failure, epoch and counters as the pre-change merge").c_str());
                if(inLists && c.hit && c.corrupt==0)check(!token && locked==1 && prechecked==0,(label+"a claim in a slot the merge clears takes the mutex and builds the plan").c_str());
                if(inLists && !c.hit && c.corrupt==0)check(token && prechecked==1 && locked==0,(label+"claims in slots the merge leaves alone are handed back without the mutex").c_str());
                if(!inLists && c.corrupt==0)check(token && prechecked==1 && locked==0,(label+"handed back without the mutex").c_str());
                if(c.corrupt!=0)check(!token && prechecked==0 && result[0].kind==0 && result[0].keysBegin.empty() && result[0].mergeFailed==1,(label+"a refused list reaches the mutex, which clears every claim").c_str());
            }
        }
    }
    // ---- C4: more than four claims ---------------------------------------------------------------------------------------------
    {
        Tree tree(1,1),source(1,2);Destination dst;tree.item(0,0,0)=record(40);
        std::vector<Record> r;for(unsigned i=0;i<6;++i)r.push_back(record(float(50+i)));
        copy::reset();for(unsigned i=0;i<4;++i)claim(r[i],kFrame);
        copy::invalidateDictionary(tree.ptr());copy::endMergeOpaque(copy::beginMergeOpaque(dst.ptr(),source.anchor(0),kFrame),true);
        auto s=copy::stats();
        check(copy::g_stats.clearCalls==0 && copy::g_stats.mergeCalls==0 && s.clearPrechecked==1 && s.mergePrechecked==1 && copy::g_claimView.count.load()==4,
              "claim check C4: with four claims live (the limit) a miss is handed back without the mutex");
        claim(r[4],kFrame);
        check(copy::g_claimView.count.load()==copy::kClaimViewMax+1 && published(),"claim check C4: a fifth claim makes the snapshot say 'more than it holds'");
        copy::invalidateDictionary(tree.ptr());void* plan=copy::beginMergeOpaque(dst.ptr(),source.anchor(0),kFrame);
        s=copy::stats();
        check(copy::g_stats.clearCalls==1 && copy::g_stats.mergeCalls==1 && s.clearPrechecked==1 && s.mergePrechecked==1 && s.precheckDeclined==2 &&
              plan!=copy::mergeNoClaimsToken() && copy::g_mergePlans.size()==1,
              "claim check C4: with five claims live both calls take the mutex as before (a plan is registered), and are counted as declined");
        copy::endMergeOpaque(plan,true);
        // The fifth claim is the one in the list: the locked path revokes it.
        claim(tree.item(0,0,0),kFrame);
        check(copy::g_emissions.size()==6 && copy::g_claimView.count.load()==copy::kClaimViewMax+1,"claim check C4: six claims live");
        copy::invalidateDictionary(tree.ptr());
        check(!heldLocked(addressOf(tree.item(0,0,0))) && copy::g_emissions.size()==5 && published(),"claim check C4: with more than four claims a claim in the freed nodes is revoked");
        copy::invalidateEmission(addressOf(r[4]));
        check(copy::g_emissions.size()==4 && copy::g_claimView.count.load()==4 && published(),"claim check C4: back to four claims the snapshot holds them again");
        copy::invalidateDictionary(tree.ptr());
        check(copy::g_stats.clearCalls==2 && copy::stats().clearPrechecked==2,"claim check C4: and the check works again");
        copy::reset();
    }
    // ---- C5: a plan in flight ----------------------------------------------------------------------------------------------------
    {
        Tree held(1,1),unrelated(1,1),other(1,1);Destination dst,dst2;held.item(0,0,0)=record(60);unrelated.item(0,0,0)=record(61);other.item(0,0,0)=record(62);
        Record live=record(63);
        copy::reset();claim(live,kFrame);claim(held.item(0,0,0),kFrame);
        void* plan=copy::beginMergeOpaque(dst.ptr(),held.anchor(0),kFrame+1);   // touches the claim of another frame: a plan that stages nothing
        check(plan!=copy::mergeNoClaimsToken() && copy::g_mergePlans.size()==1 && copy::g_claimView.plans.load() && published(),"claim check C5: a plan in flight is in the snapshot");
        const auto lockedBefore=copy::g_stats.clearCalls+copy::g_stats.mergeCalls;const auto declined=copy::stats().precheckDeclined;
        const auto walked=copy::g_stats.clearNodes;
        copy::invalidateDictionary(unrelated.ptr());
        void* second=copy::beginMergeOpaque(dst2.ptr(),other.anchor(0),kFrame);
        check(copy::g_stats.clearCalls+copy::g_stats.mergeCalls==lockedBefore+2 && copy::g_stats.clearNodes>walked && copy::stats().precheckDeclined==declined+2 &&
              second!=copy::mergeNoClaimsToken(),"claim check C5: while a plan is in flight a miss takes the mutex, walks and registers a plan as before");
        copy::endMergeOpaque(second,true);
        copy::invalidateDictionary(held.ptr());
        check(static_cast<copy::MergePlan*>(plan)->invalid,"claim check C5: and a clear of the plan's list marks that plan invalid");
        copy::endMergeOpaque(plan,true);
        check(!copy::g_claimView.plans.load() && published() && heldLocked(addressOf(live)),"claim check C5: once the plans end the snapshot says so, and the claim is untouched");
        const auto lockedAfter=copy::g_stats.clearCalls+copy::g_stats.mergeCalls;
        copy::invalidateDictionary(unrelated.ptr());copy::endMergeOpaque(copy::beginMergeOpaque(dst.ptr(),other.anchor(0),kFrame),true);
        check(copy::g_stats.clearCalls+copy::g_stats.mergeCalls==lockedAfter,"claim check C5: afterwards a miss is lock-free again");
        copy::reset();
    }
    // ---- C6: the snapshot is exact, sorted, and rewritten only when it changes -------------------------------------------------------
    {
        std::vector<Record> r;for(unsigned i=0;i<6;++i)r.push_back(record(float(70+i)));
        copy::reset();
        const uint32_t seq0=copy::g_claimView.seq.load();
        check(copy::g_claimView.count.load()==0 && !copy::g_claimView.plans.load() && published(),"claim check C6: after a reset the snapshot is empty");
        claim(r[3],kFrame);claim(r[0],kFrame);claim(r[5],kFrame);
        uintptr_t a[3];for(unsigned i=0;i<3;++i)a[i]=copy::g_claimView.address[i].load();
        check(copy::g_claimView.count.load()==3 && a[0]<a[1] && a[1]<a[2] && published() && copy::g_claimView.seq.load()==seq0+6,
              "claim check C6: three claims are published sorted, one rewrite each (an even sequence)");
        const uint32_t seq1=copy::g_claimView.seq.load();
        claim(r[0],kFrame);copy::invalidateMapped(0);copy::invalidateEmission(addressOf(r[1]));
        check(copy::g_claimView.seq.load()==seq1 && published(),"claim check C6: a repeated claim and calls that change no claim do not rewrite the snapshot");
        copy::invalidateEmission(addressOf(r[0]));
        check(copy::g_claimView.count.load()==2 && copy::g_claimView.seq.load()==seq1+2 && published(),"claim check C6: erasing a claim rewrites it once");
        for(unsigned i=0;i<5;++i)claim(r[i],kFrame);
        check(copy::g_claimView.count.load()==copy::kClaimViewMax+1 && published(),"claim check C6: five claims publish 'more than the snapshot holds'");
        copy::invalidateEmission(0);
        check(copy::g_claimView.count.load()==0 && published(),"claim check C6: clearing every claim empties it");
        copy::reset();
    }
    // ---- C7: a claim published while a call is checking --------------------------------------------------------------------------------
    {
        Tree tree(1,1),mergeTree(1,1);Destination dst;tree.item(0,0,0)=record(80);mergeTree.item(0,0,0)=record(81);Record live=record(82);
        // A claim published between the snapshot's reads and its re-check tears the read: the call takes the mutex and sees it.
        copy::reset();claim(live,kFrame);g_hookRecord=&tree.item(0,0,0);copy::g_claimMidRead=&publishHookClaim;
        copy::invalidateDictionary(tree.ptr());
        check(copy::g_claimMidRead==nullptr && !heldLocked(addressOf(tree.item(0,0,0))) && copy::g_stats.clearCalls==1 && copy::stats().precheckDeclined==1 &&
              copy::stats().clearPrechecked==0 && published(),"claim check C7: a claim published inside a clear's snapshot read tears it; the clear takes the mutex and revokes that claim");
        copy::reset();claim(live,kFrame);g_hookRecord=&mergeTree.item(0,0,0);copy::g_claimMidRead=&publishHookClaim;
        void* plan=copy::beginMergeOpaque(dst.ptr(),mergeTree.anchor(0),kFrame);
        check(plan!=copy::mergeNoClaimsToken() && static_cast<copy::MergePlan*>(plan)->transfers.size()==1 && copy::g_stats.mergeCalls==1 && copy::stats().precheckDeclined==1,
              "claim check C7: a claim published inside a merge's snapshot read tears it; the merge takes the mutex and stages that claim");
        copy::endMergeOpaque(plan,true);
        check(heldLocked(addressOf(mergeTree.item(0,0,0))) && published(),"claim check C7: and the plan's end gives it back");
        // A claim published after the check found the list clean is ordered after the call: the call did not see it and did not touch it,
        // exactly as the mutex allowed for a producer that had not yet taken it.
        copy::reset();claim(live,kFrame);g_hookRecord=&tree.item(0,0,0);copy::g_claimAfterWalk=&publishHookClaim;
        copy::invalidateDictionary(tree.ptr());
        check(copy::stats().clearPrechecked==1 && copy::g_stats.clearCalls==0 && heldLocked(addressOf(tree.item(0,0,0))) && published(),
              "claim check C7: a claim published after a clear's check is ordered after the clear and left alone");
        copy::invalidateDictionary(tree.ptr());
        check(!heldLocked(addressOf(tree.item(0,0,0))) && copy::g_stats.clearCalls==1,"claim check C7: and the next clear sees it and revokes it");
        copy::reset();claim(live,kFrame);g_hookRecord=&mergeTree.item(0,0,0);copy::g_claimAfterWalk=&publishHookClaim;
        plan=copy::beginMergeOpaque(dst.ptr(),mergeTree.anchor(0),kFrame);
        check(plan==copy::mergeNoClaimsToken() && copy::stats().mergePrechecked==1 && heldLocked(addressOf(mergeTree.item(0,0,0))) && copy::g_mergePlans.empty(),
              "claim check C7: a claim published after a merge's check is ordered after the merge: no plan, and the claim is left alone");
        copy::endMergeOpaque(plan,true);
        check(heldLocked(addressOf(mergeTree.item(0,0,0))) && published(),"claim check C7: and the merge's completed end leaves it");
        copy::reset();
        // Four threads. A producer claims a record in a clear-tree and a merge-tree; once it has returned it hands over to a caller (a
        // release/acquire handshake: the claim happened before the call), which clears and merges and must always find the claim. A churn
        // thread keeps the claim count between two and six and the snapshot rewriting. A sentinel claim must survive every call, and a
        // merge of a list holding no claim must never stage one.
        constexpr unsigned K=3000;
        Tree treeC(1,1),treeM(1,1),treeQ(1,1);Destination dstM,dstQ;
        treeC.item(0,0,0)=record(90);treeM.item(0,0,0)=record(91);treeQ.item(0,0,0)=record(92);
        Record sentinel=record(93);std::vector<std::unique_ptr<Record>> noise;for(unsigned i=0;i<3;++i)noise.push_back(std::make_unique<Record>(record(float(100+i))));
        copy::reset();claim(sentinel,kFrame);
        std::atomic<unsigned> turn{0};std::atomic<bool> stop{false};std::atomic<unsigned> lost{0},wrong{0},fell{0};
        const uintptr_t clearAddress=addressOf(treeC.item(0,0,0)),mergeAddress=addressOf(treeM.item(0,0,0));
        std::thread producer([&]{
            for(unsigned i=0;i<K;++i) {
                while(turn.load(std::memory_order_acquire)!=2*i)std::this_thread::yield();
                claim(treeC.item(0,0,0),kFrame);claim(treeM.item(0,0,0),kFrame);
                turn.store(2*i+1,std::memory_order_release);
            }
        });
        std::thread caller([&]{
            for(unsigned i=0;i<K;++i) {
                while(turn.load(std::memory_order_acquire)!=2*i+1)std::this_thread::yield();
                copy::invalidateDictionary(treeC.ptr());
                if(heldLocked(clearAddress))++lost;
                void* p=copy::beginMergeOpaque(dstM.ptr(),treeM.anchor(0),kFrame);
                if(p==copy::mergeNoClaimsToken() || !p || static_cast<copy::MergePlan*>(p)->transfers.size()!=1)++lost;
                else if(static_cast<copy::MergePlan*>(p)->transfers[0].target!=mergeAddress)++wrong;
                copy::endMergeOpaque(p,true);
                copy::invalidateEmission(mergeAddress);
                void* q=copy::beginMergeOpaque(dstQ.ptr(),treeQ.anchor(0),kFrame);
                if(q!=copy::mergeNoClaimsToken() && (!q || !static_cast<copy::MergePlan*>(q)->transfers.empty()))++wrong;
                if(q!=copy::mergeNoClaimsToken())++fell;
                copy::endMergeOpaque(q,true);
                copy::invalidateDictionary(treeQ.ptr());
                if(!heldLocked(addressOf(sentinel)))++wrong;
                turn.store(2*i+2,std::memory_order_release);
            }
        });
        std::thread churn([&]{
            while(!stop.load(std::memory_order_relaxed)) {
                for(auto& n:noise)claim(*n,kFrame);
                for(auto& n:noise)copy::invalidateEmission(addressOf(*n));
            }
        });
        producer.join();caller.join();stop.store(true);churn.join();
        std::printf("claim check stress: %u rounds, %u lost, %u wrong, %u merges of a clean list fell back to the mutex (churn above four claims or mid-write)\n",
                    K,lost.load(),wrong.load(),fell.load());
        check(lost.load()==0,"claim check C7: four threads of churn: a claim published before a call is never missed (clears revoke it, merges stage it)");
        check(wrong.load()==0,"claim check C7: four threads of churn: nothing is moved that is not in the list, and the sentinel claim is never erased");
        copy::invalidateEmission(addressOf(sentinel));
        check(published(),"claim check C7: four threads of churn: the snapshot is exact once they stop");
        copy::reset();
    }
}
}
