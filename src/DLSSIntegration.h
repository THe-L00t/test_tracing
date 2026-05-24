#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// Forward declarations (avoids pulling NGX headers into every translation unit)
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

// ---------------------------------------------------------------
// DLSS Super Resolution 통합 클래스
//
// 힙 슬롯 배치 (Init 후):
//   [7]  UAV m_renderColor  (RGBA8,  render res) ← RT shader u0
//   [8]  UAV m_renderAccum  (RGBA32F, render res) ← RT shader u1
//   [9]  SRV TLAS 미러
//  [10]  SRV plane VB 미러
//  [11]  SRV cube VB 미러
//  [12]  SRV room VB 미러
//  [13]  SRV sphere VB 미러
//
// DLSS 모드 시 SetComputeRootDescriptorTable(0, heap[7]) 로
// 기존 루트 시그니처를 그대로 재사용한다.
// ---------------------------------------------------------------
class DLSSIntegration
{
public:
    // BuildDescriptors() 완료 후 호출. 힙 슬롯 7..13 에 UAV/SRV 등록.
    // shader-visible 힙은 CopyDescriptors src 불가 → 리소스 직접 전달하여 SRV 재생성
    bool Init(ID3D12Device*               device,
              ID3D12GraphicsCommandList*  cmdList,
              uint32_t                    displayW,
              uint32_t                    displayH,
              DescriptorHeap&             sharedHeap,
              D3D12_GPU_VIRTUAL_ADDRESS   tlasGpuVA,
              ID3D12Resource*             planeVb,   uint32_t planeVertCount,
              ID3D12Resource*             cubeVb,    uint32_t cubeVertCount,
              ID3D12Resource*             roomVb,    uint32_t roomVertCount,
              ID3D12Resource*             sphereVb,  uint32_t sphereVertCount);

    void Shutdown(ID3D12Device* device);

    // 씬 전환 후 TLAS SRV 미러(슬롯 9) 갱신
    void RefreshTLASSRV(ID3D12Device* device, D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA);

    // RT 셰이더 디스패치 해상도 (레더 해상도)
    uint32_t RenderWidth()  const noexcept { return m_renderW; }
    uint32_t RenderHeight() const noexcept { return m_renderH; }

    // DispatchRays 완료 후 UAV 배리어
    void UAVBarriers(ID3D12GraphicsCommandList* cmdList);

    // DLSS 업스케일 수행 (render res → display res)
    // jitterX/Y: Halton 시퀀스 오프셋 [-0.5, 0.5] 픽셀 공간
    // reset: true 시 시간적 축적 리셋 (씬 전환 등)
    void Evaluate(ID3D12GraphicsCommandList* cmdList,
                  float jitterX, float jitterY, bool reset);

    // m_dlssOutput → backBuffer 복사
    void CopyOutputToBackBuffer(ID3D12GraphicsCommandList* cmdList,
                                ID3D12Resource*             backBuffer);

    bool IsAvailable() const noexcept { return m_available; }

    // A-trous 디노이저가 render-res에서 읽고 쓸 수 있도록 리소스 노출
    ID3D12Resource* RenderColorResource() const noexcept { return m_renderColor.Get(); }
    ID3D12Resource* RenderAccumResource() const noexcept { return m_renderAccum.Get(); }

private:
    NVSDK_NGX_Handle*    m_feature = nullptr;
    NVSDK_NGX_Parameter* m_params  = nullptr;

    // 렌더 해상도 입력 버퍼 (RT 셰이더가 여기에 씀)
    ComPtr<ID3D12Resource> m_renderColor;  // RGBA8,   render res (u0)
    ComPtr<ID3D12Resource> m_renderAccum;  // RGBA32F, render res (u1)

    // DLSS 보조 입력 (모두 0으로 유지 — 경로 추적기는 GBuffer 없음)
    ComPtr<ID3D12Resource> m_depth;        // R32_FLOAT, render res
    ComPtr<ID3D12Resource> m_motionVec;    // R16G16_FLOAT, render res

    // DLSS 출력 (디스플레이 해상도, 백버퍼와 동일 포맷)
    ComPtr<ID3D12Resource> m_dlssOutput;

    // 씬 전환 시 TLAS SRV 미러(슬롯 9)를 갱신하기 위한 CPU 핸들
    D3D12_CPU_DESCRIPTOR_HANDLE m_tlasMirrorCPU{};

    uint32_t m_renderW  = 0, m_renderH  = 0;
    uint32_t m_displayW = 0, m_displayH = 0;
    bool     m_available = false;
};
