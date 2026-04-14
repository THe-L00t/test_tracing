// ──────────────────────────────────────────────────────────────
//  Common_ReSTIR.hlsli  –  ReSTIR DI 공용 헤더
//  참조: Bitterli et al. 2020 "Spatiotemporal reservoir resampling
//        for real-time ray tracing with dynamic direct lighting"
// ──────────────────────────────────────────────────────────────
//
//  ── 힙 슬롯 / 레지스터 레이아웃 ──────────────────────────────
//
//  UAV (쓰기 가능):
//   u0 : g_output          RGBA8    - 최종 SDR 출력
//   u1 : g_accumulation    RGBA32F  - HDR 누적
//   u2 : gbuf_worldPos     RGBA32F  - 월드위치 XYZ + 히트거리 W
//   u3 : gbuf_normal       RGBA32F  - 월드법선 XYZ + matIdx(float) W
//   u4 : gbuf_albedo       RGBA8    - Albedo RGB + metallic A
//   u5 : gbuf_matInfo      RGBA32F  - roughness R, linearDepth G, flags B, pad A
//   u6 : reservoir_cur     RWStructuredBuffer<Reservoir>  (동적: A 또는 B)
//   u7 : reservoir_prev    RWStructuredBuffer<Reservoir>  (동적: B 또는 A)
//
//  SRV (읽기 전용):
//   t0 : g_tlas            RTAS
//   t1 : g_vbPlane
//   t2 : g_vbCube
//   t3 : g_vbRoom
//   t4 : g_vbSphere
//   t5 : g_lightList       StructuredBuffer<LightData>
//   t6 : gbuf_worldPos     Texture2D SRV (Compute/DXR 읽기용)
//   t7 : gbuf_normal       Texture2D SRV
//   t8 : gbuf_albedo       Texture2D SRV
//   t9 : gbuf_matInfo      Texture2D SRV
//   t10: reservoir_in      StructuredBuffer<Reservoir> (Spatial 입력 & Shade 입력, 동적)
//
//  CBV:
//   b0 : SceneConstants    (SceneCB, 256B)
//   b1 : ReSTIRConstants   (ReSTIRCB, 128B)
//
//  ── G-Buffer flags (gbuf_matInfo.b) ─────────────────────────
//   0.0 = 일반 불투명·비발광 픽셀  → ReSTIR 직접광 처리
//   1.0 = 유리/반투명 픽셀        → ReSTIR 스킵, Shade에서 환경광 근사
//   2.0 = 발광 픽셀               → Shade에서 emission 직접 출력
// ──────────────────────────────────────────────────────────────

static const float GB_FLAG_NORMAL   = 0.0f;
static const float GB_FLAG_GLASS    = 1.0f;
static const float GB_FLAG_EMISSIVE = 2.0f;

// ── 수학 상수 ────────────────────────────────────────────────
static const float PI      = 3.14159265358979323846f;
static const float INV_PI  = 0.31830988618379067154f;
static const float TWO_PI  = 6.28318530717958647692f;

// ── WangHash RNG ─────────────────────────────────────────────
// Vitter 1985 WRS에서 사용하는 빠른 비상관 해시 기반 의사난수
uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s ^= s >> 4u;
    s *= 0x27d4eb2du;
    s ^= s >> 15u;
    return s;
}

// [0, 1) 균일 분포 실수 반환
float RandFloat(inout uint seed)
{
    seed = WangHash(seed);
    return float(seed & 0x00FFFFFFu) / float(0x01000000u);
}

// ── Reservoir ────────────────────────────────────────────────
// Vitter 1985 "Random Sampling with a Reservoir" 기반 스트리밍 WRS
struct Reservoir
{
    uint  lightIdx;  // 선택된 광원 인덱스 (0xFFFFFFFF = 유효하지 않음)
    float wSum;      // 누적 가중치 Σ w_i
    float W;         // 비편향 기여 가중치 = wSum / (M * p̂(y))
    uint  M;         // 시도한 후보 수
};

Reservoir MakeEmptyReservoir()
{
    Reservoir r;
    r.lightIdx = 0xFFFFFFFFu;
    r.wSum     = 0.0f;
    r.W        = 0.0f;
    r.M        = 0u;
    return r;
}

// RIS 단일 후보 갱신 (Talbot 2005, Algorithm 2)
//   w_i = p̂(x_i) / q(x_i)   (q = uniform → w_i = p̂(x_i) * N)
//   확률 w_i / Σw 로 lightIdx 교체
void UpdateReservoir(inout Reservoir r, uint candidateLight,
                     float w, inout uint seed)
{
    r.wSum += w;
    r.M    += 1u;
    if (RandFloat(seed) < w / max(r.wSum, 1e-8f))
        r.lightIdx = candidateLight;
}

// Reservoir 병합 (Bitterli 2020, Eq.6)
//   R_b를 R_a 에 합산
//   pHat_b: R_b.lightIdx 의 타겟 함수값 @ 현재 픽셀 (재사용 대상 픽셀에서 평가)
void MergeReservoir(inout Reservoir r_a, Reservoir r_b,
                    float pHat_b, inout uint seed)
{
    float w_b  = pHat_b * r_b.W * float(r_b.M);  // Eq.6 기여 가중치
    uint  Msum = r_a.M + r_b.M;
    UpdateReservoir(r_a, r_b.lightIdx, w_b, seed);
    r_a.M = Msum;
}

// 최종 비편향 가중치 확정 (Talbot 2005, Algorithm 2 line 8)
//   W = wSum / (M * p̂(y_selected))
//   이 W를 Shade에서 L = fr * Li * NdotL * vis * W 에 사용
void FinalizeReservoir(inout Reservoir r, float pHat_selected)
{
    r.W = (pHat_selected > 0.0f)
        ? r.wSum / (float(r.M) * pHat_selected)
        : 0.0f;
}

// ── LightData ────────────────────────────────────────────────
struct LightData
{
    float3   pos;       // point/area: 위치, directional: 정규화 방향
    float    intensity;
    float3   color;
    uint     type;      // 0=point, 1=directional, 2=area(box)
    float    halfSize;  // area box 반크기 (type==2)
    float3   center;    // area box 중심 (type==2)
};

// ── 타겟 함수 p̂ 평가 ─────────────────────────────────────────
// Bitterli 2020, Section 5.1 / Talbot 2005, unstratified RIS
//
//   p̂(y) = luma( L_i(y→x) · max(N·L, 0) )
//
// ▶ BRDF를 p̂에 포함하지 않는 이유:
//   GGX 스펙큘러 로브가 좁은 스무스 메탈릭 표면에서 거의 모든 후보의 p̂≈0이 돼
//   wSum=0 → Reservoir 비어있음 → 검은색.
//   L_i·NdotL 만 사용하면 반구상의 모든 광원이 양수 가중치를 가짐.
//
// ▶ 가시성은 포함하지 않음 (Final Shade 단계에서만 shadow ray)
float EvalTargetPDF(float3 hitPos, float3 N,
                    LightData light)
{
    float3 L;
    float3 Li;

    if (light.type == 1u)
    {
        L  = normalize(light.pos);
        Li = light.color * light.intensity;
    }
    else
    {
        float3 toL = light.pos - hitPos;
        float  d   = max(length(toL), 0.01f);
        L  = toL / d;
        Li = light.color * light.intensity / (d * d);
    }

    float NdotL = max(dot(N, L), 0.0f);
    if (NdotL <= 0.0f) return 0.0f;

    float3 contrib = Li * NdotL;
    // BT.709 luma
    return max(dot(contrib, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

// ── Jacobian 보정 (Bitterli 2020, Eq.11) ─────────────────────
// 이웃 픽셀 q 의 광원 샘플을 픽셀 p 에서 재사용할 때 입체각 측도 변환
//
//   J(q→p) = |cosθ_q| · dist(x_p, y)²
//             ─────────────────────────
//             |cosθ_p| · dist(x_q, y)²
//
// ▶ Point light (lightNormal = float3(0,0,0)):
//   cosine term이 분자/분모에서 상쇄 → J = dist²_p / dist²_q
//
// ▶ Area light (lightNormal != 0):
//   면 법선 기준 cosine 비율 포함
float CalcJacobian(float3 hitPos_p, float3 hitPos_q,
                   float3 lightPos,  float3 lightNormal)
{
    float3 dir_p = hitPos_p - lightPos;
    float3 dir_q = hitPos_q - lightPos;

    float distP2 = max(dot(dir_p, dir_p), 1e-8f);
    float distQ2 = max(dot(dir_q, dir_q), 1e-8f);

    // Point light: lightNormal 크기가 0에 가까우면 cosine 항 상쇄
    if (dot(lightNormal, lightNormal) < 1e-6f)
        return clamp(distP2 / distQ2, 0.0f, 10.0f);

    // Area light: 면 법선 기준 cosine 비율
    float3 dirPN = normalize(dir_p);
    float3 dirQN = normalize(dir_q);
    float  cosP  = abs(dot(lightNormal, dirPN));
    float  cosQ  = abs(dot(lightNormal, dirQN));

    float J = (cosQ * distP2) / max(cosP * distQ2, 1e-8f);
    return clamp(J, 0.0f, 10.0f);
}

// ── GGX BRDF 헬퍼 ───────────────────────────────────────────
// NDF: GGX (Trowbridge-Reitz), alpha2 = (roughness²)²
float D_GGX(float NdotH, float alpha2)
{
    float b = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / (PI * b * b);
}

// 마스킹 함수 G1 (Smith 근사)
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

// ── 유틸리티 ─────────────────────────────────────────────────
uint PixelIndex(uint2 px, uint width)
{
    return px.y * width + px.x;
}

bool IsValidPixel(int2 px, uint w, uint h)
{
    return px.x >= 0 && px.y >= 0
        && uint(px.x) < w && uint(px.y) < h;
}
