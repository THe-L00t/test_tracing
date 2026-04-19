// ──────────────────────────────────────────────────────────────
//  ReSTIR_GI_Initial.hlsl  –  [GI Pass 1] GI Initial Sampling
//
//  역할: 픽셀당 BRDF 샘플링으로 첫 번째 간접 바운스 경로를 추적하고,
//        x_s(첫 번째 히트 위치)와 해당 방향의 누적 Radiance를
//        GI Reservoir에 기록.
//
//  알고리즘 (Ouyang et al. 2021, ReSTIR GI):
//    For each pixel p:
//      ω_i ~ SampleBRDF(N_v, V, material)    // 첫 번째 바운스 방향
//      x_s = Trace(x_v + ε·N, ω_i)           // 첫 번째 히트
//      L_s = PathTrace(x_s, k_maxBounce)       // x_s에서 누적 Radiance
//      R_gi.samplePos    = x_s
//      R_gi.sampleNormal = N_s
//      R_gi.radiance     = L_s
//      R_gi.W = 1 / p̂(x_s)                   // 초기 비편향 가중치
//
//  레지스터:
//    입력:  G-Buffer SRV (t6-t9), LightList (t5), TLAS (t0), VBs (t1-t4)
//    출력:  gi_reservoir_cur UAV (u8)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
RaytracingAccelerationStructure g_tlas        : register(t0);
struct VertexPN { float3 pos; float3 normal; };
StructuredBuffer<VertexPN>      g_vbPlane     : register(t1);
StructuredBuffer<VertexPN>      g_vbCube      : register(t2);
StructuredBuffer<VertexPN>      g_vbRoom      : register(t3);
StructuredBuffer<VertexPN>      g_vbSphere    : register(t4);
StructuredBuffer<LightData>     g_lightList   : register(t5);
Texture2D<float4>               gbuf_worldPos : register(t6);
Texture2D<float4>               gbuf_normal   : register(t7);
Texture2D<float4>               gbuf_albedo   : register(t8);
Texture2D<float4>               gbuf_matInfo  : register(t9);

RWStructuredBuffer<GIReservoir> gi_reservoir_cur : register(u8);

cbuffer SceneConstants : register(b0)
{
    float3 camPos;       uint  sceneID;
    float3 camRight;     float tanHalfFovY;
    float3 camUp;        float aspectRatio;
    float3 camForward;   float _p0;
    float4 _lightBlock[4];
    float4 matAlbedoRoughness[4];
    float4 matMetallic;
    float4 matEmissive;
    uint   frameCount; uint randomSeed;
    float  emissBoxHalfSize; float _p1;
    float3 emissBoxCenter;   float _p2;
}
cbuffer ReSTIRConstants : register(b1)
{
    float4 _prevCam[4];
    uint   lightCount; uint candidateCount;
    uint   screenW;    uint screenH;
    uint   frameIndex; float temporalMaxM;
    uint   spatialRadius; uint spatialSamples;
    float4 _pad[2];
}

// ── 페이로드 ──────────────────────────────────────────────────
struct ShadowPayload { float vis; };

// GI 경로 추적 페이로드 (88 bytes)
struct GIPathPayload
{
    float3 emission;        // 현재 히트에서의 직접광(NEE) 기여
    float3 attenuation;     // 다음 바운스용 처리량
    float3 nextOrigin;      // 다음 레이 시작점
    float3 nextDirection;   // 다음 레이 방향
    float  scatterPdf;      // 0 = delta 산란
    uint   seed;
    uint   terminated;
    float3 firstHitPos;     // 첫 번째 히트 위치 (x_s)
    float3 firstHitNormal;  // 첫 번째 히트 법선 (N_s)
    uint   firstHitValid;   // 1 = 유효한 x_s
};

// ── 그림자 레이 (ClosestHit에서 shadow ray 발사, depth=2) ────
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

// 광원 방향 / 거리 / 복사 조도 계산
void CalcLightContrib(LightData light, float3 hitPos,
                      out float3 L, out float dist, out float3 Li)
{
    if (light.type == 1u)
    {
        L    = normalize(light.pos);
        dist = 1e6f;
        Li   = light.color * light.intensity;
    }
    else
    {
        float3 toL = light.pos - hitPos;
        dist = max(length(toL), 0.01f);
        L    = toL / dist;
        Li   = light.color * light.intensity / (dist * dist);
    }
}

// GGX Cook-Torrance BRDF 평가
float3 EvalBRDF_GI(float3 N, float3 V, float3 L,
                   float3 albedo, float metallic, float roughness)
{
    float NdotL = max(dot(N, L), 0.0f);
    if (NdotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

    float  alpha  = max(roughness * roughness, 0.001f);
    float  alpha2 = alpha * alpha;
    float  NdotV  = max(dot(N, V), 0.0001f);
    float3 VplusL = V + L;
    float3 H      = normalize(length(VplusL) > 1e-4f ? VplusL : N);
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

// Miss[0]: 간접 레이 미스 → 환경광
[shader("miss")]
void Miss_GI_Initial(inout GIPathPayload payload)
{
    float3 d = normalize(WorldRayDirection());
    float3 env;
    if (sceneID == 0)
    {
        float  t   = saturate(0.5f * (d.y + 1.0f));
        env = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.5f, 0.7f, 1.0f), t);
        float3 sunDir = normalize(float3(1.0f, 2.0f, -0.5f));
        env += float3(1.0f, 0.9f, 0.7f) * pow(max(dot(d, sunDir), 0.0f), 128.0f);
    }
    else
        env = float3(0.01f, 0.01f, 0.015f);

    payload.emission      = env;
    payload.firstHitValid = 0u;   // 히트 없음 (miss)
    payload.terminated    = 1u;
}

// Miss[1]: 그림자 레이 미스 = 광원 가시
[shader("miss")]
void MissShadow_GI_Initial(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
}

// ClosestHit_GI_Initial: 히트 포인트에서 NEE + BRDF 샘플링
// - 항상 firstHitPos / firstHitNormal 기록 (RayGen은 bounce 0에서만 읽음)
[shader("closesthit")]
void ClosestHit_GI_Initial(inout GIPathPayload payload,
                           BuiltInTriangleIntersectionAttributes attr)
{
    uint rawID    = InstanceID();
    uint geomType = rawID & 0xFu;
    uint matIdx   = rawID >> 4u;

    uint   primIdx = PrimitiveIndex();
    float2 bary    = attr.barycentrics;
    float3 b       = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    uint   vi      = primIdx * 3;

    float3 n0, n1, n2;
    if      (geomType == 0u) { n0=g_vbPlane[vi].normal;  n1=g_vbPlane[vi+1].normal;  n2=g_vbPlane[vi+2].normal;  }
    else if (geomType == 2u) { n0=g_vbRoom[vi].normal;   n1=g_vbRoom[vi+1].normal;   n2=g_vbRoom[vi+2].normal;   }
    else if (geomType == 3u) { n0=g_vbSphere[vi].normal; n1=g_vbSphere[vi+1].normal; n2=g_vbSphere[vi+2].normal; }
    else                     { n0=g_vbCube[vi].normal;   n1=g_vbCube[vi+1].normal;   n2=g_vbCube[vi+2].normal;   }

    float3 rawN  = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
    float3 rayInc = normalize(WorldRayDirection());
    float3 V_hit = -rayInc;
    bool   entering = dot(rawN, V_hit) >= 0.0f;
    float3 N     = entering ? rawN : -rawN;
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    float3 albedo    = matAlbedoRoughness[matIdx].xyz;
    float  roughness = matAlbedoRoughness[matIdx].w;
    float  metallic  = matMetallic[matIdx];
    float  emissive  = matEmissive[matIdx];
    uint   seed      = payload.seed;

    // x_s 기록 (항상 기록, RayGen이 bounce 0에서만 읽음)
    payload.firstHitPos    = hitPos;
    payload.firstHitNormal = N;
    payload.firstHitValid  = 1u;

    // 발광체: emission 반환
    if (emissive > 0.0f)
    {
        payload.emission   = albedo * emissive;
        payload.terminated = 1u;
        payload.seed       = seed;
        return;
    }

    // 유리/반투명: delta 산란 (NEE 없음)
    if (emissive < -0.5f)
    {
        float  ior  = (emissive < -1.5f) ? 1.5f : 1.3f;
        float  eta  = entering ? (1.0f / ior) : ior;
        float  cosT = max(dot(N, V_hit), 0.0f);
        float  r0   = (1.0f - ior) / (1.0f + ior); r0 *= r0;
        float  fres = r0 + (1.0f - r0) * pow(1.0f - cosT, 5.0f);
        float3 refr = refract(rayInc, N, eta);
        bool   tir  = dot(refr, refr) < 0.0001f;
        float3 sdir = (tir || RandFloat(seed) < fres) ? reflect(rayInc, N) : refr;

        payload.emission      = float3(0.0f, 0.0f, 0.0f);
        payload.attenuation   = albedo;
        payload.nextOrigin    = hitPos + sdir * 0.002f;
        payload.nextDirection = sdir;
        payload.scatterPdf    = 0.0f;
        payload.terminated    = 0u;
        payload.seed          = seed;
        return;
    }

    // 불투명: NEE (1개 랜덤 광원 샘플링, shadow ray)
    // GI 경로 내부라서 전수 조회보다 1-sample NEE가 효율적
    float3 directAtHit = float3(0.0f, 0.0f, 0.0f);
    if (lightCount > 0u)
    {
        uint li = uint(RandFloat(seed) * float(lightCount));
        li = min(li, lightCount - 1u);
        LightData ld = g_lightList[li];
        float3 L; float dist; float3 Li;
        CalcLightContrib(ld, hitPos, L, dist, Li);
        float NdotL = max(dot(N, L), 0.0f);
        if (NdotL > 0.0f)
        {
            float vis = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
            directAtHit = EvalBRDF_GI(N, V_hit, L, albedo, metallic, roughness)
                        * Li * vis * float(lightCount);
        }
    }

    float3 atten;
    float  scatterPdf;
    float3 sdir = SampleBRDF(N, V_hit, albedo, metallic, roughness,
                             seed, atten, scatterPdf);

    payload.emission      = directAtHit;
    payload.attenuation   = atten;
    payload.nextOrigin    = hitPos + N * 0.001f;
    payload.nextDirection = sdir;
    payload.scatterPdf    = scatterPdf;
    payload.terminated    = (dot(sdir, N) <= 0.0f) ? 1u : 0u;
    payload.seed          = seed;
}

// ── RayGen_GI_Initial ─────────────────────────────────────────
[shader("raygeneration")]
void RayGen_GI_Initial()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    uint  pidx = PixelIndex(idx, dim.x);

    float4 wPos   = gbuf_worldPos[idx];
    float4 matInf = gbuf_matInfo[idx];

    // 배경 / 유리 / 발광: 빈 GI Reservoir
    if (wPos.w < 0.0f || matInf.b > 0.5f)
    {
        gi_reservoir_cur[pidx] = MakeEmptyGIReservoir();
        return;
    }

    float3 hitPos    = wPos.xyz;
    float3 N         = gbuf_normal[idx].xyz;
    float3 albedo    = gbuf_albedo[idx].rgb;
    float  metallic  = gbuf_albedo[idx].a;
    float  roughness = matInf.r;
    float3 V         = normalize(camPos - hitPos);

    uint seed = WangHash(pidx ^ (frameIndex * 0x9E3779B9u) ^ (frameCount * 0x85EBCA6Bu));

    // 첫 번째 바운스 방향 샘플링
    float3 initAtten;
    float  initPdf;
    float3 initDir = SampleBRDF(N, V, albedo, metallic, roughness,
                                seed, initAtten, initPdf);

    GIReservoir R = MakeEmptyGIReservoir();

    if (dot(initDir, N) <= 0.0f)
    {
        gi_reservoir_cur[pidx] = R;
        return;
    }

    // 경로 추적 (k_maxGIBounce 바운스)
    static const uint k_maxGIBounce = 3u;

    float3 throughput    = initAtten;
    float3 accumulated   = float3(0.0f, 0.0f, 0.0f);
    bool   firstHitValid = false;
    float3 firstHitPos   = float3(0.0f, 0.0f, 0.0f);
    float3 firstHitNormal = float3(0.0f, 1.0f, 0.0f);

    RayDesc ir;
    ir.Origin    = hitPos + N * 0.001f;
    ir.Direction = initDir;
    ir.TMin      = 0.001f;
    ir.TMax      = 1e6f;

    for (uint b = 0u; b < k_maxGIBounce; b++)
    {
        GIPathPayload ip = (GIPathPayload)0;
        ip.seed = seed;
        TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ir, ip);

        seed = ip.seed;

        // 첫 번째 히트 기록
        if (b == 0u)
        {
            firstHitValid  = (ip.firstHitValid != 0u);
            firstHitPos    = ip.firstHitPos;
            firstHitNormal = ip.firstHitNormal;
        }

        accumulated += throughput * ip.emission;

        if (ip.terminated != 0u) break;

        throughput *= ip.attenuation;

        // Russian Roulette (bounce 2부터)
        if (b >= 1u)
        {
            float rrProb = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.01f, 1.0f);
            if (RandFloat(seed) > rrProb) break;
            throughput /= rrProb;
        }

        ir.Origin    = ip.nextOrigin;
        ir.Direction = ip.nextDirection;
    }

    // Firefly 억제
    float lum = dot(accumulated, float3(0.2126f, 0.7152f, 0.0722f));
    if (lum > 3.0f) accumulated *= 3.0f / lum;

    if (!firstHitValid)
    {
        // 첫 바운스가 sky miss: 환경광을 먼 거리의 "하늘 샘플"로 저장.
        // Shade에서 visibility ray가 막히지 않으면 올바른 간접광 기여.
        float pHat = EvalGITargetPDF(accumulated);
        if (pHat > 0.0f)
        {
            R.samplePos    = hitPos + initDir * 1e5f;  // 하늘 방향 먼 점
            R.sampleNormal = -initDir;                  // 시점을 향한 법선
            R.radiance     = accumulated;
            R.valid        = 1u;
            R.wSum         = pHat;
            R.M            = 1u;
            FinalizeGIReservoir(R, pHat);
        }
        gi_reservoir_cur[pidx] = R;
        return;
    }

    // GI Reservoir 생성 (M=1, 단일 초기 샘플)
    float pHat     = EvalGITargetPDF(accumulated);
    R.samplePos    = firstHitPos;
    R.sampleNormal = firstHitNormal;
    R.radiance     = accumulated;
    R.valid        = 1u;
    R.wSum         = pHat;
    R.M            = 1u;
    FinalizeGIReservoir(R, pHat);

    gi_reservoir_cur[pidx] = R;
}
