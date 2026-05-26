#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// HPAR-PT Stage 2 (PASS 1) — Perceptual Importance Estimation
//
// Phase 2: percentile_99 정규화 + S/M/V 항 + Daly 가중치
// 3-Pass 컴퓨트 파이프라인:
//   1. MetricHistogram — 픽셀별 E/S/M 메트릭 → 768-uint 히스토그램
//   2. MetricReduce   — 히스토그램 prefix scan → percentile_99 (1×float4)
//   3. ImportanceMap  — 정규화 + 가중치 합 + V 멀티플라이어 → Î(x,y)
//
// 입력 SRV (App 에서 전달):
//   depth, accumulation, normals, motionVec, specAlbedo
//
// 출력: m_importance (R16F render-res)
class ImportanceMapPass
{
public:
    bool Init(ID3D12Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    // 매 프레임 — 5개 GBuffer/PT 텍스처를 읽어 Î(x,y) 계산
    void Apply(ID3D12GraphicsCommandList* cmdList,
               ID3D12Resource* depthRes,
               ID3D12Resource* accumRes,
               ID3D12Resource* normalsRes,
               ID3D12Resource* motionVecRes,
               ID3D12Resource* specAlbedoRes);

    ID3D12Resource* Resource() const noexcept { return m_importance.Get(); }

    uint32_t Width()  const noexcept { return m_width;  }
    uint32_t Height() const noexcept { return m_height; }

private:
    ID3D12Device* m_device = nullptr;
    uint32_t      m_width  = 0;
    uint32_t      m_height = 0;

    // 출력
    ComPtr<ID3D12Resource> m_importance;       // R16F render-res

    // Phase 2 신규 — 히스토그램 + percentile
    ComPtr<ID3D12Resource> m_histogram;        // RWByteAddressBuffer 768 uint
    ComPtr<ID3D12Resource> m_percentile;       // StructuredBuffer<float4> 1 elem
    DescriptorHandle       m_histogramUavCpuShaderInvisible;  // ClearUnorderedAccessViewUint 용 non-visible UAV
    ComPtr<ID3D12DescriptorHeap> m_clearHeap;  // non-shader-visible 힙 (clear UAV 전용)

    // 3 root sig × 3 PSO
    ComPtr<ID3D12RootSignature> m_rsHisto;
    ComPtr<ID3D12RootSignature> m_rsReduce;
    ComPtr<ID3D12RootSignature> m_rsImportance;
    ComPtr<ID3D12PipelineState> m_psoHisto;
    ComPtr<ID3D12PipelineState> m_psoReduce;
    ComPtr<ID3D12PipelineState> m_psoImportance;

    // 디스크립터 힙 (shader-visible, 3 패스 모두 공유)
    //   slots 0..4 SRV (depth, accum, normals, motionVec, specAlbedo) — histo + importance 공통
    //   slot  5  SRV percentile (importance 만 사용)
    //   slot  6  UAV histogram   (histo + reduce)
    //   slot  7  UAV percentile  (reduce 만)
    //   slot  8  UAV importance  (importance 만)
    ComPtr<ID3D12DescriptorHeap> m_descHeap;
    uint32_t                     m_descIncSize = 0;

    // 상수 버퍼들 (upload heap)
    ComPtr<ID3D12Resource> m_cbHisto;
    ComPtr<ID3D12Resource> m_cbReduce;
    ComPtr<ID3D12Resource> m_cbImportance;
    void* m_cbHistoMapped     = nullptr;
    void* m_cbReduceMapped    = nullptr;
    void* m_cbImportanceMapped = nullptr;

    // 히스토그램 범위 (binning 상한)
    float m_eMax = 5.0f;
    float m_sMax = 5.0f;
    float m_mMax = 10.0f;

    // Daly 1993 HVS CSF 기반 가중치 + Phase 16 V 항
    float m_weightE = 0.40f;
    float m_weightD = 0.25f;
    float m_weightS = 0.20f;
    float m_weightM = 0.15f;
    float m_weightV = 1.00f;  // multiplier scale (V 가 0 이면 무효)

public:
    // 디버그 — 0=All, 1=E only, 2=D only, 3=S only, 4=M only
    void SetMetricFilter(int mode) { m_metricFilter = mode; }
    int  MetricFilter() const noexcept { return m_metricFilter; }
private:
    int m_metricFilter = 0;
};
