// MetricReduce.hlsl
// HPAR-PT Phase 2 — 히스토그램 → percentile_99 추출
//
// 256-bin × 3 metric (E, S, M) 히스토그램에서 각각 percentile_99 위치 bin 을 찾고
// bin index 를 metric value 로 역변환하여 4-component 결과에 저장.
// (4번째 채널은 D 용 placeholder = 1.0 — D 는 이미 bounded)
//
// 단일 스레드 시퀀셜 스캔: 768 ops 라 µs 단위.
// (병렬 prefix sum 도 가능하나 256 = 한 그룹 크기, 추가 복잡도 대비 이득 미미)

RWByteAddressBuffer            g_histogram  : register(u0);  // 3 × 256 uint
RWStructuredBuffer<float4>     g_percentile : register(u1);  // 1 elem: (E_99, S_99, M_99, 1.0)

cbuffer ReduceCB : register(b0)
{
    float g_eMax;
    float g_sMax;
    float g_mMax;
    float _pad0;
};

static const uint BIN_BYTES_PER_METRIC = 256u * 4u;

// 단일 메트릭의 percentile_99 bin → value 변환
float ComputePercentile99(uint metricOffsetBytes, float metricMax)
{
    uint total = 0u;
    for (uint b = 0u; b < 256u; ++b)
    {
        total += g_histogram.Load(metricOffsetBytes + b * 4u);
    }
    if (total == 0u) return metricMax;  // 빈 분포 → 안전한 fallback

    uint threshold  = (uint)(0.99f * (float)total);
    uint cumulative = 0u;
    uint pct99_bin  = 255u;
    for (uint b = 0u; b < 256u; ++b)
    {
        cumulative += g_histogram.Load(metricOffsetBytes + b * 4u);
        if (cumulative >= threshold) { pct99_bin = b; break; }
    }
    return ((float)pct99_bin + 0.5f) / 256.0f * metricMax;
}

[numthreads(1, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x != 0u || DTid.y != 0u) return;

    float pE = ComputePercentile99(0u,                          g_eMax);
    float pS = ComputePercentile99(BIN_BYTES_PER_METRIC,         g_sMax);
    float pM = ComputePercentile99(BIN_BYTES_PER_METRIC * 2u,    g_mMax);

    g_percentile[0] = float4(pE, pS, pM, 1.0f);
}
