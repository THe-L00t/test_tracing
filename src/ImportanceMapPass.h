#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// HPAR-PT Stage 2 (PASS 1) — Perceptual Importance Estimation
//
// Phase 1: E + D 만 (raw Sobel 합)
// Phase 2: percentile_99 정규화 + S/M/V 항 (별도 클래스 또는 확장)
// Phase 3: Temporal EMA smoothing
//
// 입력: g_depth (linear viewZ 또는 NDC depth), g_accumulation (PT HDR 결과)
// 출력: m_importance (R16F, render-res) — Î(x,y) ∈ [0, 1]
//
// 자체 root signature + PSO + descriptor heap 사용 (App 공유 힙 미사용)
class ImportanceMapPass
{
public:
    bool Init(ID3D12Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    // 매 프레임 호출: g_depth + g_accumulation 을 읽어 m_importance 계산
    //   cmdList: 열린 상태 (graphics 또는 compute)
    //   depthRes: linear viewZ 또는 NDC depth (R16F/R32F/R32F any)
    //   accumRes: HDR radiance (RGBA32F)
    void Apply(ID3D12GraphicsCommandList* cmdList,
               ID3D12Resource* depthRes,
               ID3D12Resource* accumRes);

    ID3D12Resource* Resource() const noexcept { return m_importance.Get(); }

    uint32_t Width()  const noexcept { return m_width;  }
    uint32_t Height() const noexcept { return m_height; }

private:
    ID3D12Device* m_device = nullptr;
    uint32_t      m_width  = 0;
    uint32_t      m_height = 0;

    // 자원
    ComPtr<ID3D12Resource> m_importance;  // R16F render-res, UAV

    // 컴퓨트 파이프라인
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // 내부 디스크립터 힙 (SRV×2 + UAV×1 = 3 슬롯, non shader-visible)
    //   매 dispatch 마다 shader-visible 힙(공유 힙) 의 슬롯에 복사하는 대신
    //   자체 shader-visible 힙 보유로 단순화
    ComPtr<ID3D12DescriptorHeap> m_descHeap;
    uint32_t                     m_descIncSize = 0;

    // 상수 버퍼 (upload heap)
    ComPtr<ID3D12Resource> m_constantBuffer;
    void*                  m_cbMapped = nullptr;

    // 가중치 (Phase 1 임시값 — D 위주, 1spp 노이즈가 E 오염하므로)
    //   Phase 3 EMA 들어가면 0.40 (E) / 0.25 (D) 로 복원 + S/M/V 추가
    float m_weightE = 0.2f;
    float m_weightD = 0.8f;
};
