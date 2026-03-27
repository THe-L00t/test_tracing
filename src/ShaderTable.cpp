#include "ShaderTable.h"
#include <cstring>

namespace
{
    // DXR 셰이더 테이블 레코드는 D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT(64바이트) 정렬
    constexpr uint64_t AlignST(uint64_t size, uint64_t align)
    {
        return (size + align - 1) & ~(align - 1);
    }
    constexpr uint64_t k_recordAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; // 64
    constexpr uint64_t k_idSize          = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;         // 32
    constexpr uint64_t k_recordSize      = AlignST(k_idSize, k_recordAlignment);          // 64
}

void ShaderTable::Build(ID3D12Device* device, const Desc& desc)
{
    m_recordStride = k_recordSize;

    // 레코드 레이아웃: [RayGen(0)] [Miss0(1)] [Miss1(2)] [HitGroup(3)]
    // 총 4개 레코드
    const uint64_t totalSize = k_recordSize * 4;

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
    m_missOffset     = k_recordSize;         // Miss[0]
    // Miss[1] is at k_recordSize*2 (내부에서 MissRange가 2개 스트라이드 반환)
    m_hitGroupOffset = k_recordSize * 3;     // HitGroup

    // 각 레코드에 셰이더 ID 복사 (나머지 바이트는 0으로 초기화됨)
    std::memcpy(mapped + m_rayGenOffset,            desc.rayGenID,   k_idSize);
    std::memcpy(mapped + m_missOffset,              desc.missID,     k_idSize);
    std::memcpy(mapped + m_missOffset + k_recordSize, desc.missID2,  k_idSize);
    std::memcpy(mapped + m_hitGroupOffset,          desc.hitGroupID, k_idSize);

    m_buffer->Unmap(0, nullptr);
}

D3D12_GPU_VIRTUAL_ADDRESS_RANGE ShaderTable::RayGenRange() const noexcept
{
    return { m_buffer->GetGPUVirtualAddress() + m_rayGenOffset, m_recordStride };
}

D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE ShaderTable::MissRange() const noexcept
{
    // Miss[0]과 Miss[1] 두 레코드를 포함 → Size = 2 * stride
    return {
        m_buffer->GetGPUVirtualAddress() + m_missOffset,
        m_recordStride * 2,   // 2개 레코드 커버
        m_recordStride
    };
}

D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE ShaderTable::HitGroupRange() const noexcept
{
    return {
        m_buffer->GetGPUVirtualAddress() + m_hitGroupOffset,
        m_recordStride,
        m_recordStride
    };
}
