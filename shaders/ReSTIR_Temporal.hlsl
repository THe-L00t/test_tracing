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
    // ▶ 재투영 수식 (Bitterli 2020, Section 4.3 + 표준 역투영 공식):
    //
    //   목표: 현재 프레임 픽셀 px 의 월드 위치를 이전 카메라로 투영하여
    //         이전 프레임에서 같은 표면 샘플을 찾는다.
    //
    //   ReSTIRCB._prevCam 구조 (b1, Common.h ReSTIRCB):
    //     _prevCam[0].xyz = prevCamPos      (이전 카메라 위치)
    //     _prevCam[1].xyz = prevCamRight    [1].w = prevTanHalfFovY
    //     _prevCam[2].xyz = prevCamUp       [2].w = prevAspectRatio
    //     _prevCam[3].xyz = prevCamForward
    //
    //   계산 단계:
    //     float3 worldPos = gbuf_worldPos[px].xyz;
    //     float3 delta    = worldPos - _prevCam[0].xyz;        // 이전 캠 기준 방향
    //     float  prevViewZ = dot(delta, _prevCam[3].xyz);      // 이전 view space Z
    //     if (prevViewZ <= 0) return false;                     // 카메라 뒤
    //
    //     float  prevTanHFov    = _prevCam[1].w;
    //     float  prevAspect     = _prevCam[2].w;
    //     float  prevNDC_x = dot(delta, _prevCam[1].xyz)
    //                         / (prevViewZ * prevAspect * prevTanHFov);
    //     float  prevNDC_y = dot(delta, _prevCam[2].xyz)
    //                         / (prevViewZ * prevTanHFov);
    //
    //     // NDC[-1,1] → UV[0,1] → 픽셀 좌표
    //     float2 prevUV  = float2(prevNDC_x, -prevNDC_y) * 0.5f + 0.5f;
    //     prevPx = int2(prevUV * float2(screenW, screenH));
    //
    //     // 화면 범위 + 표면 연속성 검사
    //     if (!IsValidPixel(prevPx, screenW, screenH)) return false;
    //     float prevDepth = gbuf_matInfo[uint2(prevPx)].g;
    //     float3 prevN    = gbuf_normal[uint2(prevPx)].xyz;
    //     bool depthOk  = abs(prevDepth - curDepth) / max(curDepth, 1e-4f) < k_depthThreshold;
    //     bool normalOk = dot(prevN, curN) > k_normalThreshold;
    //     return depthOk && normalOk;
    //
    //   ※ motionVec (u8/t10) 는 GBuffer 패스에서 계산 가능 (선택적):
    //     motionVec[px] = prevUV - curUV  (화면공간 오프셋)
    //     여기서 직접 계산하는 방식이 더 단순함 (motionVec 없어도 동작)

    // TODO [Session 2]:
    // 1. motionVec[px] 읽기 (또는 위 수식으로 직접 재투영)
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

        // ▶ M-클램프 (Bitterli 2020, Section 4.3 "Preventing Temporal Lag"):
        //   R_prev.M 를 현재 R_cur.M 의 temporalMaxM 배 이하로 제한.
        //   이유: M이 너무 크면 이전 프레임 가중치가 압도적으로 커져
        //         씬 변화/카메라 이동 시 고스팅(temporal lag) 발생.
        //   권장값: temporalMaxM = 20.0  (논문 Section 4.3 실험값)
        //   공식:   R_prev.M = min(R_prev.M, temporalMaxM × R_cur.M)
        R_prev.M = min(R_prev.M, uint(temporalMaxM * float(R_cur.M)));

        // ▶ 병합 가중치 (Bitterli 2020, Eq.(6)):
        //   두 reservoir를 합칠 때 R_b의 기여 가중치:
        //     w_b = p̂_p(y_b) · R_b.W · R_b.M
        //   여기서 p̂_p = 현재 픽셀 p 의 표면에서 y_b(이전 선택 광원)를 평가
        //   (MergeReservoir 내부에서 UpdateReservoir 호출 시 w_b 계산)
        //
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
