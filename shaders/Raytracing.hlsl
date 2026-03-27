// ──────────────────────────────────────────────────────────────
//  Raytracing.hlsl
//  DXR 레이트레이싱 셰이더
//  씬 0 (야외): 하늘 그라데이션 + 태양광 + 반사
//  씬 1 (실내): 포인트 라이트 + 그림자
// ──────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────
//  리소스 바인딩 (글로벌 루트 시그니처)
// ──────────────────────────────────────────────────────────────

// 파라미터 0: 디스크립터 테이블
RWTexture2D<float4>                 g_output : register(u0);        // 출력 UAV
RaytracingAccelerationStructure     g_tlas   : register(t0);        // TLAS SRV

// 정점 버퍼 (StructuredBuffer<VertexPN>, stride=24)
struct VertexPN { float3 pos; float3 normal; };
StructuredBuffer<VertexPN> g_vbPlane : register(t1);   // 평면 정점 버퍼
StructuredBuffer<VertexPN> g_vbCube  : register(t2);   // 큐브 정점 버퍼
StructuredBuffer<VertexPN> g_vbRoom  : register(t3);   // 방 정점 버퍼

// 파라미터 1: 인라인 루트 CBV (씬 상수)
cbuffer SceneConstants : register(b0)
{
    float3 camPos;      uint   sceneID;
    float3 camRight;    float  tanHalfFovY;
    float3 camUp;       float  aspectRatio;
    float3 camForward;  float  _pad0;
    float3 lightPos;    float  lightRadius;
    float3 lightColor;  float  lightIntensity;
    float4 matAlbedoRoughness[4];  // .xyz=albedo .w=roughness
    float4 matMetallic[4];         // .x=metallic
};

// ──────────────────────────────────────────────────────────────
//  페이로드 정의
// ──────────────────────────────────────────────────────────────
struct RayPayload
{
    float3 color;
    uint   depth;
};

struct ShadowPayload
{
    float vis;   // 1.0 = 빛 도달 (그림자 없음), 0.0 = 차단됨
};

// ──────────────────────────────────────────────────────────────
//  RayGen 셰이더 – 핀홀 카메라
// ──────────────────────────────────────────────────────────────
[shader("raygeneration")]
void RayGen()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    // NDC [-1, 1] 좌표 계산 (Y 반전)
    float2 uv  = ((float2)idx + 0.5f) / (float2)dim;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // 카메라 방향 계산 (핀홀)
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

    RayPayload payload = { float3(0.0f, 0.0f, 0.0f), 0u };
    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    g_output[idx] = float4(payload.color, 1.0f);
}

// ──────────────────────────────────────────────────────────────
//  Miss[0] – 기본 미스: 하늘(씬0) 또는 어두운 주변광(씬1)
// ──────────────────────────────────────────────────────────────
[shader("miss")]
void MissShader(inout RayPayload payload)
{
    float3 d = normalize(WorldRayDirection());

    if (sceneID == 0)
    {
        // 씬 0: 하늘 그라데이션 + 태양 원반
        float t   = saturate(0.5f * (d.y + 1.0f));
        float3 sky = lerp(float3(1.0f, 1.0f, 1.0f),
                          float3(0.5f, 0.7f, 1.0f), t);

        // 태양 원반
        float3 sunDir = normalize(float3(1.0f, 2.0f, -0.5f));
        float  s = max(0.0f, dot(d, sunDir));
        sky += float3(1.0f, 0.9f, 0.7f) * pow(s, 128.0f);

        payload.color = sky;
    }
    else
    {
        // 씬 1: 매우 어두운 주변광
        payload.color = float3(0.02f, 0.02f, 0.03f);
    }
}

// ──────────────────────────────────────────────────────────────
//  Miss[1] – 그림자 미스: 광원에 도달 (차단 없음)
// ──────────────────────────────────────────────────────────────
[shader("miss")]
void MissShadow(inout ShadowPayload payload)
{
    payload.vis = 1.0f;
}

// ──────────────────────────────────────────────────────────────
//  ClosestHit – 완전한 셰이딩
// ──────────────────────────────────────────────────────────────
[shader("closesthit")]
void ClosestHit(inout RayPayload payload,
                BuiltInTriangleIntersectionAttributes attr)
{
    uint instID  = InstanceID();
    uint primIdx = PrimitiveIndex();
    float2 bary  = attr.barycentrics;
    float3 b     = float3(1.0f - bary.x - bary.y, bary.x, bary.y);

    // 정점 인덱스 (비인덱스 삼각형 리스트)
    uint vi = primIdx * 3;

    // 인스턴스 ID에 따라 올바른 정점 버퍼에서 법선 가져오기
    float3 n0, n1, n2;
    if (instID == 0)
    {
        // 평면 (재질 0)
        n0 = g_vbPlane[vi].normal;
        n1 = g_vbPlane[vi + 1].normal;
        n2 = g_vbPlane[vi + 2].normal;
    }
    else if (instID == 2)
    {
        // 방 (재질 2)
        n0 = g_vbRoom[vi].normal;
        n1 = g_vbRoom[vi + 1].normal;
        n2 = g_vbRoom[vi + 2].normal;
    }
    else
    {
        // 큐브 (재질 1 또는 3)
        n0 = g_vbCube[vi].normal;
        n1 = g_vbCube[vi + 1].normal;
        n2 = g_vbCube[vi + 2].normal;
    }

    // 보간된 법선
    float3 N = normalize(n0 * b.x + n1 * b.y + n2 * b.z);

    // 카메라 방향 (뷰 벡터)
    float3 V = -normalize(WorldRayDirection());

    // 법선이 카메라를 향하도록 보장
    if (dot(N, V) < 0.0f)
        N = -N;

    // 히트 위치
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // 재질 속성 (인스턴스 ID = 재질 인덱스)
    uint   matIdx    = instID;
    float3 albedo    = matAlbedoRoughness[matIdx].xyz;
    float  roughness = matAlbedoRoughness[matIdx].w;
    float  metallic  = matMetallic[matIdx].x;

    float3 color = float3(0.0f, 0.0f, 0.0f);

    if (sceneID == 0)
    {
        // ──────────────────────────────
        // 씬 0: 야외 태양광 + 반사
        // ──────────────────────────────
        float3 sunDir   = normalize(float3(1.0f, 2.0f, -0.5f));
        float3 sunColor = float3(1.0f, 0.95f, 0.8f) * 3.0f;

        // 그림자 레이 (태양 방향)
        ShadowPayload shadow = { 0.0f };
        RayDesc shadowRay;
        shadowRay.Origin    = hitPos + N * 0.001f;
        shadowRay.Direction = sunDir;
        shadowRay.TMin      = 0.001f;
        shadowRay.TMax      = 1e6f;
        TraceRay(g_tlas,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE,
            0xFF, 0, 1, 1, shadowRay, shadow);

        float NdotL = max(0.0f, dot(N, sunDir));
        float3 diffuse = albedo * (1.0f - metallic) * NdotL * sunColor * shadow.vis;

        // 하늘 주변광 (법선 방향에 따른 그라데이션)
        float3 skyAmb = lerp(float3(0.1f, 0.1f, 0.2f),
                             float3(0.4f, 0.6f, 0.9f),
                             saturate(N.y * 0.5f + 0.5f));
        float3 ambient = albedo * (1.0f - metallic) * skyAmb * 0.3f;

        color = diffuse + ambient;

        // 금속 재질 반사 레이 (depth < 2)
        if (metallic > 0.1f && payload.depth == 0)
        {
            RayDesc reflRay;
            reflRay.Origin    = hitPos + N * 0.001f;
            reflRay.Direction = reflect(-V, N);
            reflRay.TMin      = 0.001f;
            reflRay.TMax      = 1e6f;

            RayPayload reflPayload = { float3(0.0f, 0.0f, 0.0f), payload.depth + 1u };
            TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, reflRay, reflPayload);

            // 프레넬 (Schlick 근사)
            float3 F0       = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
            float  cosTheta = max(0.0f, dot(N, V));
            float3 F        = F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);

            color = lerp(color, reflPayload.color, F * metallic);
        }
    }
    else
    {
        // ──────────────────────────────
        // 씬 1: 실내 포인트 라이트
        // ──────────────────────────────
        float3 toLight = lightPos - hitPos;
        float  dist    = length(toLight);
        float3 L       = toLight / dist;

        // 거리 감쇠 (물리 기반)
        float atten = lightIntensity / (1.0f + dist * dist);

        // 그림자 레이 (광원 방향, TMax = dist)
        ShadowPayload shadow = { 0.0f };
        RayDesc shadowRay;
        shadowRay.Origin    = hitPos + N * 0.001f;
        shadowRay.Direction = L;
        shadowRay.TMin      = 0.001f;
        shadowRay.TMax      = dist - 0.01f;
        TraceRay(g_tlas,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE,
            0xFF, 0, 1, 1, shadowRay, shadow);

        float  NdotL   = max(0.0f, dot(N, L));
        float3 diffuse = albedo * NdotL * lightColor * atten * shadow.vis;
        float3 ambient = albedo * float3(0.03f, 0.03f, 0.05f);

        color = diffuse + ambient;
    }

    payload.color = color;
}
