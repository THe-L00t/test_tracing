// ──────────────────────────────────────────────────────────────
//  GBuffer.hlsl  –  [Pass 1] Primary Ray → G-Buffer
//
//  역할: 카메라 레이를 쏴서 첫 번째 교점의 표면 정보를 G-Buffer에 기록.
//        ReSTIR Initial/Temporal/Spatial 패스가 이 데이터를 읽음.
//
//  G-Buffer 출력 (UAV):
//   u2 gbuf_worldPos  RGBA32F : XYZ=월드위치,  W=히트거리 (miss: W<0)
//   u3 gbuf_normal    RGBA32F : XYZ=월드법선,  W=matIdx(float)
//   u4 gbuf_albedo    RGBA8   : RGB=albedo,    A=metallic
//   u5 gbuf_matInfo   RGBA32F : R=roughness, G=linearDepth, B=flags, A=0
//       flags: GB_FLAG_NORMAL(0) / GB_FLAG_GLASS(1) / GB_FLAG_EMISSIVE(2)
// ──────────────────────────────────────────────────────────────

#include "Common_ReSTIR.hlsli"

// ── 리소스 ──────────────────────────────────────────────────────
RWTexture2D<float4>             gbuf_worldPos : register(u2);
RWTexture2D<float4>             gbuf_normal   : register(u3);
RWTexture2D<float4>             gbuf_albedo   : register(u4);
RWTexture2D<float4>             gbuf_matInfo  : register(u5);

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

// ── G-Buffer 전용 레이 페이로드 ──────────────────────────────
struct GBPayload
{
    float3 worldPos;
    float  hitDist;   // -1 = miss
    float3 normal;
    float  matIdxF;
};

// ── RayGen_GB ─────────────────────────────────────────────────
[shader("raygeneration")]
void RayGen_GB()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    // 픽셀 중심 레이 (지터 없음 – 정확한 재투영 필요)
    float2 uv  = ((float2)idx + 0.5f) / (float2)dim;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float3 dir = normalize(
        camForward
        + camRight  * (ndc.x * aspectRatio * tanHalfFovY)
        + camUp     * (ndc.y * tanHalfFovY)
    );

    RayDesc ray;
    ray.Origin    = camPos;
    ray.Direction = dir;
    ray.TMin      = 0.001f;
    ray.TMax      = 1e6f;

    GBPayload payload;
    payload.worldPos = float3(0, 0, 0);
    payload.hitDist  = -1.0f;
    payload.normal   = float3(0, 1, 0);
    payload.matIdxF  = 0.0f;

    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    gbuf_worldPos[idx] = float4(payload.worldPos, payload.hitDist);
    gbuf_normal[idx]   = float4(payload.normal,   payload.matIdxF);
    // albedo/matInfo 는 ClosestHit_GB 에서 직접 기록
}

// ── Miss_GB ───────────────────────────────────────────────────
[shader("miss")]
void Miss_GB(inout GBPayload payload)
{
    payload.hitDist = -1.0f;  // 배경: Initial/Temporal/Spatial에서 skip
}

// ── ClosestHit_GB ─────────────────────────────────────────────
[shader("closesthit")]
void ClosestHit_GB(inout GBPayload payload,
                   BuiltInTriangleIntersectionAttributes attr)
{
    uint2 idx = DispatchRaysIndex().xy;

    // InstanceID 디코딩: 하위 4비트 = geomType, 상위 = matIdx
    uint rawID    = InstanceID();
    uint geomType = rawID & 0xFu;
    uint matIdx   = rawID >> 4u;

    float3 albedo    = matAlbedoRoughness[matIdx].xyz;
    float  metallic  = matMetallic[matIdx];
    float  roughness = matAlbedoRoughness[matIdx].w;
    float  emissive  = matEmissive[matIdx];

    // 바리센트릭 보간으로 법선 계산
    uint   primIdx = PrimitiveIndex();
    float2 bary    = attr.barycentrics;
    float3 b       = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    uint   vi      = primIdx * 3;

    float3 n0, n1, n2;
    if      (geomType == 0u) { n0=g_vbPlane[vi].normal;  n1=g_vbPlane[vi+1].normal;  n2=g_vbPlane[vi+2].normal; }
    else if (geomType == 2u) { n0=g_vbRoom[vi].normal;   n1=g_vbRoom[vi+1].normal;   n2=g_vbRoom[vi+2].normal;  }
    else if (geomType == 3u) { n0=g_vbSphere[vi].normal; n1=g_vbSphere[vi+1].normal; n2=g_vbSphere[vi+2].normal;}
    else                     { n0=g_vbCube[vi].normal;   n1=g_vbCube[vi+1].normal;   n2=g_vbCube[vi+2].normal;  }

    float3 rawN  = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
    float3 V     = -normalize(WorldRayDirection());
    float3 N     = dot(rawN, V) < 0.0f ? -rawN : rawN;  // 시점 기준 플립
    float  t     = RayTCurrent();
    float3 hPos  = WorldRayOrigin() + WorldRayDirection() * t;

    payload.worldPos = hPos;
    payload.hitDist  = t;
    payload.normal   = N;
    payload.matIdxF  = float(matIdx);

    // G-Buffer flags: 재질 종류 분류
    float flags;
    if      (emissive < -0.5f) flags = GB_FLAG_GLASS;     // 유리/반투명 (convention: emissive<0)
    else if (emissive >  0.0f) flags = GB_FLAG_EMISSIVE;  // 발광
    else                       flags = GB_FLAG_NORMAL;     // 일반

    gbuf_albedo[idx]  = float4(albedo, metallic);
    gbuf_matInfo[idx] = float4(roughness, t, flags, 0.0f);
}
