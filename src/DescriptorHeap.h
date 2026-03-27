#pragma once
#include "Common.h"

// CPU/GPU 디스크립터 핸들 쌍
struct DescriptorHandle
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
};

// 단순 선형 할당 디스크립터 힙
class DescriptorHeap
{
public:
    void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
              uint32_t capacity, bool shaderVisible);

    [[nodiscard]] DescriptorHandle Allocate();
    [[nodiscard]] DescriptorHandle GetHandle(uint32_t index) const;

    ID3D12DescriptorHeap* Get() const noexcept { return m_heap.Get(); }
    uint32_t              Count() const noexcept { return m_count; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE  m_gpuStart{};
    uint32_t                     m_incrementSize = 0;
    uint32_t                     m_capacity      = 0;
    uint32_t                     m_count         = 0;
};
