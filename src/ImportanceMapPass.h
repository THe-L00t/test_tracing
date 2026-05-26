#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// HPAR-PT Stage 2 (PASS 1) — Perceptual Importance Estimation
//
// Phase 2: percentile_99 정규화 + S/M/V 항 + Daly 가중치
// Phase 3: Temporal EMA smoothing + V stub
//
// 4-Pass 컴퓨트 파이프라인:
//   1. MetricHistogram — 픽셀별 E/S/M → 768-uint 히스토그램
//   2. MetricReduce   — 히스토그램 → percentile_99 (float4)
//   3. ImportanceMap  — 정규화 + Daly 가중치 + V multiplier → Î_raw
//   4. ImportanceEMA  — α blending + motion gate → Î_smooth (다음 프레임의 history)
//
// Resource() 는 EMA 출력(smooth) 을 반환. 외부 사용자(시각화/ray allocation)는
// 항상 smooth 만 본다.
class ImportanceMapPass
{
public:
    bool Init(ID3D12Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    void Apply(ID3D12GraphicsCommandList* cmdList,
               ID3D12Resource* depthRes,
               ID3D12Resource* accumRes,
               ID3D12Resource* normalsRes,
               ID3D12Resource* motionVecRes,
               ID3D12Resource* specAlbedoRes);

    // 외부용 — Phase 3 EMA smooth 결과 (가장 최근에 쓰여진 ping-pong 버퍼)
    //   m_writeIdx 는 "다음 frame 의 write target" 을 가리킨다 (Apply 끝에 toggle 됨).
    //   따라서 "방금 작성된" 버퍼는 그 반대.
    ID3D12Resource* Resource() const noexcept
    {
        return m_smooth[1u - m_writeIdx].Get();
    }

    uint32_t Width()  const noexcept { return m_width;  }
    uint32_t Height() const noexcept { return m_height; }

    // 진단/Ablation (Phase 2/3 공통)
    void SetMetricFilter(int mode) { m_metricFilter = mode; }
    int  MetricFilter() const noexcept { return m_metricFilter; }

    // EMA 파라미터 (Phase 15 ablation 용 — 현재는 기본값 고정)
    void SetEmaAlpha(float a) { m_alpha = a; }
    void SetMotionThreshold(float t) { m_motionThreshold = t; }

    // 씬 전환 등 history 무효화 — 다음 Apply 에서 α=1 강제
    void ResetHistory() { m_firstFrameFlag = true; }

private:
    ID3D12Device* m_device = nullptr;
    uint32_t      m_width  = 0;
    uint32_t      m_height = 0;

    // 자원
    ComPtr<ID3D12Resource> m_importanceRaw;   // Phase 2 output (R16F)
    ComPtr<ID3D12Resource> m_smooth[2];        // Phase 3 ping-pong (R16F)
    ComPtr<ID3D12Resource> m_semanticV;        // Phase 16 stub (R8, 0-fill)
    ComPtr<ID3D12Resource> m_histogram;
    ComPtr<ID3D12Resource> m_percentile;

    // ping-pong: m_writeIdx 가 이번 프레임의 write 대상, 1-writeIdx 가 history(read)
    uint32_t m_writeIdx = 0;
    bool     m_firstFrameFlag = true;  // 첫 frame / scene 전환 직후

    // Clear UAV 용 non-visible heap
    ComPtr<ID3D12DescriptorHeap> m_clearHeap;
    DescriptorHandle             m_histogramUavCpuShaderInvisible;

    // 4 root sig × 4 PSO
    ComPtr<ID3D12RootSignature> m_rsHisto;
    ComPtr<ID3D12RootSignature> m_rsReduce;
    ComPtr<ID3D12RootSignature> m_rsImportance;
    ComPtr<ID3D12RootSignature> m_rsEMA;
    ComPtr<ID3D12PipelineState> m_psoHisto;
    ComPtr<ID3D12PipelineState> m_psoReduce;
    ComPtr<ID3D12PipelineState> m_psoImportance;
    ComPtr<ID3D12PipelineState> m_psoEMA;

    // 공유 디스크립터 힙
    ComPtr<ID3D12DescriptorHeap> m_descHeap;
    uint32_t                     m_descIncSize = 0;

    // 상수 버퍼들
    ComPtr<ID3D12Resource> m_cbHisto;
    ComPtr<ID3D12Resource> m_cbReduce;
    ComPtr<ID3D12Resource> m_cbImportance;
    ComPtr<ID3D12Resource> m_cbEMA;
    void* m_cbHistoMapped      = nullptr;
    void* m_cbReduceMapped     = nullptr;
    void* m_cbImportanceMapped = nullptr;
    void* m_cbEMAMapped        = nullptr;

    // 히스토그램 범위
    float m_eMax = 5.0f;
    float m_sMax = 5.0f;
    float m_mMax = 10.0f;

    // Daly 1993 가중치
    float m_weightE = 0.40f;
    float m_weightD = 0.25f;
    float m_weightS = 0.20f;
    float m_weightM = 0.15f;
    float m_weightV = 1.00f;

    // EMA 파라미터
    float m_alpha           = 0.2f;
    float m_motionThreshold = 2.0f;

    // 진단
    int m_metricFilter = 0;
};
