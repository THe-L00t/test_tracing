// Denoise.hlsl
// A-trous 웨이블릿 디노이저 컴퓨트 셰이더
//
// 3패스 사용 방식 (App에서 step을 바꿔 3번 Dispatch):
//   Pass 0 (step=1, tonemap=0): g_accumulation → ping
//   Pass 1 (step=2, tonemap=0): ping            → pong
//   Pass 2 (step=4, tonemap=1): pong            → g_output  (Reinhard + gamma)
//
// 엣지 스토핑: 색상 유사도(luminance 적응형) + NDC depth + oct-법선

Texture2D<float4>    g_input   : register(t0);  // 노이즈 있는 HDR 입력
Texture2D<float>     g_depth   : register(t1);  // NDC depth [0..1]
Texture2D<float2>    g_normals : register(t2);  // oct-인코딩 월드 법선
RWTexture2D<float4>  g_output  : register(u0);  // 디노이즈 출력

cbuffer DenoiseCB : register(b0)
{
    uint  g_width;        // 렌더 해상도 X
    uint  g_height;       // 렌더 해상도 Y
    uint  g_stepSize;     // A-trous 스텝 (1, 2, 4)
    float g_sigmaColor;   // 색상 유사도 시그마 기저값
    uint  g_doTonemap;    // 1 = Reinhard 톤맵 + 감마 보정 (최종 패스)
    float g_sigmaDepth;   // depth 유사도 시그마 (NDC 단위)
    float g_sigmaNormal;  // 법선 유사도 시그마 (1-cos 단위)
    uint  _pad0;
};

// A-trous 5탭 B3 스플라인 커널
static const float k_h[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

// Oct-디코딩: float2 [-1,1]^2 → 단위 법선 float3
float3 OctDecode(float2 f)
{
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    if (n.z < 0.0f)
    {
        float x = n.x; float y = n.y;
        n.x = (1.0f - abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        n.y = (1.0f - abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(n);
}

// Reinhard 글로벌 톤맵 + 2.2 감마 보정
float3 tonemap(float3 hdr)
{
    hdr = max(0.0f, hdr);
    return pow(hdr / (1.0f + hdr), 1.0f / 2.2f);
}

// 색상 유사도 (luminance 적응형 sigma: 밝은 픽셀일수록 넓은 sigma)
float colorWeight(float3 a, float3 b)
{
    float3 d   = a - b;
    float  lum = max(dot(a, float3(0.2126f, 0.7152f, 0.0722f)), 0.01f);
    float  sig = g_sigmaColor * (1.0f + lum);  // HDR 휘도에 따라 sigma 조정
    return exp(-dot(d, d) / (2.0f * sig * sig + 1e-6f));
}

// depth 유사도 (NDC 공간 절대 차이)
float depthWeight(float d_c, float d_s)
{
    float diff = abs(d_c - d_s);
    return exp(-diff * diff / (2.0f * g_sigmaDepth * g_sigmaDepth + 1e-6f));
}

// 법선 유사도 (1 - cos(angle): 0=동일, 1=직교)
float normalWeight(float2 enc_c, float2 enc_s)
{
    float3 n_c = OctDecode(enc_c);
    float3 n_s = OctDecode(enc_s);
    float  diff = 1.0f - saturate(dot(n_c, n_s));
    return exp(-diff * diff / (2.0f * g_sigmaNormal * g_sigmaNormal + 1e-6f));
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    float4 center   = g_input.Load(int3(px, 0));
    float  d_center = g_depth.Load(int3(px, 0));
    float2 n_center = g_normals.Load(int3(px, 0));

    float4 sum  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    // 5x5 A-trous 커널
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 sp;
            sp.x = clamp((int)px.x + dx * (int)g_stepSize, 0, (int)g_width  - 1);
            sp.y = clamp((int)px.y + dy * (int)g_stepSize, 0, (int)g_height - 1);

            float4 s  = g_input.Load(int3(sp, 0));
            float  d  = g_depth.Load(int3(sp, 0));
            float2 n  = g_normals.Load(int3(sp, 0));

            float kw = k_h[dx + 2] * k_h[dy + 2];
            float cw = colorWeight(center.rgb, s.rgb);
            float dw = depthWeight(d_center, d);
            float nw = normalWeight(n_center, n);
            float w  = kw * cw * dw * nw;

            sum  += s * w;
            wsum += w;
        }
    }

    float4 result = (wsum > 1e-6f) ? (sum / wsum) : center;

    if (g_doTonemap)
        g_output[px] = float4(tonemap(result.rgb), 1.0f);
    else
        g_output[px] = float4(result.rgb, 1.0f);
}
