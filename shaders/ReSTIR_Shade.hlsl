// ──────────────────────────────────────────────────────────────
//  ReSTIR_Shade.hlsl  –  [Pass 5] Final Shading
//
//  역할: DI Reservoir의 선택 광원으로 shadow ray를 쏘고 직접광을 계산.
//        GI Reservoir의 x_s까지 visibility ray 1개로 간접광을 계산.
//
//  렌더링 방정식:
//    L_total = L_direct(ReSTIR DI) + L_indirect(ReSTIR GI)
//    L_direct  = f_r(V,L,N) * Li * NdotL * vis * R_di.W
//    L_indirect = f_r(V, x_s, N) * L_incoming(x_s) * vis(x_v, x_s) * R_gi.W
//
//  픽셀 분류:
//    1. 배경 (hitDist < 0)         → sky gradient
//    2. 유리 (flags == GB_GLASS)   → Fresnel 굴절/반사 레이 추적
//    3. 발광 (flags == GB_EMISSIVE)→ emission 직접 출력
//    4. 일반                       → ReSTIR 직접광 + 1-bounce 간접광
//
//  셰이더 구성:
//    RayGen_Shade      – 메인 Shade
//    MissIndirect_Shade– 간접/굴절 레이 미스 (Miss[0])
//    MissShadow_Shade  – 그림자 레이 미스  (Miss[1])
//    ClosestHit_Shade  – 간접/굴절 레이 ClosestHit (HitGroup[0])
//
//  입력:  G-Buffer SRV (t6-t9), reservoir_in SRV (t10), TLAS (t0)
//  출력:  g_output UAV (u0)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
RWTexture2D<float4>            g_output       : register(u0);
RWTexture2D<float4>            g_accumulation : register(u1);

Texture2D<float4>              gbuf_worldPos  : register(t6);
Texture2D<float4>              gbuf_normal    : register(t7);
Texture2D<float4>              gbuf_albedo    : register(t8);
Texture2D<float4>              gbuf_matInfo   : register(t9);
StructuredBuffer<Reservoir>    reservoir_in    : register(t10);  // DI Spatial 최종 결과 SRV
StructuredBuffer<GIReservoir>  gi_reservoir_in : register(t13);  // GI Spatial 최종 결과 SRV
StructuredBuffer<LightData>    g_lightList     : register(t5);

RaytracingAccelerationStructure g_tlas        : register(t0);

struct VertexPN { float3 pos; float3 normal; };
StructuredBuffer<VertexPN> g_vbPlane   : register(t1);
StructuredBuffer<VertexPN> g_vbCube    : register(t2);
StructuredBuffer<VertexPN> g_vbRoom    : register(t3);
StructuredBuffer<VertexPN> g_vbSphere  : register(t4);

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
struct ShadowPayload { float vis; };  // 4 bytes

// IndirectPayload: Path Tracer bounce 상태 전달 (ClosestHit → RayGen 루프)
// float3×4 + float + uint×2 = 60 bytes → MaxPayloadSizeInBytes = 64
struct IndirectPayload
{
    float3 emission;        // 이 hit point에서의 직접광 (NEE) + miss 환경광
    float3 attenuation;     // 다음 bounce용 BRDF 처리량
    float3 nextOrigin;
    float3 nextDirection;
    float  scatterPdf;      // 0 = delta 산란 (유리)
    uint   seed;
    uint   terminated;      // 1 = 다음 bounce 없음
};

// ── 그림자 레이 (RayGen depth1, ClosestHit depth2 모두 사용) ────
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
        0xFD, 0, 1, 1, sr, sp);  // Miss[1] = MissShadow_Shade
    return sp.vis;
}

// LightData → 방향 L, 거리 dist, 복사 조도 Li
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

// ── GGX Cook-Torrance BRDF 평가 (bounce hit 직접광용) ───────────
float3 EvalBRDF(float3 N, float3 V, float3 L,
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

// Miss[0]: 간접광/굴절 레이 미스 → 환경광
[shader("miss")]
void MissIndirect_Shade(inout IndirectPayload payload)
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
    {
        // 실내: 천장/벽이 막고 있으므로 거의 검정 (누설된 레이)
        env = float3(0.01f, 0.01f, 0.015f);
    }
    payload.emission   = env;
    payload.terminated = 1u;
}

// Miss[1]: 그림자 레이 미스 = 광원 가시
[shader("miss")]
void MissShadow_Shade(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
}

// ── ClosestHit_Shade: bounce hit point에서 NEE + BRDF 샘플링 ────
// RayGen 루프에서 iterative bounce로 호출됨 (MaxTraceRecursionDepth=2).
// 이 안에서 ShadowVis를 호출하므로 depth2가 필요.
[shader("closesthit")]
void ClosestHit_Shade(inout IndirectPayload payload,
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

    float3 rawN    = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
    float3 rayInc  = normalize(WorldRayDirection());
    float3 V_hit   = -rayInc;
    bool   entering = dot(rawN, V_hit) >= 0.0f;
    float3 N       = entering ? rawN : -rawN;  // face-forward
    float3 hitPos  = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    float3 albedo    = matAlbedoRoughness[matIdx].xyz;
    float  roughness = matAlbedoRoughness[matIdx].w;
    float  metallic  = matMetallic[matIdx];
    float  emissive  = matEmissive[matIdx];
    uint   seed      = payload.seed;

    // ── 발광체: emission 반환 후 종료 ──────────────────────────────
    if (emissive > 0.0f)
    {
        payload.emission   = albedo * emissive;
        payload.terminated = 1u;
        payload.seed       = seed;
        return;
    }

    // ── 유리/반투명: Fresnel 굴절·반사, delta 산란 ──────────────────
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

    // ── 불투명 표면: NEE (shadow ray) + BRDF 샘플링 ─────────────────
    float3 directAtHit = float3(0.0f, 0.0f, 0.0f);
    for (uint li = 0u; li < lightCount; li++)
    {
        LightData ld = g_lightList[li];
        float3 L; float dist; float3 Li;
        CalcLightContrib(ld, hitPos, L, dist, Li);
        float NdotL = max(dot(N, L), 0.0f);
        if (NdotL <= 0.0f) continue;
        float vis = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);  // depth 2
        directAtHit += EvalBRDF(N, V_hit, L, albedo, metallic, roughness) * Li * vis;
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

// ── RayGen_Shade ──────────────────────────────────────────────
[shader("raygeneration")]
void RayGen_Shade()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    uint   pidx  = PixelIndex(idx, dim.x);
    float4 wPos  = gbuf_worldPos[idx];

    // ── 배경 픽셀: sky gradient ──────────────────────────────────
    if (wPos.w < 0.0f)
    {
        float2 uv  = ((float2)idx + 0.5f) / (float2)dim;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float3 dir = normalize(camForward
                               + camRight * (ndc.x * aspectRatio * tanHalfFovY)
                               + camUp    * (ndc.y * tanHalfFovY));

        float  t   = saturate(0.5f * (dir.y + 1.0f));
        float3 sky = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.5f, 0.7f, 1.0f), t);
        if (sceneID == 0)
        {
            float3 sunDir = normalize(float3(1.0f, 2.0f, -0.5f));
            sky += float3(1.0f, 0.9f, 0.7f) * pow(max(dot(dir, sunDir), 0.0f), 128.0f);
        }
        float3 c = sky / (sky + 1.0f);
        c = pow(max(c, 0.0f), 1.0f / 2.2f);
        g_output[idx] = float4(c, 1.0f);
        return;
    }

    float4 matInf   = gbuf_matInfo[idx];
    float3 hitPos   = wPos.xyz;
    float4 normData = gbuf_normal[idx];
    float3 N        = normData.xyz;
    float3 albedo   = gbuf_albedo[idx].rgb;
    float  metallic = gbuf_albedo[idx].a;
    float  roughness= matInf.r;
    float  flags    = matInf.b;
    float3 V        = normalize(camPos - hitPos);  // 시점 방향 (공통)

    // ── 발광 픽셀: emission 직접 출력 ──────────────────────────────
    if (flags > 1.5f)  // GB_FLAG_EMISSIVE
    {
        uint  matIdx  = uint(normData.w + 0.5f);
        float emissive = matEmissive[matIdx];
        float3 em = albedo * emissive;
        float3 c  = em / (em + 1.0f);
        c = pow(max(c, 0.0f), 1.0f / 2.2f);
        g_output[idx] = float4(c, 1.0f);
        return;
    }

    // ── 직접광: ReSTIR DI (불투명 픽셀만, Bitterli 2020 Eq.5) ──────
    float3 directLight = float3(0.0f, 0.0f, 0.0f);
    if (flags < 0.5f)  // GB_FLAG_NORMAL
    {
        Reservoir R = reservoir_in[pidx];
        if (R.lightIdx != 0xFFFFFFFFu && R.W > 0.0f)
        {
            LightData light = g_lightList[R.lightIdx];
            float3 L; float dist; float3 Li;
            CalcLightContrib(light, hitPos, L, dist, Li);
            float NdotL = max(dot(N, L), 0.0f);
            if (NdotL > 0.0f)
            {
                float vis = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
                directLight = EvalBRDF(N, V, L, albedo, metallic, roughness)
                              * Li * vis * R.W;
            }
        }
    }

    // ── 간접광: ReSTIR GI Reservoir 연결 레이 ─────────────────────
    // GI Reservoir의 x_s까지 가시성 레이 1개 발사.
    // directLight=0인 유리 픽셀은 별도 refraction 처리.
    uint   seed      = WangHash(pidx ^ (frameIndex * 0x9E3779B9u) ^ (frameCount * 0x517CC1B7u));
    float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

    if (flags > 0.5f && flags < 1.5f)  // GB_FLAG_GLASS: 굴절/반사 경로 추적
    {
        // matIdx → emissive 값으로 IOR 복원 (GBuffer 기록 방식과 동일)
        uint   matIdxGlass = uint(normData.w + 0.5f);
        float  emissGlass  = matEmissive[matIdxGlass];
        float  ior         = (emissGlass < -1.5f) ? 1.5f : 1.3f;
        float3 inc  = -V;
        float  cosT = max(dot(N, V), 0.0f);
        float  r0   = (1.0f - ior) / (1.0f + ior); r0 *= r0;
        float  fres = r0 + (1.0f - r0) * pow(1.0f - cosT, 5.0f);
        float3 refr = refract(inc, N, 1.0f / ior);
        bool   tir  = dot(refr, refr) < 0.0001f;
        float3 sdir = (tir || RandFloat(seed) < fres) ? reflect(inc, N) : refr;

        RayDesc ir;
        ir.Origin    = hitPos + sdir * 0.002f;
        ir.Direction = sdir;
        ir.TMin      = 0.001f;
        ir.TMax      = 1e6f;

        float3 throughput = albedo;
        for (uint b = 0u; b < 4u; b++)
        {
            IndirectPayload ip = (IndirectPayload)0;
            ip.seed = seed;
            TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ir, ip);
            seed = ip.seed;
            indirectLight += throughput * ip.emission;
            if (ip.terminated != 0u) break;
            throughput *= ip.attenuation;
            float rrProb = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.01f, 1.0f);
            if (b >= 1u && RandFloat(seed) > rrProb) break;
            if (b >= 1u) throughput /= rrProb;
            ir.Origin    = ip.nextOrigin;
            ir.Direction = ip.nextDirection;
        }
    }
    else if (flags < 0.5f)  // GB_FLAG_NORMAL: GI Reservoir로 간접광
    {
        GIReservoir gi = gi_reservoir_in[pidx];
        if (gi.valid != 0u && gi.W > 0.0f)
        {
            float3 toSample = gi.samplePos - hitPos;
            float  dist     = max(length(toSample), 0.001f);
            float3 dir      = toSample / dist;
            float  NdotL    = max(dot(N, dir), 0.0f);

            if (NdotL > 0.0f)
            {
                float vis = ShadowVis(hitPos + N * 0.001f, dir, dist - 0.01f);
                // gi.radiance = initAtten * L_o. brdf는 GI_Initial SampleBRDF에서 이미 적용됨.
                indirectLight = gi.radiance * vis * gi.W;
                // Firefly 클램프: W 폭발로 인한 극단값이 누적 버퍼를 오염시키는 것을 방지
                float ilLum = dot(indirectLight, float3(0.2126f, 0.7152f, 0.0722f));
                if (ilLum > 3.0f) indirectLight *= 3.0f / ilLum;
            }
        }
    }

    // ── 시간적 누적 → Reinhard 톤매핑 → gamma 보정 ───────────────
    // randomSeed = App::m_shadeAccumCount (최대 64까지 누적)
    //   0 : 씬 전환/이동 직후 (g_accumulation 클리어됨) → 전체 교체 (alpha=1.0)
    //   N : 누적 N 프레임 → alpha = 1/(N+1) → N=64 이후 alpha ≥ 1.5% 유지
    float3 total = directLight + indirectLight;
    float  alpha = (randomSeed == 0u) ? 1.0f : (1.0f / float(randomSeed + 1u));
    float3 accumulated = lerp(g_accumulation[idx].rgb, total, alpha);
    g_accumulation[idx] = float4(accumulated, 1.0f);

    float3 color = accumulated / (accumulated + 1.0f);
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    g_output[idx] = float4(color, 1.0f);
}
