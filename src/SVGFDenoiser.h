#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// 분산 유도 A-trous 웨이블릿 디노이저 (공간 전용, temporal 없음)
//
// DLSS 앞에서 현재 프레임의 1spp 공간 노이즈만 제거.
// Temporal accumulation은 DLSS가 전담하므로 여기서 하지 않음.
//
// 내부 패스 (3×3 로컬 분산으로 sigma 결정, 엣지 스토핑: depth + normal):
//   Pass 0 (step=1): curIllum  → ping
//   Pass 1 (step=2): ping      → pong
//   Pass 2 (step=4): pong      → outputColor (RGBA8, Reinhard+gamma)
class SVGFDenoiser
{
public:
    void Init(ID3D12Device5* device, uint32_t width, uint32_t height);
    void Shutdown();

    // DispatchRays + UAVBarrier 완료 후 호출. 모든 리소스 UAV 상태로 진입.
    void Apply(ID3D12GraphicsCommandList4* cmdList,
               ID3D12Resource* curIllum,
               ID3D12Resource* outputColor,
               ID3D12Resource* depth,
               ID3D12Resource* normals);

    bool enabled = false;

private:
    void CreateBuffers(uint32_t width, uint32_t height);
    void CreatePSO();
    void BuildDescriptors(ID3D12Resource* curIllum,
                          ID3D12Resource* outputColor,
                          ID3D12Resource* depth,
                          ID3D12Resource* normals);

    ID3D12Device5* m_device = nullptr;

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    ComPtr<ID3D12Resource> m_ping;  // RGBA32F ping-pong
    ComPtr<ID3D12Resource> m_pong;  // RGBA32F ping-pong

    // 12-slot 힙 (Denoiser와 동일 구조, SVGFWavelet.hlsl 사용)
    // Pass 0 base=0:  [SRV:curIllum(t0), SRV:depth(t1), SRV:normals(t2), UAV:ping(u0)]
    // Pass 1 base=4:  [SRV:ping(t0),     SRV:depth(t1), SRV:normals(t2), UAV:pong(u0)]
    // Pass 2 base=8:  [SRV:pong(t0),     SRV:depth(t1), SRV:normals(t2), UAV:output(u0)]
    DescriptorHeap m_heap;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    bool m_initialized      = false;
    bool m_descriptorsBuilt = false;
};
