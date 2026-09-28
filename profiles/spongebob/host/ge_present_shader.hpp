#pragma once

namespace spongebob {

inline constexpr char kGePresentShaderHlsl[] = R"PRESENT_HLSL(
Texture2D<float4> PresentTexture0 : register(t0);
Texture2D<float4> PresentTexture1 : register(t1);
SamplerState PresentSampler : register(s0);

struct PresentVertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
PresentVertexOutput PresentVS(uint id : SV_VertexID) {
    PresentVertexOutput o;
    if (id == 0u) { o.position=float4(-1,-1,0,1); o.uv=float2(0,1); }
    else if (id == 1u) { o.position=float4(-1,3,0,1); o.uv=float2(0,-1); }
    else { o.position=float4(3,-1,0,1); o.uv=float2(2,1); }
    return o;
}
float4 PresentPS(PresentVertexOutput i) : SV_TARGET {
    float width, height;
    PresentTexture0.GetDimensions(width, height);
    float2 uv_step = fwidth(i.uv);
    float2 footprint = uv_step * float2(width, height);
    if (footprint.x <= 1.0 && footprint.y <= 1.0)
        return PresentTexture0.SampleLevel(PresentSampler, i.uv, 0.0);
    uint2 taps = uint2(clamp(ceil(footprint), 1.0, 4.0));
    float2 tap_step = uv_step / float2(taps);
    float2 origin = i.uv - uv_step * 0.5 + tap_step * 0.5;
    float4 sum = 0.0;
    [loop] for (uint y = 0u; y < taps.y; ++y)
        [loop] for (uint x = 0u; x < taps.x; ++x)
            sum += PresentTexture0.SampleLevel(PresentSampler, origin + tap_step * float2(x, y), 0.0);
    return sum / float(taps.x * taps.y);
}
)PRESENT_HLSL";

}
