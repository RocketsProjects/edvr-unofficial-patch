// The two compute passes of the HDR route's energy instrument and exposure measurement (flat_hdr_energy.h; TEMPORARY, the
// 2026-10-14 DLAA bloom entry, design doc section 107). Compiled at build time by tools\temporal_shader_build into
// kFlatHdrEnergyBytecode and kFlatHdrExposureBytecode; the DLL carries no HLSL compiler.
#pragma once

namespace edvr {

// ---- the energy pass -----------------------------------------------------------------------------------------------
// One dispatch of 32x32 groups of 16x16 threads strides over the whole scene image. The raw output buffer, in 32-bit words
// (flat_hdr_energy.h names the layout and decodes it):
//   [0]  pixels whose luma is above the threshold                    ("over")
//   [1]  the largest finite luma, as float bits (non-negative floats order as unsigned)
//   [2]  non-finite or negative pixels, skipped                      ("bad")
//   [3]  pixels with a channel at or above 60000, the top of what R11G11B10_FLOAT holds ("saturated")
//   [4]  finite pixels counted
//   [8 .. 8+1023]            one float per group: the sum of luma over the pixels above the threshold ("bright")
//   [8+1024 .. 8+2047]       one float per group: the sum of luma over every finite pixel ("total")
//   [8+2048 .. 8+3071]       one float per group: the sum of log2(max(luma, 1e-5)) over every finite pixel ("log")
// The CPU adds the group sums in double, so the energy ratio is not limited by a float accumulator over millions of pixels.
constexpr char kFlatHdrEnergyCsHlsl[] = R"HLSL(
Texture2D<float4> Src : register(t0);
RWByteAddressBuffer Out : register(u0);
cbuffer Params : register(b0) { uint2 size; float threshold; float pad; };
groupshared float gsBright[256];
groupshared float gsTotal[256];
groupshared float gsLog[256];
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    float bright = 0.0, total = 0.0, logSum = 0.0, mx = 0.0;
    uint over = 0, bad = 0, saturated = 0, finitePixels = 0;
    for (uint y = gid.y * 16 + gtid.y; y < size.y; y += 512) {
        for (uint x = gid.x * 16 + gtid.x; x < size.x; x += 512) {
            float3 c = Src.Load(int3(x, y, 0)).rgb;
            float l = dot(c, float3(0.2126, 0.7152, 0.0722));
            if (!(l >= 0.0 && l < 3.0e38)) { ++bad; continue; }
            ++finitePixels;
            total += l;
            mx = max(mx, l);
            logSum += log2(max(l, 1.0e-5));
            if (max(c.r, max(c.g, c.b)) >= 60000.0) ++saturated;
            if (l > threshold) { bright += l; ++over; }
        }
    }
    gsBright[gi] = bright; gsTotal[gi] = total; gsLog[gi] = logSum;
    if (over) Out.InterlockedAdd(0, over);
    if (mx > 0.0) Out.InterlockedMax(4, asuint(mx));
    if (bad) Out.InterlockedAdd(8, bad);
    if (saturated) Out.InterlockedAdd(12, saturated);
    if (finitePixels) Out.InterlockedAdd(16, finitePixels);
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (gi < stride) {
            gsBright[gi] += gsBright[gi + stride];
            gsTotal[gi] += gsTotal[gi + stride];
            gsLog[gi] += gsLog[gi + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) {
        uint g = gid.y * 32 + gid.x;
        Out.Store((8 + g) * 4, asuint(gsBright[0]));
        Out.Store((8 + 1024 + g) * 4, asuint(gsTotal[0]));
        Out.Store((8 + 2048 + g) * 4, asuint(gsLog[0]));
    }
}
)HLSL";

// ---- the exposure pass ---------------------------------------------------------------------------------------------
// One group of 32x32 threads samples a 128x128 grid of the scene image (thread (tx, ty) takes the points
// (tx + 32 i, ty + 32 j), i and j in 0..3; point (gx, gy) is the pixel ((2 gx + 1) W / 256, (2 gy + 1) H / 256), integer
// division), bins each valid sample's luma into 64 half-stop bins (bin = clamp(floor((log2 l + 16) * 2), 0, 63), luma 0 in
// bin 0), and takes the 90th percentile bin: ceil(0.9 n) samples at or below it. The "white" is that bin's centre,
// log2 W = (bin + 0.5) * 0.5 - 16. The exposure is 1 / W, so the 90th percentile of the picture lands at 1.0 before the
// game's own tone curve, which is the range NVIDIA's network expects. The log2 is smoothed across frames (alpha per frame,
// at once on the first valid frame and on a reset) and stored in State; the exposure goes to the 1x1 texture.
//   State words: [0] smoothed log2 W (float bits), [1] 1 once a valid measurement has been made, [2] the exposure written
//   (float bits), [3] this frame's unsmoothed log2 W (float bits), [4] the valid samples this frame.
constexpr char kFlatHdrExposureCsHlsl[] = R"HLSL(
Texture2D<float4> Src : register(t0);
RWByteAddressBuffer State : register(u0);
RWTexture2D<float> Exposure : register(u1);
cbuffer Params : register(b0) { uint2 size; float alpha; uint reset; };
groupshared uint gsHist[64];
[numthreads(32, 32, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gi < 64) gsHist[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint j = 0; j < 4; ++j) {
        [unroll] for (uint i = 0; i < 4; ++i) {
            uint gx = gtid.x + 32 * i, gy = gtid.y + 32 * j;
            uint2 p = uint2(((2 * gx + 1) * size.x) / 256, ((2 * gy + 1) * size.y) / 256);
            float3 c = Src.Load(int3(p, 0)).rgb;
            float l = dot(c, float3(0.2126, 0.7152, 0.0722));
            if (l >= 0.0 && l < 3.0e38) {
                int bin = l > 0.0 ? clamp(int(floor((log2(l) + 16.0) * 2.0)), 0, 63) : 0;
                InterlockedAdd(gsHist[bin], 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        uint n = 0;
        for (uint b = 0; b < 64; ++b) n += gsHist[b];
        float prev = asfloat(State.Load(0));
        uint have = State.Load(4);
        float cur = 0.0;
        float s = prev;
        if (n > 0) {
            uint target = (n * 9 + 9) / 10;
            uint run = 0, bin = 63;
            for (uint k = 0; k < 64; ++k) {
                run += gsHist[k];
                if (run >= target) { bin = k; break; }
            }
            cur = (float(bin) + 0.5) * 0.5 - 16.0;
            s = (have == 0 || reset != 0) ? cur : lerp(prev, cur, alpha);
            have = 1;
        } else if (have == 0) {
            s = 0.0;
        }
        float e = exp2(-s);
        State.Store(0, asuint(s));
        State.Store(4, have);
        State.Store(8, asuint(e));
        State.Store(12, asuint(cur));
        State.Store(16, n);
        Exposure[uint2(0, 0)] = e;
    }
}
)HLSL";

}  // namespace edvr
