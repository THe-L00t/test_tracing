#include "DescriptorHeap.h"

void DescriptorHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                          uint32_t capacity, bool shaderVisible)
{
    m_capacity = capacity;

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type           = type;
    desc.NumDescriptors = capacity;
    desc.Flags          = shaderVisible
                          ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                          : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)));

    m_incrementSize = device->GetDescriptorHandleIncrementSize(type);
    m_cpuStart      = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuStart      = shaderVisible
                      ? m_heap->GetGPUDescriptorHandleForHeapStart()
                      : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

DescriptorHandle DescriptorHeap::Allocate()
{
    if (m_count >= m_capacity)
        throw std::runtime_error("DescriptorHeap: 용량 초과");

    return GetHandle(m_count++);
}

DescriptorHandle DescriptorHeap::GetHandle(uint32_t index) const
{
    DescriptorHandle h;
    h.cpu.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_incrementSize;
    if (m_gpuStart.ptr != 0)
        h.gpu.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_incrementSize;
    return h;
}
