#pragma once
#include "Common.h"
#include "ShaderTable.h"

// DXIL 라이브러리 로드 후 DXR 파이프라인 스테이트 오브젝트(RTPSO) 생성
class DXRPipeline
{
public:
    void Init(ID3D12Device5* device, ID3D12RootSignature* globalRootSig);

    void BuildShaderTable(ID3D12Device* device);

    ID3D12StateObject* PSO()           const noexcept { return m_pso.Get(); }
    const ShaderTable& GetShaderTable()const noexcept { return m_shaderTable; }

private:
    ComPtr<ID3D12StateObject>            m_pso;
    ComPtr<ID3D12StateObjectProperties>  m_psoProps;
    ShaderTable                          m_shaderTable;
};

// 글로벌 루트 시그니처 생성
ComPtr<ID3D12RootSignature> CreateGlobalRootSignature(ID3D12Device* device);
