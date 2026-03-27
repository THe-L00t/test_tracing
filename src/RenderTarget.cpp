#include "RenderTarget.h"

void RenderTarget::Init(ID3D12Device* device, DescriptorHeap& heap,
                        uint32_t width, uint32_t height)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&m_texture)));

    m_uavHandle = heap.Allocate();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(
        m_texture.Get(), nullptr, &uavDesc, m_uavHandle.cpu);

    m_currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void RenderTarget::TransitionTo(ID3D12GraphicsCommandList* cmdList,
                                D3D12_RESOURCE_STATES      newState)
{
    if (m_currentState == newState) return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_texture.Get();
    barrier.Transition.StateBefore = m_currentState;
    barrier.Transition.StateAfter  = newState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
    m_currentState = newState;
}

void RenderTarget::CopyToBackBuffer(ID3D12GraphicsCommandList* cmdList,
                                    ID3D12Resource*             backBuffer)
{
    // UAV → COPY_SOURCE
    TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // 백버퍼 → COPY_DEST
    D3D12_RESOURCE_BARRIER bbBarrier{};
    bbBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bbBarrier.Transition.pResource   = backBuffer;
    bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bbBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    bbBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &bbBarrier);

    cmdList->CopyResource(backBuffer, m_texture.Get());

    // 백버퍼 → PRESENT
    bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bbBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(1, &bbBarrier);

    // UAV 텍스처는 다음 프레임을 위해 다시 UAV로
    TransitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
