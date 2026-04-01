// Denoise.hlsl
// A-trous 웨이블릿 디노이저 컴퓨트 셰이더
//
// 3패스 사용 방식 (App에서 step을 바꿔 3번 Dispatch):
//   Pass 0 (step=1, tonemap=0): g_accumulation → ping
//   Pass 1 (step=2, tonemap=0): ping            → pong
//   Pass 2 (step=4, tonemap=1): pong            → g_output  (Reinhard + gamma)
//
// 색상 유사도 기반 양방향 가중치로 엣지를 보존하면서 노이즈를 제거한다.
// G-Buffer(법선/깊이)가 없으므로 순수 색상 기반 bilateral filter를 사용한다.

Texture2D<float4>    g_input  : register(t0);   // 노이즈 있는 HDR 입력
RWTexture2D<float4>  g_output : register(u0);   // 디노이즈 출력

cbuffer DenoiseCB : register(b0)
{
    uint  g_width;        // 렌더 해상도 X
    uint  g_height;       // 렌더 해상도 Y
    uint  g_stepSize;     // A-trous 스텝 (1, 2, 4)
    float g_sigmaColor;   // 색상 유사도 시그마 (0.1~0.5 권장)
    uint  g_doTonemap;    // 1 = Reinhard 톤맵 + 감마 보정 적용 (최종 패스)
    uint  _pad0, _pad1, _pad2;
};

// A-trous 5탭 B3 스플라인 커널 (1D, 행·열로 분리하여 2D 커널 구성)
// h = [1/16, 1/4, 3/8, 1/4, 1/16]
static const float k_h[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

// 색상 유사도 가중치: 두 픽셀의 색 차이가 클수록 가중치 감소 (엣지 보존)
float colorWeight(float3 a, float3 b)
{
    float3 d = a - b;
    return exp(-dot(d, d) / (2.0f * g_sigmaColor * g_sigmaColor + 1e-6f));
}

// Reinhard 글로벌 톤맵 + 2.2 감마 보정
float3 tonemap(float3 hdr)
{
    hdr = max(0.0f, hdr);
    return pow(hdr / (1.0f + hdr), 1.0f / 2.2f);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    float4 center = g_input.Load(int3(px, 0));
    float4 sum    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float  wsum   = 0.0f;

    // 5x5 A-trous 커널 (스텝 크기만큼 간격을 두고 샘플링)
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 sp;
            sp.x = clamp((int)px.x + dx * (int)g_stepSize, 0, (int)g_width  - 1);
            sp.y = clamp((int)px.y + dy * (int)g_stepSize, 0, (int)g_height - 1);

            float4 s  = g_input.Load(int3(sp, 0));
            float  kw = k_h[dx + 2] * k_h[dy + 2];    // 공간 커널 가중치
            float  cw = colorWeight(center.rgb, s.rgb); // 색상 유사도 가중치
            float  w  = kw * cw;

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
