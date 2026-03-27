#pragma once
#include "Common.h"
#include "D3D12Core.h"
#include "DXRPipeline.h"
#include "AccelerationStructure.h"
#include "RenderTarget.h"

class App
{
public:
    void Init(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void OnRender();
    void OnResize(uint32_t width, uint32_t height);

private:
    void BuildScene();
    void BuildDescriptors();

    D3D12Core   m_core;
    DXRPipeline m_pipeline;
    BLAS        m_blas;
    TLAS        m_tlas;
    RenderTarget m_renderTarget;

    ComPtr<ID3D12RootSignature> m_globalRS;

    // TLAS SRV 핸들 (디스크립터 힙 인덱스 1)
    DescriptorHandle m_tlasSRV;

    uint32_t m_width  = k_defaultWidth;
    uint32_t m_height = k_defaultHeight;
    bool     m_sceneBuilt = false;
};
