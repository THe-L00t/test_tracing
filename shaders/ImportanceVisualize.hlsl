// ImportanceVisualize.hlsl
// HPAR-PT Phase 1 디버그 시각화
//
// importance map (R16F, render-res) → grayscale (RGBA8, display-res)
// bilinear upscale 내장. F1 키로 토글된 debug 모드 전용.

Texture2D<float>     g_importance : register(t0);  // R16F, render-res (854x480)
SamplerState         s_linear     : register(s0);
RWTexture2D<float4>  g_output     : register(u0);  // RGBA8, display-res (1280x720)

cbuffer VisualizeCB : register(b0)
{
    uint  g_displayW;
    uint  g_displayH;
    float g_renderW;
    float g_renderH;
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 dpx = DTid.xy;
    if (dpx.x >= g_displayW || dpx.y >= g_displayH) return;

    // display-res 픽셀 중심의 UV (0..1) → render-res 의 bilinear sample
    float2 uv = (float2(dpx) + 0.5f) / float2(g_displayW, g_displayH);
    float I = g_importance.SampleLevel(s_linear, uv, 0);

    // 단순 grayscale + 작은 색상 매핑으로 가독성 ↑
    //   I → (R=2I, G=2I-1, B=clamp(I-0.5)*2) 로 heatmap 풍
    float3 color;
    if (I < 0.5f)
    {
        // 0 → 검정, 0.5 → 노랑
        color = float3(I * 2.0f, I * 2.0f, 0.0f);
    }
    else
    {
        // 0.5 → 노랑, 1.0 → 흰색
        float t = (I - 0.5f) * 2.0f;
        color = float3(1.0f, 1.0f, t);
    }
    g_output[dpx] = float4(color, 1.0f);
}
