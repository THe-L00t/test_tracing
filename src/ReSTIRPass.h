#pragma once
#include "Common.h"
#include "DescriptorHeap.h"
#include "ShaderTable.h"

// ──────────────────────────────────────────────────────────────
//  ReSTIRPass – G-Buffer / Reservoir 버퍼 + Compute/DXR 파이프라인 관리
//
//  ── 힙 슬롯 레이아웃 (Init 후) ────────────────────────────────
//   Slot  7: UAV u2 – gbuf_worldPos  RGBA32F
//   Slot  8: UAV u3 – gbuf_normal    RGBA32F
//   Slot  9: UAV u4 – gbuf_albedo    RGBA8
//   Slot 10: UAV u5 – gbuf_matInfo   RGBA32F
//   Slot 11: UAV u6 – reservoir_cur  RWStructuredBuffer  ← CopyDescriptor로 동적
//   Slot 12: UAV u7 – reservoir_prev RWStructuredBuffer  ← CopyDescriptor로 동적
//   Slot 13: SRV t5 – lightList      StructuredBuffer<LightData>
//   Slot 14: SRV t6 – gbuf_worldPos  Texture2D SRV
//   Slot 15: SRV t7 – gbuf_normal    Texture2D SRV
//   Slot 16: SRV t8 – gbuf_albedo    Texture2D SRV
//   Slot 17: SRV t9 – gbuf_matInfo   Texture2D SRV
//   Slot 18: SRV t10– reservoir_in   StructuredBuffer SRV ← CopyDescriptor로 동적
//   Slots 19-22: 스테이징 (셰이더 비접근, CopyDescriptor 소스)
//     19: ResA_UAV, 20: ResB_UAV, 21: ResA_SRV, 22: ResB_SRV
//
//  ── 패스 실행 순서 (App::OnRender) ───────────────────────────
//   1. DispatchGBuffer   – DXR primary ray → G-Buffer 채우기
//   2. DispatchInitial   – CS, RIS 후보 생성 (Talbot 2005)
//   3. DispatchTemporal  – CS, 시간적 재사용 (Bitterli 2020 Sec.4.3)
//   4. DispatchSpatial   – CS, 공간 재사용 2회 ping-pong (Bitterli 2020 Alg.4)
//   5. DispatchShade     – DXR, shadow ray + GGX BRDF → g_output
//   6. SwapReservoirs    – cur/prev 버퍼 포인터 교환
//
//  ── 리소스 상태 관리 ─────────────────────────────────────────
//   각 패스 함수가 자신의 진입/퇴장 시점에 확정적으로 전환함.
//   m_gbufState, m_resAState, m_resBState 로 현재 상태 추적.
//   TransitionRes() 헬퍼: 상태가 다를 때만 barrier 발행.
// ──────────────────────────────────────────────────────────────
class ReSTIRPass
{
public:
    void Init(ID3D12Device5*      device,
              DescriptorHeap&    heap,
              ID3D12RootSignature* globalRS,
              uint32_t           width,
              uint32_t           height);

    void Shutdown();

    void Resize(ID3D12Device5*  device,
                DescriptorHeap& heap,
                uint32_t        width,
                uint32_t        height);

    void UploadLights(ID3D12GraphicsCommandList* cmdList,
                      const std::vector<LightData>& lights);

    void UpdateCB(const ReSTIRCB& cb);

    // ── 패스 디스패치 ─────────────────────────────────────────
    void DispatchGBuffer(ID3D12GraphicsCommandList4* cmdList,
                         ID3D12StateObject*          gbufferPSO,
                         const ShaderTable&          shaderTable,
                         uint32_t width, uint32_t height);

    void DispatchInitial(ID3D12GraphicsCommandList* cmdList,
                         uint32_t width, uint32_t height);

    void DispatchTemporal(ID3D12GraphicsCommandList* cmdList,
                          uint32_t width, uint32_t height);

    void DispatchSpatial(ID3D12GraphicsCommandList* cmdList,
                         uint32_t width, uint32_t height);

    void DispatchShade(ID3D12GraphicsCommandList4* cmdList,
                       ID3D12StateObject*          shadePSO,
                       const ShaderTable&          shaderTable,
                       uint32_t width, uint32_t height);

    // 프레임 마지막: cur/prev 포인터 교환 + 힙 슬롯 갱신
    void SwapReservoirs();

    D3D12_GPU_VIRTUAL_ADDRESS RestirCBAddress() const noexcept
    {
        return m_restirCB ? m_restirCB->GetGPUVirtualAddress() : 0;
    }

    // 씬 전환 시 G-Buffer 클리어 (hitDist=-1로 배경 마킹)
    void ClearGBuffer(ID3D12GraphicsCommandList* cmdList);

private:
    // ── G-Buffer 텍스처 ──────────────────────────────────────
    ComPtr<ID3D12Resource> m_gbWorldPos;
    ComPtr<ID3D12Resource> m_gbNormal;
    ComPtr<ID3D12Resource> m_gbAlbedo;
    ComPtr<ID3D12Resource> m_gbMatInfo;

    DescriptorHandle m_gbWorldPosUAV, m_gbNormalUAV, m_gbAlbedoUAV, m_gbMatInfoUAV;
    DescriptorHandle m_gbWorldPosSRV, m_gbNormalSRV, m_gbAlbedoSRV, m_gbMatInfoSRV;

    // ── Reservoir 버퍼 (double-buffered, A/B) ───────────────
    ComPtr<ID3D12Resource> m_reservoirA;
    ComPtr<ID3D12Resource> m_reservoirB;
    bool m_curIsA = true;  // true: cur=A(u6), prev=B(u7)

    // ── 광원 리스트 ──────────────────────────────────────────
    ComPtr<ID3D12Resource> m_lightListBuf;
    ComPtr<ID3D12Resource> m_lightListUpload;
    DescriptorHandle       m_lightListSRV;
    uint32_t               m_lightCount = 0;

    // ── 상수 버퍼 (ReSTIRCB, b1) ─────────────────────────────
    ComPtr<ID3D12Resource> m_restirCB;

    // ── Compute PSO ──────────────────────────────────────────
    ComPtr<ID3D12PipelineState> m_psoInitial;
    ComPtr<ID3D12PipelineState> m_psoTemporal;
    ComPtr<ID3D12PipelineState> m_psoSpatial;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    // ── 리소스 상태 추적 ────────────────────────────────────
    // 각 패스 함수가 진입/퇴장 시 갱신. TransitionRes()가 barrier 발행 여부 판단.
    D3D12_RESOURCE_STATES m_gbufState      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_resAState      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_resBState      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_lightListState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // ── 디스크립터 동적 갱신용 ──────────────────────────────
    // 힙 슬롯 11(u6), 12(u7), 18(t10) 의 CPU 핸들
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapSlot11_cpu{};  // reservoir_cur  UAV
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapSlot12_cpu{};  // reservoir_prev UAV
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapSlot18_cpu{};  // reservoir_in   SRV

    // 스테이징 소스 핸들 (CopyDescriptorsSimple 소스)
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResA_UAV{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResB_UAV{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResA_SRV{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_stageResB_SRV{};

    // ── 디바이스 (CopyDescriptorsSimple 용) ──────────────────
    ID3D12Device* m_device = nullptr;

    // ── 내부 헬퍼 ────────────────────────────────────────────
    void CreateBuffers(ID3D12Device* device, DescriptorHeap& heap,
                       uint32_t w, uint32_t h);
    void CreateComputePSOs(ID3D12Device5* device, ID3D12RootSignature* globalRS);

    // 리소스 상태 전환 (상태가 동일하면 no-op)
    void TransitionRes(ID3D12GraphicsCommandList* cmd,
                       ID3D12Resource*            res,
                       D3D12_RESOURCE_STATES&     curState,
                       D3D12_RESOURCE_STATES      newState);

    // G-Buffer 4개 일괄 전환
    void TransitionGBuf(ID3D12GraphicsCommandList* cmd,
                        D3D12_RESOURCE_STATES      newState);

    // 디스크립터 복사 헬퍼
    void CopyDesc(D3D12_CPU_DESCRIPTOR_HANDLE dst,
                  D3D12_CPU_DESCRIPTOR_HANDLE src) const
    {
        m_device->CopyDescriptorsSimple(1, dst, src,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    ComPtr<ID3D12Resource> CreateUAVTexture(ID3D12Device* device,
                                            DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h,
                                            const wchar_t* name);
    ComPtr<ID3D12Resource> CreateStructuredBuffer(ID3D12Device* device,
                                                   uint32_t elementSize,
                                                   uint32_t count,
                                                   const wchar_t* name);
};
