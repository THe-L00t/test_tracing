#include "MotionVectorReusePass.h"
#include "ShaderCompile.h"
#include <print>

// ──────────────────────────────────────────────────────────────────
// 상수 버퍼 레이아웃 (256B 정렬)
// ──────────────────────────────────────────────────────────────────
struct ReuseCB
{
    uint32_t width;
    uint32_t height;
    float    tierLow;
    float    tierHigh;

    float    depthThreshold;
    float    normalCosThreshold;
    uint32_t firstFrame;
    float    motionLenMaxPx;

    float    _pad[56];
};
static_assert(sizeof(ReuseCB) == 256, "ReuseCB must be 256B");

// ──────────────────────────────────────────────────────────────────
// 힙 슬롯 레이아웃 (9 슬롯)
//   0..6 : SRV (currDepth, currNormal, motionVec, prevDepth, prevNormal, prevRadiance, importance)
//   7    : UAV reusedRadiance
//   8    : UAV reuseValidMask
// ──────────────────────────────────────────────────────────────────
constexpr uint32_t SLOT_SRV_BASE        = 0;
constexpr uint32_t SLOT_SRV_COUNT       = 7;
constexpr uint32_t SLOT_UAV_REUSED      = 7;
constexpr uint32_t SLOT_UAV_VALID_MASK  = 8;
constexpr uint32_t TOTAL_SLOTS          = 9;

// ──────────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────────
static ComPtr<ID3D12Resource> CreateUploadCB(ID3D12Device* device, size_t size, void** mapped)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size;
    rd.Height           = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = {1, 0};
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)));
    D3D12_RANGE nr{0, 0};
    ThrowIfFailed(res->Map(0, &nr, mapped));
    return res;
}

static ComPtr<ID3D12Resource> CreateUAVTex2D(ID3D12Device* device,
    uint32_t w, uint32_t h, DXGI_FORMAT fmt, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;  rd.Height = h;
    rd.DepthOrArraySize = 1;  rd.MipLevels = 1;
    rd.Format           = fmt; rd.SampleDesc = {1, 0};
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)));
    res->SetName(name);
    return res;
}

// ──────────────────────────────────────────────────────────────────
// Init
// ──────────────────────────────────────────────────────────────────
bool MotionVectorReusePass::Init(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_device = device;
    m_width  = width;
    m_height = height;

    // ── 1. 출력 리소스 ───────────────────────────────────────────
    m_reusedRadiance = CreateUAVTex2D(device, width, height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, L"HPAR_ReusedRadiance");
    m_reuseValidMask = CreateUAVTex2D(device, width, height,
                                       DXGI_FORMAT_R8_UNORM,           L"HPAR_ReuseValidMask");

    // ── 2. Root signature: SRV table + UAV table + CBV + static sampler ──
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors                    = SLOT_SRV_COUNT;
        srvRange.BaseShaderRegister                = 0;
        srvRange.RegisterSpace                     = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors                    = 2;
        uavRange.BaseShaderRegister                = 0;
        uavRange.RegisterSpace                     = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor.ShaderRegister           = 0;
        params[2].Descriptor.RegisterSpace            = 0;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU       = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxAnisotropy  = 1;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MinLOD         = 0.0f; samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0; samp.RegisterSpace = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = 3;
        rsDesc.pParameters       = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers   = &samp;

        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr))
        {
            std::println("[MVReuse] RootSig 직렬화 실패: {}",
                         err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_rootSig)));
        m_rootSig->SetName(L"HPAR_RS_MotionVectorReuse");
    }

    // ── 3. PSO ─────────────────────────────────────────────────
    {
        auto blob = CompileComputeCS(L"shaders/MotionVectorReuse.hlsl");
        if (!blob) { std::println("[MVReuse] 셰이더 컴파일 실패"); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature       = m_rootSig.Get();
        pd.CS.pShaderBytecode   = blob->GetBufferPointer();
        pd.CS.BytecodeLength    = blob->GetBufferSize();
        ThrowIfFailed(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso)));
        m_pso->SetName(L"HPAR_PSO_MotionVectorReuse");
    }

    // ── 4. Descriptor heap (9 슬롯) ─────────────────────────────
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = TOTAL_SLOTS;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_descHeap)));
        m_descIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // ── 5. UAV 슬롯 정적 채움 (출력 자원) ──────────────────────
    auto cpu = m_descHeap->GetCPUDescriptorHandleForHeapStart();
    auto slotCpu = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += (SIZE_T)m_descIncSize * slot;
        return h;
    };
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_reusedRadiance.Get(), nullptr, &uav,
                                          slotCpu(SLOT_UAV_REUSED));
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R8_UNORM;
        device->CreateUnorderedAccessView(m_reuseValidMask.Get(), nullptr, &uav,
                                          slotCpu(SLOT_UAV_VALID_MASK));
    }

    // ── 6. 상수 버퍼 ────────────────────────────────────────────
    m_cb = CreateUploadCB(device, sizeof(ReuseCB), &m_cbMapped);

    std::println("[MVReuse] Init 완료 — {}x{}, Phase 7 motion vector reuse (Tier 2 전용)",
                 width, height);
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Shutdown
// ──────────────────────────────────────────────────────────────────
void MotionVectorReusePass::Shutdown()
{
    if (m_cb && m_cbMapped)
    {
        D3D12_RANGE nw{0, 0};
        m_cb->Unmap(0, &nw);
        m_cbMapped = nullptr;
    }
}

// ──────────────────────────────────────────────────────────────────
// Apply
// ──────────────────────────────────────────────────────────────────
void MotionVectorReusePass::Apply(ID3D12GraphicsCommandList* cmdList,
                                   ID3D12Resource* currDepth,
                                   ID3D12Resource* currNormal,
                                   ID3D12Resource* motionVec,
                                   ID3D12Resource* prevDepth,
                                   ID3D12Resource* prevNormal,
                                   ID3D12Resource* prevRadiance,
                                   ID3D12Resource* importance)
{
    // ── 1. CB 업데이트 ───────────────────────────────────────────
    ReuseCB cb{};
    cb.width              = m_width;
    cb.height             = m_height;
    cb.tierLow            = m_tierLow;
    cb.tierHigh           = m_tierHigh;
    cb.depthThreshold     = m_depthThreshold;
    cb.normalCosThreshold = m_normalCosThreshold;
    cb.firstFrame         = m_firstFrameFlag ? 1u : 0u;
    cb.motionLenMaxPx     = m_motionLenMaxPx;
    std::memcpy(m_cbMapped, &cb, sizeof(ReuseCB));

    // ── 2. 매 frame SRV 채우기 ───────────────────────────────────
    auto cpu = m_descHeap->GetCPUDescriptorHandleForHeapStart();
    auto slotCpu = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += (SIZE_T)m_descIncSize * slot;
        return h;
    };
    auto writeSrv = [&](uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format              = fmt;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(res, &srv, slotCpu(slot));
    };
    writeSrv(0, currDepth,    DXGI_FORMAT_R32_FLOAT);
    writeSrv(1, currNormal,   DXGI_FORMAT_R16G16B16A16_FLOAT);
    writeSrv(2, motionVec,    DXGI_FORMAT_R16G16_FLOAT);
    writeSrv(3, prevDepth,    DXGI_FORMAT_R32_FLOAT);
    writeSrv(4, prevNormal,   DXGI_FORMAT_R16G16B16A16_FLOAT);
    // m_radianceHistory 자원은 m_dlss.RenderAccum 과 동일 포맷 (RGBA32F) 이어야 CopyResource 가능.
    // SRV 도 반드시 자원 포맷과 일치 — RGBA16F SRV 지정 시 비트 깊이 불일치로 device removed.
    writeSrv(5, prevRadiance, DXGI_FORMAT_R32G32B32A32_FLOAT);
    writeSrv(6, importance,   DXGI_FORMAT_R16_FLOAT);

    // ── 3. 입력 자원들 transition ───────────────────────────────
    //   curr*/importance: UAV → SRV
    //   prev*: COMMON → SRV (CopyResource 직후 COMMON 상태)
    auto makeTrans = [](ID3D12Resource* res,
                        D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return b;
    };
    D3D12_RESOURCE_BARRIER toSRV[7]{};
    toSRV[0] = makeTrans(currDepth,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[1] = makeTrans(currNormal,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[2] = makeTrans(motionVec,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[3] = makeTrans(prevDepth,    D3D12_RESOURCE_STATE_COMMON,           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[4] = makeTrans(prevNormal,   D3D12_RESOURCE_STATE_COMMON,           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[5] = makeTrans(prevRadiance, D3D12_RESOURCE_STATE_COMMON,           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[6] = makeTrans(importance,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(7, toSRV);

    // ── 4. Dispatch ─────────────────────────────────────────────
    ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(m_rootSig.Get());

    auto gpuBase = m_descHeap->GetGPUDescriptorHandleForHeapStart();
    auto slotGpu = [&](uint32_t slot)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = gpuBase;
        h.ptr += (UINT64)m_descIncSize * slot;
        return h;
    };
    cmdList->SetComputeRootDescriptorTable(0, slotGpu(SLOT_SRV_BASE));
    cmdList->SetComputeRootDescriptorTable(1, slotGpu(SLOT_UAV_REUSED));
    cmdList->SetComputeRootConstantBufferView(2, m_cb->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_pso.Get());

    const uint32_t gx = (m_width  + 7) / 8;
    const uint32_t gy = (m_height + 7) / 8;
    cmdList->Dispatch(gx, gy, 1);

    // ── 5. 복원 ─────────────────────────────────────────────────
    //   curr*/importance: SRV → UAV (다음 패스/RT 셰이더 위해 복원)
    //   prev*: SRV → COMMON (다음 frame CopyResource 위해 COMMON 으로 돌림)
    D3D12_RESOURCE_BARRIER restore[7]{};
    restore[0] = makeTrans(currDepth,    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    restore[1] = makeTrans(currNormal,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    restore[2] = makeTrans(motionVec,    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    restore[3] = makeTrans(prevDepth,    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    restore[4] = makeTrans(prevNormal,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    restore[5] = makeTrans(prevRadiance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    restore[6] = makeTrans(importance,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(7, restore);

    // ── 6. first-frame 해제 ────────────────────────────────────
    m_firstFrameFlag = false;
}
