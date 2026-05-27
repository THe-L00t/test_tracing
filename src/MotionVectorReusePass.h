#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// HPAR-PT Phase 7 (재정의) — Motion Vector Radiance Reuse (Tier 2 전용)
//
// 사용자 의도:
//   Tier 1 (Î > tierHigh)              → full PT (이 패스 처리 안 함)
//   Tier 2 (tierLow < Î ≤ tierHigh)    → 이 패스가 prev frame radiance 재투영
//   Tier 3 (Î ≤ tierLow)               → hole, Phase 10 A-trous fill 대상
//
// **exclusivity 원칙**: 출력 reusedRadiance/validMask 는 Phase 12 composite 의 input.
//   각 픽셀은 PT / reuse / denoise 중 정확히 하나만 사용된다.
//
// 산출:
//   m_reusedRadiance — RGBA16F render-res (Tier 2 valid 만 채워짐, 그 외 0)
//   m_reuseValidMask — R8_UNORM render-res (1=valid Tier 2 reuse, 0=else)
//
// History 자원 (m_radianceHistory/m_depthHistory/m_normalHistory) 은 App 이 매
// frame 끝에 CopyResource 로 갱신한다. 이 패스는 SRV 로만 본다.
//
// Phase 12 wiring 전까지는 시각적 효과 없음 (architecture-only).
class MotionVectorReusePass
{
public:
    bool Init(ID3D12Device* device, uint32_t width, uint32_t height);
    void Shutdown();

    // 매 frame dispatch. 호출 전제:
    //   - currDepth/currNormal/motionVec: UAV 상태 (RT 셰이더가 방금 씀)
    //     → ImportanceMapPass.Apply 이후 호출할 경우, 이미 UAV→SRV→UAV 복원된 상태.
    //     호출자가 UAV 상태로 넘겨주면 패스 내부에서 SRV transition 후 복원.
    //   - prevDepth/prevNormal/prevRadiance: COMMON 상태 (CopyResource 직후)
    //   - importance: UAV 상태 (ImportanceMap.Apply 직후)
    //   - m_reusedRadiance/m_reuseValidMask: UAV 상태 (이전 frame Apply 끝에서 복원됨 / 첫 frame init)
    void Apply(ID3D12GraphicsCommandList* cmdList,
               ID3D12Resource* currDepth,
               ID3D12Resource* currNormal,
               ID3D12Resource* motionVec,
               ID3D12Resource* prevDepth,
               ID3D12Resource* prevNormal,
               ID3D12Resource* prevRadiance,
               ID3D12Resource* importance);

    // 출력 리소스 (Phase 12 composite, F6 debug 시각화용)
    ID3D12Resource* ReusedRadianceResource() const noexcept { return m_reusedRadiance.Get(); }
    ID3D12Resource* ReuseValidMaskResource() const noexcept { return m_reuseValidMask.Get(); }

    uint32_t Width()  const noexcept { return m_width;  }
    uint32_t Height() const noexcept { return m_height; }

    // 씬 전환 등 — history 무효화
    void ResetHistory() { m_firstFrameFlag = true; }

    // Tier 임계값 (App 이 Phase 6 와 동기 — 단일 source 는 App::k_tierLow/High)
    void SetTierThresholds(float low, float high) { m_tierLow = low; m_tierHigh = high; }

    // 유효성 임계값 (현재 기본값 고정, ablation 대비)
    void SetDepthThreshold(float t)     { m_depthThreshold = t; }
    void SetNormalCosThreshold(float t) { m_normalCosThreshold = t; }
    void SetMotionLenMax(float p)       { m_motionLenMaxPx = p; }

private:
    ID3D12Device* m_device = nullptr;
    uint32_t      m_width  = 0;
    uint32_t      m_height = 0;

    // 출력 리소스 (UAV)
    ComPtr<ID3D12Resource> m_reusedRadiance;  // RGBA16F
    ComPtr<ID3D12Resource> m_reuseValidMask;  // R8_UNORM

    // 파이프라인
    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_pso;
    ComPtr<ID3D12DescriptorHeap> m_descHeap;       // shader-visible, 9 슬롯
    uint32_t                     m_descIncSize = 0;

    // 상수 버퍼
    ComPtr<ID3D12Resource> m_cb;
    void*                  m_cbMapped = nullptr;

    // 상태
    bool  m_firstFrameFlag      = true;
    float m_tierLow             = 0.30f;
    float m_tierHigh            = 0.70f;
    // depth: NDC 절대 차이. 0.001 은 sub-pixel jitter 만으로도 넘어 너무 엄격.
    //   0.01 = 가까운 표면(v_z~1m)의 ~1cm 정도, 같은 surface point 라면 충분히 안전.
    float m_depthThreshold      = 0.01f;
    float m_normalCosThreshold  = 0.906f;  // ~25°
    float m_motionLenMaxPx      = 256.0f;
};
