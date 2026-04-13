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

// ── 수학 상수 ────────────────────────────────────────────────
static const float PI      = 3.14159265358979323846f;
static const float INV_PI  = 0.31830988618379067154f;
static const float TWO_PI  = 6.28318530717958647692f;

// ── WangHash RNG ─────────────────────────────────────────────
// 빠른 비상관 해시 기반 의사난수 생성
uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s ^= s >> 4u;
    s *= 0x27d4eb2du;
    s ^= s >> 15u;
    return s;
}

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

// ── GGX BRDF 헬퍼 (Raytracing.hlsl 와 동일 수식) ────────────
// NdotH: N·H,  alpha2: (roughness^2)^2  (Disney convention)
float D_GGX(float NdotH, float alpha2)
{
    float b = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / (PI * b * b);
}

// Smith G1 마스킹 함수
float G1_Smith(float NdotX, float alpha2)
{
    return 2.0f * NdotX / (NdotX + sqrt(alpha2 + (1.0f - alpha2) * NdotX * NdotX));
}

// Schlick Fresnel 근사
float3 SchlickF(float cosTheta, float3 F0)
{
    float t  = 1.0f - saturate(cosTheta);
    float t2 = t * t;
    return F0 + (1.0f - F0) * (t2 * t2 * t);
}

// ── 타겟 함수 p_hat 평가 ────────────────────────────────────
// ReSTIR에서 샘플 품질을 나타내는 스칼라 (unshadowed radiance)
// 가시성 검사는 포함하지 않음 (Final Shade 단계에서 처리)
//
// ▶ 논문 근거 (Bitterli et al. 2020, Eq.(1)):
//   p̂(y) ∝ L_e(y→x) · G(x↔y)  (기하항만, BRDF 제외)
//
// ▶ 실용 공식 (BRDF-free, 메탈릭/스펙큘러 표면에서도 항상 양수):
//   p̂(y) = luma( Li · NdotL )
//   luma  = dot(val, float3(0.2126, 0.7152, 0.0722))  (BT.709)
//
// ▶ BRDF를 p̂에 포함하면 안 되는 이유:
//   GGX 스펙큘러 로브가 매우 좁은 스무스 메탈릭 표면(roughness≈0.1)에서
//   무작위 샘플된 거의 모든 광원의 p̂≈0이 돼 wSum=0 → reservoir 비어있음 → 검은색
//   Li·NdotL 만 사용하면 반구 상의 모든 광원이 양수 가중치를 가짐 (Talbot 2005 unbiased)
float EvalTargetPDF(float3 hitPos, float3 N, float3 V,
                    float3 albedo, float metallic, float roughness,
                    LightData light)
{
    // 광원 방향 L, 조도 Li 계산
    float3 L;
    float3 Li;
    if (light.type == 1u)
    {
        // 방향광: pos 필드가 이미 정규화된 방향
        L  = normalize(light.pos);
        Li = light.color * light.intensity;
    }
    else
    {
        // 포인트광 / area(근사)
        float3 toL = light.pos - hitPos;
        float  dist = max(length(toL), 0.01f);
        L  = toL / dist;
        Li = light.color * light.intensity / (dist * dist);
    }

    float NdotL = max(dot(N, L), 0.0f);
    if (NdotL <= 0.0f) return 0.0f;

    // p̂ = luma(Li * NdotL): BRDF 없이 기하항만 (Bitterli 2020 Section 5.1 관례)
    float3 contrib = Li * NdotL;
    return max(dot(contrib, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

// ── Jacobian 보정 (공간 재사용용) ────────────────────────────
// Bitterli 2020 Eq.(11): 이웃 픽셀 q의 광원 샘플을 픽셀 p에서 재사용할 때
// 입체각 측도 변환에 필요한 Jacobian 행렬식
//
//   J(q→p) = |cos θ_q| · dist(x_p, y)²
//             ─────────────────────────────
//             |cos θ_p| · dist(x_q, y)²
//
//   lightNormal이 zero-vector이면 point light로 간주하여
//   cosine term이 상쇄 → J = dist(x_p,y)² / dist(x_q,y)²
float CalcJacobian(float3 hitPos_p, float3 hitPos_q,
                   float3 lightPos, float3 lightNormal)
{
    float3 dir_p = hitPos_p - lightPos;
    float3 dir_q = hitPos_q - lightPos;

    float distP2 = max(dot(dir_p, dir_p), 1e-8f);
    float distQ2 = max(dot(dir_q, dir_q), 1e-8f);

    float lenN = dot(lightNormal, lightNormal);
    if (lenN < 1e-6f)
    {
        // Point light: cosine term 상쇄
        return clamp(distP2 / distQ2, 0.0f, 1e4f);
    }

    // Area light: 법선 기준 cosine 비율 포함
    float3 normL  = lightNormal;  // 이미 정규화 가정
    float3 dirPN  = normalize(dir_p);
    float3 dirQN  = normalize(dir_q);
    float  cosP   = abs(dot(normL, dirPN));
    float  cosQ   = abs(dot(normL, dirQN));

    float J = (cosQ * distP2) / max(cosP * distQ2, 1e-8f);
    return clamp(J, 0.0f, 10.0f);  // 극단적 J 값에 의한 스트리크 방지
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
