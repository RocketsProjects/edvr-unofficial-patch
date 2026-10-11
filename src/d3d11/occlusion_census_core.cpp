// The occlusion census's pure half: see occlusion_census_core.h for what the numbers mean.
#include "occlusion_census_core.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

namespace edvr {
namespace occlusion {
namespace {

const char* statusText(const DrawRecord& r) {
    switch (r.status) {
        case Status::kMeasured: return r.ready ? "measured" : "pending";
        case Status::kNoDepth:  return "nodepth";
        case Status::kOverflow: return "overflow";
        case Status::kNoQuery:  return "noquery";
        case Status::kLost:     return "lost";
    }
    return "?";
}

double pct(uint64_t part, uint64_t whole) {
    return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

struct PairAcc {
    uint32_t draws = 0, zero = 0;
    uint64_t tris = 0, zeroTris = 0, maxTris = 0;
    std::vector<uint64_t> samples;
};

}  // namespace

uint64_t trianglesOf(char kind, uint32_t count, uint32_t instances, uint32_t topology) {
    if (kind != 'D' && kind != 'I' && kind != 'N' && kind != 'X') return 0;
    uint64_t prims = 0;
    switch (topology) {
        case 4:  prims = count / 3; break;                                  // TRIANGLELIST
        case 5:  prims = count >= 3 ? count - 2 : 0; break;                 // TRIANGLESTRIP
        case 12: prims = count / 6; break;                                  // TRIANGLELIST_ADJ
        case 13: prims = count >= 6 ? count / 2 - 2 : 0; break;             // TRIANGLESTRIP_ADJ
        default: prims = 0; break;                                          // points, lines, patches: no triangles counted
    }
    return prims * static_cast<uint64_t>(instances ? instances : 1);
}

Recorder::Recorder() : pool_(kPoolSize, nullptr) {
    records_.reserve(4096);
}

Recorder::~Recorder() {
    for (ID3D11Query*& q : pool_) {
        if (q) q->Release();
        q = nullptr;
    }
}

void Recorder::reset() {
    records_.clear();
    pending_.clear();
    counts_ = Counts();
    measured_ = 0;
}

uint32_t Recorder::push(const DrawRecord& r) {
    records_.push_back(r);
    return static_cast<uint32_t>(records_.size() - 1);
}

bool Recorder::ensureQuery(ID3D11DeviceContext* ctx, uint32_t slot) {
    if (pool_[slot]) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    const HRESULT hr = dev->CreateQuery(&qd, &pool_[slot]);
    dev->Release();
    return SUCCEEDED(hr) && pool_[slot] != nullptr;
}

void Recorder::begin(ID3D11DeviceContext* ctx, const DrawInput& in, bool scene, Ticket* t) {
    if (t) t->index = kNoIndex;
    if (!ctx || !t) return;
    // A deferred context's draw cannot be bracketed by this context's queries, so it is counted and left alone.
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        ++counts_.foreign;
        return;
    }
    if (!scene) {
        ++counts_.otherTarget;
        return;
    }
    ++counts_.sceneDraws;

    DrawRecord r;
    r.in = in;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);
    r.topology = static_cast<uint32_t>(topo);
    r.tris = trianglesOf(in.kind, in.count, in.instances, r.topology);

    // The state the game set. A null state object is the API's default: depth on, LESS, writes on.
    ID3D11DepthStencilState* dss = nullptr;
    D3D11_DEPTH_STENCIL_DESC dd{};
    ctx->OMGetDepthStencilState(&dss, nullptr);
    if (dss) {
        dss->GetDesc(&dd);
        dss->Release();
    } else {
        dd.DepthEnable = TRUE;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    }
    // ALWAYS with the test enabled passes every pixel, so nothing is occluded and the draw is not a candidate.
    const bool test = in.hasDsv && dd.DepthEnable && dd.DepthFunc != D3D11_COMPARISON_ALWAYS;
    r.depthWrite = test && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
    if (!test) {
        r.status = Status::kNoDepth;
        ++counts_.noDepth;
        push(r);
        return;
    }
    if (measured_ >= kPoolSize || records_.size() >= kMaxRecords) {
        r.status = Status::kOverflow;
        ++counts_.overflow;
        push(r);
        return;
    }
    if (!ensureQuery(ctx, measured_)) {
        r.status = Status::kNoQuery;
        ++counts_.noQuery;
        push(r);
        return;
    }
    r.status = Status::kMeasured;
    r.slot = measured_++;
    const uint32_t idx = push(r);
    ++counts_.measured;
    pending_.push_back(idx);
    ctx->Begin(pool_[records_[idx].slot]);
    t->index = idx;
}

void Recorder::end(ID3D11DeviceContext* ctx, const Ticket& t) {
    if (!ctx || t.index == kNoIndex || t.index >= records_.size()) return;
    const DrawRecord& r = records_[t.index];
    if (r.status == Status::kMeasured) ctx->End(pool_[r.slot]);
}

uint32_t Recorder::poll(ID3D11DeviceContext* ctx) {
    if (!ctx) return static_cast<uint32_t>(pending_.size());
    size_t keep = 0;
    for (size_t i = 0; i < pending_.size(); ++i) {
        const uint32_t idx = pending_[i];
        DrawRecord& r = records_[idx];
        UINT64 v = 0;
        // DONOTFLUSH: a query the GPU has not finished answers S_FALSE, and the next frame asks again. Nothing blocks.
        const HRESULT hr = ctx->GetData(pool_[r.slot], &v, sizeof(v), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            r.samples = v;
            r.ready = true;
        } else if (hr == S_FALSE) {
            pending_[keep++] = idx;
        } else {
            r.status = Status::kLost;   // a device error on the query: it will never answer
            ++counts_.lost;
        }
    }
    pending_.resize(keep);
    return static_cast<uint32_t>(keep);
}

uint32_t Recorder::loseOutstanding() {
    const uint32_t n = static_cast<uint32_t>(pending_.size());
    for (uint32_t idx : pending_) {
        records_[idx].status = Status::kLost;
        ++counts_.lost;
    }
    pending_.clear();
    return n;
}

std::vector<std::string> formatReport(const Recorder& rec, uint32_t frames) {
    std::vector<std::string> out;
    char buf[800];
    const Counts& c = rec.counts();
    const std::vector<DrawRecord>& recs = rec.records();

    uint32_t draws = 0, zero = 0;
    uint64_t tris = 0, zeroTris = 0;
    std::map<std::pair<uint64_t, uint64_t>, PairAcc> pairs;
    std::vector<uint32_t> zeroDraws;
    for (uint32_t i = 0; i < recs.size(); ++i) {
        const DrawRecord& r = recs[i];
        if (r.status != Status::kMeasured || !r.ready) continue;
        ++draws;
        tris += r.tris;
        PairAcc& p = pairs[std::make_pair(r.in.vs, r.in.ps)];
        ++p.draws;
        p.tris += r.tris;
        p.maxTris = std::max(p.maxTris, r.tris);
        p.samples.push_back(r.samples);
        if (r.samples == 0) {
            ++zero;
            zeroTris += r.tris;
            ++p.zero;
            p.zeroTris += r.tris;
            zeroDraws.push_back(i);
        }
    }

    // The window as a whole, first. "draws" are measured draws whose query answered; skipped_no_depth is counted, not queried.
    std::snprintf(buf, sizeof(buf),
                  "occlusion census: frames=%u draws=%u zero_sample_draws=%u (%.1f%%) triangles=%llu "
                  "zero_sample_triangles=%llu (%.1f%%) skipped_no_depth=%u",
                  frames, draws, zero, pct(zero, draws), static_cast<unsigned long long>(tris),
                  static_cast<unsigned long long>(zeroTris), pct(zeroTris, tris), c.noDepth);
    out.push_back(buf);
    std::snprintf(buf, sizeof(buf),
                  "occlusion census: accounting scene_draws=%u measured=%u ready=%u lost=%u overflow=%u no_query=%u "
                  "other_target=%u foreign_context=%u",
                  c.sceneDraws, c.measured, draws, c.lost, c.overflow, c.noQuery, c.otherTarget, c.foreign);
    out.push_back(buf);
    if (c.sceneDraws == 0) {
        std::snprintf(buf, sizeof(buf),
                      "occlusion census: armed, but no draw reached the scene target in these frames (other_target=%u "
                      "foreign_context=%u): the scene target test matched nothing, or nothing was drawn there.",
                      c.otherTarget, c.foreign);
        out.push_back(buf);
    }
    if (c.lost > 0) {
        std::snprintf(buf, sizeof(buf),
                      "occlusion census: results lost: %u measured draws never returned a result; they are left out of "
                      "every figure here.", c.lost);
        out.push_back(buf);
    }

    // The shader pairs, by the triangles that were occluded.
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, PairAcc*>> order;
    order.reserve(pairs.size());
    for (auto& kv : pairs) order.emplace_back(kv.first, &kv.second);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        if (a.second->zeroTris != b.second->zeroTris) return a.second->zeroTris > b.second->zeroTris;
        if (a.second->zero != b.second->zero) return a.second->zero > b.second->zero;
        return a.first < b.first;
    });
    for (size_t i = 0; i < order.size() && i < 40; ++i) {
        PairAcc& p = *order[i].second;
        std::sort(p.samples.begin(), p.samples.end());
        const uint64_t p50 = p.samples.empty() ? 0 : p.samples[p.samples.size() / 2];
        std::snprintf(buf, sizeof(buf),
                      "occlusion census: vs=%016llX ps=%016llX draws=%u zero=%u tris=%llu zero_tris=%llu "
                      "max_tris_single_draw=%llu samples_p50=%llu",
                      static_cast<unsigned long long>(order[i].first.first),
                      static_cast<unsigned long long>(order[i].first.second), p.draws, p.zero,
                      static_cast<unsigned long long>(p.tris), static_cast<unsigned long long>(p.zeroTris),
                      static_cast<unsigned long long>(p.maxTris), static_cast<unsigned long long>(p50));
        out.push_back(buf);
    }

    // The largest single draws that were fully occluded.
    std::stable_sort(zeroDraws.begin(), zeroDraws.end(), [&recs](uint32_t a, uint32_t b) {
        return recs[a].tris > recs[b].tris;
    });
    for (size_t i = 0; i < zeroDraws.size() && i < 30; ++i) {
        const DrawRecord& r = recs[zeroDraws[i]];
        std::snprintf(buf, sizeof(buf),
                      "occlusion census: top zero draw rank=%u frame=%u eye=%s vs=%016llX ps=%016llX kind=%c count=%u "
                      "instances=%u tris=%llu rt=%u dsv=%u depth=%s",
                      static_cast<unsigned>(i + 1), r.in.frame, r.in.flat ? "flat" : "eye",
                      static_cast<unsigned long long>(r.in.vs), static_cast<unsigned long long>(r.in.ps), r.in.kind,
                      r.in.count, r.in.instances, static_cast<unsigned long long>(r.tris), r.in.rt, r.in.dsv,
                      r.depthWrite ? "test-write" : "test-nowrite");
        out.push_back(buf);
    }
    return out;
}

std::string formatCsv(const Recorder& rec) {
    std::string out = "frame,eye,vs,ps,kind,count,instances,samples,tris,status\n";
    char buf[320];
    for (const DrawRecord& r : rec.records()) {
        char samples[24] = "";
        if (r.status == Status::kMeasured && r.ready) {
            std::snprintf(samples, sizeof(samples), "%llu", static_cast<unsigned long long>(r.samples));
        }
        std::snprintf(buf, sizeof(buf), "%u,%s,%016llX,%016llX,%c,%u,%u,%s,%llu,%s\n", r.in.frame,
                      r.in.flat ? "flat" : "eye", static_cast<unsigned long long>(r.in.vs),
                      static_cast<unsigned long long>(r.in.ps), r.in.kind, r.in.count, r.in.instances, samples,
                      static_cast<unsigned long long>(r.tris), statusText(r));
        out += buf;
    }
    return out;
}

}  // namespace occlusion
}  // namespace edvr
