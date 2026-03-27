#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// 레이트레이싱 출력용 UAV 텍스처 (화면 해상도와 동일)
class RenderTarget
{
public:
    void Init(ID3D12Device* device, DescriptorHeap& heap,
              uint32_t width, uint32_t height);

    void TransitionTo(ID3D12GraphicsCommandList* cmdList,
                      D3D12_RESOURCE_STATES      newState);

    void CopyToBackBuffer(ID3D12GraphicsCommandList* cmdList,
                          ID3D12Resource*             backBuffer);

    ID3D12Resource*             Resource()    const noexcept { return m_texture.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE UAVHandle()   const noexcept { return m_uavHandle.gpu; }

private:
    ComPtr<ID3D12Resource> m_texture;
    DescriptorHandle       m_uavHandle;
    D3D12_RESOURCE_STATES  m_currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
