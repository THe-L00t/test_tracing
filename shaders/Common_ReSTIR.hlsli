// ──────────────────────────────────────────────────────────────
//  Common_ReSTIR.hlsli  –  ReSTIR 셰이더 공용 헤더
//
//  모든 ReSTIR 셰이더에서 #include "Common_ReSTIR.hlsli"로 포함.
//  - 공유 리소스 바인딩 레지스터 매핑
//  - Reservoir / LightData 구조체
//  - RIS 업데이트, 병합, 타겟 함수 평가 헬퍼
// ──────────────────────────────────────────────────────────────

// ── 디스크립터 힙 레지스터 레이아웃 ─────────────────────────────
//  UAV (u0..u8):
//   u0 : g_output          RGBA8    - 최종 SDR 출력
//   u1 : g_accumulation    RGBA32F  - HDR 누적 (denoiser용)
//   u2 : gbuf_worldPos     RGBA32F  - 월드 위치 XYZ + 히트 거리 W
//   u3 : gbuf_normal       RGBA32F  - 월드 법선 XYZ + matIdx(float) W
//   u4 : gbuf_albedo       RGBA8    - Albedo RGB + metallic A
//   u5 : gbuf_matInfo      RGBA32F  - roughness R, depth G, pad B, pad A
//   u6 : reservoir_cur     RWStructuredBuffer<Reservoir>  현재 프레임
//   u7 : reservoir_prev    RWStructuredBuffer<Reservoir>  이전 프레임
//   u8 : motionVec         RG32F    - 화면공간 모션 벡터 (재투영용)
//
//  SRV (t0..t5):
//   t0 : g_tlas            RTAS
//   t1 : g_vbPlane
//   t2 : g_vbCube
//   t3 : g_vbRoom
//   t4 : g_vbSphere
//   t5 : g_lightList       StructuredBuffer<LightData>
//
//  CBV:
//   b0 : SceneConstants    (기존 SceneCB, 256B)
//   b1 : ReSTIRConstants   (ReSTIRCB, 128B)

// ── Reservoir ────────────────────────────────────────────────
struct Reservoir
{
    uint  lightIdx;  // 선택된 광원 인덱스 (0xFFFFFFFF = 무효)
    float wSum;      // Σ w_i
    float W;         // 비편향 가중치 = wSum / (M * p_hat(y))
    uint  M;         // 후보 시도 횟수
};

// Reservoir 초기화
Reservoir MakeEmptyReservoir()
{
    Reservoir r;
    r.lightIdx = 0xFFFFFFFFu;
    r.wSum     = 0.0f;
    r.W        = 0.0f;
    r.M        = 0u;
    return r;
}

// RIS 단일 후보 갱신 (Talbot 2005)
// w_i = p_hat(x_i) / source_pdf(x_i)
// 확률 w_i / (r.wSum + w_i) 로 r.lightIdx를 x_i로 교체
void UpdateReservoir(inout Reservoir r, uint candidateLight,
                     float w, inout uint seed)
{
    r.wSum += w;
    r.M    += 1u;
    // 균일 난수 < w/wSum 이면 교체
    float u = float(WangHash(seed) & 0x00FFFFFFu) / float(0x01000000u);
    seed = WangHash(seed);
    if (u < w / max(r.wSum, 1e-8f))
        r.lightIdx = candidateLight;
}

// 두 Reservoir 병합 (Bitterli 2020 Eq.6)
// R_b를 R_a 에 합산, pHat_b: R_b.lightIdx의 타겟 함수값 @ 현재 픽셀
void MergeReservoir(inout Reservoir r_a, Reservoir r_b,
                    float pHat_b, inout uint seed)
{
    uint  M_sum = r_a.M + r_b.M;
    float w_b   = pHat_b * r_b.W * float(r_b.M);
    UpdateReservoir(r_a, r_b.lightIdx, w_b, seed);
    r_a.M = M_sum;
}

// 최종 W 계산 (비편향 가중치 확정)
// W = wSum / (M * p_hat(selected))
void FinalizeReservoir(inout Reservoir r, float pHat_selected)
{
    r.W = (pHat_selected > 0.0f)
        ? r.wSum / (float(r.M) * pHat_selected)
        : 0.0f;
}

// ── LightData ────────────────────────────────────────────────
struct LightData
{
    float3 pos;        // point: 위치, directional: 방향(정규화)
    float  intensity;
    float3 color;
    uint   type;       // 0=point, 1=directional, 2=area(box)
    float  halfSize;   // area box half-extent
    float3 center;     // area box center
};

// ── 타겟 함수 p_hat 평가 ────────────────────────────────────
// ReSTIR에서 샘플 품질을 나타내는 스칼라 (unshadowed radiance)
// 가시성 검사는 포함하지 않음 (Final Shade 단계에서 처리)
//
// ▶ 논문 근거 (Bitterli et al. 2020, "Spatiotemporal reservoir resampling
//              for real-time ray tracing with dynamic direct lighting"):
//   Eq.(1): p̂(y) ∝ ρ(x,y) = f(x,ω_x→y) · G(x↔y) · L_e(y→x)
//   여기서 가시성 V(x,y)는 제외 (Biased estimator accepted for performance)
//
// ▶ 실용 공식 (Bitterli 2020 Supplemental + Talbot 2005):
//   p̂(y) = luma( f_r(V,L,N) · L_i(y) · NdotL )
//
//   단계별 계산:
//   1) 광원 방향 L, 거리 dist 계산:
//        point:  L = normalize(light.pos - hitPos)
//                Li = light.color * light.intensity / (dist * dist)
//        dir:    L = normalize(light.pos)   // 이미 정규화된 방향 저장
//                Li = light.color * light.intensity
//   2) NdotL = max(dot(N, L), 0)
//   3) f_r(V,L,N) = GGX Cook-Torrance:
//        H  = normalize(V + L)
//        D  = D_GGX(NdotH, alpha2)           // alpha = roughness^2
//        G  = G1_Smith(NdotV, alpha2) * G1_Smith(NdotL, alpha2)
//        F  = SchlickF(VdotH, F0)            // F0 = lerp(0.04, albedo, metallic)
//        f_spec = D * G * F / (4 * NdotV * NdotL)
//        f_diff = albedo * INV_PI * (1 - metallic) * (1 - F)
//        f_r = f_spec + f_diff
//   4) p̂ = luma(f_r * Li * NdotL)
//        luma = 0.2126*R + 0.7152*G + 0.0722*B  (BT.709 권장)
//        또는 dot(val, float3(0.2126, 0.7152, 0.0722))
//
//   ※ 구현 참조: feature/pbr-path-tracing 브랜치 Raytracing.hlsl
//     D_GGX / G1_Smith / SchlickF / EvalBRDFPdf 함수와 동일 수식
//     해당 함수를 Common_ReSTIR.hlsli에 복사하거나, 별도 BRDFUtil.hlsli 분리
//
// TODO [Session 1]: 아래 스텁을 실제 PBR BRDF 평가로 교체
float EvalTargetPDF(float3 hitPos, float3 N, float3 V,
                    float3 albedo, float metallic, float roughness,
                    LightData light)
{
    // TODO [Session 1]: 아래 스텁을 실제 PBR BRDF 평가로 교체
    // 참조: Common_ReSTIR.hlsli 의 LightData.type 분기 필요
    return 0.0f;
}

// ── Jacobian 보정 (공간 재사용용) ────────────────────────────
// 이웃 픽셀 q 에서 가져온 광원 샘플을 현재 픽셀 p 에 재사용할 때
// 입체각 측도 변환에 필요한 Jacobian 행렬식
//
// ▶ 논문 근거 (Bitterli et al. 2020, Eq.(11)):
//   공간 재사용 시 q의 domain에서 샘플링된 광원 y를
//   p의 domain으로 변환할 때의 입체각 비율:
//
//   J(q→p) = |cos θ_q| · dist(x_p, y)²
//             ─────────────────────────────
//             |cos θ_p| · dist(x_q, y)²
//
//   기호 정의:
//     y        = 선택된 광원 샘플 위치  (lightPos)
//     x_p      = 현재 픽셀 p의 표면 히트 포인트  (hitPos_p)
//     x_q      = 이웃 픽셀 q의 표면 히트 포인트  (hitPos_q)
//     lightNormal = 광원 표면 법선 (area light용; point light는 임의)
//     θ_q      = y에서 (y→x_q) 방향과 lightNormal 사이 각도
//     θ_p      = y에서 (y→x_p) 방향과 lightNormal 사이 각도
//     dist(a,b) = ||a - b||₂
//
//   계산 단계:
//     float3 dir_q = normalize(hitPos_q - lightPos);
//     float3 dir_p = normalize(hitPos_p - lightPos);
//     float  cosQ  = abs(dot(lightNormal, dir_q));
//     float  cosP  = abs(dot(lightNormal, dir_p));
//     float  distP = length(hitPos_p - lightPos);
//     float  distQ = length(hitPos_q - lightPos);
//     J = (cosQ * distP * distP) / max(cosP * distQ * distQ, 1e-8f);
//     J = clamp(J, 0.0f, 1e4f);  // 극단값 방지
//
//   ▶ Point light 단순화 (법선 정보 없음 → cosQ = cosP 가정):
//     J_point(q→p) = dist(x_p, y)² / dist(x_q, y)²
//
//   ▶ 사용 위치: ReSTIR_Spatial.hlsl CS_Spatial 내
//     pHat_q_at_p = EvalTargetPDF(x_p, R_q.lightIdx) * CalcJacobian(...)
//     (Bitterli 2020, Algorithm 4, line 7)
//
// TODO [Session 2]: 아래를 실제 Jacobian으로 교체
float CalcJacobian(float3 hitPos_p, float3 hitPos_q,
                   float3 lightPos, float3 lightNormal)
{
    // TODO [Session 2]
    return 1.0f;
}

// ── 화면 좌표 → 버퍼 인덱스 ─────────────────────────────────
uint PixelIndex(uint2 px, uint width)
{
    return px.y * width + px.x;
}

// ── 픽셀 유효성 검사 ─────────────────────────────────────────
bool IsValidPixel(int2 px, uint w, uint h)
{
    return px.x >= 0 && px.y >= 0
        && uint(px.x) < w && uint(px.y) < h;
}
