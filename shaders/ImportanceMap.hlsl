// ImportanceMap.hlsl
// HPAR-PT Stage 2 (PASS 1) — Perceptual Importance Estimation
//
// Phase 2 완성형:
//   I(x,y)        = w_e·Ê + w_d·D̂ + w_s·Ŝ + w_m·M̂
//   I_final(x,y)  = I · (1 + w_v · V)
//   각 항 percentile_99 정규화 (E, S, M). D 는 이미 [0,1] bounded.
//   V (semantic saliency) 는 stub = 0 (Phase 16 이후 NN 추가)
//
// 입력 SRV:
//   t0: g_depth         R32F NDC depth
//   t1: g_accumulation  RGBA32F path-traced HDR
//   t2: g_normals       RGBA16F world normal(xyz) + roughness(w)
//   t3: g_motionVec     RG16F  pixel-space motion vector
//   t4: g_specAlbedo    RGBA16F specular F0
//   t5: g_percentile    StructuredBuffer<float4> (E_99, S_99, M_99, 1)
//
// 출력 UAV:
//   u0: g_importance    R16F  Î(x,y) ∈ [0, 1]

Texture2D<float>            g_depth        : register(t0);
Texture2D<float4>           g_accumulation : register(t1);
Texture2D<float4>           g_normals      : register(t2);
Texture2D<float2>           g_motionVec    : register(t3);
Texture2D<float4>           g_specAlbedo   : register(t4);
StructuredBuffer<float4>    g_percentile   : register(t5);
RWTexture2D<float>          g_importance   : register(u0);

cbuffer ImportanceCB : register(b0)
{
    uint  g_width;
    uint  g_height;
    float g_weightE;   // 0.40 (Daly 1993 HVS CSF edge peak)
    float g_weightD;   // 0.25
    float g_weightS;   // 0.20
    float g_weightM;   // 0.15
    float g_weightV;   // semantic multiplier (V항 stub 이라 효과 없음)
    float _pad0;
};

float LumLog(float3 c)
{
    float L = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    return log(1.0f + max(L, 0.0f));
}

float Sobel3x3LumLog(int2 c, Texture2D<float4> tex)
{
    float p00 = LumLog(tex.Load(int3(c + int2(-1,-1),0)).rgb);
    float p10 = LumLog(tex.Load(int3(c + int2( 0,-1),0)).rgb);
    float p20 = LumLog(tex.Load(int3(c + int2( 1,-1),0)).rgb);
    float p01 = LumLog(tex.Load(int3(c + int2(-1, 0),0)).rgb);
    float p21 = LumLog(tex.Load(int3(c + int2( 1, 0),0)).rgb);
    float p02 = LumLog(tex.Load(int3(c + int2(-1, 1),0)).rgb);
    float p12 = LumLog(tex.Load(int3(c + int2( 0, 1),0)).rgb);
    float p22 = LumLog(tex.Load(int3(c + int2( 1, 1),0)).rgb);
    float gx = (-p00 + p20) + 2.0f * (-p01 + p21) + (-p02 + p22);
    float gy = (-p00 - 2.0f * p10 - p20) + (p02 + 2.0f * p12 + p22);
    return sqrt(gx * gx + gy * gy);
}

float Sobel3x3Depth(int2 c, Texture2D<float> tex)
{
    float p00 = tex.Load(int3(c + int2(-1,-1),0));
    float p10 = tex.Load(int3(c + int2( 0,-1),0));
    float p20 = tex.Load(int3(c + int2( 1,-1),0));
    float p01 = tex.Load(int3(c + int2(-1, 0),0));
    float p21 = tex.Load(int3(c + int2( 1, 0),0));
    float p02 = tex.Load(int3(c + int2(-1, 1),0));
    float p12 = tex.Load(int3(c + int2( 0, 1),0));
    float p22 = tex.Load(int3(c + int2( 1, 1),0));
    float gx = (-p00 + p20) + 2.0f * (-p01 + p21) + (-p02 + p22);
    float gy = (-p00 - 2.0f * p10 - p20) + (p02 + 2.0f * p12 + p22);
    return sqrt(gx * gx + gy * gy);
}

// 안쪽 모서리 (concave): normal 변화 최대값 from 3×3 neighbors
float NormalEdgeMax3x3(int2 c, Texture2D<float4> tex)
{
    float3 n_c = normalize(tex.Load(int3(c, 0)).xyz);
    float maxDiff = 0.0f;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        float3 n_s = normalize(tex.Load(int3(c + int2(dx, dy), 0)).xyz);
        maxDiff = max(maxDiff, 1.0f - saturate(dot(n_c, n_s)));
    }
    return maxDiff;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    int2 ip = (int2)px;

    // ── 1. 메트릭 계산 ────────────────────────────────────────────
    float E   = Sobel3x3LumLog(ip, g_accumulation);
    float Dz  = Sobel3x3Depth(ip, g_depth);
    float Dn  = NormalEdgeMax3x3(ip, g_normals);

    float  r      = g_normals.Load(int3(ip, 0)).w;
    float3 F0     = g_specAlbedo.Load(int3(ip, 0)).rgb;
    float  L_log  = LumLog(g_accumulation.Load(int3(ip, 0)).rgb);
    float  S      = (1.0f - r) * (1.0f - r) * length(F0) * L_log;

    float2 mv_c   = g_motionVec.Load(int3(ip, 0));
    float2 mv_sum = float2(0.0f, 0.0f);
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        mv_sum += g_motionVec.Load(int3(ip + int2(dx, dy), 0));
    }
    float M = length(mv_c - mv_sum / 9.0f);

    // V (Semantic saliency) — Phase 16 이후 NN 추론으로 채울 자리
    //   현재는 stub = 0 이라 I_final = I (V 항 효과 없음)
    float V = 0.0f;

    // ── 2. percentile_99 정규화 ──────────────────────────────────
    //   X̂ = X / (percentile_99(X) + ε)
    //   D 는 max(Dz_n, Dn) 형태로 이미 [0,1] bounded → 별도 정규화 불필요
    const float eps = 1e-4f;
    float4 pct = g_percentile[0];   // (E_99, S_99, M_99, 1.0)

    float E_hat = saturate(E / (pct.x + eps));
    float D_hat = max(saturate(Dz * 50.0f), Dn);  // depth Sobel * 50 + normal edge
    float S_hat = saturate(S / (pct.y + eps));
    float M_hat = saturate(M / (pct.z + eps));

    // ── 3. 가중치 합 + V 멀티플라이어 ────────────────────────────
    float I       = g_weightE * E_hat
                  + g_weightD * D_hat
                  + g_weightS * S_hat
                  + g_weightM * M_hat;
    float I_final = I * (1.0f + g_weightV * V);

    g_importance[px] = saturate(I_final);
}
