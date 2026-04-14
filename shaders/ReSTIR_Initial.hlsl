// ──────────────────────────────────────────────────────────────
//  ReSTIR_Initial.hlsl  –  [Pass 2] Initial Candidate Generation
//
//  역할: 픽셀당 M개의 광원 후보를 RIS로 샘플링하여 Reservoir에 저장.
//
//  알고리즘 (Talbot 2005, Algorithm 2 / Bitterli 2020, Algorithm 2):
//    For each pixel p:
//      R = empty Reservoir
//      For i = 1..M:
//        j  ~ Uniform(0, lightCount-1)         // 후보 광원 균일 선택
//        w  = p̂(j) * lightCount                // w = p̂ / q, q = 1/N
//        UpdateReservoir(R, j, w)
//      W = wSum / (M * p̂(selected))            // Eq.(3): 비편향 가중치
//      Write R → reservoir_cur[p]
//
//  입력:  G-Buffer SRV (t6-t9), LightList (t5), ReSTIRCB (b1)
//  출력:  reservoir_cur UAV (u6)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>              gbuf_worldPos  : register(t6);
Texture2D<float4>              gbuf_normal    : register(t7);
Texture2D<float4>              gbuf_albedo    : register(t8);
Texture2D<float4>              gbuf_matInfo   : register(t9);
StructuredBuffer<LightData>    g_lightList    : register(t5);
RWStructuredBuffer<Reservoir>  reservoir_cur  : register(u6);

cbuffer SceneConstants  : register(b0) { float3 camPos; uint sceneID; float4 _sc[15]; }
cbuffer ReSTIRConstants : register(b1)
{
    float4   _prevCam[4];
    uint     lightCount;
    uint     candidateCount;
    uint     screenW;
    uint     screenH;
    uint     frameIndex;
    float    temporalMaxM;
    uint     spatialRadius;
    uint     spatialSamples;
    float4   _pad[2];
}

[numthreads(8, 8, 1)]
void CS_Initial(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint   pidx   = PixelIndex(px, screenW);
    float4 wPos   = gbuf_worldPos[px];
    float4 matInf = gbuf_matInfo[px];

    // 배경(hitDist<0) 또는 유리/발광 픽셀: 빈 Reservoir 기록 후 종료
    // → Temporal/Spatial이 자동으로 skip (W=0, lightIdx=invalid)
    if (wPos.w < 0.0f || matInf.b > 0.5f)
    {
        reservoir_cur[pidx] = MakeEmptyReservoir();
        return;
    }

    float3 hitPos = wPos.xyz;
    float3 N      = gbuf_normal[px].xyz;

    // RNG 초기화 (픽셀 + 프레임 기반 seed)
    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u) | 1u;

    Reservoir R = MakeEmptyReservoir();

    // ── RIS 메인 루프 (Talbot 2005, Algorithm 2) ────────────────
    for (uint i = 0u; i < candidateCount; i++)
    {
        // 후보 광원 균일 선택: source PDF q = 1/lightCount
        uint jIdx = uint(RandFloat(seed) * float(lightCount));
        jIdx = min(jIdx, lightCount - 1u);

        LightData light = g_lightList[jIdx];

        // 가중치: w_i = p̂(x_i) / q(x_i) = p̂(x_i) * lightCount
        float pHat = EvalTargetPDF(hitPos, N, light);
        float w    = pHat * float(lightCount);

        UpdateReservoir(R, jIdx, w, seed);
    }

    // 최종 비편향 가중치 확정 (Talbot 2005, Algorithm 2, line 8)
    if (R.lightIdx != 0xFFFFFFFFu)
    {
        float pHat = EvalTargetPDF(hitPos, N, g_lightList[R.lightIdx]);
        FinalizeReservoir(R, pHat);
    }

    reservoir_cur[pidx] = R;
}
