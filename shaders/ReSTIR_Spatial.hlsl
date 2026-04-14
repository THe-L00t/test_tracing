// ──────────────────────────────────────────────────────────────
//  ReSTIR_Spatial.hlsl  –  [Pass 4] Spatial Reuse
//
//  역할: 주변 픽셀의 Reservoir를 현재 픽셀에 재사용하여 분산 추가 감소.
//        Jacobian 보정으로 픽셀 간 입체각 측도 차이 교정.
//
//  알고리즘 (Bitterli 2020, Algorithm 4):
//    For each pixel p:
//      R_out = reservoir_in[p]          // 이번 pass 입력 (Temporal 결과 or pass-0 결과)
//      For k = 1..spatialSamples:
//        q = RandomNeighbor(p, spatialRadius)
//        if IsSimilarSurface(p, q) == false: continue
//        R_q = reservoir_in[q]
//        J   = CalcJacobian(x_p, x_q, light_q.pos, lightNormal)  // Eq.11
//        pHat_q_at_p = EvalTargetPDF(p.surface, R_q.lightIdx) * J
//        MergeReservoir(R_out, R_q, pHat_q_at_p)
//      FinalizeReservoir(R_out)
//      reservoir_out[p] = R_out
//
//  입력:  G-Buffer SRV (t6-t9), reservoir_in SRV (t10) ← 동적 (A 또는 B)
//  출력:  reservoir_out UAV (u6) ← 동적 (B 또는 A)
//
//  주의: App에서 2회 dispatch 시 t10/u6 역할이 ping-pong으로 교환됨
//        Pass 0: t10=cur(A) → u6=temp(B)
//        Pass 1: t10=temp(B) → u6=cur(A)  ← 최종 결과가 항상 cur(A)에
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>              gbuf_worldPos  : register(t6);
Texture2D<float4>              gbuf_normal    : register(t7);
Texture2D<float4>              gbuf_albedo    : register(t8);
Texture2D<float4>              gbuf_matInfo   : register(t9);
StructuredBuffer<LightData>    g_lightList    : register(t5);

StructuredBuffer<Reservoir>    reservoir_in   : register(t10);  // 이번 pass 읽기 전용
RWStructuredBuffer<Reservoir>  reservoir_out  : register(u6);   // 이번 pass 쓰기 전용

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

// ── 표면 유사도 검사 (Bitterli 2020, Section 4.4) ─────────────
static const float k_depthThreshold  = 0.1f;
static const float k_normalThreshold = 0.906f;  // cos(25°)

bool IsSimilarSurface(float3 N_p, float depth_p,
                      float3 N_q, float depth_q)
{
    bool depthOk  = abs(depth_p - depth_q) / max(depth_p, 1e-4f) < k_depthThreshold;
    bool normalOk = dot(N_p, N_q) > k_normalThreshold;
    return depthOk && normalOk;
}

[numthreads(8, 8, 1)]
void CS_Spatial(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint   pidx   = PixelIndex(px, screenW);
    float4 wPos   = gbuf_worldPos[px];
    float4 matInf = gbuf_matInfo[px];

    if (wPos.w < 0.0f || matInf.b > 0.5f)
    {
        reservoir_out[pidx] = MakeEmptyReservoir();
        return;
    }

    float3 hitPos_p   = wPos.xyz;
    float3 N_p        = gbuf_normal[px].xyz;
    float  depth_p    = matInf.g;

    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u + 3571u) | 1u;

    Reservoir R_out = reservoir_in[pidx];  // 현재 픽셀 기저 Reservoir

    // ── 공간 이웃 병합 ────────────────────────────────────────────
    for (uint k = 0u; k < spatialSamples; k++)
    {
        // 반경 내 균일 랜덤 이웃 픽셀 선택 (원판 균일 샘플링)
        float angle = RandFloat(seed) * TWO_PI;
        float rad   = sqrt(RandFloat(seed)) * float(spatialRadius);

        int2  offset = int2(int(rad * cos(angle)), int(rad * sin(angle)));
        int2  qPx    = int2(px) + offset;

        if (!IsValidPixel(qPx, screenW, screenH)) continue;

        uint2  qPxU    = uint2(qPx);
        float4 wPos_q  = gbuf_worldPos[qPxU];
        float4 matInf_q = gbuf_matInfo[qPxU];

        if (wPos_q.w < 0.0f || matInf_q.b > 0.5f) continue;

        float3 N_q    = gbuf_normal[qPxU].xyz;
        float  depth_q = matInf_q.g;

        if (!IsSimilarSurface(N_p, depth_p, N_q, depth_q)) continue;

        Reservoir R_q = reservoir_in[PixelIndex(qPxU, screenW)];
        if (R_q.lightIdx == 0xFFFFFFFFu) continue;

        // Jacobian 보정 (Bitterli 2020, Eq.11 / Algorithm 4, line 7)
        // Point light(type==0): lightNormal=float3(0,0,0) → cosine 항 상쇄 → J=dist²_p/dist²_q
        // Area light(type==2):  lightNormal을 전달해야 하나 현재 LightData에 면 법선 없음 → zero로 통일
        LightData light_q    = g_lightList[R_q.lightIdx];
        float3    lightNormal = float3(0.0f, 0.0f, 0.0f);  // point/directional: cosine 상쇄
        float     J          = CalcJacobian(hitPos_p, wPos_q.xyz, light_q.pos, lightNormal);
        float     pHat_q     = EvalTargetPDF(hitPos_p, N_p, light_q) * J;

        MergeReservoir(R_out, R_q, pHat_q, seed);
    }

    // 최종화
    if (R_out.lightIdx != 0xFFFFFFFFu)
        FinalizeReservoir(R_out, EvalTargetPDF(hitPos_p, N_p, g_lightList[R_out.lightIdx]));

    reservoir_out[pidx] = R_out;
}
