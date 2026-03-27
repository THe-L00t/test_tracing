#pragma once
#include "Common.h"

// GPU 버퍼 헬퍼: 업로드 또는 디폴트 힙에 버퍼 생성
ComPtr<ID3D12Resource> CreateBuffer(
    ID3D12Device*            device,
    uint64_t                 size,
    D3D12_HEAP_TYPE          heapType,
    D3D12_RESOURCE_STATES    initialState,
    D3D12_RESOURCE_FLAGS     flags = D3D12_RESOURCE_FLAG_NONE);

// Bottom-Level Acceleration Structure (삼각형 메시 하나)
class BLAS
{
public:
    // VertexPN 배열로 BLAS 빌드 (위치만 사용, 노멀은 SRV용)
    void Build(ID3D12Device5*              device,
               ID3D12GraphicsCommandList4* cmdList,
               std::span<const VertexPN>  vertices);

    ID3D12Resource* Resource()    const noexcept { return m_blas.Get(); }
    ID3D12Resource* VertexBuffer()const noexcept { return m_vertexBuffer.Get(); }
    uint32_t        VertexCount() const noexcept { return m_vertexCount; }

private:
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_scratch;
    ComPtr<ID3D12Resource> m_blas;
    uint32_t               m_vertexCount = 0;
};

// TLAS 인스턴스 기술자
struct TLASInstance
{
    ID3D12Resource* blasResource = nullptr;
    float           transform[3][4] = {};   // row-major 3x4 변환 행렬
    uint32_t        instanceID  = 0;
    uint32_t        mask        = 0xFF;
};

// Top-Level Acceleration Structure (다중 인스턴스 지원)
class TLAS
{
public:
    void Build(ID3D12Device5*              device,
               ID3D12GraphicsCommandList4* cmdList,
               std::span<const TLASInstance> instances);

    ID3D12Resource* Resource() const noexcept { return m_tlas.Get(); }

private:
    ComPtr<ID3D12Resource> m_instanceDescs;
    ComPtr<ID3D12Resource> m_scratch;
    ComPtr<ID3D12Resource> m_tlas;
};
