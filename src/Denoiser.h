#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// A-trous 웨이블릿 컴퓨트 셰이더 기반 디노이저
//
// 사용 흐름:
//   Init()        → 초기화 (버퍼, PSO 생성)
//   enabled       → 런타임 온/오프 토글
//   Apply()       → DispatchRays + UAVBarrier 직후, CopyToBackBuffer 직전에 호출
//   Shutdown()    → 해제
//
// 내부 패스 (엣지 스토핑: color + depth + normal):
//   Pass 0 (step=1): g_accumulation → ping
//   Pass 1 (step=2): ping           → pong
//   Pass 2 (step=4): pong           → g_output  (+Reinhard 톤맵)
class Denoiser
{
public:
    void Init(ID3D12Device5* device, uint32_t width, uint32_t height);
    void Shutdown();

    // DispatchRays + UAVBarrier 완료 후 호출
    // accumResource : g_accumulation (RGBA32F,    UAV 상태)
    // outputResource: g_output       (RGBA8,       UAV 상태)
    // depthResource : NDC depth      (R32_FLOAT,   UAV 상태)
    // normalResource: oct-법선       (RG16_FLOAT,  UAV 상태)
    void Apply(ID3D12GraphicsCommandList4* cmdList,
               ID3D12Resource* accumResource,
               ID3D12Resource* outputResource,
               ID3D12Resource* depthResource,
               ID3D12Resource* normalResource);

    bool enabled = false;

private:
    void CreateBuffers(uint32_t width, uint32_t height);
    void CreatePSO();
    void BuildDescriptors(ID3D12Resource* accumResource,
                          ID3D12Resource* outputResource,
                          ID3D12Resource* depthResource,
                          ID3D12Resource* normalResource);

    ID3D12Device5* m_device = nullptr;  // non-owning (D3D12Core 소유)

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // 핑퐁 버퍼 (RGBA32F, A-trous 중간 결과 저장)
    ComPtr<ID3D12Resource> m_ping;
    ComPtr<ID3D12Resource> m_pong;

    // 디스크립터 힙 (12 슬롯, shader-visible CBV/SRV/UAV)
    // Pass 0 base = slot 0:  [SRV:accum(t0), SRV:depth(t1), SRV:normal(t2), UAV:ping(u0)]
    // Pass 1 base = slot 4:  [SRV:ping(t0),  SRV:depth(t1), SRV:normal(t2), UAV:pong(u0)]
    // Pass 2 base = slot 8:  [SRV:pong(t0),  SRV:depth(t1), SRV:normal(t2), UAV:output(u0)]
    DescriptorHeap m_heap;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    bool m_initialized      = false;
    bool m_descriptorsBuilt = false;
};
