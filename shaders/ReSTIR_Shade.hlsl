// ──────────────────────────────────────────────────────────────
//  ReSTIR_Shade.hlsl  –  [Pass 5] Final Shading
//
//  역할: Reservoir의 최종 선택 광원으로 shadow ray를 쏘고
//        직접광을 계산한 뒤, 1-bounce 간접광을 BRDF 샘플링으로 합산.
//
//  렌더링 방정식:
//    L_total = L_direct(ReSTIR, Eq.5) + L_indirect(1-bounce Path Tracer)
//    L_direct = f_r(V,L,N) * Li * NdotL * vis * R.W
//    L_indirect = SampleBRDF(N,V) → TraceRay → radiance at bounce
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
StructuredBuffer<Reservoir>    reservoir_in   : register(t10);  // Spatial 최종 결과 SRV
StructuredBuffer<LightData>    g_lightList    : register(t5);

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
struct ShadowPayload   { float vis; };                  // 4 bytes
struct IndirectPayload { float3 radiance; uint seed; }; // 16 bytes

// Miss[0]: 간접광/굴절 레이가 아무것도 맞지 않음 = 하늘 방사
[shader("miss")]
void MissIndirect_Shade(inout IndirectPayload payload)
{
    float3 d = normalize(WorldRayDirection());
    float  t = saturate(0.5f * (d.y + 1.0f));
    float3 sky = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.5f, 0.7f, 1.0f), t);
    if (sceneID == 0)
    {
        float3 sunDir = normalize(float3(1.0f, 2.0f, -0.5f));
        sky += float3(1.0f, 0.9f, 0.7f) * pow(max(dot(d, sunDir), 0.0f), 128.0f);
    }
    payload.radiance = sky;
}

// Miss[1]: 그림자 레이가 아무것도 맞지 않음 = 광원 가시
[shader("miss")]
void MissShadow_Shade(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
}

// ── ClosestHit_Shade: 간접광/굴절 레이가 표면에 맞닿음 ──────────
// 재귀 없이 emission 또는 반구 앰비언트를 반환
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

    float3 rawN   = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
    float3 rayDir = normalize(WorldRayDirection());
    float3 N      = dot(rawN, -rayDir) > 0.0f ? rawN : -rawN;  // 입사면 기준 법선

    float3 albedo   = matAlbedoRoughness[matIdx].xyz;
    float  metallic = matMetallic[matIdx];
    float  emissive = matEmissive[matIdx];

    // 발광: emission 직접 반환
    if (emissive > 0.0f)
    {
        payload.radiance = albedo * emissive;
        return;
    }

    // 유리/반투명: albedo 틴트로 투과 근사 (재귀 불가)
    if (emissive < -0.5f)
    {
        payload.radiance = albedo * float3(0.85f, 0.90f, 0.95f);
        return;
    }

    // 불투명 표면: 반구 앰비언트 (직접광은 이 단계에서 추가 shadow ray 없음)
    float  upFactor  = saturate(N.y * 0.5f + 0.5f);
    float3 ambSky    = float3(0.20f, 0.28f, 0.40f);
    float3 ambGnd    = float3(0.06f, 0.05f, 0.04f);
    payload.radiance = albedo * lerp(ambGnd, ambSky, upFactor)
                       * (1.0f - metallic * 0.7f);
}

// 그림자 레이 발사 → 가시성 반환 (0=차폐, 1=가시)
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

    // ── 유리/반투명 픽셀: Fresnel 굴절/반사 레이 추적 ───────────
    if (flags > 0.5f && flags < 1.5f)  // GB_FLAG_GLASS
    {
        // G-Buffer N은 face-forward (카메라 기준 항상 입사면)
        float  ior     = 1.5f;
        float3 inc     = -V;
        float  cosTheta = max(dot(N, V), 0.0f);
        float  r0       = (1.0f - ior) / (1.0f + ior); r0 *= r0;
        float  fresnel  = r0 + (1.0f - r0) * pow(1.0f - cosTheta, 5.0f);
        float3 refracted = refract(inc, N, 1.0f / ior);
        bool   tir       = dot(refracted, refracted) < 0.0001f;

        uint   gseed      = WangHash(pidx ^ (frameIndex * 0x9E3779B9u));
        float3 scatterDir = (tir || RandFloat(gseed) < fresnel)
                            ? reflect(inc, N) : refracted;

        IndirectPayload gp = (IndirectPayload)0;
        gp.seed = gseed;
        RayDesc gr;
        gr.Origin    = hitPos + scatterDir * 0.002f;
        gr.Direction = scatterDir;
        gr.TMin      = 0.001f;
        gr.TMax      = 1e6f;
        TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, gr, gp);

        float3 glassColor = gp.radiance * albedo;
        float3 c = glassColor / (glassColor + 1.0f);
        c = pow(max(c, 0.0f), 1.0f / 2.2f);
        g_output[idx] = float4(c, 1.0f);
        return;
    }

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

    // ── 일반 픽셀: ReSTIR 직접광 (Bitterli 2020, Eq.5) ───────────
    Reservoir R = reservoir_in[pidx];

    float3 directLight = float3(0.0f, 0.0f, 0.0f);

    if (R.lightIdx != 0xFFFFFFFFu && R.W > 0.0f)
    {
        LightData light = g_lightList[R.lightIdx];
        float3 L; float dist; float3 Li;
        CalcLightContrib(light, hitPos, L, dist, Li);

        float NdotL = max(dot(N, L), 0.0f);
        if (NdotL > 0.0f)
        {
            float vis = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);

            // GGX Cook-Torrance BRDF
            float  alpha  = max(roughness * roughness, 0.001f);
            float  alpha2 = alpha * alpha;
            float  NdotV  = max(dot(N, V), 0.0001f);
            float3 VplusL = V + L;
            // NaN-safe half vector: V ≈ -L 이면 V+L ≈ 0 → N으로 폴백
            float3 H      = normalize(length(VplusL) > 1e-4f ? VplusL : N);
            float  NdotH  = max(dot(N, H), 0.0001f);
            float  VdotH  = max(dot(V, H), 0.0001f);

            float3 F0  = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
            float3 F   = SchlickF(VdotH, F0);
            float  D   = D_GGX(NdotH, alpha2);
            float  G   = G1_Smith(NdotV, alpha2) * G1_Smith(NdotL, alpha2);
            float3 spec = D * G * F / max(4.0f * NdotV * NdotL, 0.0001f);
            float3 diff = (1.0f - F) * (1.0f - metallic) * albedo * INV_PI;
            float3 fr   = spec + diff;

            // L = f_r * Li * NdotL * vis * W  (Eq.5)
            directLight = fr * Li * NdotL * vis * R.W;
        }
    }

    // ── 1-bounce 간접광 (BRDF 샘플링 + Path Tracer 통합) ─────────
    uint   indSeed = WangHash(pidx ^ (frameIndex * 0x9E3779B9u) ^ (frameCount * 0x517CC1B7u));
    float3 indAtten;
    float  indPdf;
    float3 indDir  = SampleBRDF(N, V, albedo, metallic, roughness,
                                indSeed, indAtten, indPdf);
    float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

    if (dot(indDir, N) > 0.0f)
    {
        IndirectPayload ip = (IndirectPayload)0;
        ip.seed = indSeed;
        RayDesc ir;
        ir.Origin    = hitPos + N * 0.001f;
        ir.Direction = indDir;
        ir.TMin      = 0.001f;
        ir.TMax      = 1e6f;
        TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ir, ip);
        indirectLight = indAtten * ip.radiance;
    }

    // ── 합산 → Reinhard 톤매핑 → gamma 보정 ───────────────────────
    float3 total = directLight + indirectLight;
    float3 color = total / (total + 1.0f);
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    g_output[idx] = float4(color, 1.0f);
}
