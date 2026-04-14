// ──────────────────────────────────────────────────────────────
//  ReSTIR_Shade.hlsl  –  [Pass 5] Final Shading
//
//  역할: Reservoir의 최종 선택 광원으로 그림자 레이를 쏘고
//        직접광 기여를 계산하여 g_output에 기록
//        간접광은 기존 PBR path tracing의 1 bounce로 처리 (선택적)
//
//  렌더링 방정식 (ReSTIR DI, Bitterli 2020 Eq.5):
//    L_direct = f_r(V,L,N) * Li * NdotL * vis * R.W
//    여기서 R.W = wSum / (M * p_hat(selected))
//
//  패스 순서 (per pixel):
//    1. G-Buffer 읽기 (hitPos, N, V, albedo, metallic, roughness)
//    2. 배경 픽셀: 환경광 샘플 → g_output
//    3. 유리/반투명 픽셀: 기존 path tracing 방식으로 처리 (fallback)
//    4. 일반 픽셀:
//       a. Reservoir에서 선택 광원 로드
//       b. 그림자 레이 (ShadowVis)
//       c. f_r * Li * NdotL * vis * R.W
//       d. 간접광 (1 bounce path tracing, optional)
//    5. Reinhard + gamma → g_output
//
//  입력:  G-Buffer, reservoir_cur, LightList, TLAS
//  출력:  g_output (u0)
//
//  스레드 그룹: DXR DispatchRays (1 ray per pixel)
//
//  TODO [Session 3]:
//   - 4c 직접광 계산 (GGX BRDF * Li * NdotL * vis * W)
//   - 4d 간접광 bounce (기존 Raytracing.hlsl SampleBRDF 재사용 가능)
//   - 환경광 처리 (배경 픽셀)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
RWTexture2D<float4>            g_output       : register(u0);
RWTexture2D<float4>            g_accumulation : register(u1);

Texture2D<float4>              gbuf_worldPos  : register(t6);
Texture2D<float4>              gbuf_normal    : register(t7);
Texture2D<float4>              gbuf_albedo    : register(t8);
Texture2D<float4>              gbuf_matInfo   : register(t9);
StructuredBuffer<Reservoir>    reservoir_cur  : register(t12);  // SRV (read-only)
StructuredBuffer<LightData>    g_lightList    : register(t5);

RaytracingAccelerationStructure g_tlas        : register(t0);

struct VertexPN { float3 pos; float3 normal; };
StructuredBuffer<VertexPN> g_vbPlane   : register(t1);
StructuredBuffer<VertexPN> g_vbCube    : register(t2);
StructuredBuffer<VertexPN> g_vbRoom    : register(t3);
StructuredBuffer<VertexPN> g_vbSphere  : register(t4);

cbuffer SceneConstants  : register(b0) { float3 camPos; uint sceneID; float3 camRight; float tanHalfFovY; float3 camUp; float aspectRatio; float3 camForward; float _p0; float4 _lightBlock[4]; float4 matAlbedoRoughness[4]; float4 matMetallic; float4 matEmissive; uint frameCount; uint randomSeed; float emissBoxHalfSize; float _p1; float3 emissBoxCenter; float _p2; }
cbuffer ReSTIRConstants : register(b1) { float4 _prevCam[4]; uint lightCount; uint candidateCount; uint screenW; uint screenH; uint frameIndex; float temporalMaxM; uint spatialRadius; uint spatialSamples; float4 _pad[2]; }

// ── 그림자 페이로드 ──────────────────────────────────────────────
struct ShadowPayload { float vis; };

// ── 그림자 레이 ─────────────────────────────────────────────────
float ShadowVis(float3 origin, float3 dir, float tmax)
{
    ShadowPayload sp = { 0.0f };
    RayDesc sr;
    sr.Origin = origin; sr.Direction = dir; sr.TMin = 0.001f; sr.TMax = tmax;
    TraceRay(g_tlas,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER         |
        RAY_FLAG_FORCE_OPAQUE,
        0xFD, 0, 1, 1, sr, sp);
    return sp.vis;
}

// ── 광원 기여 계산 ───────────────────────────────────────────────
// LightData → 히트포인트를 향하는 방향 L, 거리 dist, 광도 Li 계산
void CalcLightContrib(LightData light, float3 hitPos,
                      out float3 L, out float dist, out float3 Li)
{
    if (light.type == 1u)
    {
        // 방향광
        L    = normalize(light.pos);
        dist = 1e6f;
        Li   = light.color * light.intensity;
    }
    else
    {
        // 포인트광 (type==0) 또는 area (type==2, 근사)
        float3 toL = light.pos - hitPos;
        dist = max(length(toL), 0.01f);
        L    = toL / dist;
        Li   = light.color * light.intensity / (dist * dist);
    }
}

// ── Miss[1]: 그림자 레이 (공간 도달 = 가시) ─────────────────────
[shader("miss")]
void MissShadow_Shade(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
}

// ── RayGen_Shade ─────────────────────────────────────────────
[shader("raygeneration")]
void RayGen_Shade()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    uint  pidx  = PixelIndex(idx, dim.x);
    float4 wPos = gbuf_worldPos[idx];

    // ── 배경 픽셀 ──────────────────────────────────────────────
    if (wPos.w < 0.0f)
    {
        // TODO [Session 3]: 환경광 샘플 (sky gradient 또는 IBL)
        // 현재: 기존 Miss 셰이더 색상 그대로 사용
        float2 uv  = ((float2)idx + 0.5f) / (float2)dim;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float3 dir = normalize(camForward + camRight*(ndc.x*aspectRatio*tanHalfFovY) + camUp*(ndc.y*tanHalfFovY));

        float  t   = saturate(0.5f * (dir.y + 1.0f));
        float3 sky = lerp(float3(1,1,1), float3(0.5f,0.7f,1.0f), t);
        if (sceneID == 0)
        {
            float3 sunDir = normalize(float3(1,2,-0.5f));
            sky += float3(1,0.9f,0.7f) * pow(max(dot(dir, sunDir), 0.0f), 128.0f);
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

    // ── 투명/반투명 픽셀: 검은색 출력 ───────────────────────────
    // matInf.b < 0 은 유리/반투명 신호 (GBuffer.hlsl skipFlag=-1)
    // ReSTIR DI는 굴절/투과 처리 불가 → 검은색으로 덮어쓰기
    // (return만 하면 경로 추적기의 이전 출력이 남아 고스트 현상 발생)
    if (matInf.b < 0.0f)
    {
        g_output[idx] = float4(0, 0, 0, 1);
        return;
    }

    // ── 발광 픽셀: emission 직접 출력 ────────────────────────────
    uint  matIdx  = uint(normData.w + 0.5f);
    float emissive = matEmissive[matIdx];
    if (emissive > 0.0f)
    {
        float3 em = albedo * emissive;
        float3 c  = em / (em + 1.0f);
        c = pow(max(c, 0.0f), 1.0f / 2.2f);
        g_output[idx] = float4(c, 1.0f);
        return;
    }

    // ── ReSTIR 직접광 ─────────────────────────────────────────
    Reservoir R = reservoir_cur[pidx];

    float3 directLight = float3(0, 0, 0);

    if (R.lightIdx != 0xFFFFFFFFu && R.W > 0.0f)
    {
        LightData light = g_lightList[R.lightIdx];
        float3 L; float dist; float3 Li;
        CalcLightContrib(light, hitPos, L, dist, Li);

        float NdotL = max(dot(N, L), 0.0f);
        if (NdotL > 0.0f)
        {
            float vis = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);

            float3 V     = normalize(camPos - hitPos);
            float  alpha  = max(roughness * roughness, 0.001f);
            float  alpha2 = alpha * alpha;
            float  NdotV  = max(dot(N, V), 0.0001f);
            float3 VplusL = V + L;
            float3 H      = normalize(length(VplusL) > 1e-4f ? VplusL : N);
            float  NdotH  = max(dot(N, H), 0.0001f);
            float  VdotH  = max(dot(V, H), 0.0001f);
            float3 F0     = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
            float3 F      = SchlickF(VdotH, F0);
            float  D      = D_GGX(NdotH, alpha2);
            float  G      = G1_Smith(NdotV, alpha2) * G1_Smith(NdotL, alpha2);
            float3 spec   = D * G * F / max(4.0f * NdotV * NdotL, 0.0001f);
            float3 diff   = (1.0f - F) * (1.0f - metallic) * albedo * INV_PI;
            float3 fr     = spec + diff;

            // ReSTIR 기여: f_r * Li * NdotL * vis * R.W
            directLight = fr * Li * NdotL * vis * R.W;
        }
    }

    // ── 간접광 (1 bounce, optional) ──────────────────────────
    float3 indirectLight = float3(0, 0, 0);
    // ▶ 간접광 1-bounce (선택적, Bitterli 2020 Section 4.4 "Indirect Illumination"):
    //   ReSTIR DI는 직접광만 처리하므로, 간접광은 별도 1-bounce PT로 추가.
    //   수식:
    //     L_indirect = ∫ f_r(ω_o, ω_i) · L_i(ω_i) · cosθ_i  dω_i
    //   Monte Carlo 추정:
    //     L_indirect ≈ (f_r(V, scatterDir, N) / pdf) · TraceRay(hitPos, scatterDir)
    //   → SampleBRDF()가 attenuation = f_r * NdotL / pdf 를 반환
    //
    //   VNDF 중요도 샘플링 (Heitz 2018):
    //     SampleBRDF(N, V, albedo, metallic, roughness, seed, atten, pdf)
    //     → scatterDir, atten = f_r * NdotL / pdf (Raytracing.hlsl 참조)
    //
    //   ClosestHit에서 emission만 반환하는 단순 모드가 필요:
    //     RayPayload { emission, terminated=1, ... }
    //     indirectLight = atten * payload.emission
    //
    // TODO [Session 3]:
    // uint seed = (idx.x * 1973u + idx.y * 9277u + frameIndex * 26699u) | 1u;
    // float3 V = normalize(camPos - hitPos);
    // float3 atten; float pdf;
    // float3 scatterDir = SampleBRDF(N, V, albedo, metallic, roughness, seed, atten, pdf);
    // TraceRay → ClosestHit에서 NEE 없이 emission만 반환하는 단순 bounce
    // indirectLight = atten * bounceResult

    // ── 합산 및 출력 ─────────────────────────────────────────
    float3 total = directLight + indirectLight;

    float3 color = total / (total + 1.0f);
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    g_output[idx] = float4(color, 1.0f);
}
