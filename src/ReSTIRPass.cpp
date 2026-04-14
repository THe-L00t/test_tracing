#include "ReSTIRPass.h"
#include "ShaderTable.h"
#include <print>
#include <dxcapi.h>
#include <filesystem>

// ──────────────────────────────────────────────────────────────
//  CompileCS – DXC cs_6_6 컴파일 헬퍼
// ──────────────────────────────────────────────────────────────
namespace
{
    ComPtr<IDxcBlob> CompileCS(const std::filesystem::path& path,
                               const char* entry)
    {
        ComPtr<IDxcUtils> utils;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));

        ComPtr<IDxcCompiler3> compiler;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

        ComPtr<IDxcBlobEncoding> sourceBlob;
        ThrowIfFailed(utils->LoadFile(path.c_str(), nullptr, &sourceBlob),
                      std::format("셰이더 파일 열기 실패: {}", path.string()));

        DxcBuffer source{};
        source.Ptr      = sourceBlob->GetBufferPointer();
        source.Size     = sourceBlob->GetBufferSize();
        source.Encoding = DXC_CP_UTF8;

        std::wstring fileW       = path.wstring();
        std::wstring includeDir  = path.parent_path().wstring();
        std::wstring entryW(entry, entry + std::strlen(entry));

        std::vector<LPCWSTR> args = {
            fileW.c_str(),
            L"-T", L"cs_6_6",
            L"-E", entryW.c_str(),
            L"-HV", L"2021",
            L"-I", includeDir.c_str(),
#if defined(_DEBUG)
            L"-Zi", L"-Od",
#else
            L"-O3",
#endif
        };

        ComPtr<IDxcIncludeHandler> includeHandler;
        ThrowIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler));

        ComPtr<IDxcResult> result;
        ThrowIfFailed(compiler->Compile(
            &source, args.data(), static_cast<UINT32>(args.size()),
            includeHandler.Get(), IID_PPV_ARGS(&result)));

        HRESULT hr = S_OK;
        result->GetStatus(&hr);
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            if (errors && errors->GetStringLength() > 0)
                std::println("[DXC CS 오류] {}:{}\n{}", path.string(), entry,
                             errors->GetStringPointer());
            ThrowIfFailed(hr, "CS 컴파일 실패");
        }

        ComPtr<IDxcBlob> dxil;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), nullptr);
        return dxil;
    }
}

// ──────────────────────────────────────────────────────────────
//  TransitionRes – 리소스 상태 전환 헬퍼 (no-op if same state)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::TransitionRes(ID3D12GraphicsCommandList* cmd,
                                ID3D12Resource*            res,
                                D3D12_RESOURCE_STATES&     curState,
                                D3D12_RESOURCE_STATES      newState)
{
    if (curState == newState) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = curState;
    b.Transition.StateAfter  = newState;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    curState = newState;
}

// G-Buffer 4개 일괄 전환
void ReSTIRPass::TransitionGBuf(ID3D12GraphicsCommandList* cmd,
                                 D3D12_RESOURCE_STATES      newState)
{
    if (m_gbufState == newState) return;

    D3D12_RESOURCE_BARRIER barriers[4]{};
    ID3D12Resource* gbuf[4] = {
        m_gbWorldPos.Get(), m_gbNormal.Get(),
        m_gbAlbedo.Get(),   m_gbMatInfo.Get()
    };
    for (int i = 0; i < 4; ++i)
    {
        barriers[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource   = gbuf[i];
        barriers[i].Transition.StateBefore = m_gbufState;
        barriers[i].Transition.StateAfter  = newState;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmd->ResourceBarrier(4, barriers);
    m_gbufState = newState;
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

    CreateBuffers(device, heap, width, height);
    CreateComputePSOs(device, globalRS);

    // ReSTIRCB 업로드 버퍼 (256B 정렬)
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 256;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc = {1,0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_restirCB)));
    m_restirCB->SetName(L"ReSTIRCB");

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

    // ── G-Buffer 텍스처 ─────────────────────────────────────
    m_gbWorldPos = CreateUAVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, L"GB_WorldPos");
    m_gbNormal   = CreateUAVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, L"GB_Normal");
    m_gbAlbedo   = CreateUAVTexture(device, DXGI_FORMAT_R8G8B8A8_UNORM,     w, h, L"GB_Albedo");
    m_gbMatInfo  = CreateUAVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, L"GB_MatInfo");

    // ── Reservoir 버퍼 (double-buffered, sizeof(Reservoir)=16) ─
    constexpr uint32_t kReservoirStride = 16u;
    m_reservoirA = CreateStructuredBuffer(device, kReservoirStride, pixCount, L"ReservoirA");
    m_reservoirB = CreateStructuredBuffer(device, kReservoirStride, pixCount, L"ReservoirB");

    // ── 광원 리스트 버퍼 (최대 64개) ──────────────────────────
    constexpr uint32_t kLightStride = static_cast<uint32_t>(sizeof(LightData));
    constexpr uint32_t kMaxLights   = 64u;
    m_lightListBuf = CreateStructuredBuffer(device, kLightStride, kMaxLights, L"LightList");
    {
        D3D12_HEAP_PROPERTIES uploadHp{}; uploadHp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width     = static_cast<uint64_t>(kLightStride) * kMaxLights;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc = {1,0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(
            &uploadHp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_lightListUpload)));
        m_lightListUpload->SetName(L"LightListUpload");
    }

    // ── 디스크립터 등록 ─────────────────────────────────────

    auto MakeUAV_Tex = [&](ComPtr<ID3D12Resource>& res, DXGI_FORMAT fmt)
    {
        auto h = heap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; d.Format = fmt;
        device->CreateUnorderedAccessView(res.Get(), nullptr, &d, h.cpu);
        return h;
    };
    auto MakeSRV_Tex = [&](ComPtr<ID3D12Resource>& res, DXGI_FORMAT fmt)
    {
        auto h = heap.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format = fmt; d.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(res.Get(), &d, h.cpu);
        return h;
    };
    auto MakeUAV_Buf = [&](ComPtr<ID3D12Resource>& res, uint32_t stride, uint32_t count)
    {
        auto h = heap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        d.Format                     = DXGI_FORMAT_UNKNOWN;
        d.Buffer.NumElements         = count;
        d.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(res.Get(), nullptr, &d, h.cpu);
        return h;
    };
    auto MakeSRV_Buf = [&](ComPtr<ID3D12Resource>& res, uint32_t stride, uint32_t count)
    {
        auto h = heap.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format                     = DXGI_FORMAT_UNKNOWN;
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
    auto resA_UAV_handle = MakeUAV_Buf(m_reservoirA, kReservoirStride, pixCount);
    // 슬롯 12: UAV u7 – reservoir_prev (초기: B)
    auto resB_UAV_handle = MakeUAV_Buf(m_reservoirB, kReservoirStride, pixCount);
    // 슬롯 13: SRV t5 – lightList
    m_lightListSRV  = MakeSRV_Buf(m_lightListBuf, kLightStride, kMaxLights);
    // 슬롯 14: SRV t6 – gbuf_worldPos
    m_gbWorldPosSRV = MakeSRV_Tex(m_gbWorldPos, DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯 15: SRV t7 – gbuf_normal
    m_gbNormalSRV   = MakeSRV_Tex(m_gbNormal,   DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯 16: SRV t8 – gbuf_albedo
    m_gbAlbedoSRV   = MakeSRV_Tex(m_gbAlbedo,   DXGI_FORMAT_R8G8B8A8_UNORM);
    // 슬롯 17: SRV t9 – gbuf_matInfo
    m_gbMatInfoSRV  = MakeSRV_Tex(m_gbMatInfo,  DXGI_FORMAT_R32G32B32A32_FLOAT);
    // 슬롯 18: SRV t10 – reservoir_in (초기: A_SRV, CopyDescriptor로 동적)
    auto resA_SRV_handle = MakeSRV_Buf(m_reservoirA, kReservoirStride, pixCount);

    // 슬롯 19-22: 스테이징 (셰이더 비접근, CopyDescriptor 소스)
    auto stageA_UAV = MakeUAV_Buf(m_reservoirA, kReservoirStride, pixCount);
    auto stageB_UAV = MakeUAV_Buf(m_reservoirB, kReservoirStride, pixCount);
    auto stageA_SRV = MakeSRV_Buf(m_reservoirA, kReservoirStride, pixCount);
    auto stageB_SRV = MakeSRV_Buf(m_reservoirB, kReservoirStride, pixCount);

    // 동적 갱신 대상 슬롯 CPU 핸들 저장
    m_heapSlot11_cpu = resA_UAV_handle.cpu;   // slot 11 = u6 cur UAV (초기 A)
    m_heapSlot12_cpu = resB_UAV_handle.cpu;   // slot 12 = u7 prev UAV (초기 B)
    m_heapSlot18_cpu = resA_SRV_handle.cpu;   // slot 18 = t10 reservoir_in SRV (초기 A)

    // 스테이징 핸들 저장
    m_stageResA_UAV = stageA_UAV.cpu;
    m_stageResB_UAV = stageB_UAV.cpu;
    m_stageResA_SRV = stageA_SRV.cpu;
    m_stageResB_SRV = stageB_SRV.cpu;

    // 초기 상태: cur=A, prev=B, reservoir_in=A
    m_curIsA    = true;
    m_gbufState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_resAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_resBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

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
        desc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
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
    uint64_t       copySize     = static_cast<uint64_t>(kLightStride) * m_lightCount;

    void* mapped = nullptr;
    D3D12_RANGE rr{0,0};
    ThrowIfFailed(m_lightListUpload->Map(0, &rr, &mapped));
    std::memcpy(mapped, lights.data(), copySize);
    m_lightListUpload->Unmap(0, nullptr);

    // 현재 상태 → COPY_DEST → NON_PIXEL_SHADER_RESOURCE
    // (씬 전환 시 반복 호출 대응: m_lightListState로 현재 상태 추적)
    TransitionRes(cmdList, m_lightListBuf.Get(), m_lightListState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(m_lightListBuf.Get(), 0, m_lightListUpload.Get(), 0, copySize);
    TransitionRes(cmdList, m_lightListBuf.Get(), m_lightListState,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// ──────────────────────────────────────────────────────────────
//  UpdateCB
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::UpdateCB(const ReSTIRCB& cb)
{
    void* mapped = nullptr;
    D3D12_RANGE rr{0,0};
    ThrowIfFailed(m_restirCB->Map(0, &rr, &mapped));
    std::memcpy(mapped, &cb, sizeof(ReSTIRCB));
    m_restirCB->Unmap(0, nullptr);
}

// ──────────────────────────────────────────────────────────────
//  DispatchGBuffer  – [Pass 1] DXR primary ray → G-Buffer
//
//  진입: GBuffer 상태 불명 (첫 프레임=UAV, 이후=SRV)
//  퇴장: GBuffer = SRV (Initial/Temporal/Spatial/Shade가 t6-t9로 읽을 수 있게)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchGBuffer(ID3D12GraphicsCommandList4* cmdList,
                                  ID3D12StateObject*          gbufferPSO,
                                  const ShaderTable&          shaderTable,
                                  uint32_t w, uint32_t h)
{
    // GBuffer가 SRV 상태라면 UAV로 전환 (GBuffer 쓰기 전 필요)
    TransitionGBuf(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetPipelineState1(gbufferPSO);

    D3D12_DISPATCH_RAYS_DESC desc{};
    desc.RayGenerationShaderRecord = shaderTable.RayGenRange();
    desc.MissShaderTable           = shaderTable.MissRange();
    desc.HitGroupTable             = shaderTable.HitGroupRange();
    desc.Width  = w;
    desc.Height = h;
    desc.Depth  = 1;
    cmdList->DispatchRays(&desc);

    // GBuffer 쓰기 완료: UAV → SRV (Compute/Shade 패스가 t6-t9로 읽음)
    TransitionGBuf(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// ──────────────────────────────────────────────────────────────
//  DispatchInitial  – [Pass 2] RIS 후보 생성
//
//  진입: reservoir cur/prev 상태 불명 (이전 프레임 Shade가 SRV로 남길 수 있음)
//  퇴장: reservoir cur = UAV (Temporal이 이어서 씀)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchInitial(ID3D12GraphicsCommandList* cmdList,
                                  uint32_t w, uint32_t h)
{
    // 양쪽 reservoir를 UAV 상태로 복원
    // - cur: Initial이 쓰기 위해 UAV 필요
    // - prev: Temporal이 RWStructuredBuffer(u7)로 읽기 위해 UAV 필요
    TransitionRes(cmdList, m_reservoirA.Get(), m_resAState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionRes(cmdList, m_reservoirB.Get(), m_resBState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetPipelineState(m_psoInitial.Get());
    cmdList->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);

    // Initial 완료: cur UAV 배리어 (Temporal이 이어서 읽을 수 있게)
    auto* curRes = m_curIsA ? m_reservoirA.Get() : m_reservoirB.Get();
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = curRes;
    cmdList->ResourceBarrier(1, &b);
}

// ──────────────────────────────────────────────────────────────
//  DispatchTemporal  – [Pass 3] 시간적 재사용
//
//  진입: reservoir cur = UAV (Initial 결과), prev = UAV
//  퇴장: reservoir cur = UAV (in-place 갱신 완료)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchTemporal(ID3D12GraphicsCommandList* cmdList,
                                   uint32_t w, uint32_t h)
{
    cmdList->SetPipelineState(m_psoTemporal.Get());
    cmdList->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);

    // Temporal 완료: cur UAV 배리어
    auto* curRes = m_curIsA ? m_reservoirA.Get() : m_reservoirB.Get();
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = curRes;
    cmdList->ResourceBarrier(1, &b);
}

// ──────────────────────────────────────────────────────────────
//  DispatchSpatial  – [Pass 4] 공간 재사용 (ping-pong 2회)
//
//  Pass 0: cur(SRV t10 읽기) → prev(UAV u6 쓰기)
//  Pass 1: prev(SRV t10 읽기) → cur(UAV u6 쓰기)   ← 최종 결과가 cur에
//
//  진입: reservoir cur = UAV (Temporal 완료)
//  퇴장: reservoir cur = SRV (Shade가 t10으로 읽음), prev = UAV (복원)
//        slot18(t10) = cur_SRV
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::DispatchSpatial(ID3D12GraphicsCommandList* cmdList,
                                  uint32_t w, uint32_t h)
{
    uint32_t gx = (w + 7u) / 8u;
    uint32_t gy = (h + 7u) / 8u;

    ID3D12Resource* curRes  = m_curIsA ? m_reservoirA.Get() : m_reservoirB.Get();
    ID3D12Resource* prevRes = m_curIsA ? m_reservoirB.Get() : m_reservoirA.Get();
    D3D12_RESOURCE_STATES& curState  = m_curIsA ? m_resAState : m_resBState;
    D3D12_RESOURCE_STATES& prevState = m_curIsA ? m_resBState : m_resAState;
    D3D12_CPU_DESCRIPTOR_HANDLE curUAV  = m_curIsA ? m_stageResA_UAV : m_stageResB_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE prevUAV = m_curIsA ? m_stageResB_UAV : m_stageResA_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE curSRV  = m_curIsA ? m_stageResA_SRV : m_stageResB_SRV;
    D3D12_CPU_DESCRIPTOR_HANDLE prevSRV = m_curIsA ? m_stageResB_SRV : m_stageResA_SRV;

    // ── Pass 0: cur(SRV) → prev(UAV) ─────────────────────────────
    TransitionRes(cmdList, curRes, curState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CopyDesc(m_heapSlot18_cpu, curSRV);    // t10 = cur_SRV (읽기)
    CopyDesc(m_heapSlot11_cpu, prevUAV);   // u6  = prev_UAV (쓰기)

    cmdList->SetPipelineState(m_psoSpatial.Get());
    cmdList->Dispatch(gx, gy, 1);

    {   // pass 0 → pass 1 전환: prev UAV→SRV, cur SRV→UAV
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource   = prevRes;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource   = curRes;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(2, barriers);
        prevState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        curState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // ── Pass 1: prev(SRV) → cur(UAV) ─────────────────────────────
    CopyDesc(m_heapSlot18_cpu, prevSRV);   // t10 = prev_SRV (읽기)
    CopyDesc(m_heapSlot11_cpu, curUAV);    // u6  = cur_UAV  (쓰기)

    cmdList->SetPipelineState(m_psoSpatial.Get());
    cmdList->Dispatch(gx, gy, 1);

    // pass 1 완료: cur UAV→SRV (Shade 읽기용), prev SRV→UAV (다음 프레임 복원)
    {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource   = curRes;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource   = prevRes;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(2, barriers);
        curState  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        prevState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // t10 = cur_SRV (Shade가 읽을 최종 Reservoir), u6 = cur_UAV (SwapReservoirs 후 안전)
    CopyDesc(m_heapSlot18_cpu, curSRV);
    CopyDesc(m_heapSlot11_cpu, curUAV);
}

// ──────────────────────────────────────────────────────────────
//  DispatchShade  – [Pass 5] DXR 그림자 레이 + GGX BRDF → g_output
//
//  진입: GBuffer = SRV, reservoir cur = SRV (slot18=t10)
//  퇴장: reservoir cur = UAV (SwapReservoirs + 다음 프레임 Initial을 위해)
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

    // Shade 완료: cur reservoir SRV→UAV (다음 프레임 Initial 진입 조건)
    // SwapReservoirs 이전에 복원해야 slot12(u7=prev) 상태가 올바름
    ID3D12Resource*            curRes   = m_curIsA ? m_reservoirA.Get() : m_reservoirB.Get();
    D3D12_RESOURCE_STATES&     curState = m_curIsA ? m_resAState : m_resBState;
    D3D12_CPU_DESCRIPTOR_HANDLE curUAV  = m_curIsA ? m_stageResA_UAV : m_stageResB_UAV;

    TransitionRes(cmdList, curRes, curState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CopyDesc(m_heapSlot11_cpu, curUAV);   // slot11(u6) = cur_UAV 복원
}

// ──────────────────────────────────────────────────────────────
//  SwapReservoirs  – cur ↔ prev 포인터 교환 (GPU 이동 없음)
//  호출 시점: DispatchShade 완료 후 (reservoir cur = UAV 상태)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::SwapReservoirs()
{
    m_curIsA = !m_curIsA;

    // 교환 후 슬롯 갱신
    D3D12_CPU_DESCRIPTOR_HANDLE newCurUAV  = m_curIsA ? m_stageResA_UAV : m_stageResB_UAV;
    D3D12_CPU_DESCRIPTOR_HANDLE newPrevUAV = m_curIsA ? m_stageResB_UAV : m_stageResA_UAV;

    CopyDesc(m_heapSlot11_cpu, newCurUAV);   // slot11 = u6 = 새 cur UAV
    CopyDesc(m_heapSlot12_cpu, newPrevUAV);  // slot12 = u7 = 새 prev UAV
}

// ──────────────────────────────────────────────────────────────
//  ClearGBuffer  – 씬 전환 시 호출 (hitDist=-1로 배경 마킹)
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::ClearGBuffer(ID3D12GraphicsCommandList* cmdList)
{
    // ClearUnorderedAccessViewFloat 는 UAV 상태 필요
    TransitionGBuf(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const float clearMiss[4] = { 0.0f, 0.0f, 0.0f, -1.0f };  // w=-1: 배경
    const float clearZero[4] = { 0.0f, 0.0f, 0.0f,  0.0f };

    cmdList->ClearUnorderedAccessViewFloat(
        m_gbWorldPosUAV.gpu, m_gbWorldPosUAV.cpu,
        m_gbWorldPos.Get(), clearMiss, 0, nullptr);
    cmdList->ClearUnorderedAccessViewFloat(
        m_gbNormalUAV.gpu, m_gbNormalUAV.cpu,
        m_gbNormal.Get(), clearZero, 0, nullptr);
    cmdList->ClearUnorderedAccessViewFloat(
        m_gbMatInfoUAV.gpu, m_gbMatInfoUAV.cpu,
        m_gbMatInfo.Get(), clearZero, 0, nullptr);
}

// ──────────────────────────────────────────────────────────────
//  Shutdown
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::Shutdown()
{
    m_gbWorldPos = nullptr; m_gbNormal   = nullptr;
    m_gbAlbedo   = nullptr; m_gbMatInfo  = nullptr;
    m_reservoirA = nullptr; m_reservoirB = nullptr;
    m_lightListBuf = nullptr; m_lightListUpload = nullptr;
    m_restirCB   = nullptr;
    m_psoInitial = nullptr; m_psoTemporal = nullptr;
    m_psoSpatial = nullptr;
    m_device     = nullptr;
}

// ──────────────────────────────────────────────────────────────
//  Resize
// ──────────────────────────────────────────────────────────────
void ReSTIRPass::Resize(ID3D12Device5*  device,
                         DescriptorHeap& heap,
                         uint32_t        width,
                         uint32_t        height)
{
    // TODO: 해상도 변경 시 버퍼 재생성 (현재 미구현 – descriptor re-allocation 필요)
    std::println("[ReSTIRPass] Resize {}x{} (미구현)", width, height);
}

// ──────────────────────────────────────────────────────────────
//  내부 헬퍼: CreateUAVTexture / CreateStructuredBuffer
// ──────────────────────────────────────────────────────────────
ComPtr<ID3D12Resource> ReSTIRPass::CreateUAVTexture(ID3D12Device* device,
                                                     DXGI_FORMAT fmt,
                                                     uint32_t w, uint32_t h,
                                                     const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format           = fmt;
    rd.SampleDesc       = {1,0};
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
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = static_cast<uint64_t>(elementSize) * count;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc = {1,0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&res)));
    if (name) res->SetName(name);
    return res;
}
