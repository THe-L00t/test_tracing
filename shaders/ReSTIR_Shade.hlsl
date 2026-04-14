// ──────────────────────────────────────────────────────────────
//  ReSTIR_Shade.hlsl  –  [Pass 5] Final Shading
//
//  역할: Reservoir의 최종 선택 광원으로 shadow ray를 쏘고
//        직접광을 계산하여 g_output에 기록.
//
//  렌더링 방정식 (Bitterli 2020, Eq.5):
//    L_direct = f_r(V,L,N) * Li * NdotL * vis * R.W
//    R.W = wSum / (M * p̂(selected))
//
//  픽셀 분류:
//    1. 배경 (hitDist < 0)         → sky gradient
//    2. 유리 (flags == GB_GLASS)   → 환경광 근사 (ReSTIR 굴절 미지원)
//    3. 발광 (flags == GB_EMISSIVE)→ emission 직접 출력
//    4. 일반                       → ShadowVis + GGX BRDF + ambient
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

// ── 그림자 레이 페이로드 ─────────────────────────────────────
struct ShadowPayload { float vis; };

// Miss[1]: 그림자 레이가 아무것도 맞지 않음 = 광원 가시
[shader("miss")]
void MissShadow_Shade(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
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

    // ── 유리/반투명 픽셀: 환경광 근사 ─────────────────────────────
    // ReSTIR DI는 굴절 미지원 → 환경광을 albedo로 tint하여 근사
    if (flags > 0.5f && flags < 1.5f)  // GB_FLAG_GLASS
    {
        float2 uv  = ((float2)idx + 0.5f) / (float2)dim;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float3 dir = normalize(camForward
                               + camRight * (ndc.x * aspectRatio * tanHalfFovY)
                               + camUp    * (ndc.y * tanHalfFovY));
        float  sky_t   = saturate(0.5f * (dir.y + 1.0f));
        float3 envColor = lerp(float3(0.15f, 0.10f, 0.08f),
                               float3(0.40f, 0.55f, 0.80f), sky_t);
        float3 tinted = envColor * (0.6f + 0.4f * albedo);
        float3 c = tinted / (tinted + 1.0f);
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
    float3 V = normalize(camPos - hitPos);

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

    // ── 반구 앰비언트 (직접광만으로는 음영면 완전 검은색 방지) ────
    float  upFactor    = saturate(N.y * 0.5f + 0.5f);
    float3 ambientSky  = float3(0.20f, 0.28f, 0.40f);
    float3 ambientGnd  = float3(0.06f, 0.05f, 0.04f);
    float3 ambient     = albedo * lerp(ambientGnd, ambientSky, upFactor)
                         * (1.0f - metallic * 0.7f);

    // ── 합산 → Reinhard 톤매핑 → gamma 보정 ───────────────────────
    float3 total = directLight + ambient;
    float3 color = total / (total + 1.0f);
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    g_output[idx] = float4(color, 1.0f);
}
