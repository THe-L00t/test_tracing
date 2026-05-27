// MotionVectorReuse.hlsl
// HPAR-PT Phase 7 (재정의) — Motion Vector Radiance Reuse (Tier 2 전용)
//
// 사용자 의도:
//   Tier 1 (Î > tierHigh)        — full PT (이 셰이더에서 처리 안 함)
//   Tier 2 (tierLow < Î ≤ tierHigh) — 이전 frame radiance 를 motion vector 로 reproject
//                                     해서 그대로 가져옴 (estimator bias 도입 없음)
//   Tier 3 (Î ≤ tierLow)         — hole, 이 셰이더에서 처리 안 함 (Phase 10)
//
// 유효성 검사 (Tier 2 only, fail 시 reuseValidMask = 0):
//   - 재투영 좌표가 화면 밖
//   - |depth_prev - depth_curr| > depthThreshold
//   - dot(normal_prev, normal_curr) < normalCosThreshold
//
// 유효한 Tier 2 → reusedRadiance = prevRadiance (bilinear sample),  validMask = 1
// 그 외 (Tier 1/3 + invalid Tier 2) → reusedRadiance = 0, validMask = 0
//
// **exclusivity 원칙**: 이 셰이더는 final composite (Phase 12) 의 input 만 만든다.
//   Tier 1 픽셀의 reusedRadiance 는 어차피 composite 에서 버려지므로 0 으로 두 어도 OK.

Texture2D<float>   g_currDepth     : register(t0); // R32F NDC depth
Texture2D<float4>  g_currNormal    : register(t1); // RGBA16F world normal(xyz) + roughness(w)
Texture2D<float2>  g_motionVec     : register(t2); // RG16F pixel-space motion (prev_px - curr_px)
Texture2D<float>   g_prevDepth     : register(t3); // R32F prev frame NDC depth
Texture2D<float4>  g_prevNormal    : register(t4); // RGBA16F prev frame world normal+roughness
Texture2D<float4>  g_prevRadiance  : register(t5); // RGBA16F prev frame PT output (m_renderAccum)
Texture2D<float>   g_importance    : register(t6); // R16F smooth Î

RWTexture2D<float4> g_reusedRadiance : register(u0); // RGBA16F (rgb=radiance, a=1)
RWTexture2D<float>  g_reuseValidMask : register(u1); // R8_UNORM (1=valid Tier 2 reuse, 0=else)

SamplerState g_linearClamp : register(s0);

cbuffer ReuseCB : register(b0)
{
    uint  g_width;
    uint  g_height;
    float g_tierLow;
    float g_tierHigh;

    float g_depthThreshold;     // ex: 0.001 (NDC) — disocclusion
    float g_normalCosThreshold; // ex: 0.906 (~25°)
    uint  g_firstFrame;         // 1=시간 history 없음 → 모든 픽셀 invalid
    float g_motionLenMaxPx;     // ex: 256 — 극단적 disocclusion 제외
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    // 기본 invalid
    g_reusedRadiance[px] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_reuseValidMask[px] = 0.0f;

    // first frame: history 없음
    if (g_firstFrame != 0u) return;

    // ── 1. Tier 판정 ─────────────────────────────────────────────
    //   Tier 1 (Î > tierHigh) 와 Tier 3 (Î ≤ tierLow) 는 이 패스에서 skip.
    //   exclusivity: Tier 1 픽셀에는 reuse 결과를 만들지 않음 (composite 에서 PT 사용).
    float Ihat = g_importance.Load(int3((int2)px, 0));
    bool isTier2 = (Ihat > g_tierLow) && (Ihat <= g_tierHigh);
    if (!isTier2) return;

    // ── 2. 재투영 좌표 ───────────────────────────────────────────
    //   motion vector 규약: prev_px = curr_px + mv  (Raytracing.hlsl 의 출력 규약)
    float2 mv = g_motionVec.Load(int3((int2)px, 0));
    if (length(mv) > g_motionLenMaxPx) return;  // 극단 disocclusion 보호

    float2 prevPx = float2(px) + mv;
    if (prevPx.x < 0.0f || prevPx.y < 0.0f ||
        prevPx.x >= (float)g_width || prevPx.y >= (float)g_height) return;

    // ── 3. Depth 유효성 ─────────────────────────────────────────
    float depthCurr = g_currDepth.Load(int3((int2)px, 0));
    // bilinear UV (텍스처 가운데 0.5 보정)
    float2 uv = (prevPx + 0.5f) / float2((float)g_width, (float)g_height);
    float depthPrev = g_prevDepth.SampleLevel(g_linearClamp, uv, 0);
    if (abs(depthPrev - depthCurr) > g_depthThreshold) return;

    // ── 4. Normal 유효성 ────────────────────────────────────────
    float3 nCurr = normalize(g_currNormal.Load(int3((int2)px, 0)).xyz);
    float3 nPrev = normalize(g_prevNormal.SampleLevel(g_linearClamp, uv, 0).xyz);
    if (dot(nCurr, nPrev) < g_normalCosThreshold) return;

    // ── 5. Radiance 재투영 (bilinear) ───────────────────────────
    //   unbiased: prev frame PT 결과를 그대로 가져옴 — statistical resampling 없음
    float4 prevRad = g_prevRadiance.SampleLevel(g_linearClamp, uv, 0);

    g_reusedRadiance[px] = float4(prevRad.rgb, 1.0f);
    g_reuseValidMask[px] = 1.0f;
}
