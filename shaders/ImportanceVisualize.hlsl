// ImportanceVisualize.hlsl
// HPAR-PT Phase 1 + Phase 6 디버그 시각화
//
// importance map (R16F, render-res) → display-res 출력 (RGBA8)
//   showTier = 0 : grayscale heatmap (검정→노랑→흰색)
//   showTier = 1 : Tier 색 (1=빨강, 2=초록, 3=파랑)  ← Phase 6 F4 토글
// bilinear upscale 내장.

Texture2D<float>     g_importance : register(t0);  // R16F, render-res (854x480)
SamplerState         s_linear     : register(s0);
RWTexture2D<float4>  g_output     : register(u0);  // RGBA8, display-res (1280x720)

cbuffer VisualizeCB : register(b0)
{
    uint  g_displayW;
    uint  g_displayH;
    float g_renderW;
    float g_renderH;
    uint  g_showTier;   // Phase 6: 0=heatmap, 1=tier 색
    float g_tierLow;    // Phase 6: Î ≤ tierLow → Tier 3
    float g_tierHigh;   // Phase 6: Î > tierHigh → Tier 1
    float g_vis_pad0;
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 dpx = DTid.xy;
    if (dpx.x >= g_displayW || dpx.y >= g_displayH) return;

    // display-res 픽셀 중심의 UV (0..1) → render-res 의 bilinear sample
    float2 uv = (float2(dpx) + 0.5f) / float2(g_displayW, g_displayH);
    float I = g_importance.SampleLevel(s_linear, uv, 0);

    float3 color;
    if (g_showTier != 0u)
    {
        // Phase 6 — Tier 분류 색 (PHTR 통합 마커)
        //   Tier 1 (Î > high)        : full PT, reuse 금지  → 빨강
        //   Tier 2 (low < Î ≤ high)  : partial reuse        → 초록
        //   Tier 3 (Î ≤ low)         : aggressive reuse     → 파랑
        if (I > g_tierHigh)      color = float3(1.0f, 0.15f, 0.15f);
        else if (I > g_tierLow)  color = float3(0.15f, 1.0f, 0.15f);
        else                     color = float3(0.15f, 0.30f, 1.0f);
    }
    else
    {
        // Phase 1 heatmap : 0→검정, 0.5→노랑, 1.0→흰색
        if (I < 0.5f) color = float3(I * 2.0f, I * 2.0f, 0.0f);
        else          { float t = (I - 0.5f) * 2.0f; color = float3(1.0f, 1.0f, t); }
    }
    g_output[dpx] = float4(color, 1.0f);
}
