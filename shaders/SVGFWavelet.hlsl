// SVGFWavelet.hlsl
// 분산 유도 A-trous 웨이블릿 — 3x3 로컬 분산 추정 (temporal 없음)
// DLSS 앞에서 공간 노이즈만 제거; temporal accumulation은 DLSS에 위임

Texture2D<float4>    g_input   : register(t0);  // RGBA32F: 입력 색상
Texture2D<float>     g_depth   : register(t1);  // R32F:    NDC depth
Texture2D<float2>    g_normals : register(t2);  // RG16F:   oct-법선
RWTexture2D<float4>  g_output  : register(u0);  // 출력 (마지막 패스는 RGBA8 UAV)

cbuffer WaveletCB : register(b0)
{
    uint  g_width;
    uint  g_height;
    uint  g_stepSize;
    float g_phiColor;    // 분산 스케일 인수 (권장: 4.0)
    uint  g_doTonemap;
    float g_sigmaDepth;
    float g_sigmaNormal;
    uint  _pad0;
};

static const float k_h[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

float3 OctDecode(float2 f)
{
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    if (n.z < 0.0f)
    {
        float2 s = float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
        n.xy = (1.0f - abs(n.yx)) * s;
    }
    return normalize(n);
}

float3 Tonemap(float3 hdr)
{
    hdr = max(0.0f, hdr);
    return pow(hdr / (1.0f + hdr), 1.0f / 2.2f);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    float4 center   = g_input.Load(int3(px, 0));
    float  d_center = g_depth.Load(int3(px, 0));
    float2 n_center = g_normals.Load(int3(px, 0));
    float  lum_c    = Luminance(center.rgb);

    // 3×3 이웃으로 현재 프레임 로컬 분산 추정 (temporal 불필요)
    float lum_sum = 0.0f, lum_sq = 0.0f;
    [unroll]
    for (int ly = -1; ly <= 1; ++ly)
    {
        [unroll]
        for (int lx = -1; lx <= 1; ++lx)
        {
            int2 sp = clamp(int2(px) + int2(lx, ly),
                            int2(0, 0),
                            int2((int)g_width - 1, (int)g_height - 1));
            float l = Luminance(g_input.Load(int3(sp, 0)).rgb);
            lum_sum += l;
            lum_sq  += l * l;
        }
    }
    float mu      = lum_sum / 9.0f;
    float var     = max(0.0f, lum_sq / 9.0f - mu * mu);
    float sigma_l = g_phiColor * sqrt(var) + 1e-4f;

    float4 sum  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 sp;
            sp.x = clamp((int)px.x + dx * (int)g_stepSize, 0, (int)g_width  - 1);
            sp.y = clamp((int)px.y + dy * (int)g_stepSize, 0, (int)g_height - 1);

            float4 s     = g_input.Load(int3(sp, 0));
            float  d     = g_depth.Load(int3(sp, 0));
            float2 n     = g_normals.Load(int3(sp, 0));
            float  lum_s = Luminance(s.rgb);

            float kw = k_h[dx + 2] * k_h[dy + 2];

            float dl = abs(lum_c - lum_s);
            float cw = exp(-dl * dl / (sigma_l * sigma_l + 1e-6f));

            float dd = abs(d_center - d);
            float dw = exp(-dd * dd / (2.0f * g_sigmaDepth * g_sigmaDepth + 1e-6f));

            float3 nc = OctDecode(n_center);
            float3 ns = OctDecode(n);
            float  dn = 1.0f - saturate(dot(nc, ns));
            float  nw = exp(-dn * dn / (2.0f * g_sigmaNormal * g_sigmaNormal + 1e-6f));

            float w = kw * cw * dw * nw;
            sum  += s * w;
            wsum += w;
        }
    }

    float4 result = (wsum > 1e-6f) ? (sum / wsum) : center;

    if (g_doTonemap)
        g_output[px] = float4(Tonemap(result.rgb), 1.0f);
    else
        g_output[px] = float4(result.rgb, 1.0f);
}
