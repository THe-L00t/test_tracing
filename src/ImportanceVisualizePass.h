#pragma once
#include "Common.h"

// HPAR-PT Phase 1 디버그 시각화 패스
//   importance map (R16F, render-res) → grayscale (RGBA8, display-res)
//   F1 키 토글 시 DLSS Evaluate 우회하고 backbuffer 로 직접 copy
class ImportanceVisualizePass
{
public:
    bool Init(ID3D12Device* device, uint32_t displayW, uint32_t displayH,
              uint32_t renderW, uint32_t renderH);
    void Shutdown();

    // importance(render-res R16F) → 내부 출력 텍스처(display-res RGBA8)
    //   importance: 호출 시 UAV 상태여야 함 — 내부에서 UAV→SRV→UAV transition
    //   srvFormat: 입력 자원의 SRV 포맷 (Phase 7 reuse mask 디버그는 R8_UNORM)
    void Apply(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* importance,
               DXGI_FORMAT srvFormat = DXGI_FORMAT_R16_FLOAT);

    // Phase 6 — Tier 시각화 모드 (F4 토글). false = Phase 1 heatmap.
    void SetTierMode(bool on, float low, float high) noexcept
    { m_showTier = on; m_tierLow = low; m_tierHigh = high; }

    // 출력 텍스처 — backbuffer 로 CopyResource 하는 데 사용
    ID3D12Resource* OutputResource() const noexcept { return m_output.Get(); }

private:
    ID3D12Device* m_device   = nullptr;
    uint32_t      m_displayW = 0;
    uint32_t      m_displayH = 0;
    uint32_t      m_renderW  = 0;
    uint32_t      m_renderH  = 0;

    ComPtr<ID3D12Resource>      m_output;          // RGBA8 display-res
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12DescriptorHeap> m_descHeap;        // SRV×1 + UAV×1
    uint32_t                     m_descIncSize = 0;
    ComPtr<ID3D12Resource>       m_constantBuffer;  // upload
    void*                        m_cbMapped = nullptr;

    // Phase 6 — Tier 시각화 (F4 토글)
    bool                         m_showTier = false;
    float                        m_tierLow  = 0.3f;
    float                        m_tierHigh = 0.7f;
};
