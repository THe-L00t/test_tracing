// ImportanceEMA.hlsl
// HPAR-PT Phase 3 — Importance Map 의 Temporal EMA Smoothing
//
//   Î_smooth(t) = α · Î_raw(t) + (1 - α) · Î_smooth(t-1)
//   α = 0.2 기본, motion 임계 초과 시 α = 1.0 (history 무효화)
//
// Ping-pong 입력:
//   t0: g_raw    — 이번 프레임 ImportanceMap pass 의 출력 (raw Î)
//   t1: g_history — 이전 프레임의 smooth Î (ping-pong 의 prev 슬롯)
//   t2: g_motionVec — render-res pixel-space MV (gate 용)
// 출력:
//   u0: g_smooth — 이번 프레임의 smooth Î (다음 프레임의 history 역할)

Texture2D<float>  g_raw       : register(t0);
Texture2D<float>  g_history   : register(t1);
Texture2D<float2> g_motionVec : register(t2);
RWTexture2D<float> g_smooth   : register(u0);

cbuffer EmaCB : register(b0)
{
    uint  g_width;
    uint  g_height;
    float g_alpha;            // 기본 0.1 ~ 0.3, 권장 0.2
    float g_motionThreshold;  // pixel 단위 (예: 2.0)
    uint  g_firstFrame;       // 1 = 첫 프레임 / 씬 전환 직후 → α 강제 1.0
    float _pad[3];
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= g_width || px.y >= g_height) return;

    float raw  = g_raw.Load(int3(px, 0));
    float hist = g_history.Load(int3(px, 0));

    float2 mv     = g_motionVec.Load(int3(px, 0));
    float  mvMag  = length(mv);

    // Motion gate: 큰 변위는 history 가 신뢰 불가 → α=1 (history 무효화)
    //   첫 프레임도 history 가 정의되지 않으므로 동일하게 처리
    float alpha = (g_firstFrame != 0u || mvMag > g_motionThreshold)
                  ? 1.0f
                  : g_alpha;

    g_smooth[px] = lerp(hist, raw, alpha);
}
