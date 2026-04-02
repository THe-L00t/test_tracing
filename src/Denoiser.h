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
// 내부 패스:
//   Pass 0 (step=1): g_output(SDR) → ping   ← bilateral σ=0.1 (SDR 0~1 기준)
//   Pass 1 (step=2): ping          → pong
//   Pass 2 (step=4): pong          → g_output  (tonemap 없음, SDR 그대로 출력)
class Denoiser
{
public:
    void Init(ID3D12Device5* device, uint32_t width, uint32_t height);
    void Shutdown();

    // DispatchRays + UAVBarrier 완료 후 호출
    // accumResource : g_accumulation (RGBA32F, UAV 상태)
    // outputResource: g_output       (RGBA8,   UAV 상태)
    void Apply(ID3D12GraphicsCommandList4* cmdList,
               ID3D12Resource* accumResource,
               ID3D12Resource* outputResource);

    bool enabled = false;

private:
    void CreateBuffers(uint32_t width, uint32_t height);
    void CreatePSO();
    void BuildDescriptors(ID3D12Resource* accumResource,
                          ID3D12Resource* outputResource);

    ID3D12Device5* m_device = nullptr;  // non-owning (D3D12Core 소유)

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // 핑퐁 버퍼 (RGBA32F, A-trous 중간 결과 저장)
    ComPtr<ID3D12Resource> m_ping;
    ComPtr<ID3D12Resource> m_pong;

    // 디스크립터 힙 (6 슬롯, shader-visible CBV/SRV/UAV)
    // [0,1] = [SRV:output(SDR), UAV:ping]  → Pass 0 테이블 베이스
    // [2,3] = [SRV:ping,   UAV:pong]    → Pass 1 테이블 베이스
    // [4,5] = [SRV:pong,   UAV:output]  → Pass 2 테이블 베이스
    DescriptorHeap m_heap;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    bool m_initialized      = false;
    bool m_descriptorsBuilt = false;
};
