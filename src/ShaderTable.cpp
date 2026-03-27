#include "ShaderTable.h"
#include <cstring>

namespace
{
    // DXR 셰이더 테이블 레코드는 D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT(64바이트) 정렬
    constexpr uint64_t Align(uint64_t size, uint64_t align)
    {
        return (size + align - 1) & ~(align - 1);
    }
    constexpr uint64_t k_recordAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    constexpr uint64_t k_idSize          = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    constexpr uint64_t k_recordSize      = Align(k_idSize, k_recordAlignment);
}

void ShaderTable::Build(ID3D12Device* device, const Desc& desc)
{
    m_recordStride = k_recordSize;

    // RayGen(1) + Miss(1) + HitGroup(1)
    const uint64_t totalSize = k_recordSize * 3;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rdesc{};
    rdesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rdesc.Width            = totalSize;
    rdesc.Height           = 1;
    rdesc.DepthOrArraySize = 1;
    rdesc.MipLevels        = 1;
    rdesc.SampleDesc       = {1, 0};
    rdesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &rdesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_buffer)));

    uint8_t* mapped = nullptr;
    m_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

    m_rayGenOffset   = 0;
    m_missOffset     = k_recordSize;
    m_hitGroupOffset = k_recordSize * 2;

    // 각 레코드에 셰이더 ID 복사
    std::memcpy(mapped + m_rayGenOffset,   desc.rayGenID,   k_idSize);
    std::memcpy(mapped + m_missOffset,     desc.missID,     k_idSize);
    std::memcpy(mapped + m_hitGroupOffset, desc.hitGroupID, k_idSize);

    m_buffer->Unmap(0, nullptr);
}

D3D12_GPU_VIRTUAL_ADDRESS_RANGE ShaderTable::RayGenRange() const noexcept
{
    return { m_buffer->GetGPUVirtualAddress() + m_rayGenOffset, m_recordStride };
}

D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE ShaderTable::MissRange() const noexcept
{
    return { m_buffer->GetGPUVirtualAddress() + m_missOffset, m_recordStride, m_recordStride };
}

D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE ShaderTable::HitGroupRange() const noexcept
{
    return { m_buffer->GetGPUVirtualAddress() + m_hitGroupOffset, m_recordStride, m_recordStride };
}
