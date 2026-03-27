// ──────────────────────────────────────────────────────────────
//  Raytracing.hlsl
//  DXR 레이트레이싱 셰이더
//  - RayGen    : 각 픽셀마다 레이 발사
//  - MissShader: 미스 시 하늘색 반환
//  - ClosestHit: 히트 시 배리센트릭 기반 컬러 반환
// ──────────────────────────────────────────────────────────────

// 글로벌 디스크립터 테이블 (루트 파라미터 0)
//   슬롯 0 : u0 - 출력 UAV 텍스처
//   슬롯 1 : t0 - TLAS SRV
RWTexture2D<float4>                    g_output : register(u0);
RaytracingAccelerationStructure        g_scene  : register(t0);

// ──────────────────────────────────────────────────────────────
//  페이로드: 셰이더 간 색상 전달
// ──────────────────────────────────────────────────────────────
struct RayPayload
{
    float4 color;
};

// ──────────────────────────────────────────────────────────────
//  RayGen 셰이더
// ──────────────────────────────────────────────────────────────
[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim   = DispatchRaysDimensions().xy;

    // NDC [-1, 1] 좌표 계산
    float2 uv = (float2(launchIndex) + 0.5f) / float2(launchDim);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y; // Y 반전

    // 간단한 핀홀 카메라 (FOV 60도, z=0 → 타겟 z=1)
    float aspectRatio = float(launchDim.x) / float(launchDim.y);
    float tanHalfFov  = 0.5774f; // tan(30도)

    RayDesc ray;
    ray.Origin    = float3(0.0f, 0.0f, -1.0f);
    ray.Direction = normalize(float3(
        ndc.x * aspectRatio * tanHalfFov,
        ndc.y * tanHalfFov,
        1.0f));
    ray.TMin = 0.001f;
    ray.TMax = 1000.0f;

    RayPayload payload;
    payload.color = float4(0.0f, 0.0f, 0.0f, 1.0f);

    TraceRay(
        g_scene,
        RAY_FLAG_NONE,
        0xFF,           // 인스턴스 마스크
        0,              // HitGroup 오프셋
        1,              // HitGroup 스트라이드
        0,              // Miss 셰이더 인덱스
        ray,
        payload);

    g_output[launchIndex] = payload.color;
}

// ──────────────────────────────────────────────────────────────
//  Miss 셰이더 – 하늘 그라데이션
// ──────────────────────────────────────────────────────────────
[shader("miss")]
void MissShader(inout RayPayload payload)
{
    float t = WorldRayDirection().y * 0.5f + 0.5f;
    float4 sky    = float4(0.53f, 0.81f, 0.98f, 1.0f); // 하늘색
    float4 horizon= float4(1.0f,  1.0f,  1.0f,  1.0f); // 수평선 흰색
    payload.color = lerp(horizon, sky, saturate(t));
}

// ──────────────────────────────────────────────────────────────
//  ClosestHit 셰이더 – 배리센트릭 컬러
// ──────────────────────────────────────────────────────────────
[shader("closesthit")]
void ClosestHit(inout RayPayload payload,
                in    BuiltInTriangleIntersectionAttributes attr)
{
    float3 bary = float3(
        1.0f - attr.barycentrics.x - attr.barycentrics.y,
        attr.barycentrics.x,
        attr.barycentrics.y);

    // 꼭짓점 색상: 빨강 / 초록 / 파랑
    float4 c0 = float4(1.0f, 0.0f, 0.0f, 1.0f);
    float4 c1 = float4(0.0f, 1.0f, 0.0f, 1.0f);
    float4 c2 = float4(0.0f, 0.0f, 1.0f, 1.0f);

    payload.color = bary.x * c0 + bary.y * c1 + bary.z * c2;
}
