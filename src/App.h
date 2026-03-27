#pragma once
#include "Common.h"
#include "Camera.h"
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
    void OnKeyDown(uint32_t key);

private:
    // 초기화 헬퍼
    void BuildBLASes();
    void BuildDescriptors();
    void SwitchScene(uint32_t id);

    // 프레임 업데이트
    void ProcessInput(float dt);
    void UpdateSceneCB();

    // TLAS SRV 재생성 (씬 전환 시)
    void RebuildTLASSRV();

    D3D12Core   m_core;
    DXRPipeline m_pipeline;
    RenderTarget m_renderTarget;

    // 가속 구조
    BLAS m_planeBLAS;
    BLAS m_cubeBLAS;
    BLAS m_roomBLAS;
    TLAS m_tlas;

    // 루트 시그니처
    ComPtr<ID3D12RootSignature> m_globalRS;

    // 디스크립터 핸들 (힙 슬롯 1..4)
    DescriptorHandle m_tlasSRV;
    DescriptorHandle m_planeVbSRV;
    DescriptorHandle m_cubeVbSRV;
    DescriptorHandle m_roomVbSRV;

    // 씬 상수 버퍼 (업로드 힙, 256바이트, 매 프레임 기록)
    ComPtr<ID3D12Resource> m_sceneCB;

    // 카메라
    Camera   m_camera;

    // 씬 상태
    uint32_t m_sceneID   = 0;    // 0 = 야외, 1 = 실내
    uint32_t m_width     = k_defaultWidth;
    uint32_t m_height    = k_defaultHeight;
    bool     m_sceneBuilt = false;
};
