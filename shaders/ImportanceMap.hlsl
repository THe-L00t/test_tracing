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

// HDR 휘도 — log 압축으로 1spp PT 의 firefly/노이즈 변동성 축소
//   raw L: 0..100+ (HDR), Sobel 시 노이즈가 곧 edge 로 오인됨
//   log(1+L): 0..5 정도로 compress → Sobel 이 진짜 휘도 edge 만 검출
float Luminance(float3 c)
{
    float L = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    return log(1.0f + max(L, 0.0f));
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
    //   E 는 log-luminance 차이 → 0..~3 범위 → 0.3x 스케일 (이전 0.1 은 노이즈 너무 강조)
    //   D 는 NDC depth 차이 → 50x (이전 100 은 작은 곡면도 saturate)
    //   Phase 1 에서는 D 위주 (1spp noise 가 E 를 오염시켜 geometry edge 가 묻힘)
    //   Phase 3 EMA 들어가면 E 가중치 복원
    float En = saturate(E * 0.3f);
    float Dn = saturate(D * 50.0f);

    float I = g_weightE * En + g_weightD * Dn;
    g_importance[px] = saturate(I);
}
