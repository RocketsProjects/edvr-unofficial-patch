// The occlusion census's rig (src\d3d11\occlusion_census_core.cpp, the pure half of the instrument; the glue in
// occlusion_census.cpp is not linked, because it needs the whole DLL). A real D3D11 WARP device, the production recorder
// linked in as a source, and quads whose outcome is known in advance.
//
// Main capture (one whole frame, depth LESS with writes, colour target bound):
//   occluder   left half,  z 0.2  -> visible, samples > 0
//   hidden     left half,  z 0.8  -> behind the occluder, samples == 0   (the one zero)
//   visible    right half, z 0.8  -> the cleared depth is 1.0, samples > 0
//   nodepth    right half, depth test OFF -> skipped_no_depth, never queried
//   other      a draw off the scene target -> other_target, never queried
//
// Depth pre-pass capture (colour write mask zero): the occluder's quad again, depth LESS with writes -> depthonly, zero.
//
// Overflow: a frame whose draws exceed a tiny pool is dropped whole; the frames before it are kept. A first frame that
// overflows alone reports that nothing fit.
//
// Run:  occlusion_census_test.exe --self-test | --dry-run
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/occlusion_census_core.h"

namespace {

using edvr::occlusion::Recorder;
using edvr::occlusion::DrawInput;
using edvr::occlusion::Ticket;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool hasLine(const std::vector<std::string>& lines, const char* needle) {
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

struct Vtx {
    float x, y, z;
};

// Two triangles covering the NDC rectangle [x0,x1] x [-1,1] at depth z.
void addQuad(std::vector<Vtx>& v, float x0, float x1, float z) {
    v.push_back({x0, -1, z});
    v.push_back({x1, -1, z});
    v.push_back({x0, 1, z});
    v.push_back({x0, 1, z});
    v.push_back({x1, -1, z});
    v.push_back({x1, 1, z});
}

ID3DBlob* compile(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) std::printf("shader error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return code;
}

// Release a COM pointer on scope exit; the rig holds a handful of them.
template <class T>
struct Hold {
    T* p = nullptr;
    ~Hold() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* get() const { return p; }
};

// The rig's draw: one quad of the vertex buffer, through the recorder, with the depth state given.
struct Rig {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11DepthStencilState* dssOn = nullptr;
    ID3D11DepthStencilState* dssOff = nullptr;

    void draw(Recorder& rec, uint32_t start, bool depthOn, uint64_t vsHash, bool scene, bool hasRtv = true) {
        DrawInput in;
        in.kind = 'D';
        in.count = 6;
        in.vs = vsHash;
        in.ps = 2;
        in.rt = 1;
        in.dsv = 2;
        in.hasRtv = hasRtv;
        in.hasDsv = true;
        in.frame = 1;
        ctx->OMSetDepthStencilState(depthOn ? dssOn : dssOff, 0);
        Ticket t;
        rec.begin(ctx, in, scene, &t);
        ctx->Draw(6, start);
        rec.end(ctx, t);
    }
};

bool drainQueries(Recorder& rec, ID3D11DeviceContext* ctx) {
    ctx->Flush();
    for (int i = 0; i < 2000 && rec.poll(ctx) != 0; ++i) Sleep(1);
    return rec.outstanding() == 0;
}

int runSelfTest() {
    // The arithmetic first: no device needed.
    check(edvr::occlusion::trianglesOf('D', 6, 1, 4) == 2, "trianglelist of 6 is 2 triangles");
    check(edvr::occlusion::trianglesOf('I', 9, 1, 5) == 7, "trianglestrip of 9 is 7 triangles");
    check(edvr::occlusion::trianglesOf('X', 6, 10, 4) == 20, "instanced trianglelist: 2 per instance x 10");
    check(edvr::occlusion::trianglesOf('Y', 6, 1, 4) == 0, "indirect draws name no count, so no triangles");
    check(edvr::occlusion::trianglesOf('D', 6, 1, 1) == 0, "points name no triangles");
    check(std::strcmp(edvr::occlusion::depthFuncName(3), "EQUAL") == 0, "depth func 3 is EQUAL");
    check(std::strcmp(edvr::occlusion::depthFuncName(7), "GREATER_EQUAL") == 0, "depth func 7 is GREATER_EQUAL");
    std::printf("occlusion census: record bytes=%zu, pool and record limits %u / %u\n",
                sizeof(edvr::occlusion::DrawRecord), edvr::occlusion::kPoolSize, edvr::occlusion::kMaxRecords);

    // A capture that never saw a scene draw says so, and names the scene target test as the suspect.
    {
        Recorder empty;
        const auto lines = edvr::occlusion::formatReport(empty);
        check(hasLine(lines, "frames=0 draws=0 zero_sample_draws=0"), "an empty capture reports zero whole frames");
        check(hasLine(lines, "no draw reached the scene target"), "an empty capture says armed-but-no-draws");
        check(edvr::occlusion::formatCsv(empty).find("frame,eye,vs,ps") == 0, "an empty capture still writes a CSV header");
    }

    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "load the system D3D11 entry point");
    Hold<ID3D11Device> device;
    Hold<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level{};
    const HRESULT dhr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     device.put(), &level, ctx.put());
    check(SUCCEEDED(dhr) && device.get() && ctx.get(), "create a WARP device");
    if (!device.get() || !ctx.get()) return 1;

    const char* vsSrc = "float4 VS(float3 p : POSITION) : SV_Position { return float4(p, 1.0f); }\n";
    const char* psSrc = "float4 PS() : SV_Target { return float4(1.0f, 0.0f, 0.0f, 1.0f); }\n";
    Hold<ID3DBlob> vsBlob, psBlob;
    vsBlob.p = compile(vsSrc, "VS", "vs_4_0");
    psBlob.p = compile(psSrc, "PS", "ps_4_0");
    check(vsBlob.p && psBlob.p, "compile the rig's shaders");
    if (!vsBlob.p || !psBlob.p) return 1;

    Hold<ID3D11VertexShader> vs;
    Hold<ID3D11PixelShader> ps;
    Hold<ID3D11InputLayout> layout;
    device.get()->CreateVertexShader(vsBlob.p->GetBufferPointer(), vsBlob.p->GetBufferSize(), nullptr, vs.put());
    device.get()->CreatePixelShader(psBlob.p->GetBufferPointer(), psBlob.p->GetBufferSize(), nullptr, ps.put());
    const D3D11_INPUT_ELEMENT_DESC ied{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    device.get()->CreateInputLayout(&ied, 1, vsBlob.p->GetBufferPointer(), vsBlob.p->GetBufferSize(), layout.put());

    // Vertices: the occluder, the hidden quad, the visible quad, the no-depth quad (6 each).
    std::vector<Vtx> verts;
    addQuad(verts, -1.0f, 0.0f, 0.2f);   // occluder
    addQuad(verts, -1.0f, 0.0f, 0.8f);   // hidden
    addQuad(verts, 0.0f, 1.0f, 0.8f);    // visible
    addQuad(verts, 0.0f, 1.0f, 0.8f);    // no depth
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(Vtx));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd{verts.data(), 0, 0};
    Hold<ID3D11Buffer> vb;
    device.get()->CreateBuffer(&vbd, &vsd, vb.put());

    // The target: a 64x64 colour view and a 64x64 depth view.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    Hold<ID3D11Texture2D> colour, depth;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    device.get()->CreateTexture2D(&td, nullptr, colour.put());
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    device.get()->CreateTexture2D(&td, nullptr, depth.put());
    Hold<ID3D11RenderTargetView> rtv;
    Hold<ID3D11DepthStencilView> dsv;
    device.get()->CreateRenderTargetView(colour.get(), nullptr, rtv.put());
    device.get()->CreateDepthStencilView(depth.get(), nullptr, dsv.put());

    // Depth state: on (LESS, writes) and off. Culling off so the winding cannot hide a quad.
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    Hold<ID3D11DepthStencilState> dssOn, dssOff;
    device.get()->CreateDepthStencilState(&dsd, dssOn.put());
    dsd.DepthEnable = FALSE;
    device.get()->CreateDepthStencilState(&dsd, dssOff.put());
    // A colour write mask of zero: the depth pre-pass shape.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = 0;
    Hold<ID3D11BlendState> bsOff;
    device.get()->CreateBlendState(&bd, bsOff.put());
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    Hold<ID3D11RasterizerState> rs;
    device.get()->CreateRasterizerState(&rd, rs.put());

    D3D11_VIEWPORT vp{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* rtvs[1] = {rtv.get()};
    const UINT stride = sizeof(Vtx), offset = 0;
    ID3D11Buffer* vbs[1] = {vb.get()};
    ctx.get()->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    ctx.get()->IASetInputLayout(layout.get());
    ctx.get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.get()->VSSetShader(vs.get(), nullptr, 0);
    ctx.get()->PSSetShader(ps.get(), nullptr, 0);
    ctx.get()->RSSetState(rs.get());
    ctx.get()->RSSetViewports(1, &vp);
    ctx.get()->OMSetRenderTargets(1, rtvs, dsv.get());
    const float black[4] = {0, 0, 0, 1};
    ctx.get()->ClearRenderTargetView(rtv.get(), black);
    ctx.get()->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx.get()->OMSetDepthStencilState(dssOn.get(), 0);

    Rig rig;
    rig.ctx = ctx.get();
    rig.dssOn = dssOn.get();
    rig.dssOff = dssOff.get();

    // ---- The main capture: one whole frame of four scene draws and one off the scene.
    Recorder rec;
    rec.beginFrame();
    rig.draw(rec, 0, true, 1, true);     // occluder
    rig.draw(rec, 6, true, 3, true);     // hidden
    rig.draw(rec, 12, true, 4, true);    // visible
    rig.draw(rec, 18, false, 5, true);   // no depth
    {
        DrawInput in;
        in.kind = 'D';
        in.count = 6;
        Ticket t;
        rec.begin(ctx.get(), in, false, &t);   // not the scene target: counted, not queried
        ctx.get()->Draw(6, 0);
        rec.end(ctx.get(), t);
    }
    check(rec.endFrame(), "a frame that fits the pool is kept whole");
    drainQueries(rec, ctx.get());

    const auto& c = rec.counts();
    check(c.sceneDraws == 4, "four scene draws were seen");
    check(c.measured == 3, "three depth-on scene draws were queried");
    check(c.noDepth == 1, "one depth-off scene draw was counted as no-depth");
    check(c.otherTarget == 1, "one draw off the scene was counted as other-target");
    check(c.lost == 0, "no query was lost");
    check(rec.outstanding() == 0, "every query answered");
    check(rec.wholeFrames() == 1 && !rec.droppedFrame(), "one whole frame, nothing dropped");

    const auto& recs = rec.records();
    check(recs.size() == 4, "one record per scene draw");
    if (recs.size() == 4) {
        check(recs[0].ready && recs[0].samples > 0, "the occluder has visible samples");
        check(recs[1].ready && recs[1].samples == 0, "the hidden quad has zero samples");
        check(recs[2].ready && recs[2].samples > 0, "the visible quad has samples");
        check(recs[3].status == edvr::occlusion::Status::kNoDepth, "the depth-off draw was not queried");
        check(recs[1].tris == 2, "a six-vertex list is two triangles");
        check(recs[1].depthFunc == 2 && recs[1].depthWrite && !recs[1].depthOnly, "the main draws are LESS, writing, colour");
    }

    const auto lines = edvr::occlusion::formatReport(rec);
    check(hasLine(lines, "frames=1 draws=3 zero_sample_draws=1 (33.3%) "), "the summary says one zero of three");
    check(hasLine(lines, "skipped_no_depth=1"), "the summary counts the depth-off draw");
    check(hasLine(lines, "whole_frames=1 dropped_partial_frame=0 dropped_draws=0"), "the accounting names the whole frames");
    check(hasLine(lines, "occlusion census: by depth state: LESS/write: draws=3 zero=1 tris=6 zero_tris=2"),
          "the depth-state line groups the main pass as LESS with writes");
    check(hasLine(lines, "top zero draw rank=1 frame=1 eye=eye vs=0000000000000003 ps=0000000000000002 kind=D count=6 "
                         "instances=1 tris=2 rt=1 dsv=2 depth=test-write func=LESS pass=colour"),
          "the single zero draw is the hidden quad, with its depth func and pass");
    check(hasLine(lines, "vs=0000000000000003 ps=0000000000000002 draws=1 zero=1 tris=2 zero_tris=2"),
          "the shader pair line carries the zero triangles");
    check(!hasLine(lines, "results lost"), "a clean capture does not say results were lost");
    check(!hasLine(lines, "exceeded the query pool"), "a capture that fits does not say it overflowed");

    const std::string csv = edvr::occlusion::formatCsv(rec);
    check(csv.find("frame,eye,vs,ps,kind,count,instances,samples,tris,status,func,write,pass\n") == 0,
          "the CSV header is the spec's, with depth func and pass");
    check(csv.find("1,eye,0000000000000003,0000000000000002,D,6,1,0,2,measured,LESS,write,colour\n") != std::string::npos,
          "the CSV has the hidden draw with samples 0, its func and pass");

    // ---- The depth pre-pass: colour write mask zero, the occluder's quad again. Depth-only, and zero behind the occluder.
    {
        Recorder pre;
        const float blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        pre.beginFrame();
        ctx.get()->OMSetBlendState(bsOff.get(), blendFactor, 0xFFFFFFFFu);
        rig.draw(pre, 0, true, 1, true);
        ctx.get()->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFFu);
        pre.endFrame();
        drainQueries(pre, ctx.get());
        const auto pl = edvr::occlusion::formatReport(pre);
        check(pre.records().size() == 1 && pre.records()[0].depthOnly, "a zero colour write mask is tagged depthonly");
        check(pre.records().size() == 1 && pre.records()[0].samples == 0, "the pre-pass draw behind the occluder is zero");
        check(hasLine(pl, "by depth state: LESS/write/depthonly: draws=1 zero=1 tris=2 zero_tris=2"),
              "the depth-state line names the pre-pass as LESS/write/depthonly");
        check(hasLine(pl, "pass=depthonly"), "the zero pre-pass draw is tagged depthonly in the top list");
    }

    // ---- Overflow keeps whole frames: a pool of 4, frame 1 uses 2 slots, frame 2 needs 3 and is dropped whole.
    {
        Recorder tiny(4, edvr::occlusion::kMaxRecords);
        tiny.beginFrame();
        rig.draw(tiny, 0, true, 1, true);
        rig.draw(tiny, 6, true, 3, true);
        check(tiny.endFrame(), "overflow test: frame 1 fits and is kept");
        tiny.beginFrame();
        rig.draw(tiny, 12, true, 4, true);
        rig.draw(tiny, 0, true, 1, true);
        rig.draw(tiny, 6, true, 3, true);   // the pool is full by now: this draw overflows and the frame goes
        const bool kept = tiny.endFrame();
        check(!kept && tiny.droppedFrame(), "overflow test: frame 2 overflowed and was dropped");
        check(tiny.wholeFrames() == 1, "overflow test: one whole frame is kept");
        check(tiny.counts().measured == 2 && tiny.records().size() == 2,
              "overflow test: the dropped frame's draws and counts are rolled back");
        check(tiny.droppedDraws() == 3, "overflow test: three draws were dropped with the frame");
        drainQueries(tiny, ctx.get());
        const auto sl = edvr::occlusion::formatReport(tiny);
        check(hasLine(sl, "whole_frames=1 dropped_partial_frame=1 dropped_draws=3 pool=4"),
              "the accounting says a partial frame was dropped");
        check(hasLine(sl, "occlusion census: frame 2 exceeded the query pool of 4 and was dropped whole (3 draws)"),
              "the report names the dropped frame");
        check(hasLine(sl, "frames=1 draws=2 "), "the summary covers only the whole frame");
    }

    // ---- A first frame that overflows alone: nothing is measured, and the report says the pool is too tiny.
    {
        Recorder one(1, edvr::occlusion::kMaxRecords);
        one.beginFrame();
        rig.draw(one, 0, true, 1, true);
        rig.draw(one, 6, true, 3, true);
        check(!one.endFrame() && one.wholeFrames() == 0, "a first frame larger than the pool is dropped");
        drainQueries(one, ctx.get());
        const auto ol = edvr::occlusion::formatReport(one);
        check(hasLine(ol, "no whole frame fit the query pool"), "the report says no whole frame fit");
        check(hasLine(ol, "frames=0 draws=0"), "a capture with no whole frame measures nothing");
    }

    // ---- A capture whose queries never answered says so, and leaves them out of the figures.
    {
        Recorder lost;
        lost.beginFrame();
        rig.draw(lost, 0, true, 1, true);
        lost.endFrame();
        const uint32_t n = lost.loseOutstanding();
        const auto ll = edvr::occlusion::formatReport(lost);
        check(n == 1, "one unread query is lost at the deadline");
        check(hasLine(ll, "results lost: 1 measured draws"), "the report names the lost results");
        check(hasLine(ll, "draws=0 zero_sample_draws=0"), "lost draws stay out of the figures");
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::printf("occlusion_census_test: dry run, nothing written\n");
        return 0;
    }
    if (!std::strcmp(argv[1], "--self-test")) {
        const int rc = runSelfTest();
        std::printf("occlusion_census_test: %s (%d failures)\n", rc == 0 ? "ok" : "FAILED", g_failures);
        return rc;
    }
    return 2;
}
