// MetricHistogram.hlsl
// HPAR-PT Phase 2 — percentile_99 정규화의 첫 패스
//
// 픽셀별로 E (log-luminance gradient), S (specular attention), M (relative motion)
// 메트릭을 계산하고 256-bin 히스토그램 (3 metrics × 256 bins = 768 uint) 에 atomic add.
// D = max(Dz_n, Dn) 는 이미 [0,1] 로 bounded 라서 percentile 불필요.
//
// 다음 패스(MetricReduce.hlsl) 가 prefix sum scan → percentile_99 추출.

Texture2D<float>    g_depth        : register(t0);
Texture2D<float4>   g_accumulation : register(t1);
Texture2D<float4>   g_normals      : register(t2);
Texture2D<float2>   g_motionVec    : register(t3);
Texture2D<float4>   g_specAlbedo   : register(t4);
RWByteAddressBuffer g_histogram    : register(u0);  // 3 × 256 uint

cbuffer HistoCB : register(b0)
{
    uint  g_width;
    uint  g_height;
    float g_eMax;   // E 히스토그램 범위 상한 (예: 5.0)
    float g_sMax;   // S 범위 상한 (예: 5.0)
    float g_mMax;   // M 범위 상한 (예: 10.0 픽셀)
    float _pad0[3];
};

// 히스토그램 영역 offset (uint 단위 → byte offset = ×4)
static const uint BIN_BYTES_PER_METRIC = 256u * 4u;
static const uint E_OFFSET = 0u;
static const uint S_OFFSET = BIN_BYTES_PER_METRIC;
static const uint M_OFFSET = 2u * BIN_BYTES_PER_METRIC;

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

uint BinIndex(float value, float vmax)
{
    float t = saturate(value / max(vmax, 1e-6f));
    return min((uint)(t * 256.0f), 255u);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    // E: log-luminance Sobel gradient
    float E = Sobel3x3LumLog((int2)px, g_accumulation);

    // S: (1-r)² · F0_magnitude · log_luminance (specular attention attractor)
    float  r        = g_normals.Load(int3((int2)px, 0)).w;
    float3 F0       = g_specAlbedo.Load(int3((int2)px, 0)).rgb;
    float  F0mag    = length(F0);
    float  L_log    = LumLog(g_accumulation.Load(int3((int2)px, 0)).rgb);
    float  S        = (1.0f - r) * (1.0f - r) * F0mag * L_log;

    // M: |v - mean(v in 3×3)| relative motion
    float2 mv_c = g_motionVec.Load(int3((int2)px, 0));
    float2 mv_sum = float2(0.0f, 0.0f);
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        mv_sum += g_motionVec.Load(int3((int2)px + int2(dx, dy), 0));
    }
    float2 mv_mean = mv_sum / 9.0f;
    float  M = length(mv_c - mv_mean);

    // Atomic add to histogram bins (3 metrics)
    uint binE = BinIndex(E, g_eMax);
    uint binS = BinIndex(S, g_sMax);
    uint binM = BinIndex(M, g_mMax);

    uint dummy;
    g_histogram.InterlockedAdd(E_OFFSET + binE * 4u, 1u, dummy);
    g_histogram.InterlockedAdd(S_OFFSET + binS * 4u, 1u, dummy);
    g_histogram.InterlockedAdd(M_OFFSET + binM * 4u, 1u, dummy);
}
