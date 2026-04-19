// ──────────────────────────────────────────────────────────────
//  ReSTIR_GI_Spatial.hlsl  –  [GI Pass 3] GI Spatial Reuse
//
//  역할: 이웃 픽셀의 GI Reservoir를 재사용하여 분산 추가 감소.
//        Jacobian + 현재 픽셀 BRDF로 pHat 재평가 (RTXDI 방식).
//
//  알고리즘 (Ouyang 2021 + RTXDI 방식):
//    For each pixel p:
//      For k random neighbors q:
//        dir_q = normalize(R_q.samplePos - x_p)    // 현재 픽셀 p 기준 방향
//        J     = CalcGIJacobian(x_p, x_q, xs, Ns)
//        pHat  = EvalGIpHat(R_q.radiance, N_p, V_p, dir_q, material_p) * J
//        MergeGIReservoir(R_p, R_q, pHat)
//      dir_sel = normalize(R_p.samplePos - x_p)
//      FinalizeGIReservoir(R_p, EvalGIpHat(R_p.radiance, N_p, V_p, dir_sel, material_p))
//
//  레지스터:
//    입력:  G-Buffer SRV (t6-t9), gi_reservoir_in SRV (t13)
//    출력:  gi_reservoir_cur UAV (u8)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>               gbuf_worldPos      : register(t6);
Texture2D<float4>               gbuf_normal        : register(t7);
Texture2D<float4>               gbuf_albedo        : register(t8);  // EvalGIpHat용
Texture2D<float4>               gbuf_matInfo       : register(t9);

StructuredBuffer<GIReservoir>   gi_reservoir_in    : register(t13);
RWStructuredBuffer<GIReservoir> gi_reservoir_cur   : register(u8);

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

static const float k_normalThresholdSp = 0.906f;  // cos(25°)

[numthreads(8, 8, 1)]
void CS_GI_Spatial(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint   pidx    = PixelIndex(px, screenW);
    float4 wPos    = gbuf_worldPos[px];
    float4 matInf  = gbuf_matInfo[px];

    if (wPos.w < 0.0f || matInf.b > 0.5f) return;

    float3 xp       = wPos.xyz;
    float3 Np       = gbuf_normal[px].xyz;
    float  roughness= matInf.r;

    float4 alb     = gbuf_albedo[px];
    float3 albedo  = alb.rgb;
    float  metallic= alb.a;

    float3 Vp = normalize(camPos - xp);

    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u + 1234u) | 1u;

    GIReservoir R_p = gi_reservoir_in[pidx];

    // 이웃 픽셀 reuse
    for (uint s = 0u; s < spatialSamples; s++)
    {
        // 원형 균일 샘플링
        float angle  = RandFloat(seed) * TWO_PI;
        float r      = RandFloat(seed) * float(spatialRadius);
        int2  offset = int2(int(r * cos(angle)), int(r * sin(angle)));
        int2  qPx    = int2(px) + offset;

        if (!IsValidPixel(qPx, screenW, screenH)) continue;

        float4 wPosQ  = gbuf_worldPos[uint2(qPx)];
        float4 mInfoQ = gbuf_matInfo[uint2(qPx)];
        if (wPosQ.w < 0.0f || mInfoQ.b > 0.5f) continue;

        float3 Nq = gbuf_normal[uint2(qPx)].xyz;
        if (dot(Np, Nq) < k_normalThresholdSp) continue;

        uint        qIdx = PixelIndex(uint2(qPx), screenW);
        GIReservoir R_q  = gi_reservoir_in[qIdx];
        if (R_q.valid == 0u || R_q.W <= 0.0f) continue;

        float3 xq = wPosQ.xyz;

        // Jacobian 보정: 이웃 q의 xs를 현재 p에서 재사용 시 입체각 측도 변환
        float J = CalcGIJacobian(xp, xq, R_q.samplePos, R_q.sampleNormal);

        // pHat: 현재 픽셀 p의 BRDF로 평가 (RTXDI 방식)
        float3 dirQ = normalize(R_q.samplePos - xp);  // p에서 xs 방향
        float  pHat = EvalGIpHat(R_q.radiance, Np, Vp, dirQ, albedo, metallic, roughness) * J;

        MergeGIReservoir(R_p, R_q, pHat, seed);
    }

    if (R_p.valid != 0u)
    {
        // 선택된 샘플의 pHat을 현재 픽셀 기준으로 재평가
        float3 dirSel   = normalize(R_p.samplePos - xp);
        float  pHatSel  = EvalGIpHat(R_p.radiance, Np, Vp, dirSel, albedo, metallic, roughness);
        FinalizeGIReservoir(R_p, pHatSel);
    }

    gi_reservoir_cur[pidx] = R_p;
}
