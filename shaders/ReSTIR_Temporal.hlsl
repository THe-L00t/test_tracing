// ──────────────────────────────────────────────────────────────
//  ReSTIR_Temporal.hlsl  –  [Pass 3] Temporal Reuse
//
//  역할: 현재 프레임 Reservoir와 이전 프레임 Reservoir를 병합.
//        이전 프레임의 유효한 샘플을 재사용하여 분산 감소.
//
//  알고리즘 (Bitterli 2020, Section 4.3):
//    For each pixel p (current frame):
//      R_cur  = reservoir_cur[p]           // Initial RIS 결과
//      pPrev  = Reproject(p)               // 이전 프레임 픽셀 좌표
//
//      if pPrev 유효:
//        R_prev = reservoir_prev[pPrev]
//        R_prev.M = min(R_prev.M, temporalMaxM * R_cur.M)   // M-클램프 (고스팅 방지)
//        pHat_prev = EvalTargetPDF(현재 표면, R_prev.lightIdx)
//        MergeReservoir(R_cur, R_prev, pHat_prev)
//
//      FinalizeReservoir(R_cur, p̂(selected))
//      reservoir_cur[p] = R_cur            // in-place 갱신
//
//  입력:  G-Buffer SRV (t6-t9), reservoir_cur UAV (u6), reservoir_prev UAV (u7)
//  출력:  reservoir_cur UAV (u6) in-place 갱신
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>              gbuf_worldPos      : register(t6);
Texture2D<float4>              gbuf_normal        : register(t7);
Texture2D<float4>              gbuf_albedo        : register(t8);
Texture2D<float4>              gbuf_matInfo       : register(t9);
StructuredBuffer<LightData>    g_lightList        : register(t5);
RWStructuredBuffer<Reservoir>  reservoir_cur      : register(u6);
RWStructuredBuffer<Reservoir>  reservoir_prev     : register(u7);
// 이전 프레임 G-Buffer: surface validation 에 사용 (CopyGBufferToPrev 로 매 프레임 복사)
Texture2D<float4>              prev_gbuf_worldPos : register(t11);
Texture2D<float4>              prev_gbuf_normal   : register(t12);

cbuffer SceneConstants  : register(b0) { float3 camPos; uint sceneID; float4 _sc[15]; }
cbuffer ReSTIRConstants : register(b1)
{
    // 이전 프레임 카메라 (재투영용, ReSTIRCB 레이아웃)
    float4   _prevCam[4];   // [0].xyz=prevPos, [1].xyz=prevRight, [1].w=prevTanHFov
                             // [2].xyz=prevUp,  [2].w=prevAspect, [3].xyz=prevForward
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

// ── 재투영 유효성 임계값 ──────────────────────────────────────
static const float k_depthThreshold  = 0.1f;    // 상대 깊이 차이 허용 범위
static const float k_normalThreshold = 0.906f;  // cos(25°) ≈ 0.906

// 현재 픽셀 px 를 이전 프레임 카메라로 재투영
// 성공 시 true + 이전 픽셀 좌표 out prevPx
// 표면 연속성 검사 (깊이·법선) 포함
bool Reproject(uint2 px, float curDepth, float3 curN,
               out int2 prevPx)
{
    // 현재 픽셀의 월드 위치를 이전 카메라 뷰로 투영
    float3 worldPos  = gbuf_worldPos[px].xyz;
    float3 delta     = worldPos - _prevCam[0].xyz;   // 이전 캠 기준 오프셋

    float prevViewZ  = dot(delta, _prevCam[3].xyz);  // 이전 forward 방향 깊이
    if (prevViewZ <= 0.0f) { prevPx = int2(px); return false; }

    float prevTanHFov = _prevCam[1].w;
    float prevAspect  = _prevCam[2].w;

    // 이전 NDC 좌표
    float prevNDC_x = dot(delta, _prevCam[1].xyz) / (prevViewZ * prevAspect * prevTanHFov);
    float prevNDC_y = dot(delta, _prevCam[2].xyz) / (prevViewZ * prevTanHFov);

    // NDC[-1,1] → UV[0,1] → 픽셀 좌표 (Y 반전)
    float2 prevUV = float2(prevNDC_x, -prevNDC_y) * 0.5f + 0.5f;
    prevPx = int2(prevUV * float2(screenW, screenH));

    if (!IsValidPixel(prevPx, screenW, screenH)) return false;

    // 표면 연속성 검사: 이전 프레임 G-Buffer에서 prevPx 위치의 깊이·법선 읽기
    // (Bitterli 2020 Sec.4.3: 이전 프레임 G-Buffer 사용이 논문 정석)
    float3 prevWPos  = prev_gbuf_worldPos[uint2(prevPx)].xyz;
    float3 prevN     = prev_gbuf_normal[uint2(prevPx)].xyz;
    // 깊이 비교: 이전 프레임 월드 위치와 이전 카메라 사이의 유클리드 거리
    // (curDepth = RayTCurrent() = 유클리드 거리이므로 단위 통일)
    float  prevDepth = length(prevWPos - _prevCam[0].xyz);

    bool depthOk  = abs(prevDepth - curDepth) / max(curDepth, 1e-4f) < k_depthThreshold;
    bool normalOk = dot(prevN, curN) > k_normalThreshold;
    return depthOk && normalOk;
}

[numthreads(8, 8, 1)]
void CS_Temporal(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint   pidx   = PixelIndex(px, screenW);
    float4 wPos   = gbuf_worldPos[px];
    float4 matInf = gbuf_matInfo[px];

    // 배경·유리·발광 픽셀: 변경 없이 유지
    if (wPos.w < 0.0f || matInf.b > 0.5f) return;

    float3 hitPos = wPos.xyz;
    float3 N      = gbuf_normal[px].xyz;
    float  depth  = matInf.g;

    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u + 7919u) | 1u;

    Reservoir R_cur = reservoir_cur[pidx];

    // ── frameIndex == 0: 시간적 재사용 스킵 ──────────────────────
    // ReSTIR 활성화 직후 또는 카메라 이동 직후에 해당.
    // reservoir_prev에 이전 세션의 구 데이터가 잔존할 수 있으므로
    // Merge 없이 Initial RIS 결과만 확정하고 반환.
    // (흰 픽셀 스파이크 → 검은 픽셀 연속 현상의 근본 원인 차단)
    if (frameIndex == 0u)
    {
        if (R_cur.lightIdx != 0xFFFFFFFFu)
            FinalizeReservoir(R_cur, EvalTargetPDF(hitPos, N, g_lightList[R_cur.lightIdx]));
        reservoir_cur[pidx] = R_cur;
        return;
    }

    // ── 이전 프레임 재투영 + 병합 ───────────────────────────────
    int2 prevPx;
    if (Reproject(px, depth, N, prevPx))
    {
        uint      prevIdx = PixelIndex(uint2(prevPx), screenW);
        Reservoir R_prev  = reservoir_prev[prevIdx];

        // M-클램프: 이전 프레임 가중치가 현재를 압도하지 않도록
        // (Bitterli 2020, Section 4.3: temporalMaxM = 20 권장)
        R_prev.M = min(R_prev.M, uint(temporalMaxM * float(max(R_cur.M, 1u))));

        // 이전 선택 광원을 현재 픽셀 표면에서 재평가 (Eq.6 p̂_p(y_b))
        float pHat_prev = 0.0f;
        if (R_prev.lightIdx != 0xFFFFFFFFu)
            pHat_prev = EvalTargetPDF(hitPos, N, g_lightList[R_prev.lightIdx]);

        MergeReservoir(R_cur, R_prev, pHat_prev, seed);
    }

    // 최종 비편향 가중치 확정
    if (R_cur.lightIdx != 0xFFFFFFFFu)
        FinalizeReservoir(R_cur, EvalTargetPDF(hitPos, N, g_lightList[R_cur.lightIdx]));

    reservoir_cur[pidx] = R_cur;
}
