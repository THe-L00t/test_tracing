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
// ▶ Point / Directional light (lightNormal = float3(0,0,0)):
//   델타 분포 광원이므로 면적 측도 변환 불필요 → J = 1.0
//   (EvalTargetPDF 내부에서 이미 1/dist² 감쇠가 적용됨)
//
// ▶ Area light (lightNormal != 0):
//   면 법선 기준 cosine 비율 포함
float CalcJacobian(float3 hitPos_p, float3 hitPos_q,
                   float3 lightPos,  float3 lightNormal)
{
    // Point / Directional: 델타 분포 → Jacobian = 1
    if (dot(lightNormal, lightNormal) < 1e-6f)
        return 1.0f;

    // Area light: 면적 측도 → 입체각 측도 변환
    float3 dir_p = hitPos_p - lightPos;
    float3 dir_q = hitPos_q - lightPos;

    float distP2 = max(dot(dir_p, dir_p), 1e-8f);
    float distQ2 = max(dot(dir_q, dir_q), 1e-8f);

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

// ── ONB (법선 기반 접선 공간) ────────────────────────────────────
void BuildONB(float3 N, out float3 T, out float3 B)
{
    if (abs(N.x) > 0.9f)
        T = normalize(cross(float3(0.0f, 1.0f, 0.0f), N));
    else
        T = normalize(cross(float3(1.0f, 0.0f, 0.0f), N));
    B = cross(N, T);
}

// ── 코사인 가중 반구 샘플링 ──────────────────────────────────────
float3 CosineSampleHemisphere(float2 u, float3 N)
{
    float r   = sqrt(u.x);
    float phi = TWO_PI * u.y;
    float lx  = r * cos(phi);
    float lz  = r * sin(phi);
    float ly  = sqrt(max(0.0f, 1.0f - u.x));
    float3 T, B;
    BuildONB(N, T, B);
    return normalize(T * lx + N * ly + B * lz);
}

// ── GGX VNDF 샘플링 (Heitz 2018) ───────────────────────────────
// alpha = roughness^2, V: 시점 방향(세계 공간), N: 표면 법선
float3 SampleGGX_VNDF(float2 u, float alpha, float3 V, float3 N, out float VdotH)
{
    float3 T, B;
    BuildONB(N, T, B);
    float3 Vl = float3(dot(V, T), dot(V, B), dot(V, N));
    float3 Vh = normalize(float3(alpha * Vl.x, alpha * Vl.y, Vl.z));

    float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1    = (lensq > 1e-8f)
        ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq)
        : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    float r   = sqrt(u.x);
    float phi = TWO_PI * u.y;
    float t1  = r * cos(phi);
    float t2  = r * sin(phi);
    float s   = 0.5f * (1.0f + Vh.z);
    t2 = lerp(sqrt(max(0.0f, 1.0f - t1 * t1)), t2, s);

    float3 Nh = t1 * T1 + t2 * T2
              + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;
    float3 Ne = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z)));
    float3 H  = normalize(T * Ne.x + B * Ne.y + N * Ne.z);
    VdotH     = max(dot(V, H), 0.0001f);
    return normalize(reflect(-V, H));
}

// ── BRDF 샘플링 (Specular + Diffuse 혼합 PDF) ──────────────────
// 반환: 산란 방향
// out attenuation: (f_spec+f_diff)·NdotL / p_mix
// out pdf:         혼합 PDF
float3 SampleBRDF(float3 N, float3 V,
                  float3 albedo, float metallic, float roughness,
                  inout uint seed,
                  out float3 attenuation, out float pdf)
{
    float  alpha  = max(roughness * roughness, 0.001f);
    float  alpha2 = alpha * alpha;
    float  NdotV  = max(dot(N, V), 0.0001f);
    float3 F0     = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float  pSpec  = metallic;
    float  pDiff  = 1.0f - metallic;
    float  G1v    = G1_Smith(NdotV, alpha2);

    float3 scatterDir;
    float  VdotH;

    if (RandFloat(seed) < pSpec)
    {
        float2 u   = float2(RandFloat(seed), RandFloat(seed));
        scatterDir = SampleGGX_VNDF(u, alpha, V, N, VdotH);
    }
    else
    {
        float2 u   = float2(RandFloat(seed), RandFloat(seed));
        scatterDir = CosineSampleHemisphere(u, N);
        float3 H   = normalize(V + scatterDir);
        VdotH      = max(dot(V, H), 0.0001f);
    }

    float  NdotL = max(dot(N, scatterDir), 0.0001f);
    float3 H     = normalize(V + scatterDir);
    float  NdotH = max(dot(N, H), 0.0001f);
    float  G1l   = G1_Smith(NdotL, alpha2);
    float  D     = D_GGX(NdotH, alpha2);
    float3 F     = SchlickF(VdotH, F0);

    float  a_pdf  = D * G1v / (4.0f * NdotV);
    float  b      = NdotL * INV_PI;
    pdf           = max(pSpec * a_pdf + pDiff * b, 1e-8f);

    float3 fNdotL = a_pdf * G1l * F + (1.0f - F) * pDiff * albedo * b;
    attenuation   = fNdotL / pdf;
    return scatterDir;
}

// ── GI Reservoir (ReSTIR GI, Ouyang et al. 2021) ─────────────
// 첫 번째 간접 바운스 히트 포인트 x_s와 해당 방향의 누적 Radiance 저장.
// 시간적·공간적 재사용으로 1spp 간접광 분산을 획기적으로 감소.
//
// 레지스터: u8 = gi_reservoir_cur, u9 = gi_reservoir_prev, t13 = gi_reservoir_in
// 크기: 64 bytes (16byte 정렬)
struct GIReservoir
{
    float3 samplePos;    // x_s: 첫 번째 간접 히트 위치       (12)
    uint   valid;        // 1 = 유효한 샘플                   ( 4)
    float3 sampleNormal; // x_s 표면 법선 (Jacobian 보정용)   (12)
    float  wSum;         // 누적 가중치                       ( 4)
    float3 radiance;     // L_o: x_s → x_v raw incoming radiance (BRDF 미포함)  (12)
    float  W;            // 비편향 기여 가중치                 ( 4)
    uint   M;            // 후보 수                           ( 4)
    uint   _pad0;        //                                   ( 4)
    float2 _pad1;        //                                   ( 8)
    // Total: 64 bytes
};

GIReservoir MakeEmptyGIReservoir()
{
    GIReservoir r;
    r.samplePos    = float3(0.0f, 0.0f, 0.0f);
    r.valid        = 0u;
    r.sampleNormal = float3(0.0f, 1.0f, 0.0f);
    r.wSum         = 0.0f;
    r.radiance     = float3(0.0f, 0.0f, 0.0f);
    r.W            = 0.0f;
    r.M            = 0u;
    r._pad0        = 0u;
    r._pad1        = float2(0.0f, 0.0f);
    return r;
}

// [Deprecated] 단순 luma(L_o). 새 코드는 EvalGIpHat(BRDF 포함)을 사용할 것.
// Initial pHat > 0 가드와 FinalizeGIReservoir 내부에서만 잔존.
float EvalGITargetPDF(float3 radiance)
{
    return max(dot(radiance, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

void UpdateGIReservoir(inout GIReservoir r, GIReservoir cand, float w, inout uint seed)
{
    r.wSum += w;
    r.M    += 1u;
    if (RandFloat(seed) < w / max(r.wSum, 1e-8f))
    {
        r.samplePos    = cand.samplePos;
        r.sampleNormal = cand.sampleNormal;
        r.radiance     = cand.radiance;
        r.valid        = cand.valid;
    }
}

void MergeGIReservoir(inout GIReservoir r_a, GIReservoir r_b,
                      float pHat_b, inout uint seed)
{
    float w_b  = pHat_b * r_b.W * float(max(r_b.M, 1u));
    uint  Msum = r_a.M + r_b.M;
    UpdateGIReservoir(r_a, r_b, w_b, seed);
    r_a.M = Msum;
}

void FinalizeGIReservoir(inout GIReservoir r, float pHat_selected)
{
    if (pHat_selected > 0.0f && r.M > 0u)
    {
        r.W = r.wSum / (float(r.M) * pHat_selected);
        // W 클램프 제거: RTXDI 방식에서 W = 1/initPdf 는 의도적인 값.
        // EvalBRDF_GI × L_o × W = f_r × NdotL × L_o / initPdf 는 BRDF 소거로 자연히 유계.
        // Shade의 파이어플라이 클램프(luma > 3)가 극단값 처리.

        // wSum 정규화: W 피드백 루프 방지 (M 클램프 k=5 와 함께 W → 수렴).
        r.wSum = float(r.M) * pHat_selected * r.W;
    }
    else
    {
        r.W    = 0.0f;
        r.wSum = 0.0f;
    }
}

// GI 공간 재사용 Jacobian (Ouyang 2021, Eq.1)
// 이웃 픽셀 q의 x_s를 픽셀 p에서 재사용할 때 입체각 보정
//   J(q→p) = |cosθ_q| · dist(x_p, x_s)²
//             ─────────────────────────────
//             |cosθ_p| · dist(x_q, x_s)²
// θ: x_s 법선과 x_v 방향 사이의 각도
float CalcGIJacobian(float3 xp, float3 xq, float3 xs, float3 xsNormal)
{
    float3 dq = xq - xs;
    float3 dp = xp - xs;
    float  dq2 = max(dot(dq, dq), 1e-8f);
    float  dp2 = max(dot(dp, dp), 1e-8f);
    float  cosQ = abs(dot(normalize(dq), xsNormal));
    float  cosP = abs(dot(normalize(dp), xsNormal));
    return clamp((cosQ * dp2) / max(cosP * dq2, 1e-8f), 0.0f, 10.0f);
}

// ── GGX Cook-Torrance BRDF (f_r × NdotL 반환) ───────────────────
// GI Temporal / Spatial pHat 평가 및 Shade 간접광 계산에 공통 사용.
float3 EvalBRDF_GI(float3 N, float3 V, float3 L,
                   float3 albedo, float metallic, float roughness)
{
    float NdotL = max(dot(N, L), 0.0f);
    if (NdotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

    float  alpha  = max(roughness * roughness, 0.001f);
    float  alpha2 = alpha * alpha;
    float  NdotV  = max(dot(N, V), 0.0001f);
    float3 VpL    = V + L;
    float3 H      = normalize(length(VpL) > 1e-4f ? VpL : N);
    float  NdotH  = max(dot(N, H), 0.0001f);
    float  VdotH  = max(dot(V, H), 0.0001f);

    float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 F    = SchlickF(VdotH, F0);
    float  D    = D_GGX(NdotH, alpha2);
    float  G    = G1_Smith(NdotV, alpha2) * G1_Smith(NdotL, alpha2);
    float3 spec = D * G * F / max(4.0f * NdotV * NdotL, 0.0001f);
    float3 diff = (1.0f - F) * (1.0f - metallic) * albedo * INV_PI;
    return (spec + diff) * NdotL;
}

// GI Target PDF (Ouyang 2021 표준 방식)
//   p̂(xs) = luma( L_o(xs) )
//
// ▶ BRDF를 pHat에 포함하지 않는 이유:
//   BRDF는 시점 방향 V에 의존 → 카메라 이동 시 pHat이 매 프레임 달라짐
//   → Reservoir 선택 확률이 불안정 → 깜빡임(flickering) 발생.
// ▶ L_o는 저장된 raw incoming radiance (BRDF 미포함, 뷰 독립적).
//   BRDF는 오직 Shade 패스에서만 적용: f_r * NdotL * L_o * vis * W.
// ▶ 뷰 독립적 pHat = 안정적 Temporal/Spatial 재사용.
float EvalGIpHat(float3 L_o)
{
    return max(dot(L_o, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}
