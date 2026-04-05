#include "ReSTIRPass.h"
#include <print>
#include <dxcapi.h>
#include <filesystem>

// ──────────────────────────────────────────────────────────────
//  ComputePSO 생성 헬퍼 (DXC 컴파일)
// ──────────────────────────────────────────────────────────────
namespace
{
    ComPtr<ID3DBlob> CompileCS(const std::filesystem::path& path,
                               const char* entry)
    {
        // TODO [Session 1]: DXC로 cs_6_6 컴파일
        // DxcCreateInstance → IDxcCompiler3::Compile
        // 참조: DXRPipeline.cpp CompileShaderDXC 패턴 동일, 타겟만 cs_6_6으로 변경
        return nullptr;  // STUB
    }
}

// ──────────────────────────────────────────────────────────────
//  Init
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::Init(ID3D12Device5*      device,
                      DescriptorHeap&     heap,
                      ID3D12RootSignature* globalRS,
                      uint32_t            width,
                      uint32_t            height)
{
    m_width  = width;
    m_height = height;

    // G-Buffer, Reservoir, 모션벡터, 광원 버퍼 생성 + 디스크립터 등록
    CreateBuffers(device, heap, width, height);

    // Compute PSO 생성 (Initial / Temporal / Spatial)
    CreateComputePSOs(device, globalRS);

    // ReSTIRCB 업로드 버퍼 생성 (128B)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 256;  // 256B 정렬
        rd.Height           = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc       = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_restirCB)));
        m_restirCB->SetName(L"ReSTIRCB");
    }

    std::println("[ReSTIRPass] 초기화 완료 ({}x{})", width, height);
}

// ──────────────────────────────────────────────────────────────
//  CreateBuffers
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::CreateBuffers(ID3D12Device* device,
                                DescriptorHeap& heap,
                                uint32_t w, uint32_t h)
{
    // TODO [Session 1]:
    // 1. G-Buffer 텍스처 생성 (CreateUAVTexture x4)
    //    - worldPos: DXGI_FORMAT_R32G32B32A32_FLOAT
    //    - normal:   DXGI_FORMAT_R32G32B32A32_FLOAT
    //    - albedo:   DXGI_FORMAT_R8G8B8A8_UNORM
    //    - matInfo:  DXGI_FORMAT_R32G32B32A32_FLOAT
    // 2. 각 텍스처에 UAV + SRV 디스크립터 등록 (heap.Allocate())
    //    힙 슬롯 7-21 순서대로 (ReSTIRPass.h 주석 참조)
    // 3. Reservoir 버퍼 생성 (CreateStructuredBuffer)
    //    elementSize = sizeof(Reservoir) = 16, count = w * h
    // 4. motionVec 생성: DXGI_FORMAT_R32G32_FLOAT
    // 5. lightList 버퍼 생성: elementSize = sizeof(LightData) = 32, count = 최대 64개
    std::println("[ReSTIRPass] TODO: CreateBuffers 구현 필요");
}

// ──────────────────────────────────────────────────────────────
//  CreateComputePSOs
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::CreateComputePSOs(ID3D12Device5*       device,
                                    ID3D12RootSignature* globalRS)
{
    // TODO [Session 1]:
    // 각 compute shader 컴파일 후 ID3D12PipelineState 생성
    // D3D12_COMPUTE_PIPELINE_STATE_DESC{pRootSignature, CS, ...}
    // device->CreateComputePipelineState(...)

    // m_psoInitial  ← shaders/ReSTIR_Initial.hlsl  CS_Initial
    // m_psoTemporal ← shaders/ReSTIR_Temporal.hlsl CS_Temporal
    // m_psoSpatial  ← shaders/ReSTIR_Spatial.hlsl  CS_Spatial
    std::println("[ReSTIRPass] TODO: CreateComputePSOs 구현 필요");
}

// ──────────────────────────────────────────────────────────────
//  UploadLights
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::UploadLights(ID3D12GraphicsCommandList* cmdList,
                               const std::vector<LightData>& lights)
{
    // TODO [Session 1]:
    // 1. lights 를 m_lightListUpload(업로드 힙)에 memcpy
    // 2. CopyBufferRegion → m_lightListBuf(default 힙)
    // 3. ResourceBarrier: COPY_DEST → SRV
    m_lightCount = static_cast<uint32_t>(lights.size());
    std::println("[ReSTIRPass] TODO: UploadLights 구현 필요");
}

// ──────────────────────────────────────────────────────────────
//  UpdateCB
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::UpdateCB(const ReSTIRCB& cb)
{
    void* mapped = nullptr;
    D3D12_RANGE rr{0, 0};
    ThrowIfFailed(m_restirCB->Map(0, &rr, &mapped));
    std::memcpy(mapped, &cb, sizeof(ReSTIRCB));
    m_restirCB->Unmap(0, nullptr);
}

// ──────────────────────────────────────────────────────────────
//  DispatchGBuffer
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchGBuffer(ID3D12GraphicsCommandList4* cmdList,
                                  ID3D12StateObject*          gbufferPSO,
                                  const ShaderTable&          shaderTable,
                                  uint32_t w, uint32_t h)
{
    // TODO [Session 1]:
    // 1. G-Buffer UAV들을 UNORDERED_ACCESS 상태로 전환
    // 2. SetComputeRootSignature + SetDescriptorHeaps
    // 3. cmdList->SetPipelineState1(gbufferPSO)
    // 4. DispatchRays (w×h)
    // 5. UAV 배리어
    std::println("[ReSTIRPass] TODO: DispatchGBuffer 구현 필요");
}

// ──────────────────────────────────────────────────────────────
//  DispatchInitial
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchInitial(ID3D12GraphicsCommandList* cmdList,
                                  uint32_t w, uint32_t h)
{
    // TODO [Session 2]:
    // 1. SetPipelineState(m_psoInitial)
    // 2. G-Buffer SRV 상태 전환
    // 3. DispatchCompute (ceil(w/8), ceil(h/8), 1)
    // 4. UAV 배리어
    std::println("[ReSTIRPass] TODO: DispatchInitial 구현 필요");
}

// ──────────────────────────────────────────────────────────────
//  DispatchTemporal
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchTemporal(ID3D12GraphicsCommandList* cmdList,
                                   uint32_t w, uint32_t h)
{
    // TODO [Session 2]:
    // 1. SetPipelineState(m_psoTemporal)
    // 2. cur = u6, prev = u7 (또는 m_curIsA에 따라 A/B 결정)
    // 3. DispatchCompute
    // 4. UAV 배리어
    std::println("[ReSTIRPass] TODO: DispatchTemporal 구현 필요");
}

// ──────────────────────────────────────────────────────────────
//  DispatchSpatial  (2회 반복, 버퍼 ping-pong)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchSpatial(ID3D12GraphicsCommandList* cmdList,
                                  uint32_t w, uint32_t h)
{
    // TODO [Session 2]:
    // Pass 0: reservoir_A(SRV, t11) → reservoir_B(UAV, u6)
    // Pass 1: reservoir_B(SRV, t11) → reservoir_A(UAV, u6)
    // 각 패스 사이 UAV 배리어
    std::println("[ReSTIRPass] TODO: DispatchSpatial 구현 필요");
}

// ──────────────────────────────────────────────────────────────
//  SwapReservoirs  (프레임 마지막 – cur → prev)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::SwapReservoirs()
{
    // m_curIsA 플래그만 토글 – GPU 복사 없음
    m_curIsA = !m_curIsA;
}

// ──────────────────────────────────────────────────────────────
//  UAVBarrier
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::UAVBarrier(ID3D12GraphicsCommandList* cmdList)
{
    // TODO [Session 1]: 각 버퍼에 D3D12_RESOURCE_BARRIER_TYPE_UAV 배리어
    std::println("[ReSTIRPass] TODO: UAVBarrier 구현 필요");
}

void ReSTIRPass::ClearGBuffer(ID3D12GraphicsCommandList* cmdList)
{
    // TODO [Session 1]: 씬 전환 시 G-Buffer 클리어
}

void ReSTIRPass::Shutdown()
{
    m_gbWorldPos = nullptr; m_gbNormal  = nullptr;
    m_gbAlbedo   = nullptr; m_gbMatInfo = nullptr;
    m_reservoirA = nullptr; m_reservoirB = nullptr;
    m_motionVec  = nullptr; m_lightListBuf = nullptr;
    m_restirCB   = nullptr;
    m_psoInitial = nullptr; m_psoTemporal = nullptr;
    m_psoSpatial = nullptr;
}

// ──────────────────────────────────────────────────────────────
//  내부 헬퍼 구현
// ──────────────────────────────────────────────────────────────
ComPtr<ID3D12Resource> ReSTIRPass::CreateUAVTexture(ID3D12Device* device,
                                                     DXGI_FORMAT fmt,
                                                     uint32_t w, uint32_t h,
                                                     const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc       = {1, 0};
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&res)));
    if (name) res->SetName(name);
    return res;
}

ComPtr<ID3D12Resource> ReSTIRPass::CreateStructuredBuffer(ID3D12Device* device,
                                                            uint32_t elementSize,
                                                            uint32_t count,
                                                            const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = static_cast<uint64_t>(elementSize) * count;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc       = {1, 0};
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&res)));
    if (name) res->SetName(name);
    return res;
}

void ReSTIRPass::Resize(ID3D12Device5*  device,
                         DescriptorHeap& heap,
                         uint32_t        width,
                         uint32_t        height)
{
    // TODO [Session 3]:
    // 기존 버퍼 해제 후 새 크기로 재생성
    m_width  = width;
    m_height = height;
    CreateBuffers(device, heap, width, height);
}
