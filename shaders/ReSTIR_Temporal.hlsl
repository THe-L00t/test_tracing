// ──────────────────────────────────────────────────────────────
//  ReSTIR_Temporal.hlsl  –  [Pass 3] Temporal Reuse
//
//  역할: 현재 프레임 Reservoir와 이전 프레임 Reservoir를 병합
//        이전 프레임의 유효한 샘플을 재사용하여 분산 감소
//
//  알고리즘 (Bitterli 2020, Section 4.3):
//    For each pixel p (current frame):
//      R_cur  = reservoir_cur[p]
//      pPrev  = Reproject(p, motionVec[p])     // 이전 프레임 픽셀 좌표
//      R_prev = reservoir_prev[pPrev]           // 이전 G-Buffer로 유효성 검사
//
//      // Ghosting 방지: M 클램프
//      R_prev.M = min(R_prev.M, temporalMaxM * R_cur.M)
//
//      // 병합: R_prev의 선택 광원을 현재 픽셀에서 평가
//      pHat_prev = EvalTargetPDF(현재 표면, R_prev.lightIdx)
//      MergeReservoir(R_cur, R_prev, pHat_prev)
//      FinalizeReservoir(R_cur, p_hat(R_cur.lightIdx))
//
//      reservoir_cur[p] = R_cur  // 덮어쓰기 (in-place merge)
//
//  입력:  reservoir_cur (u6), reservoir_prev (u7), motionVec (u8), G-Buffer
//  출력:  reservoir_cur (u6) 갱신
//
//  스레드 그룹: 8×8
//
//  TODO [Session 2]:
//   - 재투영 유효성 검사 (깊이/법선 차이 임계값 비교)
//   - M 클램프 적용
//   - MergeReservoir 호출
//   - motionVec 생성 (GBuffer 패스에서 이전 카메라로 재투영)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>              gbuf_worldPos  : register(t6);
Texture2D<float4>              gbuf_normal    : register(t7);
Texture2D<float4>              gbuf_albedo    : register(t8);
Texture2D<float4>              gbuf_matInfo   : register(t9);
Texture2D<float2>              motionVec      : register(t10);  // 화면공간 오프셋 (픽셀 단위)
StructuredBuffer<LightData>    g_lightList    : register(t5);

RWStructuredBuffer<Reservoir>  reservoir_cur  : register(u6);
RWStructuredBuffer<Reservoir>  reservoir_prev : register(u7);

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

// ── 재투영 유효성 검사 임계값 ─────────────────────────────────
static const float k_depthThreshold  = 0.1f;   // 상대 깊이 차이
static const float k_normalThreshold = 0.906f;  // cos(25도) ≈ 0.906

// 이전 프레임 픽셀 재투영 + 표면 연속성 검사
// 반환: 유효하면 true, 이전 픽셀 좌표 out
bool Reproject(uint2 px, float curDepth, float3 curN,
               out int2 prevPx)
{
    // TODO [Session 2]:
    // 1. motionVec[px] 읽기
    // 2. prevPx = px + round(motionVec)
    // 3. 유효 범위 검사 (IsValidPixel)
    // 4. 이전 G-Buffer 읽어 깊이/법선 비교
    //    |prevDepth - curDepth| / curDepth < k_depthThreshold
    //    dot(prevN, curN) > k_normalThreshold
    // 5. 유효성 반환
    prevPx = int2(px);
    return false;  // STUB
}

// ── 진입점 ───────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CS_Temporal(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint  pidx   = PixelIndex(px, screenW);
    float4 wPos  = gbuf_worldPos[px];
    float4 matInf = gbuf_matInfo[px];

    // 배경/투명 픽셀: 그대로 유지
    if (wPos.w < 0.0f || matInf.b < 0.0f) return;

    Reservoir R_cur = reservoir_cur[pidx];

    // G-Buffer 데이터
    float3 hitPos   = wPos.xyz;
    float3 N        = gbuf_normal[px].xyz;
    float3 albedo   = gbuf_albedo[px].rgb;
    float  metallic = gbuf_albedo[px].a;
    float  roughness= matInf.r;
    float  depth    = matInf.g;
    float3 V        = float3(0,1,0);  // STUB – SceneCB.camPos - hitPos 정규화

    // RNG
    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u + 7919u) | 1u;

    // ── 이전 프레임 재투영 ───────────────────────────────────
    int2 prevPx;
    if (Reproject(px, depth, N, prevPx))
    {
        uint  prevIdx  = PixelIndex(uint2(prevPx), screenW);
        Reservoir R_prev = reservoir_prev[prevIdx];

        // TODO [Session 2]: Ghosting 방지 – M 클램프
        R_prev.M = min(R_prev.M, uint(temporalMaxM * float(R_cur.M)));

        // TODO [Session 2]: 이전 프레임 선택 광원의 p_hat @ 현재 픽셀 계산
        float pHat_prev = 0.0f;
        if (R_prev.lightIdx != 0xFFFFFFFFu)
        {
            LightData prevLight = g_lightList[R_prev.lightIdx];
            pHat_prev = EvalTargetPDF(hitPos, N, V, albedo, metallic, roughness, prevLight);
        }

        // 병합
        MergeReservoir(R_cur, R_prev, pHat_prev, seed);
    }

    // 최종 비편향 가중치 확정
    if (R_cur.lightIdx != 0xFFFFFFFFu)
    {
        LightData sel = g_lightList[R_cur.lightIdx];
        float pHat = EvalTargetPDF(hitPos, N, V, albedo, metallic, roughness, sel);
        FinalizeReservoir(R_cur, pHat);
    }

    reservoir_cur[pidx] = R_cur;
}
