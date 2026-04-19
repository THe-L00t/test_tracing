// ──────────────────────────────────────────────────────────────
//  ReSTIR_GI_Temporal.hlsl  –  [GI Pass 2] GI Temporal Reuse
//
//  역할: 현재 프레임 GI Reservoir와 이전 프레임 GI Reservoir를 병합.
//        DI Temporal과 동일한 재투영·표면 연속성 검사 사용.
//
//  알고리즘 (Ouyang 2021 기반):
//    R_cur  = gi_reservoir_cur[p]       // GI Initial 결과
//    pPrev  = Reproject(p)
//    if pPrev 유효:
//      R_prev = gi_reservoir_prev[pPrev]
//      R_prev.M = min(R_prev.M, temporalMaxM * R_cur.M)
//      pHat_prev = EvalGITargetPDF(R_prev.radiance)
//      MergeGIReservoir(R_cur, R_prev, pHat_prev)
//    FinalizeGIReservoir(R_cur)
//
//  레지스터:
//    입력:  G-Buffer SRV (t6-t9), prev G-Buffer (t11-t12)
//    입출력: gi_reservoir_cur UAV (u8), gi_reservoir_prev UAV (u9)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>               gbuf_worldPos      : register(t6);
Texture2D<float4>               gbuf_normal        : register(t7);
Texture2D<float4>               gbuf_matInfo       : register(t9);
Texture2D<float4>               prev_gbuf_worldPos : register(t11);
Texture2D<float4>               prev_gbuf_normal   : register(t12);

RWStructuredBuffer<GIReservoir> gi_reservoir_cur   : register(u8);
RWStructuredBuffer<GIReservoir> gi_reservoir_prev  : register(u9);

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

static const float k_depthThreshold  = 0.1f;
static const float k_normalThreshold = 0.906f;

bool Reproject(uint2 px, float curDepth, float3 curN, out int2 prevPx)
{
    float3 worldPos  = gbuf_worldPos[px].xyz;
    float3 delta     = worldPos - _prevCam[0].xyz;
    float  prevViewZ = dot(delta, _prevCam[3].xyz);
    if (prevViewZ <= 0.0f) { prevPx = int2(px); return false; }

    float prevTanHFov = _prevCam[1].w;
    float prevAspect  = _prevCam[2].w;
    float prevNDC_x   = dot(delta, _prevCam[1].xyz) / (prevViewZ * prevAspect * prevTanHFov);
    float prevNDC_y   = dot(delta, _prevCam[2].xyz) / (prevViewZ * prevTanHFov);
    float2 prevUV     = float2(prevNDC_x, -prevNDC_y) * 0.5f + 0.5f;
    prevPx = int2(prevUV * float2(screenW, screenH));

    if (!IsValidPixel(prevPx, screenW, screenH)) return false;

    float3 prevWPos  = prev_gbuf_worldPos[uint2(prevPx)].xyz;
    float3 prevN     = prev_gbuf_normal[uint2(prevPx)].xyz;
    float  prevDepth = dot(prevWPos - _prevCam[0].xyz, _prevCam[3].xyz);

    bool depthOk  = abs(prevDepth - curDepth) / max(curDepth, 1e-4f) < k_depthThreshold;
    bool normalOk = dot(prevN, curN) > k_normalThreshold;
    return depthOk && normalOk;
}

[numthreads(8, 8, 1)]
void CS_GI_Temporal(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint   pidx   = PixelIndex(px, screenW);
    float4 wPos   = gbuf_worldPos[px];
    float4 matInf = gbuf_matInfo[px];

    if (wPos.w < 0.0f || matInf.b > 0.5f) return;

    float3 N     = gbuf_normal[px].xyz;
    float  depth = matInf.g;

    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u + 7919u) | 1u;

    GIReservoir R_cur = gi_reservoir_cur[pidx];

    // 카메라 이동 직후 2프레임: stale 데이터 완전 차단
    // frameIndex=0: 이동 중, frameIndex=1~2: 정지 직후 첫 2프레임 (prev가 이동 시대 데이터)
    if (frameIndex <= 2u)
    {
        if (R_cur.valid != 0u)
            FinalizeGIReservoir(R_cur, EvalGITargetPDF(R_cur.radiance));
        gi_reservoir_cur[pidx] = R_cur;
        return;
    }

    // GI 전용 M 클램프: DI temporalMaxM(20)보다 낮게 고정.
    // 수렴 분석: W_new = (1 + k*W_prev)/(k+1), k=5 → W → 1 수렴.
    static const float k_GI_temporalMaxM = 5.0f;

    int2 prevPx;
    if (Reproject(px, depth, N, prevPx))
    {
        uint        prevIdx = PixelIndex(uint2(prevPx), screenW);
        GIReservoir R_prev  = gi_reservoir_prev[prevIdx];

        // M-클램프 (GI 전용 낮은 값)
        R_prev.M = min(R_prev.M, uint(k_GI_temporalMaxM * float(max(R_cur.M, 1u))));

        float pHat_prev = 0.0f;
        if (R_prev.valid != 0u)
        {
            // Jacobian 보정 (Ouyang 2021, Eq.1): 이전 프레임 primary hit x_q와 현재 x_p 간
            // 입체각 측도 차이를 보정. 없으면 카메라 이동 시 temporal bias 발생.
            float3 prevWPos = prev_gbuf_worldPos[uint2(prevPx)].xyz;
            float  J        = CalcGIJacobian(wPos.xyz, prevWPos,
                                             R_prev.samplePos, R_prev.sampleNormal);
            pHat_prev = EvalGITargetPDF(R_prev.radiance) * J;
        }

        MergeGIReservoir(R_cur, R_prev, pHat_prev, seed);
    }

    if (R_cur.valid != 0u)
        FinalizeGIReservoir(R_cur, EvalGITargetPDF(R_cur.radiance));

    gi_reservoir_cur[pidx] = R_cur;
}
