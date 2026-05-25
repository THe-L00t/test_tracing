#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// SVGF 디노이저 — Spatiotemporal Variance-Guided Filtering
//
// 패스 순서:
//   Temporal: curIllum + prevAccum → accumPing, momentsCur  (지수이동평균 + 모멘트 누적)
//   Wavelet 0 (step=1): accumPing → waveletPing
//   Wavelet 1 (step=2): waveletPing → waveletPong
//   Wavelet 2 (step=4, tonemap): waveletPong → renderColor (RGBA8, DLSS 입력)
//   CopyResource: accumPing→accumPong, momentsCur→momentsPrev, depth→prevDepth, normals→prevNormal
//
// 사용 흐름:
//   Init()   → 내부 버퍼·PSO·힙 초기화
//   Apply()  → DispatchRays + UAVBarrier 완료 후, DLSS Evaluate 전에 호출
//   Shutdown()→ 해제
class SVGFDenoiser
{
public:
    void Init(ID3D12Device5* device, uint32_t width, uint32_t height);
    void Shutdown();

    // DispatchRays + UAVBarrier 완료 후 호출. 모든 외부 리소스는 UAV 상태로 진입.
    // curIllum    : render-res RGBA32F — 현재 1spp PT 출력 (UAV)
    // outputColor : render-res RGBA8   — DLSS 색상 입력 (UAV)
    // depth       : render-res R32F    — NDC depth       (UAV)
    // normals     : render-res RG16F   — oct-법선        (UAV)
    // motionVec   : render-res RG16F   — MV curr→prev   (UAV)
    // reset       : true 시 시간적 히스토리 강제 초기화 (씬 전환 등)
    void Apply(ID3D12GraphicsCommandList4* cmdList,
               ID3D12Resource* curIllum,
               ID3D12Resource* outputColor,
               ID3D12Resource* depth,
               ID3D12Resource* normals,
               ID3D12Resource* motionVec,
               bool reset);

    bool enabled = false;

private:
    void CreateBuffers(uint32_t width, uint32_t height);
    void CreateTemporalPSO();
    void CreateWaveletPSO();
    void BuildDescriptors(ID3D12Resource* curIllum,
                          ID3D12Resource* outputColor,
                          ID3D12Resource* depth,
                          ID3D12Resource* normals,
                          ID3D12Resource* motionVec);

    ID3D12Device5* m_device = nullptr;

    ComPtr<ID3D12RootSignature> m_temporalRS;
    ComPtr<ID3D12PipelineState> m_temporalPSO;

    ComPtr<ID3D12RootSignature> m_waveletRS;
    ComPtr<ID3D12PipelineState> m_waveletPSO;

    // 내부 버퍼 (render-res)
    ComPtr<ID3D12Resource> m_accumPing;    // RGBA32F — 시간적 누적 결과 (현재 프레임)
    ComPtr<ID3D12Resource> m_accumPong;    // RGBA32F — 이전 프레임 누적 히스토리
    ComPtr<ID3D12Resource> m_momentsCur;   // RGBA32F — 현재 모멘트 (.xy = μ, μ²)
    ComPtr<ID3D12Resource> m_momentsPrev;  // RGBA32F — 이전 프레임 모멘트
    ComPtr<ID3D12Resource> m_prevDepth;    // R32F    — 이전 프레임 depth
    ComPtr<ID3D12Resource> m_prevNormal;   // RG16F   — 이전 프레임 oct-법선
    ComPtr<ID3D12Resource> m_waveletPing;  // RGBA32F — 웨이블릿 중간 버퍼 A
    ComPtr<ID3D12Resource> m_waveletPong;  // RGBA32F — 웨이블릿 중간 버퍼 B

    // 25-slot 내부 디스크립터 힙
    // [0-9]:   Temporal pass — SRV×8 (t0-t7) + UAV×2 (u0-u1)
    // [10-14]: Wavelet pass 0 (step=1) — SRV×4 + UAV×1
    // [15-19]: Wavelet pass 1 (step=2) — SRV×4 + UAV×1
    // [20-24]: Wavelet pass 2 (step=4, tonemap) — SRV×4 + UAV×1 (RGBA8 outputColor)
    DescriptorHeap m_heap;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    bool m_initialized      = false;
    bool m_descriptorsBuilt = false;
    bool m_firstFrame       = true;
};
