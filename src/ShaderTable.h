#pragma once
#include "Common.h"

// DXR 셰이더 테이블: RayGen / Miss / HitGroup 각 1개
class ShaderTable
{
public:
    struct Desc
    {
        const void* rayGenID   = nullptr;
        const void* missID     = nullptr;
        const void* hitGroupID = nullptr;
    };

    void Build(ID3D12Device* device, const Desc& desc);

    D3D12_GPU_VIRTUAL_ADDRESS_RANGE           RayGenRange()   const noexcept;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE MissRange()    const noexcept;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE HitGroupRange()const noexcept;

private:
    ComPtr<ID3D12Resource> m_buffer;
    uint64_t m_rayGenOffset   = 0;
    uint64_t m_missOffset     = 0;
    uint64_t m_hitGroupOffset = 0;
    uint64_t m_recordStride   = 0;
};
