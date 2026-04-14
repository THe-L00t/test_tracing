// ──────────────────────────────────────────────────────────────
//  ReSTIR_Initial.hlsl  –  [Pass 2] Initial Candidate Generation
//
//  역할: 픽셀당 M개의 광원 후보를 RIS로 샘플링하여 Reservoir에 저장
//        가시성 검사 없음 (비편향 target PDF 사용)
//
//  알고리즘 (Bitterli 2020, Algorithm 2):
//    For each pixel p:
//      R = empty Reservoir
//      For i = 1..M:
//        j  ~ UniformRandom(lightList)           // 후보 광원 선택
//        w  = p_hat(j) / (1/lightCount)           // 가중치
//        UpdateReservoir(R, j, w)
//      R.W = R.wSum / (R.M * p_hat(R.lightIdx))  // 비편향 가중치 확정
//      Write R → reservoir_cur[p]
//
//  입력:  G-Buffer (u2-u5), LightList (t5), ReSTIRCB (b1)
//  출력:  reservoir_cur (u6)
//
//  스레드 그룹: 8×8
//
//  TODO [Session 1]:
//   - EvalTargetPDF 구현 (Common_ReSTIR.hlsli)
//   - 메인 RIS 루프 작성
//   - 결과 Reservoir 기록
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>              gbuf_worldPos  : register(t6);  // SRV로 읽기
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

// ── 진입점 ───────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CS_Initial(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint  pidx    = PixelIndex(px, screenW);
    float4 wPos   = gbuf_worldPos[px];
    float4 matInf = gbuf_matInfo[px];

    // 배경 또는 유리/반투명 픽셀: 빈 Reservoir 기록 후 종료
    if (wPos.w < 0.0f || matInf.b < 0.0f)
    {
        reservoir_cur[pidx] = MakeEmptyReservoir();
        return;
    }

    // G-Buffer 읽기
    float3 hitPos    = wPos.xyz;
    float4 normData  = gbuf_normal[px];
    float3 N         = normData.xyz;
    float4 albData   = gbuf_albedo[px];
    float3 albedo    = albData.rgb;
    float  metallic  = albData.a;
    float  roughness = matInf.r;

    float3 V = normalize(camPos - hitPos);

    // RNG 초기화
    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u) | 1u;

    Reservoir R = MakeEmptyReservoir();

    // ── RIS 메인 루프 ────────────────────────────────────────
    // ▶ 논문 근거 (Talbot et al. 2005, "Importance Resampling for Global Illumination",
    //             Algorithm 2, "Weighted Reservoir Sampling"):
    //   목표: target PDF p̂(x) 에 비례하여 M개 후보 중 하나를 선택
    //   source PDF: q(x) = 1/lightCount  (균일 분포)
    //
    //   가중치 공식 (Talbot 2005, Eq.(3)):
    //     w_i = p̂(x_i) / q(x_i) = p̂(x_i) · lightCount
    //
    //   WRS 업데이트 (Vitter 1985, "Random Sampling with a Reservoir"):
    //     R.wSum += w_i
    //     if (Uniform[0,1) < w_i / R.wSum): R.lightIdx = i
    //
    //   최종 비편향 가중치 (Talbot 2005, Algorithm 2, line 8):
    //     R.W = (1/p̂(R.lightIdx)) · (R.wSum / R.M)
    //         = R.wSum / (R.M · p̂(y_selected))
    //   이는 FinalizeReservoir()가 담당 (아래 참조)
    //
    // TODO [Session 1]: M개 후보 광원을 균일 샘플링하고 p_hat 가중치로 UpdateReservoir
    for (uint i = 0u; i < candidateCount; i++)
    {
        // 후보 광원 균일 선택
        uint jIdx = uint(float(WangHash(seed) & 0x00FFFFFFu) / float(0x01000000u) * float(lightCount));
        seed = WangHash(seed);
        jIdx = min(jIdx, lightCount - 1u);

        LightData light = g_lightList[jIdx];

        // p_hat 계산 (가시성 제외)
        float pHat = EvalTargetPDF(hitPos, N, V, albedo, metallic, roughness, light);

        // source PDF = 1/lightCount (균일)
        float w = pHat * float(lightCount);

        UpdateReservoir(R, jIdx, w, seed);
    }

    // 비편향 가중치 확정
    if (R.lightIdx != 0xFFFFFFFFu)
    {
        LightData selected = g_lightList[R.lightIdx];
        float pHat = EvalTargetPDF(hitPos, N, V, albedo, metallic, roughness, selected);
        FinalizeReservoir(R, pHat);
    }

    reservoir_cur[pidx] = R;
}

// WangHash는 Common_ReSTIR.hlsli에 정의됨
