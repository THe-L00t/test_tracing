// ──────────────────────────────────────────────────────────────
//  Raytracing.hlsl  –  PBR Path Tracing (GGX + MIS)
//
//  변경 사항 (feature/pbr-path-tracing):
//    - GGX 미세면 BRDF (D_GGX, G1_Smith, SchlickF)
//    - GGX VNDF 샘플링 (Heitz 2018) – roughness 반영
//    - 혼합 BRDF 샘플링 (specular/diffuse 균일 혼합 PDF)
//    - Balance Heuristic MIS (NEE ↔ BRDF 이중 계산 방지)
//    - 가우시안 포커스 샘플링 제거 → 프레임당 1spp 균일 누적
//    - Lambertian 1/pi 수정
//
//  InstanceID 인코딩:
//    하위 4비트 = geomType (0=plane, 1=cube, 2=room, 3=sphere)
//    상위 비트  = matIdx   (0..3)
//
//  재질 특수 인코딩 (matEmissive 필드):
//    matEmissive >  0   : 발광체 (area light)
//    matEmissive == 0   : 불투명 (PBR Lambertian/metallic)
//    matEmissive < -0.5 : 반투명 (확산+투과 혼합)
//    matEmissive < -1.5 : 유리 (Fresnel 굴절/반사, IOR=1.5)
// ──────────────────────────────────────────────────────────────

// ── 리소스 ──────────────────────────────────────────────────────
RWTexture2D<float4>             g_output       : register(u0);
RWTexture2D<float4>             g_accumulation : register(u1);
RWTexture2D<float>              g_depth        : register(u2); // NDC depth [0..1] (DLSS용)
RWTexture2D<float2>             g_motionVec    : register(u3); // 모션벡터 픽셀 단위 (DLSS용) / 비DLSS: oct-법선
RWTexture2D<float4>             g_normals      : register(u4); // world-법선(xyz) + roughness(w) (A-trous·DLSS-RR Packed 공용)
RWTexture2D<float4>             g_diffAlbedo   : register(u5); // DLSS-RR diffuse albedo (linear) — albedo*(1-metallic)
RWTexture2D<float4>             g_specAlbedo   : register(u6); // DLSS-RR specular F0 (linear)  — lerp(0.04, albedo, metallic)

// ── NRD 입력 (DLSS 경로 전용) ──────────────────────────────────────
// NRD RELAX_DIFFUSE_SPECULAR 는 primary hit 에서 lobe-separated radiance + first-bounce hitDist 를
// 요구한다. 1spp probabilistic lobe selection: primary 에서 diff/spec 중 하나만 샘플 → 해당 버퍼로.
// 나머지 버퍼는 0 으로 두고 NRD가 neighbor 에서 reconstruct (HitDistanceReconstructionMode::AREA_3X3).
RWTexture2D<float4>             g_diffRadianceHitDist : register(u7); // RGBA16F: rgb=diff radiance, w=hitDist
RWTexture2D<float4>             g_specRadianceHitDist : register(u8); // RGBA16F: rgb=spec radiance, w=hitDist
RWTexture2D<float>              g_viewZ               : register(u9); // R16F  : linear view-space Z (primary)

// HPAR-PT Phase 7 — Spatial Reservoir Reuse (RGBA32_UINT render-res ping-pong)
//   .x = dirOct (2×16-bit oct-encoded first-bounce direction packed in u32)
//   .y = asuint(W)          (RIS unbiased contribution weight W = w_sum / (M · p̂_chosen))
//   .z = M                  (sample count, capped at reservoirMCap)
//   .w = reserved (Phase 8 temporal flags)
// reservoirEnabled = 0 인 비DLSS 모드에서는 접근 금지 (UAV 만 바인딩됨).
RWTexture2D<uint4>              g_reservoirIn  : register(u10);   // 이전 frame 의 reservoir (읽기)
RWTexture2D<uint4>              g_reservoirOut : register(u11);   // 현재 frame 의 reservoir (쓰기)

RaytracingAccelerationStructure g_tlas         : register(t0);

struct VertexPN { float3 pos; float3 normal; };
StructuredBuffer<VertexPN> g_vbPlane   : register(t1);
StructuredBuffer<VertexPN> g_vbCube    : register(t2);
StructuredBuffer<VertexPN> g_vbRoom    : register(t3);
StructuredBuffer<VertexPN> g_vbSphere  : register(t4);

// HPAR-PT Phase 4 — Perceptual Importance smooth output (R16F)
//   adaptiveRayEnabled=0 인 비DLSS 모드에서는 사용 안 함 (stale 값 OK).
Texture2D<float>           g_importance : register(t5);

cbuffer SceneConstants : register(b0)
{
    float3 camPos;       uint   sceneID;
    float3 camRight;     float  tanHalfFovY;
    float3 camUp;        float  aspectRatio;
    float3 camForward;   float  _pad0;

    float3 lightPos;     float  lightRadius;
    float3 lightColor;   float  lightIntensity;
    float3 light2Pos;    float  light2Radius;
    float3 light2Color;  float  light2Intensity;

    float4 matAlbedoRoughness[4];   // .xyz=albedo  .w=perceptualRoughness
    float4 matMetallic;             // .xyzw = metallic per mat
    float4 matEmissive;             // .xyzw = emissive signal

    uint   frameCount;  uint   randomSeed;  float  emissBoxHalfSize; float jitterX;
    float3 emissBoxCenter;  float jitterY;

    // 이전 프레임 카메라 (DLSS 모션벡터 계산용)
    float3 prevCamPos;     uint   isDLSSMode;
    float3 prevCamRight;   float  prevTanHalfFovY;
    float3 prevCamUp;      float  prevAspectRatio;
    float3 prevCamForward; uint   adaptiveRayEnabled;  // Phase 4

    // Phase 4 — Adaptive Ray Allocation
    uint   rMin;           uint   rMax;
    float  gamma;          float  _pad2;

    // Phase 6 — Tier 분류 (PHTR 통합)
    //   tier = (Î > tierHigh) ? 1 : (Î > tierLow) ? 2 : 3
    //   Tier 1: full PT (현 동작)  Tier 2: partial reuse  Tier 3: aggressive reuse
    float  tierLow;        float  tierHigh;
    float  _pad3a;         float  _pad3b;

    // Phase 7 — Spatial Reservoir Reuse
    //   reservoirEnabled : 1 = reservoir 읽기/쓰기 활성 (DLSS 모드 + adaptive 활성)
    //   reservoirReset   : 1 = prev frame 무시 (첫 frame / 씬 전환 / 큰 카메라 변화)
    //   reservoirMCap    : M(샘플 수) 상한 — history 가 너무 커지지 않도록
    uint   reservoirEnabled; uint reservoirReset;
    uint   reservoirMCap;    uint _pad4;
}

// ── 상수 ────────────────────────────────────────────────────────
static const float PI     = 3.14159265358979f;
static const float INV_PI = 0.31830988618379f;
static const float TWO_PI = 6.28318530717959f;
static const uint  k_maxBounce = 8u;

// ── 페이로드 ────────────────────────────────────────────────────
struct RayPayload
{
    float3 emission;        // NEE 직접광 + 미스 방사
    float3 attenuation;     // BRDF 처리량 가중치
    float3 nextOrigin;
    float3 nextDirection;
    float  scatterPdf;      // 산란 방향의 혼합 PDF (MIS용); 0 = delta(유리 등)
    uint   seed;
    uint   depth;
    uint   terminated;
    float  hitDist;         // 1차 히트 거리 (depth/motion 계산용, depth==0일 때만 유효)
    float3 hitNormal;       // 1차 히트 월드 법선 (A-trous 엣지 스토핑용, depth==0일 때만 유효)
    float  hitRoughness;    // 1차 히트 GGX 러프니스 (DLSS-RR normals.w 패킹용)
    float3 hitAlbedo;       // 1차 히트 albedo (DLSS-RR diffuse/specular 분리용)
    float  hitMetallic;     // 1차 히트 metallic (DLSS-RR F0 보간 계수)
    // NRD 라우팅 — primary hit (depth==0) 에서 결정
    uint   primaryLobeIsSpec; // 0=diff lobe, 1=spec lobe (NRD radiance 버퍼 라우팅용)
    float  firstBounceHitDist;// primary→secondary 거리 (NRD hitDist, primary hit 거리 제외)
};

struct ShadowPayload { float vis; };

// ── RNG (Wang Hash) ─────────────────────────────────────────────
uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s ^= s >> 4u;
    s *= 0x27d4eb2du;
    s ^= s >> 15u;
    return s;
}
float RandFloat(inout uint s)
{
    s = WangHash(s);
    return float(s & 0x00FFFFFFu) / float(0x01000000u);
}

// ── Oct-인코딩 (월드 법선 → float2 [-1,1]^2) ────────────────────
float2 OctEncode(float3 n)
{
    float3 o = n / (abs(n.x) + abs(n.y) + abs(n.z) + 1e-8f);
    float2 r;
    r.x = (o.z < 0.0f) ? ((1.0f - abs(o.y)) * (o.x >= 0.0f ? 1.0f : -1.0f)) : o.x;
    r.y = (o.z < 0.0f) ? ((1.0f - abs(o.x)) * (o.y >= 0.0f ? 1.0f : -1.0f)) : o.y;
    return r;
}

// ── Phase 7 — Reservoir 헬퍼 ────────────────────────────────────
// 단위 방향 v → uint32 (2 × 16-bit oct, [0..65535] 양자화)
uint OctPack32(float3 v)
{
    float2 e = OctEncode(v);             // [-1, 1]
    uint2 q = uint2(saturate(e * 0.5f + 0.5f) * 65535.0f + 0.5f);
    return (q.x & 0xFFFFu) | ((q.y & 0xFFFFu) << 16u);
}
float3 OctUnpack32(uint p)
{
    float2 q = float2(float(p & 0xFFFFu), float((p >> 16u) & 0xFFFFu));
    float2 e = q / 65535.0f * 2.0f - 1.0f;   // [-1, 1]
    float3 v = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f)
    {
        // HLSL 2021+: 벡터 비교의 ternary 금지 → 컴포넌트별 처리
        float2 s = float2(v.x >= 0.0f ? 1.0f : -1.0f,
                          v.y >= 0.0f ? 1.0f : -1.0f);
        v.xy = (1.0f - abs(v.yx)) * s;
    }
    return normalize(v);
}

struct Reservoir
{
    uint  dirOct;    // first-bounce direction (oct-packed)
    float W;         // RIS unbiased weight = w_sum / (M · p̂_chosen)
    uint  M;         // sample count (capped)
};

Reservoir EmptyReservoir()
{
    Reservoir r;
    r.dirOct = 0u; r.W = 0.0f; r.M = 0u;
    return r;
}

Reservoir LoadReservoir(uint2 px)
{
    // RWTexture2D<uint4> 는 Load(int3) 가 truncate 되므로 인덱서 사용.
    uint4 v = g_reservoirIn[px];
    Reservoir r;
    r.dirOct = v.x;
    r.W      = asfloat(v.y);
    r.M      = v.z;
    return r;
}

void StoreReservoir(uint2 px, Reservoir r)
{
    g_reservoirOut[px] = uint4(r.dirOct, asuint(r.W), r.M, 0u);
}

// p̂ : 우리 픽셀의 primary hit 에서 dir 방향의 target function 근사.
//      |N·L| 만 사용 (간단·낮은 비용, ReSTIR DI 시점에선 unshadowed luminance 였음).
float ReservoirTargetPdf(float3 N, float3 dir)
{
    return max(0.0f, dot(N, dir));
}

// ── WRS update (Bitterli 2020 Algorithm 2) ─────────────────────
//   M_inc : 이 sample 이 대표하는 관측 횟수 (own = 1, neighbor reservoir = n.M)
//   w     : importance weight (own: p̂/pdf, neighbor: p̂·n.W·n.M)
void UpdateReservoir(inout Reservoir r, uint dirOctNew, uint M_inc, float w,
                     inout uint seed, inout float wSum)
{
    if (w <= 0.0f) return;
    r.M += M_inc;
    wSum += w;
    if (RandFloat(seed) * wSum < w)
    {
        r.dirOct = dirOctNew;
    }
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

// ── GGX 분포 함수 D ──────────────────────────────────────────────
// NdotH: 법선·반벡터 코사인, alpha2: alpha^2 (alpha = roughness^2)
float D_GGX(float NdotH, float alpha2)
{
    float b = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / (PI * b * b);
}

// ── Smith G1 마스킹 함수 ─────────────────────────────────────────
float G1_Smith(float NdotX, float alpha2)
{
    return 2.0f * NdotX / (NdotX + sqrt(alpha2 + (1.0f - alpha2) * NdotX * NdotX));
}

// ── Schlick Fresnel ──────────────────────────────────────────────
float3 SchlickF(float cosTheta, float3 F0)
{
    float t  = 1.0f - saturate(cosTheta);
    float t2 = t * t;
    return F0 + (1.0f - F0) * (t2 * t2 * t);
}

// ── GGX VNDF 샘플링 (Heitz 2018) ───────────────────────────────
// alpha = roughness^2, V: 시점 방향(세계 공간), N: 표면 법선
// 반환: 반사 방향, out VdotH: Fresnel 계산용
float3 SampleGGX_VNDF(float2 u, float alpha, float3 V, float3 N, out float VdotH)
{
    float3 T, B;
    BuildONB(N, T, B);
    float3 Vl = float3(dot(V, T), dot(V, B), dot(V, N));

    float3 Vh = normalize(float3(alpha * Vl.x, alpha * Vl.y, Vl.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = (lensq > 1e-8f)
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

    float3 H = normalize(T * Ne.x + B * Ne.y + N * Ne.z);
    VdotH    = max(dot(V, H), 0.0001f);
    return normalize(reflect(-V, H));
}

// ── 혼합 BRDF PDF (MIS용) ────────────────────────────────────────
// pSpec·p_spec(L) + pDiff·p_diff(L)
float EvalBRDFPdf(float3 N, float3 V, float3 L,
                  float metallic, float alpha, float alpha2)
{
    float NdotL = max(dot(N, L), 0.0001f);
    float NdotV = max(dot(N, V), 0.0001f);
    float3 H    = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0001f);

    float G1v    = G1_Smith(NdotV, alpha2);
    float pdfSpec = (alpha > 0.01f)
        ? D_GGX(NdotH, alpha2) * G1v / (4.0f * NdotV)
        : 0.0f;
    float pdfDiff = NdotL * INV_PI;

    return metallic * pdfSpec + (1.0f - metallic) * pdfDiff;
}

// ── Power-Heuristic MIS 가중치 ──────────────────────────────────
float MISWeight(float pdf_a, float pdf_b)
{
    float a2 = pdf_a * pdf_a;
    float b2 = pdf_b * pdf_b;
    return a2 / max(a2 + b2, 1e-10f);
}

// ── BRDF 샘플링 (혼합 PDF) ──────────────────────────────────────
// 반환: 산란 방향
// out attenuation: (f_spec+f_diff)·NdotL / p_mix
// out pdf:         혼합 PDF (외부 MIS용)
float3 SampleBRDF(float3 N, float3 V,
                  float3 albedo, float metallic, float roughness,
                  inout uint seed,
                  out float3 attenuation, out float pdf,
                  out uint  outIsSpecLobe)   // 선택된 lobe (0=diff, 1=spec) — NRD 라우팅용
{
    float alpha  = max(roughness * roughness, 0.001f);
    float alpha2 = alpha * alpha;
    float NdotV  = max(dot(N, V), 0.0001f);
    float3 F0    = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float  pSpec = metallic;
    float  pDiff = 1.0f - metallic;
    float  G1v   = G1_Smith(NdotV, alpha2);

    float3 scatterDir;
    float  VdotH;

    if (RandFloat(seed) < pSpec)
    {
        float2 u   = float2(RandFloat(seed), RandFloat(seed));
        scatterDir = SampleGGX_VNDF(u, alpha, V, N, VdotH);
        outIsSpecLobe = 1u;
    }
    else
    {
        float2 u   = float2(RandFloat(seed), RandFloat(seed));
        scatterDir = CosineSampleHemisphere(u, N);
        float3 H   = normalize(V + scatterDir);
        VdotH      = max(dot(V, H), 0.0001f);
        outIsSpecLobe = 0u;
    }

    float  NdotL = max(dot(N, scatterDir), 0.0001f);
    float3 H     = normalize(V + scatterDir);
    float  NdotH = max(dot(N, H), 0.0001f);
    float  G1l   = G1_Smith(NdotL, alpha2);
    float  D     = D_GGX(NdotH, alpha2);
    float3 F     = SchlickF(VdotH, F0);

    // a_pdf = D·G1v / (4·NdotV)  → specular pdf 기저항
    // b     = NdotL / pi          → diffuse pdf 기저항
    float a_pdf = D * G1v / (4.0f * NdotV);
    float b     = NdotL * INV_PI;

    // 혼합 PDF
    pdf = max(pSpec * a_pdf + pDiff * b, 1e-8f);

    // 전체 BRDF · NdotL:
    //   specular 기여: a_pdf · G1l · F
    //   diffuse  기여: (1-F)·pDiff·albedo·b
    float3 fNdotL = a_pdf * G1l * F + (1.0f - F) * pDiff * albedo * b;

    attenuation = fNdotL / pdf;
    return scatterDir;
}

// ── 그림자 가시성 ────────────────────────────────────────────────
float ShadowVis(float3 origin, float3 dir, float tmax)
{
    ShadowPayload sp = { 0.0f };
    RayDesc sr;
    sr.Origin    = origin;
    sr.Direction = dir;
    sr.TMin      = 0.001f;
    sr.TMax      = tmax;
    TraceRay(g_tlas,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER         |
        RAY_FLAG_FORCE_OPAQUE,
        0xFD, 0, 1, 1, sr, sp);
    return sp.vis;
}

// ── 면적광 NEE (MIS 포함) ────────────────────────────────────────
float3 AreaLightNEE(float3 hitPos, float3 N, float3 V,
                    float3 albedo, float metallic, float alpha, float alpha2,
                    inout uint seed)
{
    if (emissBoxHalfSize <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

    uint  faceIdx = uint(RandFloat(seed) * 6.0f) % 6u;
    float fu      = RandFloat(seed) * 2.0f - 1.0f;
    float fv      = RandFloat(seed) * 2.0f - 1.0f;

    float3 samplePos, faceN;
    float  h = emissBoxHalfSize;
    if      (faceIdx == 0u) { samplePos = emissBoxCenter + float3( h, fu*h, fv*h); faceN = float3( 1,0,0); }
    else if (faceIdx == 1u) { samplePos = emissBoxCenter + float3(-h, fu*h, fv*h); faceN = float3(-1,0,0); }
    else if (faceIdx == 2u) { samplePos = emissBoxCenter + float3(fu*h,  h, fv*h); faceN = float3(0, 1,0); }
    else if (faceIdx == 3u) { samplePos = emissBoxCenter + float3(fu*h, -h, fv*h); faceN = float3(0,-1,0); }
    else if (faceIdx == 4u) { samplePos = emissBoxCenter + float3(fu*h, fv*h,  h); faceN = float3(0,0, 1); }
    else                    { samplePos = emissBoxCenter + float3(fu*h, fv*h, -h); faceN = float3(0,0,-1); }

    float3 toS       = samplePos - hitPos;
    float  dist2     = dot(toS, toS);
    float  dist      = sqrt(dist2);
    float3 L         = toS / dist;
    float  NdotL     = max(dot(N, L), 0.0f);
    float  faceNdotL = max(dot(faceN, -L), 0.0f);

    if (NdotL <= 0.0f || faceNdotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

    float totalArea = 24.0f * h * h;
    // 입체각 기준 pdf_light
    float pdf_light = dist2 / (faceNdotL * totalArea);
    float pdf_brdf  = EvalBRDFPdf(N, V, L, metallic, alpha, alpha2);
    float w_nee     = MISWeight(pdf_light, pdf_brdf);

    // PBR BRDF 평가
    float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 Hv   = normalize(V + L);
    float NdotH = max(dot(N, Hv), 0.0001f);
    float NdotV = max(dot(N, V),  0.0001f);
    float VdotH = max(dot(V, Hv), 0.0001f);
    float3 F    = SchlickF(VdotH, F0);
    float  D    = D_GGX(NdotH, alpha2);
    float  G    = G1_Smith(NdotV, alpha2) * G1_Smith(NdotL, alpha2);
    float3 spec = D * G * F / max(4.0f * NdotV * NdotL, 0.0001f);
    float3 diff = (1.0f - F) * (1.0f - metallic) * albedo * INV_PI;
    float3 brdf = spec + diff;

    float3 Le  = matAlbedoRoughness[3].xyz * matEmissive[3];
    float  vis = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);

    // w_nee · brdf · NdotL · Le / pdf_area · G_geo
    return w_nee * brdf * NdotL * Le * faceNdotL * totalArea / dist2 * vis;
}

// ── 델타 광원 PBR BRDF 평가 헬퍼 ────────────────────────────────
// (MIS 불필요한 포인트/방향광 전용)
float3 EvalDeltaLight(float3 N, float3 V, float3 L, float3 Li,
                      float3 albedo, float metallic, float alpha2)
{
    float NdotL = max(dot(N, L), 0.0f);
    if (NdotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 H    = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0001f);
    float NdotV = max(dot(N, V), 0.0001f);
    float VdotH = max(dot(V, H), 0.0001f);
    float3 F    = SchlickF(VdotH, F0);
    float  D    = D_GGX(NdotH, alpha2);
    float  G    = G1_Smith(NdotV, alpha2) * G1_Smith(NdotL, alpha2);
    float3 spec = D * G * F / max(4.0f * NdotV * NdotL, 0.0001f);
    float3 diff = (1.0f - F) * (1.0f - metallic) * albedo * INV_PI;
    return (spec + diff) * NdotL * Li;
}

// ── NEE 직접광 ──────────────────────────────────────────────────
float3 DirectLighting(float3 hitPos, float3 N, float3 V,
                      float3 albedo, float metallic, float roughness,
                      inout uint seed)
{
    float  alpha  = max(roughness * roughness, 0.001f);
    float  alpha2 = alpha * alpha;
    float3 result = float3(0.0f, 0.0f, 0.0f);

    if (sceneID == 0)
    {
        float3 sunDir   = normalize(float3(1.0f, 2.0f, -0.5f));
        float3 sunColor = float3(1.0f, 1.0f, 1.0f) * 3.0f;
        float  vis      = ShadowVis(hitPos + N * 0.001f, sunDir, 1e6f);
        result = EvalDeltaLight(N, V, sunDir, sunColor, albedo, metallic, alpha2) * vis;
    }
    else if (sceneID == 1)
    {
        float3 toL   = lightPos - hitPos;
        float  dist  = max(length(toL), 0.01f);
        float3 L     = toL / dist;
        float  atten = lightIntensity / (dist * dist);
        float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
        result = EvalDeltaLight(N, V, L, lightColor * atten, albedo, metallic, alpha2) * vis;
    }
    else
    {
        {
            float3 toL   = lightPos - hitPos;
            float  dist  = max(length(toL), 0.01f);
            float3 L     = toL / dist;
            float  atten = lightIntensity / (dist * dist);
            float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
            result += EvalDeltaLight(N, V, L, lightColor * atten, albedo, metallic, alpha2) * vis;
        }
        {
            float3 toL   = light2Pos - hitPos;
            float  dist  = max(length(toL), 0.01f);
            float3 L     = toL / dist;
            float  atten = light2Intensity / (dist * dist);
            float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
            result += EvalDeltaLight(N, V, L, light2Color * atten, albedo, metallic, alpha2) * vis;
        }
        result += AreaLightNEE(hitPos, N, V, albedo, metallic, alpha, alpha2, seed);
    }
    return result;
}

// ── RayGen – 균일 1spp 패스 트레이싱 ───────────────────────────
[shader("raygeneration")]
void RayGen()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    uint seed = WangHash(idx.x + idx.y * dim.x + randomSeed * 719393u);

    float2 dlssJ = float2(jitterX, jitterY);
    // DLSS 모드: Halton 지터만 사용.
    // aaJ를 더하면 실제 레이 위치가 DLSS에 전달한 jitter와 달라져
    // depth·motion 불일치 → 고스팅·시간적 누적 오류가 발생한다.
    // 비DLSS 모드: dlssJ=(0,0)이므로 aaJ만 유효.
    float2 aaJ = float2(0.0f, 0.0f);
    if (!isDLSSMode)
        aaJ = float2(RandFloat(seed), RandFloat(seed)) - 0.5f;
    float2 uv = ((float2)idx + 0.5f + dlssJ + aaJ) / (float2)dim;
    float2 ndc    = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float3 dir = normalize(
        camForward +
        camRight * (ndc.x * aspectRatio * tanHalfFovY) +
        camUp    * (ndc.y * tanHalfFovY)
    );

    RayDesc ray;
    ray.Origin    = camPos;
    ray.Direction = dir;
    ray.TMin      = 0.001f;
    ray.TMax      = 1e6f;

    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 radiance   = float3(0.0f, 0.0f, 0.0f);

    // NRD 라우팅 (cross-bounce 누적, payload 가 매 bounce 새로 생성되므로 별도 변수로 보관)
    uint   nrdPrimaryLobeIsSpec  = 0u;
    float  nrdFirstBounceHitDist = 0.0f;
    float  nrdViewZ              = 0.0f;

    // Phase 4 — Adaptive Ray Allocation: per-pixel maxBounce 결정
    //   R_i = R_min + (R_max - R_min) · Î^γ
    //   비DLSS 모드(adaptiveRayEnabled=0) 또는 importance 미할당 시 k_maxBounce 유지
    uint maxBounce = k_maxBounce;
    // Phase 6 — Tier 분류 결과는 Phase 7 reservoir 분기에서 사용
    uint primaryTier = 1u;
    if (adaptiveRayEnabled != 0u)
    {
        float I = g_importance.Load(int3((int2)idx, 0));
        float r = (float)rMin + ((float)rMax - (float)rMin) * pow(saturate(I), gamma);
        maxBounce = max(1u, (uint)round(r));
        maxBounce = min(maxBounce, k_maxBounce);  // hard upper bound 안전망

        // Phase 6 tier (Tier 1 = full PT / Tier 2 = partial reuse / Tier 3 = aggressive reuse)
        primaryTier = (I > tierHigh) ? 1u : ((I > tierLow) ? 2u : 3u);
    }

    for (uint bounce = 0u; bounce < maxBounce; bounce++)
    {
        RayPayload payload;
        payload.emission      = float3(0.0f, 0.0f, 0.0f);
        payload.attenuation   = float3(0.0f, 0.0f, 0.0f);
        payload.nextOrigin    = float3(0.0f, 0.0f, 0.0f);
        payload.nextDirection = float3(0.0f, 0.0f, 0.0f);
        payload.scatterPdf    = 0.0f;
        payload.seed          = seed;
        payload.depth         = bounce;
        payload.terminated    = 0u;
        payload.hitDist       = ray.TMax;
        payload.hitNormal     = float3(0.0f, 1.0f, 0.0f);  // 미스 시 기본값 (up)
        payload.hitRoughness  = 1.0f;                        // 미스 시 기본값 (fully rough)
        payload.hitAlbedo     = float3(0.0f, 0.0f, 0.0f);    // 미스 시: 스카이/배경 (diffuse·specular=0)
        payload.hitMetallic   = 0.0f;
        payload.primaryLobeIsSpec  = 0u;
        payload.firstBounceHitDist = 0.0f;

        TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

        seed = payload.seed;
        radiance += throughput * payload.emission;

        // 1차 히트: depth 항상 기록, motion(DLSS) 또는 oct-normal(비DLSS) 기록
        if (bounce == 0u)
        {
            // NRD 라우팅: primary lobe 캡처 (ClosestHit 에서 설정)
            nrdPrimaryLobeIsSpec = payload.primaryLobeIsSpec;

            float t = payload.hitDist;
            // 뷰 공간 Z: 레이 방향과 광축(camForward) 사이 코사인 보정
            // unnorm 길이의 역수 = cos(시야각) → viewZ = t / length(unnorm)
            float2 pxUV  = ((float2)idx + 0.5f) / (float2)dim;
            float2 pxNDC = float2(pxUV.x * 2.0f - 1.0f, 1.0f - pxUV.y * 2.0f);
            float3 unnorm = camForward
                          + camRight * (pxNDC.x * aspectRatio * tanHalfFovY)
                          + camUp    * (pxNDC.y * tanHalfFovY);
            float viewZ = t / length(unnorm);  // view-space Z (cos(θ) 보정)
            nrdViewZ = viewZ;  // NRD IN_VIEWZ 출력용
            const float nearZ = 0.1f, farZ = 1000.0f;
            float ndcZ = saturate(farZ * (viewZ - nearZ) / (viewZ * (farZ - nearZ)));
            g_depth[idx] = ndcZ;  // 항상 기록 (DLSS + A-trous 공용)

            // world-법선(xyz) + roughness(w): 항상 u4(g_normals)에 기록 — DLSS-RR Packed 모드 + A-trous 공용
            // DLSS-RR 사양: normals.xyz = world-space normal, normals.w = roughness (Packed Roughness)
            // ※ oct-encoding을 .xy 에 넣으면 DLSS-RR이 (octX, octY, 0) 을 잘못된 3D 법선으로 해석하여
            //    구 표면에 octahedral seam(다이아몬드 십자) 아티팩트 발생.
            g_normals[idx] = float4(payload.hitNormal, payload.hitRoughness);

            // DLSS-RR GBuffer: diffuse albedo + specular F0 (Schlick 모델)
            //   diffuse  = albedo · (1 - metallic)         (금속은 diffuse 가 거의 0)
            //   specular = lerp(0.04, albedo, metallic)    (유전체 F0=0.04, 금속 F0=albedo tint)
            const float3 dielectricF0 = float3(0.04f, 0.04f, 0.04f);
            float3 diff = payload.hitAlbedo * (1.0f - payload.hitMetallic);
            float3 spec = lerp(dielectricF0, payload.hitAlbedo, payload.hitMetallic);
            g_diffAlbedo[idx] = float4(diff, 1.0f);
            g_specAlbedo[idx] = float4(spec, 1.0f);

            if (isDLSSMode)
            {
                // DLSS: 렌더 해상도 픽셀 단위 모션벡터
                // ※ DLSS 사양: MV 는 표면의 모션만 표현해야 하며 jitter 오프셋을 포함하면 안 된다.
                //    hitW 는 jittered ray 가 hit한 월드 포인트라 prevUV ≈ (idx+0.5+jitter)/dim
                //    이 된다. 따라서 (prevUV - (idx+0.5)/dim)*dim 은 정적 씬에서도 ≈ jitter 라는
                //    가짜 모션을 생성 → RR 이 매 프레임 다른 위치로 재학습해 "종이 구겨짐"
                //    아티팩트 유발. mv 에서 현재 jitter 를 빼주어 정적 씬에서 mv≈0 보장.
                float3 hitW   = ray.Origin + ray.Direction * t;
                float3 pLocal = hitW - prevCamPos;
                float  pz     = dot(pLocal, prevCamForward);
                float2 mv     = float2(0.0f, 0.0f);
                if (pz > 0.001f)
                {
                    float px = dot(pLocal, prevCamRight);
                    float py = dot(pLocal, prevCamUp);
                    float prevNdcX = px / (pz * prevAspectRatio  * prevTanHalfFovY);
                    float prevNdcY = py / (pz * prevTanHalfFovY);
                    float2 prevUV  = float2(prevNdcX * 0.5f + 0.5f,
                                            1.0f - (prevNdcY * 0.5f + 0.5f));
                    float2 currUV  = ((float2)idx + 0.5f) / (float2)dim;
                    mv = (prevUV - currUV) * (float2)dim - float2(jitterX, jitterY);
                }
                g_motionVec[idx] = mv;
            }
            // 비DLSS에서는 u3(motionVec) 미사용. 슬롯 3·4가 같은 m_gbufferNormal 리소스를
            // 가리키므로 u3에 별도 기록 시 u4(world-normal+roughness)와 race condition 발생.
        }

        // NRD: secondary hit 거리 (bounce 1) 캡처 — primary hit 거리 제외
        //   ClosestHit 에서 depth==1 일 때 payload.firstBounceHitDist 에 RayTCurrent() 기록함
        if (bounce == 1u)
        {
            nrdFirstBounceHitDist = payload.firstBounceHitDist;
        }

        // ── Phase 7 — Spatial Reservoir Reuse (bounce == 0, primary hit) ───
        //   ※ terminated / delta lobe (유리, scatterPdf=0) 픽셀은 reservoir 무효화 처리.
        //      안 그러면 다음 frame 이웃이 garbage 또는 1/eps 폭주값 읽음.
        //   Tier 1 = write only (자체 샘플), Tier 2/3 = prev frame neighbor 와 RIS combine.
        //   throughput 보정은 diffuse approximation (Lambertian) — Tier 2/3 = flat low-importance
        //   영역으로 대부분 diffuse 라 bias 가 시각적으로 작다. Tier 1 (specular) 은 교체 없음.
        bool primaryReservoirValid = (bounce == 0u) &&
                                     (reservoirEnabled != 0u) &&
                                     (payload.terminated == 0u) &&
                                     (payload.scatterPdf > 0.0f);
        if (bounce == 0u && reservoirEnabled != 0u && !primaryReservoirValid)
        {
            // emissive 직격 / miss / 유리 등 → reservoir 무효화 (M=0)
            //   다음 frame 이웃이 이 픽셀 읽어도 `nbr.M==0` 분기에서 스킵.
            StoreReservoir(idx, EmptyReservoir());
        }

        if (payload.terminated != 0u) break;

        if (primaryReservoirValid)
        {
            // 자체 first-bounce 샘플 정보
            float3 N      = payload.hitNormal;
            float3 ownDir = payload.nextDirection;
            float  ownPdf = max(payload.scatterPdf, 1e-6f);   // 0 = delta(유리) → 사실상 비활성
            uint   ownOct = OctPack32(ownDir);
            float  p_own  = ReservoirTargetPdf(N, ownDir);

            // 초기 reservoir = own sample (Bitterli 2020 Algorithm 5 / 6)
            //   own: M_inc=1, w = p̂_own / pdf_own
            Reservoir r;
            r.dirOct = ownOct;
            r.W      = 0.0f;
            r.M      = 0u;
            float wSum = 0.0f;
            UpdateReservoir(r, ownOct, 1u, p_own / ownPdf, seed, wSum);

            // Tier 2/3 + reservoirReset=0 + delta lobe 아님 + prev frame valid
            bool doReuse = (primaryTier >= 2u) &&
                           (reservoirReset == 0u) &&
                           (payload.scatterPdf > 0.0f);
            if (doReuse)
            {
                // Tier 2: 3×3 radius=1, 1 neighbor
                // Tier 3: 5×5 radius=2, 3 neighbors
                int  radius = (primaryTier == 2u) ? 1 : 2;
                uint nCount = (primaryTier == 2u) ? 1u : 3u;
                for (uint k = 0u; k < nCount; ++k)
                {
                    int2 off = int2(
                        (int)(RandFloat(seed) * (float)(2 * radius + 1)) - radius,
                        (int)(RandFloat(seed) * (float)(2 * radius + 1)) - radius);
                    if (off.x == 0 && off.y == 0) continue;
                    int2 npx = int2((int2)idx + off);
                    int2 idim = int2((int2)dim);
                    if (npx.x < 0 || npx.y < 0 || npx.x >= idim.x || npx.y >= idim.y) continue;

                    Reservoir nbr = LoadReservoir((uint2)npx);
                    if (nbr.M == 0u || nbr.W <= 0.0f) continue;

                    float3 nbrDir = OctUnpack32(nbr.dirOct);
                    float  p_nbr  = ReservoirTargetPdf(N, nbrDir);
                    if (p_nbr <= 0.0f) continue;

                    // CombineReservoir: M_inc = nbr.M, w = p̂_at_us · nbr.W · nbr.M
                    float w_n = p_nbr * nbr.W * (float)nbr.M;
                    UpdateReservoir(r, nbr.dirOct, nbr.M, w_n, seed, wSum);
                }
            }

            // 최종 W = w_sum / (M · p̂_chosen) — capping 전 uncapped M 사용해야 estimator 수렴
            float3 chosenDir = OctUnpack32(r.dirOct);
            float  p_chosen  = ReservoirTargetPdf(N, chosenDir);
            r.W = (p_chosen > 0.0f && r.M > 0u) ? (wSum / ((float)r.M * p_chosen)) : 0.0f;

            // 저장 직전 M cap (history 무한 증대 방지)
            //   다음 frame neighbor 가 nbr.M 으로 곱셈할 때 폭주 방지.
            //   W 는 이미 계산됐으므로 cap 의 영향 없음 (Bitterli 2020 권장).
            r.M = min(r.M, reservoirMCap);

            // 현재 frame reservoir 출력 (다음 frame 의 neighbor 가 읽음)
            StoreReservoir(idx, r);

            // 방향 교체: Tier 2/3 에서 chosen != own 일 때만 적용
            //   diffuse approximation throughput 보정: albedo·(1-metallic)·NoL_chosen·W / π
            //   (path tracer ClosestHit 의 풀 BRDF·MIS attenuation 을 단순 Lambertian 으로 대체)
            //   ※ Tier 1 / glass / Tier 2-3 의 specular 영역에는 영향 없음 (else 분기로 own 유지)
            bool chosenIsOwn = (r.dirOct == ownOct);
            if (doReuse && !chosenIsOwn && r.W > 0.0f)
            {
                float NoL_c = saturate(dot(N, chosenDir));
                float3 diffAlbedo = payload.hitAlbedo * (1.0f - payload.hitMetallic);
                throughput *= diffAlbedo * NoL_c * r.W * INV_PI;
                payload.nextDirection = chosenDir;
            }
            else
            {
                throughput *= payload.attenuation;  // 평소 경로 (Tier 1 또는 chosen == own)
            }
        }
        else
        {
            throughput *= payload.attenuation;
        }

        // Russian Roulette (bounce 3부터)
        if (bounce >= 3u)
        {
            float rrProb = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.01f, 1.0f);
            if (RandFloat(seed) > rrProb) break;
            throughput /= rrProb;
        }

        ray.Origin    = payload.nextOrigin;
        ray.Direction = payload.nextDirection;
        ray.TMin      = 0.001f;
        ray.TMax      = 1e6f;
    }

    // ── NRD 입력 버퍼 출력 (probabilistic lobe routing) ─────────────
    //   1spp 에서 primary hit lobe 가 spec 이면 spec 버퍼로, 아니면 diff 버퍼로.
    //   다른 버퍼는 (0, hitDist) 로 — NRD HitDistanceReconstructionMode::AREA_3X3 가
    //   neighbor 에서 보간한다. radiance 에 firefly clamp 한 번 더 적용 (NRD 안정성).
    {
        float3 nrdRadiance = radiance;
        float  lum         = dot(nrdRadiance, float3(0.2126f, 0.7152f, 0.0722f));
        const float maxLumNRD = 16.0f;
        if (lum > maxLumNRD) nrdRadiance *= (maxLumNRD / lum);

        float diffHitDist = (nrdPrimaryLobeIsSpec == 0u) ? nrdFirstBounceHitDist : 0.0f;
        float specHitDist = (nrdPrimaryLobeIsSpec == 1u) ? nrdFirstBounceHitDist : 0.0f;
        float3 diffRad    = (nrdPrimaryLobeIsSpec == 0u) ? nrdRadiance : float3(0.0f, 0.0f, 0.0f);
        float3 specRad    = (nrdPrimaryLobeIsSpec == 1u) ? nrdRadiance : float3(0.0f, 0.0f, 0.0f);

        g_diffRadianceHitDist[idx] = float4(diffRad, diffHitDist);
        g_specRadianceHitDist[idx] = float4(specRad, specHitDist);
        g_viewZ[idx]               = nrdViewZ;
    }

    // 시간적 누적 (프레임당 1spp)
    float3 accumulated;
    if (frameCount == 0u)
    {
        // DLSS-RR 입력 분산 안정화: firefly clamping
        //   1spp brute-force MIS PT 의 specular/caustics 경로에서 발생하는 극단값(>16)을
        //   휘도 기준으로 클램프. 본질적 노이즈는 줄지 않지만 매 프레임 튀는 극단값이
        //   사라져 RR transformer 의 history rejection 빈도가 감소 → paper crumpled 완화.
        //   (NVIDIA RTXPT 샘플도 firefly 가드를 권장)
        float lum = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
        const float maxLum = 16.0f;
        if (lum > maxLum) radiance *= (maxLum / lum);
        accumulated = radiance;
    }
    else
        accumulated = lerp(g_accumulation[idx].rgb, radiance, 1.0f / float(frameCount + 1u));

    g_accumulation[idx] = float4(accumulated, 1.0f);

    // Reinhard 톤매핑 + 감마 보정
    float3 color = accumulated / (accumulated + 1.0f);
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    g_output[idx] = float4(color, 1.0f);
}

// ── Miss[0]: 환경광 ─────────────────────────────────────────────
[shader("miss")]
void MissShader(inout RayPayload payload)
{
    // payload.hitDist 는 RayGen 에서 ray.TMax 로 미리 초기화됨 → 별도 기록 불필요

    float3 d = normalize(WorldRayDirection());

    if (sceneID == 0)
    {
        float  t   = saturate(0.5f * (d.y + 1.0f));
        float3 sky = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.5f, 0.7f, 1.0f), t);
        float3 sunDir = normalize(float3(1.0f, 2.0f, -0.5f));
        float  s   = max(0.0f, dot(d, sunDir));
        sky += float3(1.0f, 0.9f, 0.7f) * pow(s, 128.0f);
        payload.emission = sky;
    }
    else
    {
        payload.emission = float3(0.01f, 0.01f, 0.015f);
    }
    payload.terminated = 1u;
}

// ── Miss[1]: 그림자 레이 ─────────────────────────────────────────
[shader("miss")]
void MissShadow(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
}

// ── ClosestHit: GGX BRDF + MIS NEE ─────────────────────────────
[shader("closesthit")]
void ClosestHit(inout RayPayload payload,
                BuiltInTriangleIntersectionAttributes attr)
{
    uint rawID    = InstanceID();
    uint geomType = rawID & 0xFu;
    uint matIdx   = rawID >> 4u;

    uint primIdx = PrimitiveIndex();
    float2 bary  = attr.barycentrics;
    float3 b     = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    uint vi = primIdx * 3;

    float3 n0, n1, n2;
    if (geomType == 0u)
    {
        n0 = g_vbPlane[vi].normal;  n1 = g_vbPlane[vi+1].normal;  n2 = g_vbPlane[vi+2].normal;
    }
    else if (geomType == 2u)
    {
        n0 = g_vbRoom[vi].normal;   n1 = g_vbRoom[vi+1].normal;   n2 = g_vbRoom[vi+2].normal;
    }
    else if (geomType == 3u)
    {
        n0 = g_vbSphere[vi].normal; n1 = g_vbSphere[vi+1].normal; n2 = g_vbSphere[vi+2].normal;
    }
    else
    {
        n0 = g_vbCube[vi].normal;   n1 = g_vbCube[vi+1].normal;   n2 = g_vbCube[vi+2].normal;
    }

    float3 rawN = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
    float3 V    = -normalize(WorldRayDirection());
    float3 N    = dot(rawN, V) < 0.0f ? -rawN : rawN;
    bool entering = dot(rawN, V) >= 0.0f;

    float3 hitPos    = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 albedo    = matAlbedoRoughness[matIdx].xyz;
    float  metallic  = matMetallic[matIdx];
    float  roughness = matAlbedoRoughness[matIdx].w;
    float  emissive  = matEmissive[matIdx];

    // NRD: primary→secondary hitDist 는 depth==1 의 RayTCurrent (primary hit 거리 제외)
    if (payload.depth == 1u && payload.firstBounceHitDist == 0.0f) {
        payload.firstBounceHitDist = RayTCurrent();
    }

    if (payload.depth == 0u) {
        payload.hitDist      = RayTCurrent();
        payload.hitNormal    = N;  // 뷰-향 보정 후 법선 (A-trous 엣지 스토핑용)

        // DLSS-RR demodulation 인코딩 (재질별 분기)
        //   RR 은 raw HDR 을 (diff + spec·F) 로 demodulate 한 뒤 transformer 로 lighting 만
        //   디노이즈한다. diffuse 가정 모델과 specular(mirror) 가정 모델이 달라서
        //   transmissive 표면은 metallic+low-roughness 로 보내 specular 경로를 타게 해야
        //   굴절·반사가 temporal blur 없이 보존된다.
        if (emissive < -1.5f) {
            // 유리(투명): metallic=1, roughness=0 → RR specular(mirror) temporal model 활성화
            //   F0=albedo, diff=0 이 되며 굴절된 raw HDR 이 metal 경로로 보존된다.
            payload.hitAlbedo    = albedo;
            payload.hitMetallic  = 1.0f;
            payload.hitRoughness = 0.0f;
        } else if (emissive < -0.5f) {
            // 반투명: diff 를 낮춰 demodulation 영향을 줄이고 spec=0.04 dielectric 유지
            //   diff=albedo 전체면 RR 이 디퓨즈 표면처럼 temporal blur → 불투명화
            payload.hitAlbedo    = albedo * 0.3f;
            payload.hitMetallic  = 0.0f;
            payload.hitRoughness = roughness;
        } else {
            // 일반 PBR (불투명·발광체): 원래 값 그대로
            payload.hitAlbedo    = albedo;
            payload.hitMetallic  = metallic;
            payload.hitRoughness = roughness;
        }
    }

    uint seed = payload.seed;

    // ── 유리 / 반투명 (기존 로직 유지, scatterPdf=0 표기) ────────
    if (emissive < -0.5f)
    {
        if (emissive < -1.5f)
        {
            // 유리 (Dielectric, IOR=1.5)
            float ior = 1.5f;
            float eta = entering ? (1.0f / ior) : ior;
            float3 inc     = normalize(WorldRayDirection());
            float cosTheta = max(dot(N, V), 0.0f);

            float r0 = (1.0f - ior) / (1.0f + ior);
            r0 = r0 * r0;
            float fresnel = r0 + (1.0f - r0) * pow(1.0f - cosTheta, 5.0f);

            float3 refracted = refract(inc, N, eta);
            bool   tir       = dot(refracted, refracted) < 0.0001f;

            float3 scatterDir = (tir || RandFloat(seed) < fresnel)
                ? reflect(inc, N)
                : refracted;

            // NRD: 유리는 specular(굴절/반사) 경로 — primary 라우팅 spec
            if (payload.depth == 0u) payload.primaryLobeIsSpec = 1u;

            payload.emission      = float3(0.0f, 0.0f, 0.0f);
            payload.attenuation   = albedo;
            payload.nextOrigin    = hitPos + scatterDir * 0.001f;
            payload.nextDirection = scatterDir;
            payload.scatterPdf    = 0.0f;
            payload.terminated    = 0u;
            payload.seed          = seed;
            return;
        }
        else
        {
            // 반투명 (확산 50% + 투과 50%)
            float3 scatterDir;
            float3 atten;

            if (entering)
            {
                if (RandFloat(seed) < 0.5f)
                {
                    float2 u   = float2(RandFloat(seed), RandFloat(seed));
                    scatterDir = CosineSampleHemisphere(u, N);
                    atten      = albedo;
                }
                else
                {
                    scatterDir = normalize(WorldRayDirection());
                    atten      = albedo * 0.85f;
                }
                // 반투명 표면 직접광 (단순 근사)
                float3 toL   = lightPos - hitPos;
                float  dist  = max(length(toL), 0.01f);
                float3 L     = toL / dist;
                float  NdotL = max(dot(N, L), 0.0f);
                float  attn  = lightIntensity / (dist * dist);
                float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
                payload.emission = albedo * 0.5f * NdotL * lightColor * attn * vis;
            }
            else
            {
                scatterDir       = normalize(WorldRayDirection());
                atten            = albedo * 0.90f;
                payload.emission = float3(0.0f, 0.0f, 0.0f);
            }

            // NRD: 반투명은 diffuse-dominant 경로
            if (payload.depth == 0u) payload.primaryLobeIsSpec = 0u;

            payload.attenuation   = atten;
            payload.nextOrigin    = hitPos + scatterDir * 0.001f;
            payload.nextDirection = scatterDir;
            payload.scatterPdf    = 0.0f;
            payload.terminated    = 0u;
            payload.seed          = seed;
            return;
        }
    }

    // ── 발광체 처리 (MIS 가중치) ─────────────────────────────────
    if (emissive > 0.0f)
    {
        // NRD: primary 히트가 발광체면 diffuse-emission 으로 라우팅
        if (payload.depth == 0u) payload.primaryLobeIsSpec = 0u;
        // 면적광(emissive box)을 BRDF 샘플로 간접 적중:
        // NEE가 이미 직접광을 계산했으므로 MIS 가중치로 이중 계산 방지
        if (emissBoxHalfSize > 0.0f && geomType == 1u
            && payload.depth > 0u && payload.scatterPdf > 0.0f)
        {
            float dist      = RayTCurrent();
            float dist2     = dist * dist;
            float3 inc      = normalize(WorldRayDirection());
            float faceNdotL = max(dot(rawN, -inc), 0.0001f);
            float totalArea = 24.0f * emissBoxHalfSize * emissBoxHalfSize;
            float pdf_light = dist2 / (faceNdotL * totalArea);
            float w         = MISWeight(payload.scatterPdf, pdf_light);
            payload.emission = w * albedo * emissive;
        }
        else
        {
            payload.emission = albedo * emissive;
        }
        payload.terminated = 1u;
        payload.seed       = seed;
        return;
    }

    // ── NEE 직접광 ───────────────────────────────────────────────
    payload.emission = DirectLighting(hitPos, N, V, albedo, metallic, roughness, seed);

    // ── BRDF 샘플링 (GGX + Lambertian 혼합) ─────────────────────
    float3 attenuation;
    float  scatterPdf;
    uint   isSpecLobe;
    float3 scatterDir = SampleBRDF(N, V, albedo, metallic, roughness,
                                   seed, attenuation, scatterPdf, isSpecLobe);

    // NRD: primary hit (depth==0) 의 lobe 선택을 기록하여 RayGen 에서
    //      radiance 를 diff/spec 버퍼 중 하나로 라우팅하게 한다.
    if (payload.depth == 0u)
    {
        payload.primaryLobeIsSpec = isSpecLobe;
    }

    if (dot(scatterDir, N) <= 0.0f)
    {
        payload.terminated = 1u;
    }
    else
    {
        payload.attenuation   = attenuation;
        payload.nextOrigin    = hitPos + N * 0.001f;
        payload.nextDirection = scatterDir;
        payload.scatterPdf    = scatterPdf;
        payload.terminated    = 0u;
    }

    payload.seed = seed;
}
