#include "AccelerationStructure.h"
#include <cstring>
#include <vector>

// ---------------------------------------------------------------
// 버퍼 생성 헬퍼
// ---------------------------------------------------------------
ComPtr<ID3D12Resource> CreateBuffer(
    ID3D12Device*         device,
    uint64_t              size,
    D3D12_HEAP_TYPE       heapType,
    D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_FLAGS  flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags            = flags;

    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE,
        &desc, initialState, nullptr,
        IID_PPV_ARGS(&resource)));
    return resource;
}

// ---------------------------------------------------------------
// BLAS (VertexPN 입력 – 위치만 BLAS에, 전체 버퍼는 SRV용으로 보존)
// ---------------------------------------------------------------
void BLAS::Build(ID3D12Device5*              device,
                 ID3D12GraphicsCommandList4* cmdList,
                 std::span<const VertexPN>  vertices)
{
    m_vertexCount = static_cast<uint32_t>(vertices.size());

    // 정점 버퍼 업로드 (VertexPN 전체 저장 → SRV에서 노멀 참조 가능)
    const uint64_t vbSize = vertices.size() * sizeof(VertexPN);
    m_vertexBuffer = CreateBuffer(device, vbSize,
                                  D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    m_vertexBuffer->Map(0, nullptr, &mapped);
    std::memcpy(mapped, vertices.data(), vbSize);
    m_vertexBuffer->Unmap(0, nullptr);

    // 지오메트리 기술자: 위치만 사용 (stride=24, offset=0)
    D3D12_RAYTRACING_GEOMETRY_DESC geoDesc{};
    geoDesc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geoDesc.Triangles.VertexBuffer.StartAddress  = m_vertexBuffer->GetGPUVirtualAddress();
    geoDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(VertexPN);  // 24바이트
    geoDesc.Triangles.VertexCount                = m_vertexCount;
    geoDesc.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;  // pos만 읽음

    // 사전 빌드 정보
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs       = 1;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geoDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    m_scratch = CreateBuffer(device, info.ScratchDataSizeInBytes,
                             D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_blas    = CreateBuffer(device, info.ResultDataMaxSizeInBytes,
                             D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs                           = inputs;
    buildDesc.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData    = m_blas->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // UAV 배리어: BLAS 빌드 완료 후 TLAS가 사용 가능하도록
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_blas.Get();
    cmdList->ResourceBarrier(1, &barrier);
}

// ---------------------------------------------------------------
// TLAS (다중 인스턴스 지원)
// ---------------------------------------------------------------
void TLAS::Build(ID3D12Device5*              device,
                 ID3D12GraphicsCommandList4* cmdList,
                 std::span<const TLASInstance> instances)
{
    const uint32_t count = static_cast<uint32_t>(instances.size());

    // 인스턴스 기술자 배열 구성
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instDescs(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& src = instances[i];
        auto&       dst = instDescs[i];

        // 변환 행렬 복사 (3x4 row-major)
        std::memcpy(dst.Transform, src.transform, sizeof(dst.Transform));

        dst.InstanceID                          = src.instanceID;
        dst.InstanceMask                        = src.mask;
        dst.InstanceContributionToHitGroupIndex = 0;
        dst.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        dst.AccelerationStructure               = src.blasResource->GetGPUVirtualAddress();
    }

    const uint64_t instBufSize = count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    m_instanceDescs = CreateBuffer(device, instBufSize,
                                   D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    m_instanceDescs->Map(0, nullptr, &mapped);
    std::memcpy(mapped, instDescs.data(), instBufSize);
    m_instanceDescs->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs      = count;
    inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = m_instanceDescs->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    m_scratch = CreateBuffer(device, info.ScratchDataSizeInBytes,
                             D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_tlas    = CreateBuffer(device, info.ResultDataMaxSizeInBytes,
                             D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs                           = inputs;
    buildDesc.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData    = m_tlas->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_tlas.Get();
    cmdList->ResourceBarrier(1, &barrier);
}
