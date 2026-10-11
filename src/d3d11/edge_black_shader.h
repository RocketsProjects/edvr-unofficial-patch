// The edge-black watch's one compute shader (docs\terrain-culling.md, 2026-10-10; src/d3d11/edge_black.cpp). Compiled at build time with the other fixed
// shaders (tools/temporal_shader_build, symbol kEdgeBlackBytecode); never in the game.
//
// One thread per grid point, 64 x 64 of them (an 8 x 8 dispatch of 8 x 8 thread groups). Each point-samples the game's submitted eye texture, through a
// plain typed view of its own format so the numbers are the texture's own encoding (an 8-bit texture reads as code / 255, a float texture as its float),
// at the centre of grid cell (x, y) of the submitted region, and writes the largest of red, green and blue: black is "every channel black", and a lit
// point of any hue is lit. The region is the part of the texture the frame was rendered into (the submit's bounds), so the grid spans the image the
// headset is shown, not whatever slack the texture has. A mirrored eye (bounds given the other way round) is un-mirrored, so a cell's place is its place
// in the displayed image whichever way the game gave the bounds.
#pragma once

namespace edvr {

inline constexpr const char* kEdgeBlackCs = R"HLSL(
Texture2D<float4> Src : register(t0);
RWStructuredBuffer<float> Out : register(u0);
cbuffer P : register(b0)
{
    int4 Region;   // left, top, right, bottom: the submitted image's texels in the texture
    int4 Flip;     // x: mirrored horizontally, y: mirrored vertically
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float u = (float(id.x) + 0.5) / 64.0;
    float v = (float(id.y) + 0.5) / 64.0;
    if (Flip.x != 0) u = 1.0 - u;
    if (Flip.y != 0) v = 1.0 - v;
    int2 size = Region.zw - Region.xy;
    int2 p = Region.xy + min(int2(u * float(size.x), v * float(size.y)), size - 1);
    float3 c = Src.Load(int3(p, 0)).rgb;
    Out[id.y * 64 + id.x] = max(c.r, max(c.g, c.b));
}
)HLSL";

}  // namespace edvr
