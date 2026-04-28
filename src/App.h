#pragma once
#include "Common.h"
#include "Camera.h"
#include "D3D12Core.h"
#include "DXRPipeline.h"
#include "AccelerationStructure.h"
#include "RenderTarget.h"
#include "Denoiser.h"
#include <chrono>
#include <vector>

class App
{
public:
    void Init(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void OnRender();
    void OnResize(uint32_t width, uint32_t height);
    void OnKeyDown(uint32_t key);

    // 마우스 캡처 (좌클릭으로 토글, ESC/포커스 잃을 때 해제)
    void ToggleMouseCapture();
    void ReleaseMouseCapture();
    bool IsMouseCaptured() const noexcept { return m_mouseCaptured; }

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
    BLAS m_sphereBLAS;
    TLAS m_tlas;

    // 루트 시그니처
    ComPtr<ID3D12RootSignature> m_globalRS;

    // 디스크립터 핸들 (힙 슬롯 0..6)
    DescriptorHandle m_tlasSRV;
    DescriptorHandle m_planeVbSRV;
    DescriptorHandle m_cubeVbSRV;
    DescriptorHandle m_roomVbSRV;
    DescriptorHandle m_sphereVbSRV;

    // 씬 상수 버퍼 (업로드 힙, 256바이트, 매 프레임 기록)
    ComPtr<ID3D12Resource> m_sceneCB;
    ComPtr<ID3D12Resource> m_sceneCBPass2;  // Pass2 전용 (debugMode 비트8=1 설정)

    // 스크린샷 읽기 버퍼 (Readback heap)
    ComPtr<ID3D12Resource>              m_screenshotBuf;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT  m_screenshotFootprint{};
    bool                                m_saveScreenshot = false;
    int32_t                             m_screenshotTargetFrame = -1;  // -1=비활성, ≥0=목표 프레임

    // 카메라
    Camera   m_camera;

    // 씬 상태
    uint32_t m_sceneID    = 0;
    uint32_t m_width      = k_defaultWidth;
    uint32_t m_height     = k_defaultHeight;
    bool     m_sceneBuilt = false;

    // 디노이저
    Denoiser m_denoiser;
    bool     m_denoiseEnabled = false;

    // 패스 트레이싱 누적
    uint32_t m_frameCount   = 0;
    bool     m_cameraMoved  = true;
    bool     m_accumDirty   = true;  // 씬 전환/디노이저 토글 시 누적 버퍼 클리어 필요

    // 연구용 디버그 모드 (V키 순환)
    // 0=PT, 1=Fresnel, 2=Var, 3=DepthEdge, 4=NormalEdge, 5=ThresholdMask
    uint32_t m_debugMode         = 0;
    float    m_fresnelThreshold  = 0.15f;  // [/] 키로 ±0.05 조정
    bool     m_pass2Enabled      = true;   // B키 토글 (false=Baseline, true=Fresnel-guided)
    uint32_t m_pass2Mode         = 0;      // N키 토글 (0=Fresnel-guided, 1=Variance-guided)

    // GPU 타이밍 (T키 — 단일 프레임 / Y키 — 10프레임 평균±σ)
    ComPtr<ID3D12QueryHeap> m_timestampHeap;
    ComPtr<ID3D12Resource>  m_timestampBuf;   // readback, 3×uint64
    uint64_t                m_gpuTickFreq  = 0;
    bool                    m_pendingTimeMeasure = false;

    static constexpr uint32_t k_multiTimeN = 10u;
    bool     m_multiTimeMeasure = false;
    uint32_t m_multiTimeIdx     = 0;
    double   m_mtPass1[k_multiTimeN]{};
    double   m_mtPass2[k_multiTimeN]{};
    double   m_mtTotal[k_multiTimeN]{};

    // 마우스 캡처
    HWND     m_hwnd          = nullptr;
    bool     m_mouseCaptured = false;

    // FPS 측정 (지수이동평균)
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point m_lastFrameTime{};
    float             m_smoothFps = 0.0f;

    // 카메라 경로 녹화/재생 (,/.// 키)
    struct CameraState { float pos[3]; float yaw; float pitch; };
    std::vector<CameraState> m_camPath;
    bool     m_isRecording = false;
    bool     m_isPlaying   = false;
    uint32_t m_playbackIdx = 0;
};
