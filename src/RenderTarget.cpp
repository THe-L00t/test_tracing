#include "RenderTarget.h"

void RenderTarget::Init(ID3D12Device* device, DescriptorHeap& heap,
                        uint32_t width, uint32_t height)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // ── 슬롯 0: g_output (RGBA8, 화면 출력용) ──
    {
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

    // ── 슬롯 1: g_accumulation (RGBA32F, HDR 누적용) ──
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&m_accumulation)));

        m_accumHandle = heap.Allocate();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(
            m_accumulation.Get(), nullptr, &uavDesc, m_accumHandle.cpu);

        // ClearUnorderedAccessViewFloat 전용 non-shader-visible CPU 핸들
        // (API 요구사항: ViewCPUHandle은 non-visible 힙에서 와야 함)
        m_clearHeap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, false);
        m_accumClearCPU = m_clearHeap.Allocate().cpu;
        device->CreateUnorderedAccessView(
            m_accumulation.Get(), nullptr, &uavDesc, m_accumClearCPU);
    }

    // ── 슬롯 2: g_fresnel (R32F, Fresnel 우선도 맵) ──
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&m_fresnel)));

        m_fresnelHandle = heap.Allocate();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format        = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(
            m_fresnel.Get(), nullptr, &uavDesc, m_fresnelHandle.cpu);
    }

    // ── 슬롯 3: g_depth (R32F, 1차 레이 히트 거리) ──
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&m_depth)));

        m_depthHandle = heap.Allocate();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format        = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(
            m_depth.Get(), nullptr, &uavDesc, m_depthHandle.cpu);
    }

    // ── 슬롯 4: g_normal (RGBA32F, 1차 레이 표면 법선) ──
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&m_normal)));

        m_normalHandle = heap.Allocate();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(
            m_normal.Get(), nullptr, &uavDesc, m_normalHandle.cpu);
    }

    // ── 슬롯 5: g_accumSq (R32F, 휘도² 누적 평균) ──
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&m_accumSq)));

        m_accumSqHandle = heap.Allocate();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Format        = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(
            m_accumSq.Get(), nullptr, &uavDesc, m_accumSqHandle.cpu);
    }
}

void RenderTarget::ClearAccumulation(ID3D12GraphicsCommandList* cmdList)
{
    const float zeros[4] = {};
    cmdList->ClearUnorderedAccessViewFloat(
        m_accumHandle.gpu,   // shader-visible GPU 핸들
        m_accumClearCPU,     // non-shader-visible CPU 핸들
        m_accumulation.Get(),
        zeros, 0, nullptr);

    // 클리어 완료 후 DispatchRays의 UAV 접근과 동기화
    D3D12_RESOURCE_BARRIER b{};
    b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = m_accumulation.Get();
    cmdList->ResourceBarrier(1, &b);
}

void RenderTarget::UAVBarriers(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barriers[6]{};
    barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_texture.Get();
    barriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_accumulation.Get();
    barriers[2].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].UAV.pResource = m_fresnel.Get();
    barriers[3].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[3].UAV.pResource = m_depth.Get();
    barriers[4].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[4].UAV.pResource = m_normal.Get();
    barriers[5].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[5].UAV.pResource = m_accumSq.Get();
    cmdList->ResourceBarrier(6, barriers);
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
