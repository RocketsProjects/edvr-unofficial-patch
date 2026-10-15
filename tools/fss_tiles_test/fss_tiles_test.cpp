// Test rig for the `fss tiles` instrument and the tiles candidate (src/d3d11/fss_tiles_core.h, fss_tiles_d3d.h, fss_tiles.cpp).
//
//   1. The pure half: identities, the view table, the zoom-window machine, the substitution rule, the exact log line, the sharing
//      tally, and the key's value parsing (tiles is new; lockstep must still mean the composite freeze).
//   2. The state restore, on a fake context: every slot swapped in comes back to what it was, including a slot that was unbound,
//      and the dispatch sees the substituted state.
//   3. The same on a WARP device with the debug layer, through the production D3dOps: two "views" (their own argument buffers,
//      list buffers and a texture), a stand-in tile shader that writes list[0] + group id into an output buffer. Stock draws the
//      second view's groups from the second view's list; the substitution draws the FIRST view's group count from the FIRST
//      view's list; afterwards the context's compute SRV state is exactly what it was, and a following stock dispatch is stock.
//   4. Source pins on the glue no rig can run (the hook is linked into the DLL only).
//
// No window, no files written. --self-test runs it; --dry-run says what it would do.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/common/eye_sync.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/fss_tiles_core.h"
#include "../../src/d3d11/fss_tiles_d3d.h"

using Microsoft::WRL::ComPtr;
namespace ft = edvr::fsstiles;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 1. The pure half

void testIdentities() {
    ft::Intern in;
    check(in.id(nullptr) == 0, "a null pointer is id 0");
    int a, b;
    const uint32_t ia = in.id(&a), ib = in.id(&b);
    check(ia == 1 && ib == 2 && in.id(&a) == 1 && in.id(&b) == 2, "ids are stable and start at 1");
    ft::Intern full;
    static int pool[ft::Intern::kCap + 4];
    uint32_t last = 0;
    for (int i = 0; i < ft::Intern::kCap + 4; ++i) last = full.id(&pool[i]);
    check(last == ft::Intern::kCap, "a full table answers its cap, not a fresh id");
}

void testViews() {
    ft::ViewTable v;
    int a, b, c;
    check(v.eyeOf(&a) == 0 && v.eyeOf(&b) == 1 && v.eyeOf(&a) == 0 && v.eyeOf(&c) == 2, "views number by first appearance");
    v.reset();
    check(v.n == 0 && v.eyeOf(&b) == 0, "a new frame renumbers");
}

void testWindow() {
    ft::WindowMachine w;
    check(w.step(false, 1, 0) == ft::WindowMachine::None && !w.capturing, "closed stays closed");
    check(w.step(true, 10, 1000) == ft::WindowMachine::Opened && w.capturing && w.open && w.windows == 1, "the zoom window opens a capture");
    check(w.step(true, 11, 1500) == ft::WindowMachine::None && w.capturing, "an open window keeps capturing");
    check(w.wantsDetail(10) && w.wantsDetail(13) && !w.wantsDetail(14) && w.wantsDetail(10 + ft::kDetailEvery), "detail lines: the first frames, then one in N");
    check(w.step(true, 900, 1000 + ft::kWindowMs + 1) == ft::WindowMachine::Closed && !w.capturing && w.open,
          "the capture ends after kWindowMs while the zoom window stays open");
    check(w.step(true, 901, 20000) == ft::WindowMachine::None, "no second close event");
    check(w.step(false, 902, 20001) == ft::WindowMachine::None && !w.open, "the zoom window closing after its capture says nothing more");
    check(w.step(true, 1000, 30000) == ft::WindowMachine::Opened && w.windows == 2, "the next zoom opens window 2");
    check(w.step(false, 1001, 30100) == ft::WindowMachine::Closed && !w.capturing, "the zoom window closing ends a running capture");
    for (uint32_t i = 0; i < 10; ++i) {
        w.step(true, 2000 + i * 10, 40000 + i * 1000);
        w.step(false, 2001 + i * 10, 40100 + i * 1000);
    }
    check(w.windows == ft::kMaxWindows, "windows are capped per session");
    check(w.step(true, 9000, 90000) == ft::WindowMachine::None && !w.capturing && w.open, "past the cap the window opens without a capture (the candidate still follows `open`)");
}

ft::Slot buf(const void* view, const void* res, uint32_t bytes) {
    ft::Slot s;
    s.view = view;
    s.res = res;
    s.isBuffer = true;
    s.bytes = bytes;
    return s;
}

void testSubstitutionRule() {
    int v0[8], v1[8], r0[8], r1[8];
    ft::Slot a[8], b[8];
    a[0].view = &v0[0]; a[0].res = &r0[0];            // textures: never taken
    b[0].view = &v1[0]; b[0].res = &r1[0];
    a[2] = buf(&v0[2], &r0[2], 10656); b[2] = buf(&v1[2], &r1[2], 10656);     // the list: taken
    a[3] = buf(&v0[3], &r0[3], 64);    b[3] = buf(&v1[3], &r1[3], 128);       // different sizes: not taken
    a[4] = buf(&v0[4], &r0[4], 64);    b[4] = buf(&v1[4], &r0[4], 64);        // the same resource already: not taken
    a[5] = buf(&v0[5], &r0[5], 64);                                           // unbound in the second view: not taken
    b[6] = buf(&v1[6], &r1[6], 64);                                           // unbound in the first view: not taken
    check(ft::substitutionMask(a, b) == 0x04u, "only a bound, equal-size buffer that differs between the views is taken");
    a[7] = buf(&v0[7], &r0[7], 16); b[7] = buf(&v1[7], &r1[7], 16);
    check(ft::substitutionMask(a, b) == 0x84u, "every such slot is taken");
    ft::Slot none[8];
    check(ft::substitutionMask(none, none) == 0, "nothing bound, nothing taken");
}

void testLineAndTallies() {
    ft::Rec r;
    r.frame = 812;
    r.eye = 1;
    r.occ = 2;
    r.cs = ft::kCsSmall;
    r.off = 12;
    r.argsId = 7;
    r.srvId[2] = 14; r.srvKind[2] = 1; r.srvBytes[2] = 10656;
    r.srvId[0] = 3; r.srvKind[0] = 2;
    r.uavId[0] = 20; r.uavId[1] = 21;
    r.groups[0] = 311; r.groups[1] = 1; r.groups[2] = 1; r.haveGroups = true;
    char line[512];
    ft::formatRec(line, sizeof(line), r);
    check(std::strcmp(line, "fss tiles: f=812 eye=1 cs=EB0245DE0BB23BB6 groups=(311,1,1) occ=2 args=A7+12 srv=[T3,-,B14:10656,-,-,-,-,-] uav=[20,21]") == 0,
          "the line has the documented shape");
    r.substituted = true;
    r.subMask = 0x04;
    r.haveGroups = false;
    r.eye = 3;
    ft::formatRec(line, sizeof(line), r);
    check(std::strstr(line, "groups=(unread)") && std::strstr(line, "eye=2 ") && std::strstr(line, "lockstep=substituted(slots 0x04)"),
          "unread groups, a clamped eye and the substitution are all visible");

    ft::Range g;
    g.add(10, 1, 1);
    g.add(40, 1, 3);
    g.add(20, 1, 1);
    check(g.n == 3 && g.lo == 10 && g.hi == 40 && g.sum == 70 && g.yzMax == 3, "the group-count range");

    ft::Rec a, b;
    a.argsId = 1; b.argsId = 2;
    a.srvId[2] = 5; b.srvId[2] = 6;
    a.srvId[4] = 9; b.srvId[4] = 9;
    a.uavId[0] = 30; b.uavId[0] = 31;
    ft::Share s;
    ft::accumulateShare(s, a, b);
    b.argsId = 1;
    ft::accumulateShare(s, a, b);
    check(s.pairs == 2 && s.argsSame == 1 && s.slotBoth[2] == 2 && s.slotSame[2] == 0 && s.slotBoth[4] == 2 && s.slotSame[4] == 2 && s.uavSame == 0,
          "the sharing tally says what the two views have in common");
}

void testKeyParsing() {
    edvr::EyeSync t = edvr::eyeSyncParse("tiles");
    check(t.tiles && t.recognised && t.healMode == 0 && !t.lockstep && !t.steady && !t.any(), "tiles is its own value: no heal, no composite freeze");
    edvr::EyeSync tu = edvr::eyeSyncParse("Tiles");
    check(tu.tiles, "case is folded");
    edvr::EyeSync l = edvr::eyeSyncParse("lockstep");
    check(l.lockstep && !l.tiles, "the legacy spelling lockstep still means the composite freeze");
    edvr::EyeSync on = edvr::eyeSyncParse("on");
    check(on.healMode == 1 && on.lockstep && !on.tiles, "on is the shipped pair and nothing more");
    edvr::EyeSync off = edvr::eyeSyncParse("off");
    check(!off.tiles && !off.any(), "off is stock");
    edvr::EyeSync mir = edvr::eyeSyncParse("mirror");
    check(mir.healMode == 2 && !mir.tiles, "mirror is unchanged");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 2. The restore, on a fake context

struct FakeOps {
    void* state[8] = {};
    int released = 0;
    int dispatched = 0;
    void* seenAtDispatch[8] = {};
    void* seenArgs = nullptr;
    uint32_t seenOff = 0;
    void* getSrv(uint32_t i) { return state[i]; }
    void setSrv(uint32_t i, void* v) { state[i] = v; }
    void release(void*) { ++released; }
    void dispatchIndirect(void* args, uint32_t off) {
        ++dispatched;
        std::memcpy(seenAtDispatch, state, sizeof(state));
        seenArgs = args;
        seenOff = off;
    }
};

void testFakeRestore() {
    int own[8], first[8], args0;
    FakeOps o;
    for (int i = 0; i < 8; ++i) o.state[i] = &own[i];
    o.state[5] = nullptr;                       // an unbound slot must come back unbound
    void* before[8];
    std::memcpy(before, o.state, sizeof(before));
    void* views[8] = {};
    for (int i = 0; i < 8; ++i) views[i] = &first[i];
    ft::lockstepDispatch(o, views, 0x2C, &args0, 12);   // slots 2, 3, 5
    check(o.dispatched == 1 && o.seenArgs == &args0 && o.seenOff == 12, "the dispatch got the first view's arguments and offset");
    check(o.seenAtDispatch[2] == &first[2] && o.seenAtDispatch[3] == &first[3] && o.seenAtDispatch[5] == &first[5], "the dispatch saw the first view's bindings");
    check(o.seenAtDispatch[0] == &own[0] && o.seenAtDispatch[1] == &own[1] && o.seenAtDispatch[4] == &own[4], "slots outside the mask kept the second view's own");
    check(std::memcmp(o.state, before, sizeof(before)) == 0, "every slot is back as it was, the unbound one unbound");
    check(o.released == 3, "each saved reference was released once");

    // The hook's fault path: swapIn, the dispatch never returns, swapBack runs from outside.
    FakeOps p;
    for (int i = 0; i < 8; ++i) p.state[i] = &own[i];
    ft::Swap sw;
    ft::swapIn(p, sw, views, 0xFF);
    check(sw.swapped == 0xFF && p.state[7] == &first[7], "swapIn takes the whole mask");
    ft::swapBack(p, sw);
    bool same = true;
    for (int i = 0; i < 8; ++i) same = same && p.state[i] == &own[i];
    check(same && sw.swapped == 0, "swapBack from outside restores everything");
    ft::swapBack(p, sw);
    check(p.released == 8, "a second swapBack is a no-op");
    FakeOps q;
    ft::Swap sw2;
    void* none[8] = {};
    ft::swapIn(q, sw2, none, 0xFF);
    check(sw2.swapped == 0, "a mask over slots with no first-view binding swaps nothing");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 3. The same on WARP, through the production ops

const char* kTileHlsl = R"(
StructuredBuffer<uint> list : register(t2);
Texture2D<float4> tex : register(t0);
RWByteAddressBuffer outBuf : register(u0);
[numthreads(1,1,1)]
void main(uint3 g : SV_GroupID) {
    outBuf.Store(g.x * 4, list[0] + g.x);
}
)";

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
};

ComPtr<ID3D11Buffer> makeArgs(ID3D11Device* d, uint32_t x) {
    uint32_t init[3] = {x, 1, 1};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 12;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
    ComPtr<ID3D11Buffer> b;
    d->CreateBuffer(&bd, &sd, &b);
    return b;
}

ComPtr<ID3D11ShaderResourceView> makeList(ID3D11Device* d, uint32_t first, ID3D11Buffer** outBuf) {
    uint32_t init[4] = {first, 0, 0, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
    ComPtr<ID3D11Buffer> b;
    d->CreateBuffer(&bd, &sd, &b);
    ComPtr<ID3D11ShaderResourceView> v;
    d->CreateShaderResourceView(b.Get(), nullptr, &v);
    if (outBuf) *outBuf = b.Detach();
    return v;
}

std::vector<uint32_t> readOut(Gpu& g, ID3D11Buffer* out) {
    D3D11_BUFFER_DESC bd{};
    out->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> st;
    g.dev->CreateBuffer(&bd, nullptr, &st);
    g.ctx->CopyResource(st.Get(), out);
    D3D11_MAPPED_SUBRESOURCE m{};
    std::vector<uint32_t> v(bd.ByteWidth / 4);
    if (SUCCEEDED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        std::memcpy(v.data(), m.pData, bd.ByteWidth);
        g.ctx->Unmap(st.Get(), 0);
    }
    return v;
}

void clearOut(Gpu& g, ID3D11UnorderedAccessView* uav) {
    const UINT z[4] = {0, 0, 0, 0};
    g.ctx->ClearUnorderedAccessViewUint(uav, z);
}

void testWarp(Gpu& g) {
    ID3D11Device* d = g.dev.Get();
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(kTileHlsl, std::strlen(kTileHlsl), "fss_tiles_test", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err);
    check(SUCCEEDED(hr), "the stand-in tile shader compiles");
    if (FAILED(hr)) return;
    ComPtr<ID3D11ComputeShader> cs;
    d->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);

    ComPtr<ID3D11Buffer> args0 = makeArgs(d, 3), args1 = makeArgs(d, 5);
    ID3D11Buffer *list0b = nullptr, *list1b = nullptr;
    ComPtr<ID3D11ShaderResourceView> list0 = makeList(d, 1000, &list0b), list1 = makeList(d, 2000, &list1b);
    ComPtr<ID3D11Buffer> l0(list0b), l1(list1b);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    d->CreateTexture2D(&td, nullptr, &tex);
    ComPtr<ID3D11ShaderResourceView> texView;
    d->CreateShaderResourceView(tex.Get(), nullptr, &texView);

    D3D11_BUFFER_DESC ob{};
    ob.ByteWidth = 64;
    ob.Usage = D3D11_USAGE_DEFAULT;
    ob.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ob.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> out;
    d->CreateBuffer(&ob, nullptr, &out);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 16;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> uav;
    d->CreateUnorderedAccessView(out.Get(), &ud, &uav);

    ID3D11DeviceContext* c = g.ctx.Get();
    c->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[1] = {uav.Get()};
    c->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // The second view's own state, and what the first view's dispatch left recorded.
    ID3D11ShaderResourceView* own[8] = {texView.Get(), nullptr, list1.Get(), nullptr, nullptr, nullptr, nullptr, nullptr};
    c->CSSetShaderResources(0, 8, own);

    // Stock: the second view draws 5 groups from its own list (2000 + g).
    clearOut(g, uav.Get());
    c->DispatchIndirect(args1.Get(), 0);
    std::vector<uint32_t> stock = readOut(g, out.Get());
    bool stockOk = true;
    for (uint32_t i = 0; i < 5; ++i) stockOk = stockOk && stock[i] == 2000 + i;
    check(stockOk && stock[5] == 0, "stock: the second view's dispatch ran 5 groups over its own list");

    // The substitution: 3 groups over the FIRST view's list (1000 + g), arguments from the first view's buffer.
    ft::D3dOps ops;
    ops.ctx = c;
    void* views[8] = {};
    views[2] = list0.Get();
    ft::Slot e0[8], e1[8];
    e0[2] = buf(list0.Get(), l0.Get(), 16);
    e1[2] = buf(list1.Get(), l1.Get(), 16);
    e0[0].view = texView.Get(); e0[0].res = tex.Get();
    e1[0] = e0[0];
    const uint32_t mask = ft::substitutionMask(e0, e1);
    check(mask == 0x04u, "the rule picks exactly the list slot (the shared texture stays)");
    clearOut(g, uav.Get());
    ft::lockstepDispatch(ops, views, mask, args0.Get(), 0);
    std::vector<uint32_t> sub = readOut(g, out.Get());
    bool subOk = true;
    for (uint32_t i = 0; i < 3; ++i) subOk = subOk && sub[i] == 1000 + i;
    check(subOk && sub[3] == 0 && sub[4] == 0, "substituted: 3 groups (the first view's count) over the first view's list, nothing beyond");

    // The restore.
    ID3D11ShaderResourceView* after[8] = {};
    c->CSGetShaderResources(0, 8, after);
    bool restored = true;
    for (int i = 0; i < 8; ++i) restored = restored && after[i] == own[i];
    for (auto* v : after) if (v) v->Release();
    check(restored, "the context's compute SRVs are exactly the second view's own again (slot 2 included)");

    // And the next stock dispatch is stock.
    clearOut(g, uav.Get());
    c->DispatchIndirect(args1.Get(), 0);
    std::vector<uint32_t> again = readOut(g, out.Get());
    bool againOk = true;
    for (uint32_t i = 0; i < 5; ++i) againOk = againOk && again[i] == 2000 + i;
    check(againOk, "after the substitution the next dispatch is stock again");

    // A held reference is balanced: the list view's count returns to what it was.
    list1->AddRef();
    ULONG with = list1->Release();
    clearOut(g, uav.Get());
    ft::lockstepDispatch(ops, views, mask, args0.Get(), 0);
    list1->AddRef();
    ULONG after2 = list1->Release();
    check(with == after2, "no reference to the second view's list leaked or was dropped");

    // The fault path: swap in, restore from outside, no dispatch at all.
    ft::Swap sw;
    ft::swapIn(ops, sw, views, mask);
    ID3D11ShaderResourceView* mid = nullptr;
    c->CSGetShaderResources(2, 1, &mid);
    check(mid == list0.Get(), "swapped in");
    if (mid) mid->Release();
    ft::swapBack(ops, sw);
    ID3D11ShaderResourceView* back = nullptr;
    c->CSGetShaderResources(2, 1, &back);
    check(back == list1.Get(), "restored from outside the dispatch");
    if (back) back->Release();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 4. Source pins

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void testSourcePins(const std::string& root) {
    const std::string ef = slurp(root + "/src/d3d11/exposure_fix.cpp");
    check(!ef.empty(), "exposure_fix.cpp is readable");
    const size_t hook = ef.find("void STDMETHODCALLTYPE hookedDispatchIndirect");
    const size_t call = ef.find("fssTilesDispatchIndirect(self, args, off, hashOf(bindingGet(BindSlot::Cs))", hook);
    const size_t real = ef.find("s->realDispatchIndirect(self, args, off);", call);
    check(hook != std::string::npos && call != std::string::npos && real != std::string::npos && hook < call && call < real,
          "the DispatchIndirect hook offers the dispatch to fss_tiles before forwarding it");
    const size_t flat = ef.find("if (runtimeFlatProfile())", hook);
    check(flat != std::string::npos && flat < call, "the flat profile never reaches fss_tiles (one view, nothing to compare)");
    check(has(ef, "fssTilesActive() && !foreignContext(self)"), "owner context only, behind the one-bool gate");

    const std::string vs = slurp(root + "/src/d3d11/vscreen.cpp");
    size_t n = 0, at = 0;
    while ((at = vs.find("fssTilesConfigure(cfg);", at)) != std::string::npos) { ++n; ++at; }
    check(n == 2, "both config sites (install and reload) configure fss_tiles");
    check(has(vs, "fssTilesFrameBoundary(s->frameNo, open)"), "the frame boundary feeds fss_tiles the arrival window");

    const std::string bat = slurp(root + "/build.bat");
    check(has(bat, "src\\d3d11\\fss_tiles.cpp"), "build.bat compiles fss_tiles.cpp into the graphics DLL");

    const std::string cpp = slurp(root + "/src/d3d11/fss_tiles.cpp");
    check(has(cpp, "swapBack(ops, g.sw)") && has(cpp, "if (!ok || g.sw.swapped)"), "the fault path restores the bindings from outside the guarded dispatch");
    check(has(cpp, "D3D11_MAP_FLAG_DO_NOT_WAIT"), "the read-back never waits");
    check(has(cpp, "fss tiles: window %u opened") && has(cpp, "fss tiles: window %u closed") && has(cpp, "fss tiles: instrument armed"),
          "the instrument says it armed, opened and closed, so never-ran reads differently from zero");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "--self-test") && std::strcmp(argv[1], "--dry-run"))) {
        std::puts("usage: fss_tiles_test --self-test [repo root] | --dry-run");
        return 2;
    }
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("Would run the fss tiles pure-half tests, a fake-context restore test, a WARP-device substitution test and source pins; writes no files.");
        return 0;
    }
    const std::string root = argc > 2 ? argv[2] : ".";

    testIdentities();
    testViews();
    testWindow();
    testSubstitutionRule();
    testLineAndTallies();
    testKeyParsing();
    testFakeRestore();

    const auto createDevice = edvr::systemD3D11CreateDevice();
    check(createDevice != nullptr, "System32's D3D11CreateDevice");
    if (createDevice) {
        Gpu g;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &level, &g.ctx);
        if (FAILED(hr)) {
            std::puts("(the D3D debug layer is not installed here: running without it)");
            hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &level, &g.ctx);
        }
        check(SUCCEEDED(hr), "a WARP device");
        check(edvr::reportSystemD3D11Only("fss_tiles_test"), "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
        if (SUCCEEDED(hr)) {
            testWarp(g);
            ComPtr<ID3D11InfoQueue> q;
            if (SUCCEEDED(g.dev.As(&q))) {
                const UINT64 cnt = q->GetNumStoredMessagesAllowedByRetrievalFilter();
                bool bad = false;
                for (UINT64 i = 0; i < cnt; ++i) {
                    SIZE_T len = 0;
                    q->GetMessage(i, nullptr, &len);
                    std::vector<char> raw(len);
                    auto* m = reinterpret_cast<D3D11_MESSAGE*>(raw.data());
                    if (SUCCEEDED(q->GetMessage(i, m, &len)) &&
                        (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                        std::printf("  debug layer: %s\n", m->pDescription);
                        bad = true;
                    }
                }
                check(!bad, "the D3D debug layer stayed clean");
            }
        }
    }

    testSourcePins(root);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
