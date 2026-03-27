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
    // 인라인 삼각형 (정점 / 인덱스 모두 CPU에서 업로드)
    void Build(ID3D12Device5*              device,
               ID3D12GraphicsCommandList4* cmdList,
               std::span<const float[3]>  vertices);  // xyz 배열

    ID3D12Resource* Resource() const noexcept { return m_blas.Get(); }

private:
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_scratch;
    ComPtr<ID3D12Resource> m_blas;
};

// Top-Level Acceleration Structure (단일 인스턴스)
class TLAS
{
public:
    void Build(ID3D12Device5*              device,
               ID3D12GraphicsCommandList4* cmdList,
               ID3D12Resource*             blasResource);

    ID3D12Resource* Resource() const noexcept { return m_tlas.Get(); }

private:
    ComPtr<ID3D12Resource> m_instanceDescs;
    ComPtr<ID3D12Resource> m_scratch;
    ComPtr<ID3D12Resource> m_tlas;
};
