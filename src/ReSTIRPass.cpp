#include "ReSTIRPass.h"
#include "ShaderTable.h"
#include <print>
#include <dxcapi.h>
#include <filesystem>

// ──────────────────────────────────────────────────────────────
//  ComputePSO 생성 헬퍼 (DXC cs_6_6 컴파일)
// ──────────────────────────────────────────────────────────────
namespace
{
    ComPtr<IDxcBlob> CompileCS(const std::filesystem::path& path,
                               const char* entry)
    {
        ComPtr<IDxcUtils> utils;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)),
                      "DxcUtils 생성 실패");

        ComPtr<IDxcCompiler3> compiler;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)),
                      "DxcCompiler3 생성 실패");

        ComPtr<IDxcBlobEncoding> sourceBlob;
        ThrowIfFailed(utils->LoadFile(path.c_str(), nullptr, &sourceBlob),
                      std::format("셰이더 파일 열기 실패: {}", path.string()));

        DxcBuffer source{};
        source.Ptr      = sourceBlob->GetBufferPointer();
        source.Size     = sourceBlob->GetBufferSize();
        source.Encoding = DXC_CP_UTF8;

        std::wstring fileW  = path.wstring();
        std::wstring includeDir = path.parent_path().wstring();
        std::wstring entryW(entry, entry + std::strlen(entry));

        std::vector<LPCWSTR> args = {
            fileW.c_str(),
            L"-T", L"cs_6_6",
            L"-E", entryW.c_str(),
            L"-HV", L"2021",
            L"-I", includeDir.c_str(),
#if defined(_DEBUG)
            L"-Zi",
            L"-Od",
#else
            L"-O3",
#endif
        };

        ComPtr<IDxcIncludeHandler> includeHandler;
        ThrowIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler),
                      "DXC IncludeHandler 생성 실패");

        ComPtr<IDxcResult> result;
        ThrowIfFailed(compiler->Compile(
            &source,
            args.data(), static_cast<UINT32>(args.size()),
            includeHandler.Get(),
            IID_PPV_ARGS(&result)), "DXC Compute Compile 호출 실패");

        HRESULT hr = S_OK;
        result->GetStatus(&hr);
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            if (errors && errors->GetStringLength() > 0)
                std::println("[DXC CS 컴파일 오류] {}:{}\n{}",
                             path.string(), entry, errors->GetStringPointer());
            ThrowIfFailed(hr, "CS 컴파일 실패");
        }

        ComPtr<IDxcBlob> dxil;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), nullptr);
        return dxil;
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
    m_device = device;
    m_width  = width;
    m_height = height;
    m_descriptorIncrementSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // G-Buffer, Reservoir, 모션벡터, 광원 버퍼 생성 + 디스크립터 등록
    CreateBuffers(device, heap, width, height);

    // Compute PSO 생성 (Initial / Temporal / Spatial)
    CreateComputePSOs(device, globalRS);

    // ReSTIRCB 업로드 버퍼 생성 (256B 정렬)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 256;
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
    uint32_t pixCount = w * h;

    // ── G-Buffer 텍스처 생성 ─────────────────────────────────
    m_gbWorldPos = CreateUAVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, L"GB_WorldPos");
    m_gbNormal   = CreateUAVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, L"GB_Normal");
    m_gbAlbedo   = CreateUAVTexture(device, DXGI_FORMAT_R8G8B8A8_UNORM,     w, h, L"GB_Albedo");
    m_gbMatInfo  = CreateUAVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, L"GB_MatInfo");

    // ── Reservoir 버퍼 (double-buffered) ────────────────────
    // sizeof(Reservoir) = 16 bytes
    constexpr uint32_t kReservoirStride = 16u;
    m_reservoirA = CreateStructuredBuffer(device, kReservoirStride, pixCount, L"ReservoirA");
    m_reservoirB = CreateStructuredBuffer(device, kReservoirStride, pixCount, L"ReservoirB");

    // ── 모션 벡터 텍스처 ─────────────────────────────────────
    m_motionVec  = CreateUAVTexture(device, DXGI_FORMAT_R32G32_FLOAT, w, h, L"MotionVec");

    // ── 광원 리스트 버퍼 (최대 64개) ────────────────────────
    // sizeof(LightData) = 48 bytes (pos12+intensity4+color12+type4+halfSize4+center12)
    const uint32_t kLightStride = static_cast<uint32_t>(sizeof(LightData));
    constexpr uint32_t kMaxLights   = 64u;
    m_lightListBuf = CreateStructuredBuffer(device, kLightStride, kMaxLights, L"LightList");
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = static_cast<uint64_t>(kLightStride) * kMaxLights;
        rd.Height           = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc       = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_lightListUpload)));
        m_lightListUpload->SetName(L"LightListUpload");
    }

    // ── 디스크립터 등록 (순서대로 힙 슬롯 7~21, 이후 스테이징 22~25) ──

    auto MakeUAV_Tex = [&](ComPtr<ID3D12Resource>& res, DXGI_FORMAT fmt) -> DescriptorHandle
    {
        auto h = heap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Format        = fmt;
        device->CreateUnorderedAccessView(res.Get(), nullptr, &d, h.cpu);
        return h;
    };

    auto MakeSRV_Tex = [&](ComPtr<ID3D12Resource>& res, DXGI_FORMAT fmt) -> DescriptorHandle
    {
        auto h = heap.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format                    = fmt;
        d.Texture2D.MipLevels       = 1;
        device->CreateShaderResourceView(res.Get(), &d, h.cpu);
        return h;
    };

    auto MakeUAV_Buf = [&](ComPtr<ID3D12Resource>& res, uint32_t stride, uint32_t count) -> DescriptorHandle
    {
        auto h = heap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        d.Format                     = DXGI_FORMAT_UNKNOWN;
        d.Buffer.FirstElement        = 0;
        d.Buffer.NumElements         = count;
        d.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(res.Get(), nullptr, &d, h.cpu);
        return h;
    };

    auto MakeSRV_Buf = [&](ComPtr<ID3D12Resource>& res, uint32_t stride, uint32_t count) -> DescriptorHandle
    {
        auto h = heap.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format                     = DXGI_FORMAT_UNKNOWN;
        d.Buffer.FirstElement        = 0;
        d.Buffer.NumElements         = count;
        d.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(res.Get(), &d, h.cpu);
        return h;
    };

    // 슬롯  7: UAV u2 – gbuf_worldPos
    m_gbWorldPosUAV = MakeUAV_Tex(m_gbWorldPos, DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯  8: UAV u3 – gbuf_normal
    m_gbNormalUAV   = MakeUAV_Tex(m_gbNormal,   DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯  9: UAV u4 – gbuf_albedo
    m_gbAlbedoUAV   = MakeUAV_Tex(m_gbAlbedo,   DXGI_FORMAT_R8G8B8A8_UNORM);
    // 슬롯 10: UAV u5 – gbuf_matInfo
    m_gbMatInfoUAV  = MakeUAV_Tex(m_gbMatInfo,  DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯 11: UAV u6 – reservoir_cur (초기: A)
    m_reservoirA_UAV = MakeUAV_Buf(m_reservoirA, kReservoirStride, pixCount);
    // 슬롯 12: UAV u7 – reservoir_prev (초기: B)
    m_reservoirB_UAV = MakeUAV_Buf(m_reservoirB, kReservoirStride, pixCount);
    // 슬롯 13: UAV u8 – motionVec
    m_motionVecUAV   = MakeUAV_Tex(m_motionVec, DXGI_FORMAT_R32G32_FLOAT);
    // 슬롯 14: SRV t5 – lightList
    m_lightListSRV   = MakeSRV_Buf(m_lightListBuf, kLightStride, kMaxLights);
    // 슬롯 15: SRV t6 – gbuf_worldPos
    m_gbWorldPosSRV  = MakeSRV_Tex(m_gbWorldPos, DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯 16: SRV t7 – gbuf_normal
    m_gbNormalSRV    = MakeSRV_Tex(m_gbNormal,   DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯 17: SRV t8 – gbuf_albedo
    m_gbAlbedoSRV    = MakeSRV_Tex(m_gbAlbedo,   DXGI_FORMAT_R8G8B8A8_UNORM);
    // 슬롯 18: SRV t9 – gbuf_matInfo
    m_gbMatInfoSRV   = MakeSRV_Tex(m_gbMatInfo,  DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯 19: SRV t10 – motionVec
    m_motionVecSRV   = MakeSRV_Tex(m_motionVec, DXGI_FORMAT_R32G32_FLOAT);
    // 슬롯 20: SRV t11 – reservoir_in (Spatial 입력, 초기: A)
    m_reservoirA_SRV = MakeSRV_Buf(m_reservoirA, kReservoirStride, pixCount);
    // 슬롯 21: SRV t12 – reservoir_cur Shade 입력 (초기: A)
    m_reservoirB_SRV = MakeSRV_Buf(m_reservoirB, kReservoirStride, pixCount);

    // 슬롯 22~25: ping-pong/swap 소스용 스테이징 (동일 힙 내)
    auto stageA_UAV = MakeUAV_Buf(m_reservoirA, kReservoirStride, pixCount);
    auto stageB_UAV = MakeUAV_Buf(m_reservoirB, kReservoirStride, pixCount);
    auto stageA_SRV = MakeSRV_Buf(m_reservoirA, kReservoirStride, pixCount);
    auto stageB_SRV = MakeSRV_Buf(m_reservoirB, kReservoirStride, pixCount);

    m_stageResA_UAV = stageA_UAV.cpu;
    m_stageResB_UAV = stageB_UAV.cpu;
    m_stageResA_SRV = stageA_SRV.cpu;
    m_stageResB_SRV = stageB_SRV.cpu;

    // 활성 슬롯 CPU 핸들 저장 (스왑/ping-pong에서 덮어씀)
    m_heapSlot11_cpu = m_reservoirA_UAV.cpu;
    m_heapSlot12_cpu = m_reservoirB_UAV.cpu;
    m_heapSlot20_cpu = m_reservoirA_SRV.cpu;
    m_heapSlot21_cpu = m_reservoirA_SRV.cpu;

    // 슬롯 21 (t12 shade)을 초기에는 A SRV로 덮어쓰기
    device->CopyDescriptorsSimple(1,
        m_heapSlot21_cpu, m_stageResA_SRV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    std::println("[ReSTIRPass] 버퍼 생성 완료 ({}x{}, {} pixels)", w, h, pixCount);
}

// ──────────────────────────────────────────────────────────────
//  CreateComputePSOs
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::CreateComputePSOs(ID3D12Device5*       device,
                                    ID3D12RootSignature* globalRS)
{
    auto createPSO = [&](const char* file, const char* entry,
                         ComPtr<ID3D12PipelineState>& pso)
    {
        auto blob = CompileCS(file, entry);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = globalRS;
        desc.CS             = { blob->GetBufferPointer(), blob->GetBufferSize() };
        ThrowIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)),
                      std::format("Compute PSO 생성 실패: {}:{}", file, entry));
    };

    createPSO("shaders/ReSTIR_Initial.hlsl",  "CS_Initial",  m_psoInitial);
    createPSO("shaders/ReSTIR_Temporal.hlsl", "CS_Temporal", m_psoTemporal);
    createPSO("shaders/ReSTIR_Spatial.hlsl",  "CS_Spatial",  m_psoSpatial);

    std::println("[ReSTIRPass] Compute PSO 생성 완료");
}

// ──────────────────────────────────────────────────────────────
//  UploadLights
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::UploadLights(ID3D12GraphicsCommandList* cmdList,
                               const std::vector<LightData>& lights)
{
    m_lightCount = static_cast<uint32_t>(lights.size());
    if (m_lightCount == 0) return;

    const uint32_t kLightStride = static_cast<uint32_t>(sizeof(LightData));
    uint64_t copySize = static_cast<uint64_t>(kLightStride) * m_lightCount;

    // 업로드 힙에 데이터 기록
    void* mapped = nullptr;
    D3D12_RANGE rr{0, 0};
    ThrowIfFailed(m_lightListUpload->Map(0, &rr, &mapped));
    std::memcpy(mapped, lights.data(), copySize);
    m_lightListUpload->Unmap(0, nullptr);

    // UNORDERED_ACCESS → COPY_DEST
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_lightListBuf.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    }

    cmdList->CopyBufferRegion(m_lightListBuf.Get(), 0,
                              m_lightListUpload.Get(), 0, copySize);

    // COPY_DEST → NON_PIXEL_SHADER_RESOURCE
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_lightListBuf.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    }
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
//  DispatchGBuffer  – DXR primary ray → G-Buffer
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchGBuffer(ID3D12GraphicsCommandList4* cmdList,
                                  ID3D12StateObject*          gbufferPSO,
                                  const ShaderTable&          shaderTable,
                                  uint32_t w, uint32_t h)
{
    // G-Buffer UAV들은 이미 UNORDERED_ACCESS 상태 (CreateUAVTexture 초기상태)
    // (이미 UAV 상태임을 가정; 최초 프레임에서는 barrier 불필요)

    cmdList->SetPipelineState1(gbufferPSO);

    D3D12_DISPATCH_RAYS_DESC desc{};
    desc.RayGenerationShaderRecord = shaderTable.RayGenRange();
    desc.MissShaderTable           = shaderTable.MissRange();
    desc.HitGroupTable             = shaderTable.HitGroupRange();
    desc.Width  = w;
    desc.Height = h;
    desc.Depth  = 1;

    cmdList->DispatchRays(&desc);

    // G-Buffer 쓰기 완료 대기 (UAV 배리어)
    {
        D3D12_RESOURCE_BARRIER barriers[4]{};
        ID3D12Resource* gbuf[4] = {
            m_gbWorldPos.Get(), m_gbNormal.Get(),
            m_gbAlbedo.Get(),   m_gbMatInfo.Get()
        };
        for (int i = 0; i < 4; ++i)
        {
            barriers[i].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[i].UAV.pResource = gbuf[i];
        }
        cmdList->ResourceBarrier(4, barriers);
    }
}

// ──────────────────────────────────────────────────────────────
//  DispatchInitial  – [Pass 2] RIS 후보 생성
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchInitial(ID3D12GraphicsCommandList* cmdList,
                                  uint32_t w, uint32_t h)
{
    cmdList->SetPipelineState(m_psoInitial.Get());

    uint32_t gx = (w + 7u) / 8u;
    uint32_t gy = (h + 7u) / 8u;
    cmdList->Dispatch(gx, gy, 1);

    // reservoir_cur (A) UAV 배리어
    {
        auto* res = m_curIsA ? m_reservoirA.Get() : m_reservoirB.Get();
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmdList->ResourceBarrier(1, &b);
    }
}

// ──────────────────────────────────────────────────────────────
//  DispatchTemporal  – [Pass 3] 시간적 재사용
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchTemporal(ID3D12GraphicsCommandList* cmdList,
                                   uint32_t w, uint32_t h)
{
    cmdList->SetPipelineState(m_psoTemporal.Get());

    uint32_t gx = (w + 7u) / 8u;
    uint32_t gy = (h + 7u) / 8u;
    cmdList->Dispatch(gx, gy, 1);

    // reservoir_cur UAV 배리어
    {
        auto* res = m_curIsA ? m_reservoirA.Get() : m_reservoirB.Get();
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmdList->ResourceBarrier(1, &b);
    }
}

// ──────────────────────────────────────────────────────────────
//  DispatchSpatial  – [Pass 4] 공간 재사용 (ping-pong 2회)
//  Pass 0: cur(A) SRV → B UAV
//  Pass 1: B SRV → A UAV  (결과: A)
//  이후 슬롯 21(t12 shade)을 A SRV로 복원
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchSpatial(ID3D12GraphicsCommandList* cmdList,
                                  uint32_t w, uint32_t h)
{
    uint32_t gx = (w + 7u) / 8u;
    uint32_t gy = (h + 7u) / 8u;

    // ── Pass 0: A → B ─────────────────────────────────────────
    // 슬롯 11 = B UAV (write), 슬롯 20 = A SRV (read)
    m_device->CopyDescriptorsSimple(1,
        m_heapSlot11_cpu, m_stageResB_UAV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1,
        m_heapSlot20_cpu, m_stageResA_SRV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    cmdList->SetPipelineState(m_psoSpatial.Get());
    cmdList->Dispatch(gx, gy, 1);

    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = m_reservoirB.Get();
        cmdList->ResourceBarrier(1, &b);
    }

    // ── Pass 1: B → A ─────────────────────────────────────────
    // 슬롯 11 = A UAV (write), 슬롯 20 = B SRV (read)
    m_device->CopyDescriptorsSimple(1,
        m_heapSlot11_cpu, m_stageResA_UAV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1,
        m_heapSlot20_cpu, m_stageResB_SRV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    cmdList->SetPipelineState(m_psoSpatial.Get());
    cmdList->Dispatch(gx, gy, 1);

    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = m_reservoirA.Get();
        cmdList->ResourceBarrier(1, &b);
    }

    // ── 슬롯 복원: 최종 결과(A)를 Shade pass(t12)용 SRV로 설정 ──
    m_device->CopyDescriptorsSimple(1,
        m_heapSlot21_cpu, m_stageResA_SRV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 슬롯 11/20 을 cur/prev 기본 상태로 복원 (curIsA=true → A)
    m_device->CopyDescriptorsSimple(1,
        m_heapSlot11_cpu, m_stageResA_UAV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1,
        m_heapSlot20_cpu, m_stageResA_SRV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// ──────────────────────────────────────────────────────────────
//  SwapReservoirs  (프레임 마지막 – cur ↔ prev)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::SwapReservoirs()
{
    m_curIsA = !m_curIsA;

    // 슬롯 11(u6 cur), 12(u7 prev), 20(t11 spatial in), 21(t12 shade in) 갱신
    auto& curUAV  = m_curIsA ? m_stageResA_UAV : m_stageResB_UAV;
    auto& prevUAV = m_curIsA ? m_stageResB_UAV : m_stageResA_UAV;
    auto& curSRV  = m_curIsA ? m_stageResA_SRV : m_stageResB_SRV;

    m_device->CopyDescriptorsSimple(1, m_heapSlot11_cpu, curUAV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, m_heapSlot12_cpu, prevUAV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, m_heapSlot20_cpu, curSRV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, m_heapSlot21_cpu, curSRV,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// ──────────────────────────────────────────────────────────────
//  UAVBarrier
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::UAVBarrier(ID3D12GraphicsCommandList* cmdList)
{
    ID3D12Resource* resources[] = {
        m_gbWorldPos.Get(), m_gbNormal.Get(),
        m_gbAlbedo.Get(),   m_gbMatInfo.Get(),
        m_reservoirA.Get(), m_reservoirB.Get(),
        m_motionVec.Get()
    };

    D3D12_RESOURCE_BARRIER barriers[7]{};
    for (int i = 0; i < 7; ++i)
    {
        barriers[i].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[i].UAV.pResource = resources[i];
    }
    cmdList->ResourceBarrier(7, barriers);
}

// ──────────────────────────────────────────────────────────────
//  ClearGBuffer  – 씬 전환 시 G-Buffer 클리어
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::ClearGBuffer(ID3D12GraphicsCommandList* cmdList)
{
    // worldPos.w = -1 로 클리어하여 배경으로 마킹
    const float clearW[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
    const float clearZ[4] = { 0.0f, 0.0f, 0.0f,  0.0f };

    cmdList->ClearUnorderedAccessViewFloat(
        m_gbWorldPosUAV.gpu, m_gbWorldPosUAV.cpu,
        m_gbWorldPos.Get(), clearW, 0, nullptr);
    cmdList->ClearUnorderedAccessViewFloat(
        m_gbNormalUAV.gpu, m_gbNormalUAV.cpu,
        m_gbNormal.Get(), clearZ, 0, nullptr);
    cmdList->ClearUnorderedAccessViewFloat(
        m_gbMatInfoUAV.gpu, m_gbMatInfoUAV.cpu,
        m_gbMatInfo.Get(), clearZ, 0, nullptr);
}

// ──────────────────────────────────────────────────────────────
//  DispatchShade  – [Pass 5] DXR 그림자 레이 + GGX BRDF 최종 출력
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchShade(ID3D12GraphicsCommandList4* cmdList,
                                ID3D12StateObject*          shadePSO,
                                const ShaderTable&          shaderTable,
                                uint32_t w, uint32_t h)
{
    cmdList->SetPipelineState1(shadePSO);

    D3D12_DISPATCH_RAYS_DESC desc{};
    desc.RayGenerationShaderRecord = shaderTable.RayGenRange();
    desc.MissShaderTable           = shaderTable.MissRange();
    desc.HitGroupTable             = shaderTable.HitGroupRange();
    desc.Width  = w;
    desc.Height = h;
    desc.Depth  = 1;

    cmdList->DispatchRays(&desc);

    // g_output UAV 쓰기 완료 대기 – 다음 패스(present copy)를 위해
    // (UAV 배리어는 App::OnRender에서 m_renderTarget.UAVBarriers()로 처리)
}

void ReSTIRPass::Shutdown()
{
    m_gbWorldPos = nullptr; m_gbNormal  = nullptr;
    m_gbAlbedo   = nullptr; m_gbMatInfo = nullptr;
    m_reservoirA = nullptr; m_reservoirB = nullptr;
    m_motionVec  = nullptr; m_lightListBuf = nullptr;
    m_lightListUpload = nullptr;
    m_restirCB   = nullptr;
    m_psoInitial = nullptr; m_psoTemporal = nullptr;
    m_psoSpatial = nullptr;
    m_device     = nullptr;
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
    // 기존 버퍼 해제 후 새 크기로 재생성
    m_width  = width;
    m_height = height;
    CreateBuffers(device, heap, width, height);
}
