// flat_hdr_energy_test -- the HDR route's energy instrument and measured exposure (src\d3d11\flat_hdr_energy.cpp, TEMPORARY,
// design doc section 107), the REAL module on WARP, the way the flat runtime and the VR world route call it.
//
//  1. The energy line against a CPU reference: two synthetic scene images (the backend's input, and the "resolved" image written
//     back, with bright pixels moved and scaled), counted by the real compute pass; R, M, N, the saturated and bad counts and
//     the geometric mean in the log line must equal what flatHdrEnergyReference computes from the same pixels.
//  2. The measured exposure against flatHdrExposureMeasure: the first value is 1/W exactly as the CPU reference says, the next
//     moves 8% in log2, a reset restarts, and the texture is handed out only on the frame the pass ran (Poll clears it).
//  3. The game's compute state (shader, SRV 0, UAV 0 and 1, constant buffer 0) is put back exactly after every pass.
//  4. "Never ran" and "ran, found nothing" read differently: no line before the first frame; a samples=0 line when frames were
//     offered and no sample finished; an armed line once.
//  5. The fallback counter reaches the line.
//
//   flat_hdr_energy_test --dry-run     (loads nothing, writes nothing)
//   flat_hdr_energy_test --self-test
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../flat_sharpen_test/flat_sharpen_rig.h"
#include "../../src/d3d11/flat_hdr_energy.h"

namespace edvr {
thread_local bool g_flatComputeInternal = false;   // the hooks' bypass flag (flat_compute_readback.cpp), which this rig does not link
void breadcrumb(const char*) {}                    // guard.cpp's crash trail and shader_swap.cpp's compile clock, neither under test
void perfMonitorNoteEvent(uint32_t, double) {}
}

using namespace rig;

namespace {

constexpr UINT kW = 64, kH = 48;

struct Image {
    std::vector<float> rgba;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
};

void setPixel(std::vector<float>& rgba, UINT x, UINT y, float r, float g, float b) {
    float* c = &rgba[(static_cast<size_t>(y) * kW + x) * 4];
    c[0] = r; c[1] = g; c[2] = b; c[3] = 1.0f;
}

bool upload(ID3D11Device* dev, Image& im, UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{im.rgba.data(), w * 16, 0};
    if (FAILED(dev->CreateTexture2D(&d, &init, &im.tex))) return false;
    return SUCCEEDED(dev->CreateShaderResourceView(im.tex.Get(), nullptr, &im.srv));
}

// The scene the backend is handed: a dim field, a cluster of bright pixels (a star), one over the format's top, a NaN and a
// negative. `scale` and `shift` make the "resolved" twin: the bright cluster scaled and moved a pixel.
Image makeScene(ID3D11Device* dev, float scale, UINT shift, float topLuma) {
    Image im;
    im.rgba.assign(static_cast<size_t>(kW) * kH * 4, 0.0f);
    for (UINT y = 0; y < kH; ++y) for (UINT x = 0; x < kW; ++x) {
        const float v = 0.05f + 0.02f * static_cast<float>((x * 7 + y * 3) % 11);
        setPixel(im.rgba, x, y, v, v * 0.9f, v * 0.8f);
    }
    for (UINT y = 10; y < 14; ++y) for (UINT x = 20; x < 25; ++x) setPixel(im.rgba, x + shift, y, 500.0f * scale, 450.0f * scale, 400.0f * scale);
    setPixel(im.rgba, 40, 30, topLuma, topLuma, topLuma);
    setPixel(im.rgba, 5, 5, std::nanf(""), 1.0f, 1.0f);
    setPixel(im.rgba, 6, 5, -2.0f, -2.0f, -2.0f);
    upload(dev, im, kW, kH);
    return im;
}

// A varied image for the exposure: 0.01 * 2^(x / 32 + y / 48), no value on a bin edge.
Image makeGradient(ID3D11Device* dev, UINT w, UINT h, float gain) {
    Image im;
    im.rgba.assign(static_cast<size_t>(w) * h * 4, 0.0f);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const float v = gain * 0.0113f * std::exp2(static_cast<float>(x) / 32.0f + static_cast<float>(y) / 48.0f);
        float* c = &im.rgba[(static_cast<size_t>(y) * w + x) * 4];
        c[0] = v; c[1] = v; c[2] = v; c[3] = 1.0f;
    }
    upload(dev, im, w, h);
    return im;
}

double readExposure(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
    if (!tex) return -1.0;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return -2.0;
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return -3.0;
    float v; std::memcpy(&v, m.pData, 4);
    ctx->Unmap(staging.Get(), 0);
    return v;
}

// The value of `key=` in a log line, up to the next space.
bool fieldOf(const std::string& line, const char* key, std::string* out) {
    const std::string k = std::string(" ") + key + "=";
    const size_t at = line.find(k);
    if (at == std::string::npos) return false;
    const size_t start = at + k.size();
    const size_t end = line.find(' ', start);
    *out = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
    return true;
}
double numberOf(const std::string& line, const char* key) {
    std::string v;
    return fieldOf(line, key, &v) ? std::atof(v.c_str()) : -1e300;
}
bool isClose(double a, double b, double rel) { return std::fabs(a - b) <= rel * (std::fabs(b) > 1e-30 ? std::fabs(b) : 1.0); }

void pollUntilIdle(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < 40; ++i) { edvr::flatHdrEnergyPoll(ctx); Sleep(10); }
}

// The game's compute state, as a sentinel the module must put back.
struct Sentinel {
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav0, uav1;
    ComPtr<ID3D11Buffer> cb;
};
bool bindSentinel(ID3D11Device* dev, ID3D11DeviceContext* ctx, Sentinel& s) {
    static const char kCs[] = "RWByteAddressBuffer O : register(u0); RWByteAddressBuffer P : register(u1); Texture2D<float4> T : register(t0);"
                              " cbuffer C : register(b0) { float4 v; } [numthreads(1,1,1)] void main() { O.Store(0, asuint(T.Load(int3(0,0,0)).x + v.x)); P.Store(0, 1); }";
    typedef HRESULT(WINAPI * Compile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE c = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = c ? reinterpret_cast<Compile>(GetProcAddress(c, "D3DCompile")) : nullptr;
    ComPtr<ID3DBlob> blob, err;
    if (!compile || FAILED(compile(kCs, sizeof(kCs) - 1, "sentinel", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err))) return false;
    if (FAILED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s.cs))) return false;
    ComPtr<ID3D11Buffer> b0, b1;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &b0)) || FAILED(dev->CreateBuffer(&bd, nullptr, &b1))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(dev->CreateUnorderedAccessView(b0.Get(), &ud, &s.uav0)) || FAILED(dev->CreateUnorderedAccessView(b1.Get(), &ud, &s.uav1))) return false;
    D3D11_BUFFER_DESC cd{}; cd.ByteWidth = 16; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cd, nullptr, &s.cb))) return false;
    std::vector<uint8_t> px(4 * 4, 0);
    ComPtr<ID3D11Texture2D> t = makeTexture(dev, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &px);
    s.srv = makeSrv(dev, t.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!s.srv) return false;
    ID3D11UnorderedAccessView* u[2] = {s.uav0.Get(), s.uav1.Get()};
    ID3D11ShaderResourceView* sv = s.srv.Get(); ID3D11Buffer* cb = s.cb.Get();
    ctx->CSSetShader(s.cs.Get(), nullptr, 0); ctx->CSSetShaderResources(0, 1, &sv);
    ctx->CSSetUnorderedAccessViews(0, 2, u, nullptr); ctx->CSSetConstantBuffers(0, 1, &cb);
    return true;
}
bool sentinelHeld(ID3D11DeviceContext* ctx, const Sentinel& s) {
    ComPtr<ID3D11ComputeShader> cs; ID3D11ClassInstance* ci[8]{}; UINT n = 8; ctx->CSGetShader(&cs, ci, &n);
    for (UINT i = 0; i < n; ++i) if (ci[i]) ci[i]->Release();
    ComPtr<ID3D11ShaderResourceView> srv; ctx->CSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11UnorderedAccessView> u[2]; ctx->CSGetUnorderedAccessViews(0, 2, u[0].GetAddressOf());
    ComPtr<ID3D11Buffer> cb; ctx->CSGetConstantBuffers(0, 1, &cb);
    return cs.Get() == s.cs.Get() && srv.Get() == s.srv.Get() && u[0].Get() == s.uav0.Get() && u[1].Get() == s.uav1.Get() && cb.Get() == s.cb.Get();
}

int selfTest() {
    Warp w = makeWarp();
    if (!w.ok) return 1;
    ID3D11Device* dev = w.device.Get();
    ID3D11DeviceContext* ctx = w.context.Get();
    Sentinel sentinel;
    check(bindSentinel(dev, ctx, sentinel), "the sentinel compute state binds");

    // ---- never ran: no line, no armed line, before the route offers a frame -------------------------------------------------
    const std::string silent = withLog(L"hdrenergy_silent", [&] {
        edvr::flatHdrEnergyFlush();
        edvr::flatHdrEnergyPoll(ctx);
        edvr::flatHdrEnergyFlush();
    });
    check(countLines(silent, "flat hdr energy") == 0, "before any frame the route offers, the instrument says nothing: never ran reads as no line");

    // ---- the energy line against the CPU reference, with the measured exposure beside it -------------------------------------
    Image in = makeScene(dev, 1.0f, 0, 70000.0f);
    Image out = makeScene(dev, 1.07f, 1, 80000.0f);
    const float threshold = edvr::flatHdrEnergyThreshold(0.0);   // the first sample: 8
    const edvr::FlatHdrEnergySample refIn = edvr::flatHdrEnergyReference(in.rgba.data(), kW, kH, threshold);
    const edvr::FlatHdrEnergySample refOut = edvr::flatHdrEnergyReference(out.rgba.data(), kW, kH, threshold);
    const edvr::FlatHdrExposureMeasure measure = edvr::flatHdrExposureMeasure(in.rgba.data(), kW, kH);
    check(refIn.over == 21 && refOut.over == 21 && refIn.bad == 2 && refIn.saturated == 1 && refOut.saturated == 1,
          "the synthetic scenes hold the 20 star pixels and the one over the top, the NaN and the negative");
    double exposureWasFresh = -1.0, texturePolled = 0.0, appliedExpected = 0.0;
    bool heldAfterEnergy = false, heldAfterExposure = false;
    const std::string log1 = withLog(L"hdrenergy_one", [&] {
        edvr::flatHdrEnergyPoll(ctx);
        check(edvr::flatHdrExposureTexture() == nullptr, "after a Poll the exposure texture is not handed out: a frame with no measurement falls back");
        const bool updated = edvr::flatHdrExposureUpdate(ctx, in.srv.Get(), kW, kH, false);
        check(updated, "the exposure pass runs on the scene image");
        heldAfterExposure = sentinelHeld(ctx, sentinel);
        ID3D11Texture2D* tex = edvr::flatHdrExposureTexture();
        check(tex != nullptr, "the exposure texture is handed out on the frame the pass ran");
        exposureWasFresh = readExposure(dev, ctx, tex);
        appliedExpected = edvr::flatHdrExposureValue(measure.whiteLog2);
        const bool sampled = edvr::flatHdrEnergyBegin(ctx, in.srv.Get(), kW, kH, "flat");
        check(sampled, "the first frame the route offers is a sample");
        edvr::flatHdrEnergyEnd(ctx, out.srv.Get(), "dlaa", 11, "scene");
        heldAfterEnergy = sentinelHeld(ctx, sentinel);
        pollUntilIdle(ctx);
        texturePolled = edvr::flatHdrExposureTexture() == nullptr ? 1.0 : 0.0;
        edvr::flatHdrExposureCountFallback();
        edvr::flatHdrEnergyFlush();
    });
    check(heldAfterExposure && heldAfterEnergy, "the game's compute shader, SRV 0, UAVs 0 and 1 and constant buffer 0 are put back exactly after both passes");
    check(texturePolled == 1.0, "Poll clears the exposure texture's freshness");
    check(isClose(exposureWasFresh, appliedExpected, 1e-4),
          "the first measured exposure is 1 / W of the CPU reference (the 90th percentile bin of the 128x128 grid)");
    const auto lines = linesWith(log1, "flat hdr energy:");
    check(countLines(log1, "flat hdr energy: armed") == 1, "an armed line is said once");
    std::string energyLine;
    for (const auto& l : lines) if (l.find("route=flat") != std::string::npos && l.find(" R=") != std::string::npos) energyLine = l;
    check(!energyLine.empty(), "the 5 s line is written");
    if (!energyLine.empty()) {
        const double refR = refOut.bright / refIn.bright, refM = refOut.max / refIn.max;
        check(isClose(numberOf(energyLine, "R"), refR, 2e-4), "R is the bright-pixel luma sum out over in, as the CPU reference says");
        check(isClose(numberOf(energyLine, "M"), refM, 2e-4), "M is the largest luma out over in, as the CPU reference says");
        std::string n, sat, bad, backend, preset, exposure;
        fieldOf(energyLine, "N", &n); fieldOf(energyLine, "saturated", &sat); fieldOf(energyLine, "bad", &bad);
        fieldOf(energyLine, "backend", &backend); fieldOf(energyLine, "preset", &preset); fieldOf(energyLine, "exposure", &exposure);
        check(n == "21.0/21.0", "N counts the pixels over T in and out");
        check(sat == "1.0/1.0", "the saturated count sees the pixel at the top of the format, in and out");
        check(bad == "2/2", "the NaN and the negative pixel are counted bad, in and out, and left out of the sums");
        check(backend == "dlaa" && preset == "K" && exposure == "scene", "the line names the backend, the preset and the exposure mode");
        check(isClose(numberOf(energyLine, "T"), threshold, 1e-3), "T is the first sample's threshold");
        check(isClose(numberOf(energyLine, "gm-in"), refIn.geoMean, 1e-3), "gm-in is the input's geometric mean luma");
        check(isClose(numberOf(energyLine, "total-out/in"), refOut.total / refIn.total, 1e-3), "total-out/in is the whole picture's luma ratio");
        check(isClose(numberOf(energyLine, "applied-exposure"), appliedExpected, 1e-3), "the line carries the exposure the pass applied");
        check(numberOf(energyLine, "fallbacks") == 1.0, "a frame that fell back to 1.0 is counted and reaches the line");
        check(numberOf(energyLine, "samples") == 1.0 && numberOf(energyLine, "frames") == 1.0, "one frame offered, one sample taken");
        check(numberOf(energyLine, "hazards") == 0.0 && numberOf(energyLine, "failures") == 0.0, "no hazard and no failure");
    }

    // ---- the exposure's smoothing and reset ---------------------------------------------------------------------------------
    Image gradA = makeGradient(dev, 256, 192, 1.0f);
    Image gradB = makeGradient(dev, 256, 192, 40.0f);
    const edvr::FlatHdrExposureMeasure mA = edvr::flatHdrExposureMeasure(gradA.rgba.data(), 256, 192);
    const edvr::FlatHdrExposureMeasure mB = edvr::flatHdrExposureMeasure(gradB.rgba.data(), 256, 192);
    check(mA.samples == 16384 && mB.samples == 16384 && mB.bin > mA.bin, "the gradient images measure 16384 samples and the brighter one a higher bin");
    edvr::flatHdrEnergyPoll(ctx);
    check(edvr::flatHdrExposureUpdate(ctx, gradA.srv.Get(), 256, 192, true), "the exposure pass runs on the gradient");
    const double e1 = readExposure(dev, ctx, edvr::flatHdrExposureTexture());
    check(isClose(e1, edvr::flatHdrExposureValue(mA.whiteLog2), 1e-4), "a reset takes the measurement at once");
    edvr::flatHdrEnergyPoll(ctx);
    check(edvr::flatHdrExposureUpdate(ctx, gradB.srv.Get(), 256, 192, false), "the next frame measures the brighter scene");
    const double e2 = readExposure(dev, ctx, edvr::flatHdrExposureTexture());
    check(isClose(e2, edvr::flatHdrExposureValue(edvr::flatHdrExposureSmooth(mA.whiteLog2, true, mB.whiteLog2, false)), 1e-4),
          "without a reset the exposure moves 8% of the way in log2 toward the new measurement");
    check(e2 < e1, "a brighter scene lowers the exposure");
    edvr::flatHdrEnergyPoll(ctx);
    check(edvr::flatHdrExposureUpdate(ctx, gradB.srv.Get(), 256, 192, true), "a reset frame measures again");
    const double e3 = readExposure(dev, ctx, edvr::flatHdrExposureTexture());
    check(isClose(e3, edvr::flatHdrExposureValue(mB.whiteLog2), 1e-4), "a reset restarts the smoothing at the new measurement");
    check(sentinelHeld(ctx, sentinel), "the game's compute state is still held after the exposure passes");

    // ---- ran and found nothing: frames offered, no sample finished ----------------------------------------------------------
    Sleep(560);   // past the sample interval, so the next frame samples
    const std::string log2 = withLog(L"hdrenergy_quiet", [&] {
        edvr::flatHdrEnergyFlush();   // nothing carried over
        const bool sampled = edvr::flatHdrEnergyBegin(ctx, in.srv.Get(), kW, kH, "flat");
        check(sampled, "a frame after the interval samples");
        edvr::flatHdrEnergyAbandon();   // the resolve refused: the slot's input is dropped
        pollUntilIdle(ctx);
        edvr::flatHdrEnergyFlush();
    });
    const auto quiet = linesWith(log2, "flat hdr energy: route=flat");
    check(quiet.size() == 1 && quiet[0].find("samples=0 frames=1 ") != std::string::npos,
          "frames offered and no sample finished prints a samples=0 line, so it reads differently from never ran");

    std::printf("flat_hdr_energy_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool dry = false, self = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        if (!std::strcmp(argv[i], "--self-test")) self = true;
    }
    if (dry) {
        std::printf("flat_hdr_energy_test: would run the energy line, exposure, state-restore and never-ran scenarios on WARP; wrote nothing\n");
        return 0;
    }
    if (self) return selfTest();
    std::printf("usage: flat_hdr_energy_test --dry-run | --self-test\n");
    return 2;
}
