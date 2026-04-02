// ──────────────────────────────────────────────────────────────
//  Raytracing.hlsl  –  Path Tracing (iterative bounce)
//
//  씬 0 (야외):  평면 바닥 + 금속 큐브, 태양 방향광
//  씬 1 (실내):  박스 룸 + 매트 큐브, 포인트 라이트 1개
//  씬 2 (구 쇼케이스): 박스 룸 + 골드 큐브 + 반투명 구 + 유리 구, 포인트 라이트 2개
//
//  InstanceID 인코딩:
//    하위 4비트 = geomType (0=plane, 1=cube, 2=room, 3=sphere)
//    상위 비트  = matIdx   (0..3)
//
//  재질 특수 인코딩 (matEmissive 필드):
//    matEmissive >  0   : 발광체
//    matEmissive == 0   : 불투명 (Lambertian/금속)
//    matEmissive < -0.5 : 반투명 (확산+투과 혼합)
//    matEmissive < -1.5 : 유리 (Fresnel 굴절/반사, IOR=1.5)
//
//  그림자 마스크:
//    일반/반투명 인스턴스: instanceMask = 0xFF
//    유리 구 인스턴스:     instanceMask = 0x02 (ShadowVis 0xFD로 제외)
// ──────────────────────────────────────────────────────────────

// ── 리소스 ──────────────────────────────────────────────────────
RWTexture2D<float4>             g_output       : register(u0);
RWTexture2D<float4>             g_accumulation : register(u1);
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

    float4 matAlbedoRoughness[4];   // .xyz=albedo  .w=roughness
    float4 matMetallic;             // .xyzw = metallic for mat 0,1,2,3
    float4 matEmissive;             // .xyzw = emission intensity (0=off, >0 → albedo×intensity)

    uint   frameCount;  uint   randomSeed;  float  emissBoxHalfSize; float _cbPad1;
    float3 emissBoxCenter;  float _cbPad2;
};

// ── 페이로드 ────────────────────────────────────────────────────
struct RayPayload
{
    float3 emission;        // NEE 직접광 + 미스 방사
    float3 attenuation;     // BRDF 처리량 가중치
    float3 nextOrigin;
    float3 nextDirection;
    uint   seed;            // RNG 상태 (입출력)
    uint   depth;           // 현재 바운스 (입력)
    uint   terminated;      // 1 = 경로 종료
};
// 60 bytes

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
// Lambertian: BRDF/pdf = albedo (cosTheta 약분)
float3 CosineSampleHemisphere(float2 u, float3 N)
{
    float r   = sqrt(u.x);
    float phi = 6.28318530718f * u.y;
    float lx  = r * cos(phi);
    float lz  = r * sin(phi);
    float ly  = sqrt(max(0.0f, 1.0f - u.x));
    float3 T, B;
    BuildONB(N, T, B);
    return normalize(T * lx + N * ly + B * lz);
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
        0xFD, 0, 1, 1, sr, sp); // 0xFD: 유리 구(마스크 0x02) 제외 – 빛이 통과
    return sp.vis;
}

// ── NEE 직접광 ──────────────────────────────────────────────────
float3 DirectLighting(float3 hitPos, float3 N, float3 albedo, float metallic, inout uint seed)
{
    float3 diffAlbedo = albedo * (1.0f - metallic);
    float3 result     = float3(0.0f, 0.0f, 0.0f);

    if (sceneID == 0)
    {
        // 씬 0: 태양 방향광
        float3 sunDir   = normalize(float3(1.0f, 2.0f, -0.5f));
        float3 sunColor = float3(1.0f, 1.0f, 1.0f) * 3.0f;
        float  NdotL    = max(0.0f, dot(N, sunDir));
        float  vis      = ShadowVis(hitPos + N * 0.001f, sunDir, 1e6f);
        result = diffAlbedo * NdotL * sunColor * vis;
    }
    else if (sceneID == 1)
    {
        // 씬 1: 포인트 라이트 1개 (역제곱 감쇠)
        float3 toL   = lightPos - hitPos;
        float  dist  = max(length(toL), 0.01f);
        float3 L     = toL / dist;
        float  NdotL = max(0.0f, dot(N, L));
        float  atten = lightIntensity / (dist * dist);   // 물리 기반 역제곱
        float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
        result = diffAlbedo * NdotL * lightColor * atten * vis;
    }
    else
    {
        // 씬 2: 포인트 라이트 2개 (역제곱 감쇠)
        // Light 1
        {
            float3 toL   = lightPos - hitPos;
            float  dist  = max(length(toL), 0.01f);
            float3 L     = toL / dist;
            float  NdotL = max(0.0f, dot(N, L));
            float  atten = lightIntensity / (dist * dist);
            float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
            result += diffAlbedo * NdotL * lightColor * atten * vis;
        }
        // Light 2
        {
            float3 toL   = light2Pos - hitPos;
            float  dist  = max(length(toL), 0.01f);
            float3 L     = toL / dist;
            float  NdotL = max(0.0f, dot(N, L));
            float  atten = light2Intensity / (dist * dist);
            float  vis   = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
            result += diffAlbedo * NdotL * light2Color * atten * vis;
        }

        // 발광 박스 면광원 NEE
        // 박스 6개 면 중 랜덤 샘플: 각 면에서 균일 분포로 한 점 선택
        if (emissBoxHalfSize > 0.0f)
        {
            // 면 인덱스 (0~5): ±X, ±Y, ±Z
            uint  faceIdx = uint(RandFloat(seed) * 6.0f) % 6u;
            float u = RandFloat(seed) * 2.0f - 1.0f;  // [-1,1]
            float v = RandFloat(seed) * 2.0f - 1.0f;

            float3 samplePos;
            float3 faceNormal;
            float  h = emissBoxHalfSize;
            if      (faceIdx == 0u) { samplePos = emissBoxCenter + float3( h, u*h, v*h); faceNormal = float3( 1,0,0); }
            else if (faceIdx == 1u) { samplePos = emissBoxCenter + float3(-h, u*h, v*h); faceNormal = float3(-1,0,0); }
            else if (faceIdx == 2u) { samplePos = emissBoxCenter + float3(u*h,  h, v*h); faceNormal = float3(0, 1,0); }
            else if (faceIdx == 3u) { samplePos = emissBoxCenter + float3(u*h, -h, v*h); faceNormal = float3(0,-1,0); }
            else if (faceIdx == 4u) { samplePos = emissBoxCenter + float3(u*h, v*h,  h); faceNormal = float3(0,0, 1); }
            else                    { samplePos = emissBoxCenter + float3(u*h, v*h, -h); faceNormal = float3(0,0,-1); }

            float3 toS   = samplePos - hitPos;
            float  dist2 = dot(toS, toS);
            float  dist  = sqrt(dist2);
            float3 L     = toS / dist;

            float NdotL     = max(0.0f, dot(N, L));
            float faceNdotL = max(0.0f, dot(faceNormal, -L));  // 광원 면→수신점 방향 코사인

            if (NdotL > 0.0f && faceNdotL > 0.0f)
            {
                // 전체 표면적 = 6 * (2h)^2 = 24h^2
                float totalArea = 24.0f * h * h;
                float pdf       = 1.0f / totalArea;          // 균일 샘플링 PDF
                float3 Le       = matAlbedoRoughness[3].xyz * matEmissive[3];
                float  G        = NdotL * faceNdotL / dist2;
                float  vis      = ShadowVis(hitPos + N * 0.001f, L, dist - 0.01f);
                result += diffAlbedo * Le * G / pdf * vis;
            }
        }
    }
    return result;
}

// ── 가우시안 포커스 샘플링 설정 ─────────────────────────────────
// NDC 거리에 따라 spp를 가우시안으로 연속 변화:
//   sppFloat = k_maxSamples * exp(-dist² / k_gaussSigma²)
//   dist=0.0 → sppFloat=16,  dist=0.6 → ~1,  dist>=1.0 → <0.25
// 소수점은 확률적 올림(stochastic rounding)으로 정수 샘플 수 결정.
// sppFloat<1 이면 해당 확률로만 레이를 쏘고, 나머지 프레임은 누적값 재사용.
static const float k_maxSamples  = 16.0f;  // 화면 중심 최대 spp
static const float k_gaussSigma  = 0.6f;   // 가우시안 폭 (NDC 단위)

// ── RayGen – 가우시안 포커스 경로 추적 ──────────────────────────
[shader("raygeneration")]
void RayGen()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    // 픽셀 중심 NDC (지터 없음, 가중치 계산용)
    float2 pixelCenter = ((float2)idx + 0.5f) / (float2)dim;
    float2 ndcCenter   = float2(pixelCenter.x * 2.0f - 1.0f, 1.0f - pixelCenter.y * 2.0f);

    // 가우시안 가중치 → 연속 spp 계산
    float dist      = length(ndcCenter);
    float gaussW    = exp(-dist * dist / (k_gaussSigma * k_gaussSigma));
    float sppFloat  = k_maxSamples * gaussW;   // 0 ~ 16 연속값

    // 확률적 올림: floor(sppFloat) + frac 확률로 +1
    uint  seed      = WangHash(idx.x + idx.y * dim.x + randomSeed * 719393u);
    uint  sppBase   = uint(sppFloat);
    float sppFrac   = sppFloat - float(sppBase);
    uint  numSamples = sppBase + (RandFloat(seed) < sppFrac ? 1u : 0u);

    // spp=0 이면 이번 프레임 스킵 (누적값 그대로 출력)
    if (numSamples == 0u)
    {
        float3 accumulated = g_accumulation[idx].rgb;
        float3 color = accumulated / (accumulated + 1.0f);
        color = pow(max(color, 0.0f), 1.0f / 2.2f);
        g_output[idx] = float4(color, 1.0f);
        return;
    }

    float3 totalRadiance = float3(0.0f, 0.0f, 0.0f);

    for (uint s = 0u; s < numSamples; s++)
    {
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

        static const uint k_maxBounce = 8u;

        for (uint bounce = 0u; bounce < k_maxBounce; bounce++)
        {
            RayPayload payload;
            payload.emission      = float3(0.0f, 0.0f, 0.0f);
            payload.attenuation   = float3(0.0f, 0.0f, 0.0f);
            payload.nextOrigin    = float3(0.0f, 0.0f, 0.0f);
            payload.nextDirection = float3(0.0f, 0.0f, 0.0f);
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

        totalRadiance += radiance;
    }

    float3 radiance = totalRadiance / float(numSamples);

    // 시간적 누적
    float3 accumulated;
    if (frameCount == 0u)
        accumulated = radiance;
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
        // 씬 1, 2: 어두운 배경 (실내이므로 하늘 없음)
        payload.emission = float3(0.01f, 0.01f, 0.015f);
    }
    payload.terminated = 1u;
}

// ── Miss[1]: 그림자 레이 (광원 도달) ───────────────────────────
[shader("miss")]
void MissShadow(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
}

// ── ClosestHit: BRDF 샘플링 + NEE ──────────────────────────────
[shader("closesthit")]
void ClosestHit(inout RayPayload payload,
                BuiltInTriangleIntersectionAttributes attr)
{
    // InstanceID 디코딩: 하위4비트=geomType, 상위=matIdx
    uint rawID    = InstanceID();
    uint geomType = rawID & 0xFu;
    uint matIdx   = rawID >> 4u;

    uint primIdx = PrimitiveIndex();
    float2 bary  = attr.barycentrics;
    float3 b     = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    uint vi = primIdx * 3;

    // 지오메트리 타입에 따라 법선 버퍼 선택
    float3 n0, n1, n2;
    if (geomType == 0u)
    {
        n0 = g_vbPlane[vi].normal; n1 = g_vbPlane[vi+1].normal; n2 = g_vbPlane[vi+2].normal;
    }
    else if (geomType == 2u)
    {
        n0 = g_vbRoom[vi].normal; n1 = g_vbRoom[vi+1].normal; n2 = g_vbRoom[vi+2].normal;
    }
    else if (geomType == 3u)
    {
        n0 = g_vbSphere[vi].normal; n1 = g_vbSphere[vi+1].normal; n2 = g_vbSphere[vi+2].normal;
    }
    else
    {
        n0 = g_vbCube[vi].normal; n1 = g_vbCube[vi+1].normal; n2 = g_vbCube[vi+2].normal;
    }

    // rawN: 플립 전 원래 법선 (유리 입사/출사 판별용)
    float3 rawN = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
    float3 V = -normalize(WorldRayDirection());
    float3 N = rawN;
    if (dot(N, V) < 0.0f) N = -N;
    bool entering = dot(rawN, V) >= 0.0f; // true=입사(공기→재질), false=출사(재질→공기)

    float3 hitPos   = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 albedo   = matAlbedoRoughness[matIdx].xyz;
    float  metallic = matMetallic[matIdx];   // float4 컴포넌트 인덱싱

    uint seed = payload.seed;

    float emissive = matEmissive[matIdx];

    // ── 투명/반투명 재질 처리 ────────────────────────────────────
    // matEmissive < -1.5 → 유리 (Fresnel 굴절+반사, IOR=1.5)
    // matEmissive < -0.5 → 반투명 (확산 산란 + 투과 혼합)
    if (emissive < -0.5f)
    {
        if (emissive < -1.5f)
        {
            // ── 유리 (Dielectric, IOR=1.5) ──────────────────────
            float ior = 1.5f;
            // 입사 시: eta=공기/유리=1/1.5, 출사 시: eta=유리/공기=1.5/1
            float eta = entering ? (1.0f / ior) : ior;

            float3 inc      = normalize(WorldRayDirection());
            float  cosTheta = max(0.0f, dot(N, V)); // N이 V방향을 향하므로 항상 양수

            // Schlick 근사 프레넬 (r0 ≈ 0.04 for glass)
            float r0 = (1.0f - ior) / (1.0f + ior);
            r0 = r0 * r0; // ≈ 0.04
            float fresnel = r0 + (1.0f - r0) * pow(1.0f - cosTheta, 5.0f);

            // Snell의 법칙 굴절 (전반사 시 refract는 0벡터 반환)
            float3 refracted = refract(inc, N, eta);
            bool   tir       = dot(refracted, refracted) < 0.0001f; // 전반사

            float3 scatterDir = (tir || RandFloat(seed) < fresnel)
                ? reflect(inc, N)   // 반사
                : refracted;        // 굴절

            payload.emission      = float3(0.0f, 0.0f, 0.0f);
            payload.attenuation   = albedo; // albedo로 유리 색조 적용
            payload.nextOrigin    = hitPos + scatterDir * 0.001f;
            payload.nextDirection = scatterDir;
            payload.terminated    = 0u;
            payload.seed          = seed;
            return;
        }
        else
        {
            // ── 반투명 (Translucent): 확산 산란 50% + 투과 50% ──
            float3 scatterDir;
            float3 atten;

            if (entering)
            {
                // 앞면: 확산 산란 또는 투과 선택
                if (RandFloat(seed) < 0.5f)
                {
                    // 확산 산란 (코사인 가중 반구, 표면 외부 방향)
                    float2 u   = float2(RandFloat(seed), RandFloat(seed));
                    scatterDir = CosineSampleHemisphere(u, N);
                    atten      = albedo;
                }
                else
                {
                    // 직진 투과 (구 내부로 진입)
                    scatterDir = normalize(WorldRayDirection());
                    atten      = albedo * 0.85f;
                }
                payload.emission = DirectLighting(hitPos, N, albedo * 0.5f, 0.0f, seed);
            }
            else
            {
                // 뒷면(출사): 투과하여 구 밖으로
                scatterDir       = normalize(WorldRayDirection());
                atten            = albedo * 0.90f;
                payload.emission = float3(0.0f, 0.0f, 0.0f);
            }

            payload.attenuation   = atten;
            payload.nextOrigin    = hitPos + scatterDir * 0.001f;
            payload.nextDirection = scatterDir;
            payload.terminated    = 0u;
            payload.seed          = seed;
            return;
        }
    }

    // 발광체: 직접 방사하고 경로 종료
    if (emissive > 0.0f)
    {
        payload.emission   = albedo * emissive;
        payload.terminated = 1u;
        payload.seed       = seed;
        return;
    }

    // NEE: 직접광
    payload.emission = DirectLighting(hitPos, N, albedo, metallic, seed);

    // BRDF 샘플링
    float3 scatterDir;
    float3 attenuation;

    if (RandFloat(seed) < metallic)
    {
        // 정반사 (Schlick 프레넬)
        scatterDir  = reflect(-V, N);
        float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        float  cosT = max(0.0f, dot(N, V));
        float3 F    = F0 + (1.0f - F0) * pow(1.0f - cosT, 5.0f);
        attenuation = F;
    }
    else
    {
        // Lambertian 확산 (코사인 가중 반구)
        float2 u    = float2(RandFloat(seed), RandFloat(seed));
        scatterDir  = CosineSampleHemisphere(u, N);
        attenuation = albedo * (1.0f - metallic);
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
        payload.terminated    = 0u;
    }

    payload.seed = seed;
}
