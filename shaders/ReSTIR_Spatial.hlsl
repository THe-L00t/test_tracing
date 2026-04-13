// ──────────────────────────────────────────────────────────────
//  ReSTIR_Spatial.hlsl  –  [Pass 4] Spatial Reuse
//
//  역할: 주변 픽셀의 Reservoir를 현재 픽셀에서 재사용하여 분산 추가 감소
//        Jacobian 보정으로 픽셀 간 입체각 측도 차이 교정
//
//  알고리즘 (Bitterli 2020, Algorithm 4):
//    For each pixel p:
//      R_out = R_center (현재 픽셀 Reservoir)
//
//      For k = 1..spatialSamples:
//        q = RandomNeighbor(p, spatialRadius)   // 이웃 픽셀 랜덤 선택
//        검증: q의 표면이 p와 충분히 유사한지 (깊이/법선 임계값)
//
//        R_q = reservoir_cur[q]
//
//        // p에서 q의 선택 광원을 평가 (Jacobian 포함)
//        J      = CalcJacobian(p, q, R_q.lightIdx)
//        pHat_q = EvalTargetPDF(p.surface, R_q.lightIdx) * J
//
//        MergeReservoir(R_out, R_q, pHat_q)
//
//      FinalizeReservoir(R_out, p_hat(R_out.lightIdx))
//      reservoir_cur[p] = R_out  (2회 반복: 첫 pass → 임시 버퍼, 두번째 pass → cur)
//
//  입력:  reservoir_cur (u6), G-Buffer, LightList
//  출력:  reservoir_cur (u6) 갱신
//
//  주의: Read-Write 충돌 방지를 위해 홀수/짝수 패스가 버퍼를 swap
//        App.cpp 에서 2회 dispatch 시 srv/uav 역할 교환 필요
//
//  스레드 그룹: 8×8
//
//  TODO [Session 2]:
//   - CalcJacobian 구현 (Common_ReSTIR.hlsli)
//   - 이웃 표면 유사도 검증
//   - MergeReservoir 호출 및 최종화
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
Texture2D<float4>              gbuf_worldPos  : register(t6);
Texture2D<float4>              gbuf_normal    : register(t7);
Texture2D<float4>              gbuf_albedo    : register(t8);
Texture2D<float4>              gbuf_matInfo   : register(t9);
StructuredBuffer<LightData>    g_lightList    : register(t5);

// 입력 (이전 패스 결과 – 읽기 전용 SRV)
StructuredBuffer<Reservoir>    reservoir_in   : register(t11);
// 출력 (현재 패스 결과)
RWStructuredBuffer<Reservoir>  reservoir_out  : register(u6);

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

// ── 이웃 표면 유사도 임계값 ──────────────────────────────────
static const float k_depthThreshold  = 0.1f;
static const float k_normalThreshold = 0.906f;

bool IsSimilarSurface(float3 N_p, float depth_p,
                      float3 N_q, float depth_q)
{
    bool depthOk  = abs(depth_p - depth_q) / max(depth_p, 1e-4f) < k_depthThreshold;
    bool normalOk = dot(N_p, N_q) > k_normalThreshold;
    return depthOk && normalOk;
}

// ── 진입점 ───────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CS_Spatial(uint3 tid : SV_DispatchThreadID)
{
    uint2 px = tid.xy;
    if (px.x >= screenW || px.y >= screenH) return;

    uint  pidx  = PixelIndex(px, screenW);
    float4 wPos = gbuf_worldPos[px];
    float4 matInf = gbuf_matInfo[px];

    if (wPos.w < 0.0f || matInf.b < 0.0f)
    {
        reservoir_out[pidx] = MakeEmptyReservoir();
        return;
    }

    float3 hitPos_p  = wPos.xyz;
    float3 N_p       = gbuf_normal[px].xyz;
    float3 albedo_p  = gbuf_albedo[px].rgb;
    float  metallic_p= gbuf_albedo[px].a;
    float  roughness_p = matInf.r;
    float  depth_p   = matInf.g;
    float3 V_p       = normalize(camPos - hitPos_p);

    uint seed = (px.x * 1973u + px.y * 9277u + frameIndex * 26699u + 3571u) | 1u;

    Reservoir R_out = reservoir_in[pidx];  // 현재 픽셀 기저 Reservoir

    // ── 공간 이웃 병합 ────────────────────────────────────────
    for (uint k = 0u; k < spatialSamples; k++)
    {
        // 반경 내 랜덤 이웃 픽셀 선택
        float angle = float(WangHash(seed) & 0x00FFFFFFu) / float(0x01000000u) * 6.28318f;
        float rad   = sqrt(float(WangHash(seed) & 0x00FFFFFFu) / float(0x01000000u)) * float(spatialRadius);
        seed = WangHash(seed);

        int2 offset = int2(int(rad * cos(angle)), int(rad * sin(angle)));
        int2 qPx    = int2(px) + offset;

        if (!IsValidPixel(qPx, screenW, screenH)) continue;

        uint2 qPxU   = uint2(qPx);
        float4 wPos_q = gbuf_worldPos[qPxU];
        float4 matInf_q = gbuf_matInfo[qPxU];

        if (wPos_q.w < 0.0f || matInf_q.b < 0.0f) continue;

        float3 N_q    = gbuf_normal[qPxU].xyz;
        float  depth_q= matInf_q.g;

        // 표면 유사도 검증
        if (!IsSimilarSurface(N_p, depth_p, N_q, depth_q)) continue;

        uint qIdx = PixelIndex(qPxU, screenW);
        Reservoir R_q = reservoir_in[qIdx];

        if (R_q.lightIdx == 0xFFFFFFFFu) continue;

        // ▶ Jacobian 보정 병합 (Bitterli 2020, Algorithm 4, line 7 + Eq.(11)):
        //   이웃 q 에서 선택된 광원 y 를 픽셀 p 에서 재평가할 때,
        //   q의 입체각 측도를 p의 측도로 변환:
        //
        //   J(q→p) = |cos θ_q| · dist(x_p, y)²
        //             ─────────────────────────────
        //             |cos θ_p| · dist(x_q, y)²
        //
        //   실제 기여 가중치:
        //     pHat_q_at_p = EvalTargetPDF(x_p 표면, y) * J(q→p)
        //
        //   MergeReservoir 호출 시 전달:
        //     MergeReservoir(R_out, R_q, pHat_q_at_p, seed)
        //
        //   ※ Point light: CalcJacobian에서 cosQ=cosP 취소되어
        //       J = dist(x_p,y)² / dist(x_q,y)²  로 단순화 (Common_ReSTIR.hlsli 참조)
        //   ※ CalcJacobian은 현재 STUB(return 1.0) → Session 2에서 구현

        LightData light_q = g_lightList[R_q.lightIdx];
        // ▶ Jacobian lightNormal:
        //   포인트 라이트(type==0): float3(0,0,0) → CalcJacobian이 point light 분기 진입
        //     (lightNormal=lightPos를 넘기면 lenN>>1e-6 → 잘못된 area light 경로 → 극단적 J)
        //   면 광원(type==2): light center를 방향 벡터로 사용 (근사)
        float3 lightNormal = float3(0.0f, 0.0f, 0.0f);  // point light: cosine 항 상쇄
        float Jacobian    = CalcJacobian(hitPos_p, wPos_q.xyz, light_q.pos, lightNormal);
        float pHat_q      = EvalTargetPDF(hitPos_p, N_p, V_p, albedo_p, metallic_p, roughness_p, light_q);
        pHat_q *= Jacobian;

        MergeReservoir(R_out, R_q, pHat_q, seed);
    }

    // 최종화
    if (R_out.lightIdx != 0xFFFFFFFFu)
    {
        LightData sel = g_lightList[R_out.lightIdx];
        float pHat = EvalTargetPDF(hitPos_p, N_p, V_p, albedo_p, metallic_p, roughness_p, sel);
        FinalizeReservoir(R_out, pHat);
    }

    reservoir_out[pidx] = R_out;
}

// WangHash는 Common_ReSTIR.hlsli에 정의됨
