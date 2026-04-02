#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// 레이트레이싱 출력 버퍼 (슬롯 0: RGBA8 표시용, 슬롯 1: RGBA32F 누적용)
class RenderTarget
{
public:
    void Init(ID3D12Device* device, DescriptorHeap& heap,
              uint32_t width, uint32_t height);

    void TransitionTo(ID3D12GraphicsCommandList* cmdList,
                      D3D12_RESOURCE_STATES      newState);

    void CopyToBackBuffer(ID3D12GraphicsCommandList* cmdList,
                          ID3D12Resource*             backBuffer);

    // UAV 쓰기 완료 보장 (DispatchRays → 다음 작업 간 배리어)
    void UAVBarriers(ID3D12GraphicsCommandList* cmdList);

    // g_accumulation을 0으로 클리어 (씬 전환/첫 프레임 잔상 제거)
    // SetDescriptorHeaps(shader-visible heap) 호출 이후에 사용해야 함
    void ClearAccumulation(ID3D12GraphicsCommandList* cmdList);

    ID3D12Resource* Resource()      const noexcept { return m_texture.Get(); }
    ID3D12Resource* AccumResource() const noexcept { return m_accumulation.Get(); }

private:
    // 슬롯 0: g_output  (RGBA8,   표시 및 톤맵 결과)
    ComPtr<ID3D12Resource> m_texture;
    DescriptorHandle       m_uavHandle;
    D3D12_RESOURCE_STATES  m_currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // 슬롯 1: g_accumulation  (RGBA32F, HDR 누적)
    ComPtr<ID3D12Resource> m_accumulation;
    DescriptorHandle       m_accumHandle;

    // ClearUnorderedAccessViewFloat용 non-shader-visible CPU 핸들
    DescriptorHeap              m_clearHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_accumClearCPU{};
};
