// SVGFTemporalAccum.hlsl
// SVGF 시간적 누적 패스 — 모션벡터로 이전 프레임을 재투영하고 지수이동평균으로 블렌딩.
// 동시에 luminance 1차/2차 모멘트(μ, μ²)를 누적하여 다음 웨이블릿 패스에서 분산 추정에 사용.

Texture2D<float4>    g_curIllum    : register(t0);  // RGBA32F: 현재 1spp 광휘도
Texture2D<float4>    g_prevAccum   : register(t1);  // RGBA32F: 이전 프레임 누적 (A=histLen)
Texture2D<float4>    g_prevMoments : register(t2);  // RGBA32F: 이전 모멘트 (μ, μ² — .xy만 사용)
Texture2D<float2>    g_motionVec   : register(t3);  // RG16F:   렌더 해상도 픽셀 MV (curr→prev)
Texture2D<float>     g_depth       : register(t4);  // R32F:    현재 NDC depth
Texture2D<float>     g_prevDepth   : register(t5);  // R32F:    이전 프레임 NDC depth
Texture2D<float2>    g_normals     : register(t6);  // RG16F:   현재 oct-법선
Texture2D<float2>    g_prevNormals : register(t7);  // RG16F:   이전 프레임 oct-법선

RWTexture2D<float4>  g_accumOut    : register(u0);  // RGBA32F: 출력 (A=histLen)
RWTexture2D<float4>  g_momentsOut  : register(u1);  // RGBA32F: 출력 모멘트 (.xy만 사용)

SamplerState g_linearClamp : register(s0);

cbuffer TemporalCB : register(b0)
{
    uint  g_width;
    uint  g_height;
    float g_alpha;         // 최소 블렌드 계수 (예: 0.1). 1.0 = 완전 리셋
    float g_depthThresh;   // NDC depth 차이 허용값 (예: 0.05)
    float g_normalThresh;  // cos(angle) 하한 (예: 0.9)
    uint  _pad0, _pad1, _pad2;
};

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

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

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    float3 curColor = g_curIllum.Load(int3(px, 0)).rgb;
    float  curLum   = Luminance(curColor);
    float  curDepth = g_depth.Load(int3(px, 0));
    float2 curNEnc  = g_normals.Load(int3(px, 0));

    // ── 재투영: 모션벡터로 이전 픽셀 좌표 계산 ─────────────────────
    float2 mv     = g_motionVec.Load(int3(px, 0));
    float2 prevPx = float2(px) + mv;
    float2 prevUV = (prevPx + 0.5f) / float2((float)g_width, (float)g_height);

    // ── 리셋 모드 (g_alpha >= 1.0): 히스토리 무효화 ────────────────
    if (g_alpha >= 1.0f)
    {
        g_accumOut  [px] = float4(curColor, 1.0f);
        g_momentsOut[px] = float4(curLum, curLum * curLum, 0.0f, 0.0f);
        return;
    }

    // ── 히스토리 로드 (유효 범위 내 재투영 결과만) ──────────────────
    bool   valid     = all(prevUV > 0.0f) && all(prevUV < 1.0f);
    float4 histAccum = float4(0, 0, 0, 0);
    float2 histMom   = float2(0.0f, 0.0f);

    if (valid)
    {
        // 컬러: bilinear 샘플 (시간적 품질 향상)
        histAccum = g_prevAccum.SampleLevel(g_linearClamp, prevUV, 0);

        // 모멘트: nearest (정확한 분산 유지)
        int2 prevPxi = (int2)clamp(prevPx,
            float2(0.0f, 0.0f),
            float2((float)(g_width - 1), (float)(g_height - 1)));
        histMom = g_prevMoments.Load(int3(prevPxi, 0)).xy;

        // 재투영 유효성 검사 — depth + 법선 불일치 시 히스토리 폐기
        float  prevDepth = g_prevDepth.Load(int3(prevPxi, 0));
        float2 prevNEnc  = g_prevNormals.Load(int3(prevPxi, 0));

        float3 curN  = OctDecode(curNEnc);
        float3 prevN = OctDecode(prevNEnc);

        bool depthOK  = abs(curDepth - prevDepth) < g_depthThresh;
        bool normalOK = dot(curN, prevN)           > g_normalThresh;

        if (!depthOK || !normalOK)
        {
            histAccum = float4(0, 0, 0, 0);
            histMom   = float2(0, 0);
        }
    }

    // ── 시간적 블렌딩 ───────────────────────────────────────────────
    float histLen    = max(0.0f, min(histAccum.a, 128.0f));
    float newHistLen = min(histLen + 1.0f, 128.0f);
    float alpha      = max(g_alpha, 1.0f / newHistLen);

    float3 blendColor = lerp(histAccum.rgb, curColor, alpha);
    float  blendMom1  = lerp(histMom.r, curLum,          alpha);
    float  blendMom2  = lerp(histMom.g, curLum * curLum, alpha);

    g_accumOut  [px] = float4(blendColor, newHistLen);
    g_momentsOut[px] = float4(blendMom1, blendMom2, 0.0f, 0.0f);
}
