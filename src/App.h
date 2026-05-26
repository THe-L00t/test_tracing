#pragma once
#include "Common.h"
#include "Camera.h"
#include "D3D12Core.h"
#include "DXRPipeline.h"
#include "AccelerationStructure.h"
#include "RenderTarget.h"
#include "Denoiser.h"
#include "DLSSIntegration.h"
#include "NRDDenoiser.h"
#include "ImportanceMapPass.h"
#include "ImportanceVisualizePass.h"
#include <chrono>

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
    DescriptorHandle m_importanceSrvNonDLSS;  // Phase 4: slot 15, smooth importance → 매 frame 갱신
    DescriptorHandle m_importanceSrvDLSS;     // Phase 4: DLSS heap slot, smooth importance → 매 frame 갱신

    // Phase 5 — Dynamic Frame Time Compensation
    //   GPU timestamp ring (2 query slots × N frames), readback 로 N frame lag 후 CPU 가 읽음
    static constexpr uint32_t TIMESTAMP_FRAMES = 3;
    ComPtr<ID3D12QueryHeap>  m_timestampHeap;        // TIMESTAMP query heap, 2 × N 슬롯
    ComPtr<ID3D12Resource>   m_timestampReadback;    // readback buffer, 같은 크기
    uint64_t                 m_timestampFreq = 0;    // CmdQueue frequency (ticks/sec)
    uint32_t                 m_timestampRingIdx = 0; // 현재 frame 의 ring 슬롯
    uint32_t                 m_timestampValidMask = 0; // bit i set: ring i 에 유효 데이터
    float                    m_gpuMsEMA       = 16.0f;  // EMA 평균 GPU ms (초기 16ms = 60fps 가정)
    uint32_t                 m_currentRMax    = 8u;     // DFTC 가 매 frame 조정
    uint32_t                 m_lastTimingLog  = 0;      // 60 frame 마다 콘솔 출력용
    float                    m_dftcTargetMs   = 16.0f;  // DFTC 목표 ms (F3 으로 1.0 ↔ 16.0 토글 — 검증용)

    // 씬 상수 버퍼 (업로드 힙, 256바이트, 매 프레임 기록)
    ComPtr<ID3D12Resource> m_sceneCB;

    // 카메라
    Camera   m_camera;

    // 씬 상태
    uint32_t m_sceneID    = 0;
    uint32_t m_width      = k_defaultWidth;
    uint32_t m_height     = k_defaultHeight;
    bool     m_sceneBuilt = false;

    // 디노이저 (비DLSS: display-res A-trous / DLSS 경로: DLSS-RR 내장)
    Denoiser m_denoiser;          // 비DLSS 경로 A-trous
    bool     m_denoiseEnabled = false;

    // DLSS
    DLSSIntegration   m_dlss;
    NRDDenoiser       m_nrdDenoiser;
    ImportanceMapPass        m_importanceMap;
    ImportanceVisualizePass  m_importanceVis;
    bool                     m_importanceDebug = false;  // F1 키로 토글 — importance map 시각화

    // Phase 6 — Tier 분류 (PHTR 통합)
    //   F4 토글 → ImportanceVis 가 heatmap 대신 Tier 색 (1=빨강/2=초록/3=파랑)
    //   tier 임계값은 RT 셰이더(Phase 7 reuse) 와 동기화: SceneCB.tierLow/High 채울 때 같은 상수 사용
    static constexpr float   k_tierLow  = 0.30f;  // Î ≤ 0.30 → Tier 3
    static constexpr float   k_tierHigh = 0.70f;  // Î >  0.70 → Tier 1
    bool                     m_tierDebug = false;

    // Phase 7 — Spatial Reservoir Reuse (Tier 2/3)
    //   RGBA32_UINT render-res ping-pong. {dirOct, asuint(W), M, _flag}
    //   m_reservoirReadIdx: 다음 frame 이 읽을 prev 버퍼 인덱스
    //   F5 키로 reservoir reuse 강제 토글 (검증용, 기본 ON)
    ComPtr<ID3D12Resource>   m_reservoir[2];
    uint32_t                 m_reservoirReadIdx = 0u;
    bool                     m_reservoirEnabled = true;          // F5 토글
    bool                     m_reservoirNeedsReset = true;       // 첫 frame / 씬 전환 / 큰 카메라 변화
    static constexpr uint32_t k_reservoirMCap = 16u;              // M 상한 (ghosting 완화)
    // 비DLSS heap slot 10/11 (u10, u11) — 매 frame ping-pong 갱신.
    // DLSS heap 의 미러는 DLSSIntegration::ReservoirIn/OutMirrorCPU() 가 제공.
    DescriptorHandle         m_reservoirInUAVNonDLSS;
    DescriptorHandle         m_reservoirOutUAVNonDLSS;

    // 이전 프레임 카메라 (모션벡터 계산용, UpdateSceneCB 끝에 갱신)
    float m_prevCamPos[3]     = {};
    float m_prevCamRight[3]   = {1,0,0};
    float m_prevCamUp[3]      = {0,1,0};
    float m_prevCamForward[3] = {0,0,1};
    float m_prevTanHalfFovY   = 0.57735f;
    float m_prevAspectRatio   = 16.0f / 9.0f;

    // G-Buffer (full-res, 비DLSS 모드에서 RT 셰이더가 씀 / A-trous 디노이저가 읽음)
    // DLSS 모드에서는 m_dlss 내부 버퍼를 u2/u3에 바인딩하므로 이 버퍼는 비활성
    ComPtr<ID3D12Resource> m_gbufferDepth;       // R32_FLOAT — NDC depth (u2)
    ComPtr<ID3D12Resource> m_gbufferNormal;      // RGBA16_FLOAT — world-법선(xyz)+roughness(w) (u3/u4)
    ComPtr<ID3D12Resource> m_gbufferDiffAlbedo;  // RGBA16_FLOAT — DLSS-RR diffuse albedo (u5)
    ComPtr<ID3D12Resource> m_gbufferSpecAlbedo;  // RGBA16_FLOAT — DLSS-RR specular F0   (u6)
    // NRD 입력 (비DLSS 모드에서는 미사용이지만 root sig 매칭을 위해 바인딩)
    ComPtr<ID3D12Resource> m_nrdDiffRadiance;    // RGBA16_FLOAT — NRD IN_DIFF_RADIANCE_HITDIST (u7)
    ComPtr<ID3D12Resource> m_nrdSpecRadiance;    // RGBA16_FLOAT — NRD IN_SPEC_RADIANCE_HITDIST (u8)
    ComPtr<ID3D12Resource> m_nrdViewZ;           // R16_FLOAT    — NRD IN_VIEWZ                  (u9)
    bool            m_dlssEnabled    = false;
    uint32_t        m_dlssFrameIdx   = 0;    // Halton 시퀀스 인덱스
    float           m_dlssJitterX    = 0.0f; // 현재 프레임 Halton X (Evaluate에도 전달)
    float           m_dlssJitterY    = 0.0f; // 현재 프레임 Halton Y

    // 패스 트레이싱 누적
    uint32_t m_frameCount   = 0;
    bool     m_cameraMoved  = true;
    bool     m_accumDirty   = true;  // 씬 전환/디노이저 토글 시 누적 버퍼 클리어 필요

    // 마우스 캡처
    HWND     m_hwnd          = nullptr;
    bool     m_mouseCaptured = false;

    // FPS 측정 (지수이동평균)
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point m_lastFrameTime{};
    float             m_smoothFps = 0.0f;
};
