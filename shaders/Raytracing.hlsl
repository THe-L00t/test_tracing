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
RWTexture2D<float>              g_fresnel      : register(u2);  // Fresnel 우선도 맵 F(p)
RWTexture2D<float>              g_depth        : register(u3);  // GBuffer: 1차 레이 히트 거리
RWTexture2D<float4>             g_normal       : register(u4);  // GBuffer: 표면 법선 (N*0.5+0.5)
RWTexture2D<float>              g_accumSq      : register(u5);  // 휘도² 누적 평균 (분산 계산용)
RaytracingAccelerationStructure g_tlas         : register(t0);

struct VertexPN { float3 pos; float3 normal; };
StructuredBuffer<VertexPN> g_vbPlane   : register(t1);
StructuredBuffer<VertexPN> g_vbCube    : register(t2);
StructuredBuffer<VertexPN> g_vbRoom    : register(t3);
StructuredBuffer<VertexPN> g_vbSphere  : register(t4);

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

    uint   frameCount;  uint   randomSeed;  float  emissBoxHalfSize; float fresnelThreshold;
    float3 emissBoxCenter;  uint  debugMode;   // 0=PT,1=Fresnel,2=Var,3=DepthEdge,4=NormalEdge,5=ThresholdMask
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
                  out float3 attenuation, out float pdf)
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

// ── GBuffer Sobel 엣지 검출 헬퍼 ────────────────────────────────
// 인접 픽셀은 이전 프레임 데이터를 읽음 (정적 씬에서는 동일, 1프레임 지연 허용)

float SampleDepthC(int2 p, int2 mx)
{
    return g_depth[uint2(clamp(p, int2(0, 0), mx))];
}
float3 SampleNormalC(int2 p, int2 mx)
{
    return g_normal[uint2(clamp(p, int2(0, 0), mx))].xyz * 2.0f - 1.0f;
}

// 상대 깊이 기준 Sobel (카메라 거리 무관)
float DepthSobel(uint2 c, uint2 dim)
{
    int2  ic  = int2(c);
    int2  mx  = int2(dim) - 1;
    float ctr = max(SampleDepthC(ic, mx), 0.001f);

    float d00 = SampleDepthC(ic + int2(-1,-1), mx) / ctr;
    float d10 = SampleDepthC(ic + int2( 0,-1), mx) / ctr;
    float d20 = SampleDepthC(ic + int2( 1,-1), mx) / ctr;
    float d01 = SampleDepthC(ic + int2(-1, 0), mx) / ctr;
    float d21 = SampleDepthC(ic + int2( 1, 0), mx) / ctr;
    float d02 = SampleDepthC(ic + int2(-1, 1), mx) / ctr;
    float d12 = SampleDepthC(ic + int2( 0, 1), mx) / ctr;
    float d22 = SampleDepthC(ic + int2( 1, 1), mx) / ctr;

    float gx = -d00 + d20 - 2.0f*d01 + 2.0f*d21 - d02 + d22;
    float gy = -d00 - 2.0f*d10 - d20 + d02 + 2.0f*d12 + d22;
    return saturate(sqrt(gx*gx + gy*gy) * 2.0f);
}

// 인접 법선 각도 차 최댓값 기반 엣지
float NormalSobel(uint2 c, uint2 dim)
{
    int2   ic      = int2(c);
    int2   mx      = int2(dim) - 1;
    float3 n       = SampleNormalC(ic, mx);
    float  maxDiff = 0.0f;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        float3 nb  = SampleNormalC(ic + int2(dx, dy), mx);
        maxDiff = max(maxDiff, 1.0f - saturate(dot(n, nb)));
    }
    return saturate(maxDiff * 4.0f);
}

// ── RayGen – 균일 1spp 패스 트레이싱 ───────────────────────────
[shader("raygeneration")]
void RayGen()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    uint seed = WangHash(idx.x + idx.y * dim.x + randomSeed * 719393u);

    // 서브픽셀 지터 (안티에일리어싱)
    float2 jitter = float2(RandFloat(seed), RandFloat(seed)) - 0.5f;
    float2 uv     = ((float2)idx + 0.5f + jitter) / (float2)dim;
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

    for (uint bounce = 0u; bounce < k_maxBounce; bounce++)
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

        TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

        seed = payload.seed;
        radiance += throughput * payload.emission;

        if (payload.terminated != 0u) break;

        throughput *= payload.attenuation;

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

    // 시간적 누적 (프레임당 1spp)
    float3 prevAccum   = g_accumulation[idx].rgb;
    float  prevAccumSq = g_accumSq[idx];

    static const float3 k_lum = float3(0.299f, 0.587f, 0.114f);
    float lumNew = dot(radiance, k_lum);

    float3 accumulated;
    float  accumulatedSq;
    if (frameCount == 0u)
    {
        accumulated   = radiance;
        accumulatedSq = lumNew * lumNew;
    }
    else
    {
        float w       = 1.0f / float(frameCount + 1u);
        accumulated   = lerp(prevAccum,   radiance,        w);
        accumulatedSq = lerp(prevAccumSq, lumNew * lumNew, w);
    }

    g_accumulation[idx] = float4(accumulated, 1.0f);
    g_accumSq[idx]      = accumulatedSq;

    // g_output 출력 (debugMode에 따라 분기)
    if (debugMode == 1u)
    {
        // Fresnel 우선도 맵
        float v = g_fresnel[idx];
        g_output[idx] = float4(v, v, v, 1.0f);
    }
    else if (debugMode == 2u)
    {
        // 통계적 분산: E[X²] - E[X]²  (수렴할수록 정확)
        // Reinhard soft saturation: sigma / (sigma + 0.05)
        //   sigma=0    → 0.0 (완전 수렴, 검정)
        //   sigma=0.05 → 0.5 (중간 노이즈, 회색)
        //   sigma=0.5  → 0.91 (고노이즈, 밝은 회색)
        //   포화 없음 → 낮은/높은 분산 영역 구분 가능
        float lumMean = dot(accumulated, k_lum);
        float sigma   = sqrt(max(accumulatedSq - lumMean * lumMean, 0.0f));
        float v       = sigma / (sigma + 0.5f);
        g_output[idx] = float4(v, v, v, 1.0f);
    }
    else if (debugMode == 3u)
    {
        // Depth edge (상대 깊이 Sobel, 1프레임 지연)
        float v = DepthSobel(idx, dim);
        g_output[idx] = float4(v, v, v, 1.0f);
    }
    else if (debugMode == 4u)
    {
        // Normal edge (법선 각도 차, 1프레임 지연)
        float v = NormalSobel(idx, dim);
        g_output[idx] = float4(v, v, v, 1.0f);
    }
    else if (debugMode == 5u)
    {
        // Threshold mask: PT 출력 위에 F(p) > threshold 픽셀을 붉게 강조
        float3 color = accumulated / (accumulated + 1.0f);
        color = pow(max(color, 0.0f), 1.0f / 2.2f);
        float fp = g_fresnel[idx];
        if (fp > fresnelThreshold)
            color = lerp(color, float3(1.0f, 0.15f, 0.0f), 0.75f);
        g_output[idx] = float4(color, 1.0f);
    }
    else
    {
        // 표준 PT: Reinhard 톤매핑 + 감마 보정
        float3 color = accumulated / (accumulated + 1.0f);
        color = pow(max(color, 0.0f), 1.0f / 2.2f);
        g_output[idx] = float4(color, 1.0f);
    }
}

// ── Miss[0]: 환경광 ─────────────────────────────────────────────
[shader("miss")]
void MissShader(inout RayPayload payload)
{
    // 하늘/배경 히트 → GBuffer 기본값 기록
    if (payload.depth == 0u)
    {
        uint2 px = DispatchRaysIndex().xy;
        g_fresnel[px] = 0.0f;
        g_depth  [px] = 1e5f;
        g_normal [px] = float4(normalize(WorldRayDirection()) * 0.5f + 0.5f, 1.0f);
    }

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

    uint seed = payload.seed;

    // ── F(p) + GBuffer 기록 (bounce 0에서만) ────────────────────────
    if (payload.depth == 0u)
    {
        uint2 px = DispatchRaysIndex().xy;

        float fresnelTerm    = pow(1.0f - saturate(dot(N, V)), 5.0f);
        float surfaceFactor  = saturate(1.0f - roughness);
        float dielectricFlag = (emissive < -0.5f) ? 1.0f : 0.0f;
        float specWeight     = saturate(metallic + dielectricFlag * 2.0f);
        g_fresnel[px] = fresnelTerm * surfaceFactor * specWeight;

        g_depth [px] = RayTCurrent();
        g_normal[px] = float4(N * 0.5f + 0.5f, 1.0f);
    }

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
            // 반투명 — 물리 기반 Fresnel + SSS 근사 (IOR=1.3)
            // entering: Schlick Fresnel → specular reflect 또는 내부 cosine scatter
            // exiting:  직진 투과 (내부 산란 완료 근사)
            float ior      = 1.3f;
            float r0       = (1.0f - ior) / (1.0f + ior);
            r0             = r0 * r0;
            float cosTheta = max(dot(N, V), 0.0f);
            float fresnel  = r0 + (1.0f - r0) * pow(1.0f - cosTheta, 5.0f);

            float3 scatterDir;
            float3 atten;

            if (entering && RandFloat(seed) < fresnel)
            {
                // 표면 specular 반사 (grazing angle일수록 확률 증가)
                scatterDir       = reflect(normalize(WorldRayDirection()), N);
                atten            = float3(1.0f, 1.0f, 1.0f);
                payload.emission = float3(0.0f, 0.0f, 0.0f);
            }
            else if (entering)
            {
                // 내부 진입 → SSS 근사: 내부 방향으로 cosine 산란
                float2 u   = float2(RandFloat(seed), RandFloat(seed));
                scatterDir = CosineSampleHemisphere(u, -N);
                atten      = albedo;
                // 직접광 근사 (투과 비율 반영)
                float3 toL   = lightPos - hitPos;
                float  dist  = max(length(toL), 0.01f);
                float3 L     = toL / dist;
                float  NdotL = max(dot(N, L), 0.0f);
                float  attn  = lightIntensity / (dist * dist);
                float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
                payload.emission = albedo * (1.0f - fresnel) * NdotL * lightColor * attn * vis;
            }
            else
            {
                // 내부 → 외부: 직진 투과
                scatterDir       = normalize(WorldRayDirection());
                atten            = albedo * 0.90f;
                payload.emission = float3(0.0f, 0.0f, 0.0f);
            }

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
    float3 scatterDir = SampleBRDF(N, V, albedo, metallic, roughness,
                                   seed, attenuation, scatterPdf);

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
