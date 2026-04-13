#pragma once
#include "Common.h"
#include "DescriptorHeap.h"
#include "ShaderTable.h"

// ──────────────────────────────────────────────────────────────
//  ReSTIRPass – G-Buffer / Reservoir 버퍼 + Compute/DXR 파이프라인 관리
//
//  디스크립터 힙 슬롯 레이아웃 (ReSTIRPass::Init 후):
//   Slot  7: UAV u2 – gbuf_worldPos  (RGBA32F)
//   Slot  8: UAV u3 – gbuf_normal    (RGBA32F)
//   Slot  9: UAV u4 – gbuf_albedo    (RGBA8)
//   Slot 10: UAV u5 – gbuf_matInfo   (RGBA32F)
//   Slot 11: UAV u6 – reservoir_cur  (RWStructuredBuffer<Reservoir>)
//   Slot 12: UAV u7 – reservoir_prev (RWStructuredBuffer<Reservoir>)
//   Slot 13: UAV u8 – motionVec      (RG32F)
//   Slot 14: SRV t5 – lightList      (StructuredBuffer<LightData>)
//   Slot 15: SRV t6 – gbuf_worldPos  (SRV 복사, compute pass용)
//   Slot 16: SRV t7 – gbuf_normal    (SRV 복사)
//   Slot 17: SRV t8 – gbuf_albedo    (SRV 복사)
//   Slot 18: SRV t9 – gbuf_matInfo   (SRV 복사)
//   Slot 19: SRV t10– motionVec      (SRV 복사)
//   Slot 20: SRV t11– reservoir_in   (Spatial pass 입력용 SRV)
//   Slot 21: SRV t12– reservoir_cur  (Shade pass 입력용 SRV)
//
//  패스 실행 순서 (App::OnRender에서 호출):
//   1. DispatchGBuffer    – DXR, G-Buffer 채우기
//   2. DispatchInitial    – Compute, RIS 후보 생성
//   3. DispatchTemporal   – Compute, 시간적 재사용
//   4. DispatchSpatial    – Compute, 공간 재사용 2회
//   5. SwapReservoirs     – cur ↔ prev 버퍼 교환 (다음 프레임을 위해)
//   6. (Denoiser는 기존 Denoiser 클래스 재사용)
// ──────────────────────────────────────────────────────────────
class ReSTIRPass
{
public:
    // 초기화: 모든 버퍼/PSO 생성, 힙에 디스크립터 등록
    void Init(ID3D12Device5*     device,
              DescriptorHeap&    heap,
              ID3D12RootSignature* globalRS,
              uint32_t           width,
              uint32_t           height);

    void Shutdown();

    // 해상도 변경 시 버퍼 재생성
    void Resize(ID3D12Device5*  device,
                DescriptorHeap& heap,
                uint32_t        width,
                uint32_t        height);

    // 광원 리스트 업로드 (씬 전환 시 호출)
    void UploadLights(ID3D12GraphicsCommandList* cmdList,
                      const std::vector<LightData>& lights);

    // 상수 버퍼 업데이트 (매 프레임)
    void UpdateCB(const ReSTIRCB& cb);

    // ── 패스 디스패치 ─────────────────────────────────────────

    // [Pass 1] G-Buffer: DXR primary ray
    void DispatchGBuffer(ID3D12GraphicsCommandList4* cmdList,
                         ID3D12StateObject*          gbufferPSO,
                         const ShaderTable&          shaderTable,
                         uint32_t width, uint32_t height);

    // [Pass 2] Initial: Compute RIS
    void DispatchInitial(ID3D12GraphicsCommandList* cmdList,
                         uint32_t width, uint32_t height);

    // [Pass 3] Temporal: Compute merge
    void DispatchTemporal(ID3D12GraphicsCommandList* cmdList,
                          uint32_t width, uint32_t height);

    // [Pass 4] Spatial: Compute merge x2
    void DispatchSpatial(ID3D12GraphicsCommandList* cmdList,
                         uint32_t width, uint32_t height);

    // [Pass 5] Shade: DXR shadow ray + GGX BRDF 최종 출력
    void DispatchShade(ID3D12GraphicsCommandList4* cmdList,
                       ID3D12StateObject*          shadePSO,
                       const ShaderTable&          shaderTable,
                       uint32_t width, uint32_t height);

    // 프레임 마지막: cur ↔ prev 교환 (포인터 스왑, GPU 이동 없음)
    void SwapReservoirs();

    // ReSTIRCB GPU 가상 주소 (App::OnRender에서 루트 CBV 바인딩용)
    D3D12_GPU_VIRTUAL_ADDRESS RestirCBAddress() const noexcept
    {
        return m_restirCB ? m_restirCB->GetGPUVirtualAddress() : 0;
    }

    // UAV 배리어 (패스 간 동기화)
    void UAVBarrier(ID3D12GraphicsCommandList* cmdList);

    // G-Buffer 클리어 (씬 전환 시)
    void ClearGBuffer(ID3D12GraphicsCommandList* cmdList);

private:
    // ── G-Buffer 텍스처 ──────────────────────────────────────
    ComPtr<ID3D12Resource> m_gbWorldPos;   // RGBA32F
    ComPtr<ID3D12Resource> m_gbNormal;     // RGBA32F
    ComPtr<ID3D12Resource> m_gbAlbedo;     // RGBA8
    ComPtr<ID3D12Resource> m_gbMatInfo;    // RGBA32F

    DescriptorHandle m_gbWorldPosUAV;
    DescriptorHandle m_gbNormalUAV;
    DescriptorHandle m_gbAlbedoUAV;
    DescriptorHandle m_gbMatInfoUAV;

    DescriptorHandle m_gbWorldPosSRV;  // compute 패스용 SRV
    DescriptorHandle m_gbNormalSRV;
    DescriptorHandle m_gbAlbedoSRV;
    DescriptorHandle m_gbMatInfoSRV;

    // ── Reservoir 버퍼 (double-buffered) ────────────────────
    ComPtr<ID3D12Resource> m_reservoirA;   // RWStructuredBuffer<Reservoir>
    ComPtr<ID3D12Resource> m_reservoirB;
    bool                   m_curIsA = true;  // SwapReservoirs로 교환

    DescriptorHandle m_reservoirA_UAV;
    DescriptorHandle m_reservoirB_UAV;
    DescriptorHandle m_reservoirA_SRV;  // Spatial/Shade pass 입력
    DescriptorHandle m_reservoirB_SRV;

    // ── 모션 벡터 ────────────────────────────────────────────
    ComPtr<ID3D12Resource> m_motionVec;    // RG32F
    DescriptorHandle       m_motionVecUAV;
    DescriptorHandle       m_motionVecSRV;

    // ── 광원 리스트 ──────────────────────────────────────────
    ComPtr<ID3D12Resource> m_lightListBuf; // StructuredBuffer<LightData>
    ComPtr<ID3D12Resource> m_lightListUpload;
    DescriptorHandle       m_lightListSRV;
    uint32_t               m_lightCount = 0;

    // ── 상수 버퍼 (ReSTIRCB, b1) ─────────────────────────────
    ComPtr<ID3D12Resource> m_restirCB;

    // ── Compute PSO ──────────────────────────────────────────
    ComPtr<ID3D12PipelineState> m_psoInitial;   // CS_Initial
    ComPtr<ID3D12PipelineState> m_psoTemporal;  // CS_Temporal
    ComPtr<ID3D12PipelineState> m_psoSpatial;   // CS_Spatial

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    // ── 디바이스 (디스크립터 복사용) ──────────────────────────
    ID3D12Device* m_device = nullptr;
    uint32_t      m_descriptorIncrementSize = 0;

    // ── 공간 ping-pong용 스테이징 핸들 (슬롯 22~25) ──────────
    // CopyDescriptorsSimple 소스: same heap 내 non-shader 슬롯
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResA_UAV{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResB_UAV{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResA_SRV{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResB_SRV{};

    // ── 메인 힙 슬롯 핸들 (스왑/ping-pong 갱신 대상) ──────────
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapSlot11_cpu{};  // u6  UAV reservoir_cur
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapSlot12_cpu{};  // u7  UAV reservoir_prev
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapSlot20_cpu{};  // t11 SRV reservoir_in
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapSlot21_cpu{};  // t12 SRV reservoir_cur(shade)

    // G-Buffer 리소스 현재 상태 추적 (UAV ↔ NON_PIXEL_SHADER_RESOURCE)
    bool m_gbufInSRVState = false;

    // ── 내부 헬퍼 ────────────────────────────────────────────
    void CreateBuffers(ID3D12Device* device, DescriptorHeap& heap,
                       uint32_t w, uint32_t h);
    void CreateComputePSOs(ID3D12Device5* device,
                           ID3D12RootSignature* globalRS);
    ComPtr<ID3D12Resource> CreateUAVTexture(ID3D12Device* device,
                                            DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h,
                                            const wchar_t* name);
    ComPtr<ID3D12Resource> CreateStructuredBuffer(ID3D12Device* device,
                                                   uint32_t elementSize,
                                                   uint32_t count,
                                                   const wchar_t* name);
};
