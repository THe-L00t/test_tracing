// ──────────────────────────────────────────────────────────────
//  GBuffer.hlsl  –  [Pass 1] Primary Ray → G-Buffer
//
//  역할: 카메라 레이를 쏴서 첫 번째 교점의 표면 정보를 G-Buffer에 기록
//        ReSTIR의 Initial/Temporal/Spatial 패스가 이 데이터를 읽음
//
//  출력 G-Buffer (UAV):
//   u2 gbuf_worldPos  RGBA32F : XYZ=월드위치, W=히트거리 (miss: W=-1)
//   u3 gbuf_normal    RGBA32F : XYZ=법선,     W=matIdx(float 캐스트)
//   u4 gbuf_albedo    RGBA8   : RGB=albedo,   A=metallic (0~1 → 0~255)
//   u5 gbuf_matInfo   RGBA32F : R=roughness,  G=선형depth, B=entering(0/1), A=미사용
//
//  TODO [Session 1]:
//   - ClosestHit_GB 에서 RayPayload_GB 채워서 각 UAV에 기록
//   - Miss_GB 에서 gbuf_worldPos.w = -1.0 (배경 마킹)
//   - RayGen_GB 는 서브픽셀 지터 없이 픽셀 중심 레이
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

cbuffer SceneConstants  : register(b0) { /* Common.h SceneCB 레이아웃 */ float3 camPos; uint sceneID; float3 camRight; float tanHalfFovY; float3 camUp; float aspectRatio; float3 camForward; float _p0; float4 _lightBlock[4]; float4 matAlbedoRoughness[4]; float4 matMetallic; float4 matEmissive; uint frameCount; uint randomSeed; float emissBoxHalfSize; float _p1; float3 emissBoxCenter; float _p2; }
cbuffer ReSTIRConstants : register(b1) { /* Common.h ReSTIRCB 레이아웃 */ float4 _rc[8]; }

// ── G-Buffer 전용 페이로드 (24바이트) ───────────────────────────
struct GBPayload
{
    float3 worldPos;   // 교점 월드 좌표
    float  hitDist;    // 레이 파라미터 t (-1 = miss)
    float3 normal;     // 월드 법선 (시점 기준 플립)
    float  matIdxF;    // matIdx를 float으로 캐스트
    // albedo, metallic, roughness 는 ClosestHit에서 직접 UAV에 씀
};

// ── RayGen_GB ────────────────────────────────────────────────
[shader("raygeneration")]
void RayGen_GB()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    // 픽셀 중심 레이 (G-Buffer는 지터 없음 – 픽셀 정확 재투영 필요)
    float2 uv  = ((float2)idx + 0.5f) / (float2)dim;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float3 dir = normalize(
        camForward
        + camRight   * (ndc.x * aspectRatio * tanHalfFovY)
        + camUp      * (ndc.y * tanHalfFovY)
    );

    RayDesc ray;
    ray.Origin    = camPos;
    ray.Direction = dir;
    ray.TMin      = 0.001f;
    ray.TMax      = 1e6f;

    GBPayload payload;
    payload.worldPos = float3(0,0,0);
    payload.hitDist  = -1.0f;
    payload.normal   = float3(0,1,0);
    payload.matIdxF  = 0.0f;

    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    // TODO [Session 1]: payload 결과를 G-Buffer UAV에 기록
    gbuf_worldPos[idx] = float4(payload.worldPos, payload.hitDist);
    gbuf_normal[idx]   = float4(payload.normal,   payload.matIdxF);
    // albedo/matInfo 는 ClosestHit_GB 에서 직접 기록
}

// ── Miss_GB ──────────────────────────────────────────────────
[shader("miss")]
void Miss_GB(inout GBPayload payload)
{
    payload.hitDist = -1.0f;  // 배경 마킹
}

// ── ClosestHit_GB ────────────────────────────────────────────
[shader("closesthit")]
void ClosestHit_GB(inout GBPayload payload,
                   BuiltInTriangleIntersectionAttributes attr)
{
    // TODO [Session 1]:
    // 1. InstanceID 디코딩 → geomType, matIdx
    // 2. 바리센트릭으로 법선 보간 → rawN
    // 3. V = -WorldRayDirection(), N = dot(rawN,V)<0 ? -rawN : rawN
    // 4. hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent()
    // 5. 재질 읽기 (matAlbedoRoughness, matMetallic)
    // 6. payload 채우기
    // 7. gbuf_albedo[idx]   = float4(albedo, metallic)
    // 8. gbuf_matInfo[idx]  = float4(roughness, hitDist/farPlane, entering, 0)
    //    단, 유리/반투명(emissive<-0.5)은 gbuf_matInfo.b = -1 로 마킹
    //    → Initial 패스에서 skip 처리

    uint2 idx = DispatchRaysIndex().xy;

    uint rawID    = InstanceID();
    uint geomType = rawID & 0xFu;
    uint matIdx   = rawID >> 4u;

    float3 albedo    = matAlbedoRoughness[matIdx].xyz;
    float  metallic  = matMetallic[matIdx];
    float  roughness = matAlbedoRoughness[matIdx].w;
    float  emissive  = matEmissive[matIdx];

    // 법선 보간
    uint primIdx = PrimitiveIndex();
    float2 bary  = attr.barycentrics;
    float3 b     = float3(1.0f - bary.x - bary.y, bary.x, bary.y);
    uint vi      = primIdx * 3;

    float3 n0, n1, n2;
    if      (geomType == 0u) { n0=g_vbPlane[vi].normal;  n1=g_vbPlane[vi+1].normal;  n2=g_vbPlane[vi+2].normal; }
    else if (geomType == 2u) { n0=g_vbRoom[vi].normal;   n1=g_vbRoom[vi+1].normal;   n2=g_vbRoom[vi+2].normal;  }
    else if (geomType == 3u) { n0=g_vbSphere[vi].normal; n1=g_vbSphere[vi+1].normal; n2=g_vbSphere[vi+2].normal;}
    else                     { n0=g_vbCube[vi].normal;   n1=g_vbCube[vi+1].normal;   n2=g_vbCube[vi+2].normal;  }

    float3 rawN  = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
    float3 V     = -normalize(WorldRayDirection());
    float3 N     = dot(rawN, V) < 0.0f ? -rawN : rawN;
    float  t     = RayTCurrent();
    float3 hPos  = WorldRayOrigin() + WorldRayDirection() * t;
    bool entering = dot(rawN, V) >= 0.0f;

    payload.worldPos = hPos;
    payload.hitDist  = t;
    payload.normal   = N;
    payload.matIdxF  = float(matIdx);

    // 투명/반투명은 B채널 -1 로 마킹 (ReSTIR Initial에서 skip)
    float skipFlag = (emissive < -0.5f) ? -1.0f : (entering ? 1.0f : 0.0f);

    gbuf_albedo[idx]  = float4(albedo, metallic);
    gbuf_matInfo[idx] = float4(roughness, t, skipFlag, 0.0f);
}
