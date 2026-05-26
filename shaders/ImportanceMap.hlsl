// ImportanceMap.hlsl
// HPAR-PT Stage 2 (PASS 1) — Perceptual Importance Estimation
//
// Phase 1 (현재): E + D 만 (raw Sobel 합), 정규화/EMA 없음
//   I(x,y) = w_e · E + w_d · D
//   E = |∇L| via Sobel 3×3 on luminance
//   D = |∇z| via Sobel 3×3 on linear viewZ
//
// 다음 단계 (Phase 2): percentile_99 정규화 + S/M/V 항 추가
// 다음 단계 (Phase 3): Temporal EMA smoothing + motion gate

Texture2D<float>   g_depth        : register(t0);  // linear viewZ (NRD 인코딩) OR NDC depth
Texture2D<float4>  g_accumulation : register(t1);  // path tracer HDR output (RGBA32F)
RWTexture2D<float> g_importance   : register(u0);  // 출력 (R16F)

cbuffer ImportanceCB : register(b0)
{
    uint  g_width;
    uint  g_height;
    float g_weightE;   // 초기값 0.5 (Phase 2에서 0.40 가중치로 변경)
    float g_weightD;   // 초기값 0.5 (Phase 2에서 0.25)
};

// HDR 휘도
float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Sobel 3×3 X kernel: [[-1,0,1],[-2,0,2],[-1,0,1]]
// Sobel 3×3 Y kernel: [[-1,-2,-1],[0,0,0],[1,2,1]]
//
// 인접 픽셀 9개의 scalar 값에 대해 gradient magnitude 반환
float Sobel3x3(int2 center, Texture2D<float> tex)
{
    float p00 = tex.Load(int3(center + int2(-1, -1), 0));
    float p10 = tex.Load(int3(center + int2( 0, -1), 0));
    float p20 = tex.Load(int3(center + int2( 1, -1), 0));
    float p01 = tex.Load(int3(center + int2(-1,  0), 0));
    float p21 = tex.Load(int3(center + int2( 1,  0), 0));
    float p02 = tex.Load(int3(center + int2(-1,  1), 0));
    float p12 = tex.Load(int3(center + int2( 0,  1), 0));
    float p22 = tex.Load(int3(center + int2( 1,  1), 0));

    float gx = (-p00 + p20) + 2.0f * (-p01 + p21) + (-p02 + p22);
    float gy = (-p00 - 2.0f * p10 - p20) + (p02 + 2.0f * p12 + p22);
    return sqrt(gx * gx + gy * gy);
}

float Sobel3x3Luminance(int2 center, Texture2D<float4> tex)
{
    float p00 = Luminance(tex.Load(int3(center + int2(-1, -1), 0)).rgb);
    float p10 = Luminance(tex.Load(int3(center + int2( 0, -1), 0)).rgb);
    float p20 = Luminance(tex.Load(int3(center + int2( 1, -1), 0)).rgb);
    float p01 = Luminance(tex.Load(int3(center + int2(-1,  0), 0)).rgb);
    float p21 = Luminance(tex.Load(int3(center + int2( 1,  0), 0)).rgb);
    float p02 = Luminance(tex.Load(int3(center + int2(-1,  1), 0)).rgb);
    float p12 = Luminance(tex.Load(int3(center + int2( 0,  1), 0)).rgb);
    float p22 = Luminance(tex.Load(int3(center + int2( 1,  1), 0)).rgb);

    float gx = (-p00 + p20) + 2.0f * (-p01 + p21) + (-p02 + p22);
    float gy = (-p00 - 2.0f * p10 - p20) + (p02 + 2.0f * p12 + p22);
    return sqrt(gx * gx + gy * gy);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    // 경계 픽셀: Sobel 3×3 가 외부 access — clamp 로 처리됨 (HLSL Load 는 boundary clamp)
    // 더 안전하게 하려면 if(1 ≤ x < W-1 && ...) 조건 두지만 일단 단순화
    float E = Sobel3x3Luminance(int2(px), g_accumulation);
    float D = Sobel3x3((int2)px, g_depth);

    // Phase 1: 단순 합 + saturate (Phase 2 에서 percentile_99 정규화로 교체)
    //   E 는 HDR 휘도 차이 → 값 범위 0~~100+ → 임시로 1/10 스케일
    //   D 는 NDC depth 또는 viewZ 차이 → 일단 100x 증폭
    //   Phase 2 에서 percentile_99 로 robust 정규화하면 이 스케일링 불필요
    float En = saturate(E * 0.1f);
    float Dn = saturate(D * 100.0f);

    float I = g_weightE * En + g_weightD * Dn;
    g_importance[px] = saturate(I);
}
