#include "NRDDenoiser.h"
#include "NRD.h"
#include <print>
#include <cstring>

// ===================================================================
// NRD Format → DXGI_FORMAT 변환
// ===================================================================
static DXGI_FORMAT NrdToDXGI(nrd::Format f)
{
    using F = nrd::Format;
    switch (f)
    {
        case F::R8_UNORM:           return DXGI_FORMAT_R8_UNORM;
        case F::R8_SNORM:           return DXGI_FORMAT_R8_SNORM;
        case F::R8_UINT:            return DXGI_FORMAT_R8_UINT;
        case F::R8_SINT:            return DXGI_FORMAT_R8_SINT;
        case F::RG8_UNORM:          return DXGI_FORMAT_R8G8_UNORM;
        case F::RG8_SNORM:          return DXGI_FORMAT_R8G8_SNORM;
        case F::RG8_UINT:           return DXGI_FORMAT_R8G8_UINT;
        case F::RG8_SINT:           return DXGI_FORMAT_R8G8_SINT;
        case F::RGBA8_UNORM:        return DXGI_FORMAT_R8G8B8A8_UNORM;
        case F::RGBA8_SNORM:        return DXGI_FORMAT_R8G8B8A8_SNORM;
        case F::RGBA8_UINT:         return DXGI_FORMAT_R8G8B8A8_UINT;
        case F::RGBA8_SINT:         return DXGI_FORMAT_R8G8B8A8_SINT;
        case F::RGBA8_SRGB:         return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case F::R16_UNORM:          return DXGI_FORMAT_R16_UNORM;
        case F::R16_SNORM:          return DXGI_FORMAT_R16_SNORM;
        case F::R16_UINT:           return DXGI_FORMAT_R16_UINT;
        case F::R16_SINT:           return DXGI_FORMAT_R16_SINT;
        case F::R16_SFLOAT:         return DXGI_FORMAT_R16_FLOAT;
        case F::RG16_UNORM:         return DXGI_FORMAT_R16G16_UNORM;
        case F::RG16_SNORM:         return DXGI_FORMAT_R16G16_SNORM;
        case F::RG16_UINT:          return DXGI_FORMAT_R16G16_UINT;
        case F::RG16_SINT:          return DXGI_FORMAT_R16G16_SINT;
        case F::RG16_SFLOAT:        return DXGI_FORMAT_R16G16_FLOAT;
        case F::RGBA16_UNORM:       return DXGI_FORMAT_R16G16B16A16_UNORM;
        case F::RGBA16_SNORM:       return DXGI_FORMAT_R16G16B16A16_SNORM;
        case F::RGBA16_UINT:        return DXGI_FORMAT_R16G16B16A16_UINT;
        case F::RGBA16_SINT:        return DXGI_FORMAT_R16G16B16A16_SINT;
        case F::RGBA16_SFLOAT:      return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case F::R32_UINT:           return DXGI_FORMAT_R32_UINT;
        case F::R32_SINT:           return DXGI_FORMAT_R32_SINT;
        case F::R32_SFLOAT:         return DXGI_FORMAT_R32_FLOAT;
        case F::RG32_UINT:          return DXGI_FORMAT_R32G32_UINT;
        case F::RG32_SINT:          return DXGI_FORMAT_R32G32_SINT;
        case F::RG32_SFLOAT:        return DXGI_FORMAT_R32G32_FLOAT;
        case F::RGBA32_UINT:        return DXGI_FORMAT_R32G32B32A32_UINT;
        case F::RGBA32_SINT:        return DXGI_FORMAT_R32G32B32A32_SINT;
        case F::RGBA32_SFLOAT:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case F::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case F::R10_G10_B10_A2_UINT:  return DXGI_FORMAT_R10G10B10A2_UINT;
        case F::R11_G11_B10_UFLOAT:   return DXGI_FORMAT_R11G11B10_FLOAT;
        case F::R9_G9_B9_E5_UFLOAT:   return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// ===================================================================
// UAV/SRV-able 텍스처 생성 헬퍼
// ===================================================================
static ComPtr<ID3D12Resource> CreateNRDTex(ID3D12Device* dev,
    uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format           = fmt; rd.SampleDesc = {1, 0};
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

// ===================================================================
// Init
// ===================================================================
bool NRDDenoiser::Init(ID3D12Device5* device, uint32_t renderW, uint32_t renderH)
{
    m_device  = device;
    m_renderW = renderW;
    m_renderH = renderH;

    // ── 1. NRD Instance 생성 (RELAX_DIFFUSE_SPECULAR) ────────────
    const nrd::LibraryDesc* libDesc = nrd::GetLibraryDesc();
    std::println("[NRD] Library v{}.{}.{} (normalEnc={}, roughnessEnc={})",
                 libDesc->versionMajor, libDesc->versionMinor, libDesc->versionBuild,
                 (uint32_t)libDesc->normalEncoding, (uint32_t)libDesc->roughnessEncoding);

    nrd::DenoiserDesc denoisers[] = {
        { 0u, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR },
    };
    nrd::InstanceCreationDesc icd{};
    icd.denoisers    = denoisers;
    icd.denoisersNum = 1;

    nrd::Result res = nrd::CreateInstance(icd, m_instance);
    if (res != nrd::Result::SUCCESS)
    {
        std::println("[NRD] CreateInstance 실패: {}", (uint32_t)res);
        return false;
    }

    const nrd::InstanceDesc* instDesc = nrd::GetInstanceDesc(*m_instance);
    std::println("[NRD] Instance: pipelines={}, permanent={}, transient={}, cbMaxSize={}",
                 instDesc->pipelinesNum,
                 instDesc->permanentPoolSize, instDesc->transientPoolSize,
                 instDesc->constantBufferMaxDataSize);

    m_perSetTexturesMaxNum        = instDesc->descriptorPoolDesc.perSetTexturesMaxNum;
    m_perSetStorageTexturesMaxNum = instDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum;
    m_constantBufferMaxDataSize   = instDesc->constantBufferMaxDataSize;

    // ── 2. 텍스처 풀 할당 ─────────────────────────────────────────
    // permanent: NRD 가 history 로 사용 (디노이저별 영구 보관)
    // transient: 매 프레임 임시 (NRD 내부에서 alias 가능하지만 안전하게 별도 alloc)
    m_permanentTextures.reserve(instDesc->permanentPoolSize);
    for (uint32_t i = 0; i < instDesc->permanentPoolSize; ++i)
    {
        const auto& td = instDesc->permanentPool[i];
        uint32_t w = renderW / td.downsampleFactor;
        uint32_t h = renderH / td.downsampleFactor;
        DXGI_FORMAT dxf = NrdToDXGI(td.format);
        m_permanentTextures.push_back(CreateNRDTex(device, w, h, dxf));
        wchar_t name[64];
        swprintf_s(name, L"NRD_Permanent_%u", i);
        m_permanentTextures.back()->SetName(name);
    }
    m_transientTextures.reserve(instDesc->transientPoolSize);
    for (uint32_t i = 0; i < instDesc->transientPoolSize; ++i)
    {
        const auto& td = instDesc->transientPool[i];
        uint32_t w = renderW / td.downsampleFactor;
        uint32_t h = renderH / td.downsampleFactor;
        DXGI_FORMAT dxf = NrdToDXGI(td.format);
        m_transientTextures.push_back(CreateNRDTex(device, w, h, dxf));
        wchar_t name[64];
        swprintf_s(name, L"NRD_Transient_%u", i);
        m_transientTextures.back()->SetName(name);
    }

    // 출력 텍스처 (RELAX_DIFFUSE_SPECULAR 의 OUT_DIFF_RADIANCE_HITDIST / OUT_SPEC_*)
    //   R11G11B10F+ — 권장 포맷. 우리는 RGBA16F 로 동일 분포 확보 후 .w 에 hitDist.
    m_outDiffRadHitDist = CreateNRDTex(device, renderW, renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_outSpecRadHitDist = CreateNRDTex(device, renderW, renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_outDiffRadHitDist->SetName(L"NRD_OUT_DiffRadHitDist");
    m_outSpecRadHitDist->SetName(L"NRD_OUT_SpecRadHitDist");

    // ── 3. Root Signature (모든 NRD 파이프라인 공통) ──────────────
    //   InstanceDesc 가 정한 register/space 사용:
    //     - constantBufferRegisterIndex (b0), constantBufferAndSamplersSpaceIndex (space1)
    //     - samplersBaseRegisterIndex (s0..), space1
    //     - resourcesBaseRegisterIndex (t0.., u0..), resourcesSpaceIndex (space0)
    const uint32_t cbReg     = instDesc->constantBufferRegisterIndex;
    const uint32_t cbSpace   = instDesc->constantBufferAndSamplersSpaceIndex;
    const uint32_t samReg    = instDesc->samplersBaseRegisterIndex;
    const uint32_t resReg    = instDesc->resourcesBaseRegisterIndex;
    const uint32_t resSpace  = instDesc->resourcesSpaceIndex;

    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = m_perSetTexturesMaxNum;
    ranges[0].BaseShaderRegister                = resReg;
    ranges[0].RegisterSpace                     = resSpace;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[0].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE
                                                | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = m_perSetStorageTexturesMaxNum;
    ranges[1].BaseShaderRegister                = resReg;
    ranges[1].RegisterSpace                     = resSpace;
    ranges[1].OffsetInDescriptorsFromTableStart = m_perSetTexturesMaxNum;
    ranges[1].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE
                                                | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    D3D12_ROOT_PARAMETER1 params[2]{};
    // param 0: 디스크립터 테이블 (SRV+UAV 하나의 테이블, NRD 가 단일 space)
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges   = ranges;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    // param 1: root CBV
    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = cbReg;
    params[1].Descriptor.RegisterSpace  = cbSpace;
    params[1].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Static samplers: NEAREST_CLAMP (s0), LINEAR_CLAMP (s1)
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MipLODBias    = 0.0f;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[i].BorderColor   = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samplers[i].MinLOD = 0.0f; samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = samReg + i;
        samplers[i].RegisterSpace  = cbSpace;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;   // NEAREST_CLAMP
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;  // LINEAR_CLAMP

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.Version                  = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsDesc.Desc_1_1.NumParameters   = 2;
    rsDesc.Desc_1_1.pParameters     = params;
    rsDesc.Desc_1_1.NumStaticSamplers = 2;
    rsDesc.Desc_1_1.pStaticSamplers   = samplers;
    rsDesc.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rsDesc, &sig, &err);
    if (FAILED(hr))
    {
        std::println("[NRD] RootSig serialize 실패: {}", err ? (const char*)err->GetBufferPointer() : "?");
        nrd::DestroyInstance(*m_instance);
        m_instance = nullptr;
        return false;
    }
    ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                              IID_PPV_ARGS(&m_rootSig)));
    m_rootSig->SetName(L"NRD_RootSig");

    // ── 4. 컴퓨트 PSO 생성 (파이프라인 개수만큼) ─────────────────
    m_pipelines.reserve(instDesc->pipelinesNum);
    for (uint32_t i = 0; i < instDesc->pipelinesNum; ++i)
    {
        const auto& pd = instDesc->pipelines[i];
        const auto& dxil = pd.computeShaderDXIL;
        if (!dxil.bytecode || dxil.size == 0)
        {
            std::println("[NRD] Pipeline {} DXIL 바이트코드 없음 — NRD 빌드 시 NRD_EMBEDS_DXIL_SHADERS=ON 확인", i);
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.CS.pShaderBytecode = dxil.bytecode;
        psoDesc.CS.BytecodeLength  = (SIZE_T)dxil.size;

        ComPtr<ID3D12PipelineState> pso;
        hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
        if (FAILED(hr))
        {
            std::println("[NRD] Pipeline {} PSO 생성 실패 (hr=0x{:08X}): {}", i, (uint32_t)hr, pd.shaderIdentifier);
            return false;
        }
        wchar_t name[128];
        swprintf_s(name, L"NRD_PSO_%u", i);
        pso->SetName(name);
        m_pipelines.push_back(std::move(pso));
    }

    // ── 5. 디스크립터 힙 (NRD 전용) ───────────────────────────────
    //   매 dispatch 가 perSetTextures + perSetStorageTextures 만큼 슬롯 사용.
    //   파이프라인 수만큼 dispatch 예상 + 여유 2배 → ring buffer 식 재사용
    const uint32_t perDispatch = m_perSetTexturesMaxNum + m_perSetStorageTexturesMaxNum;
    m_descriptorHeapCapacity = perDispatch * instDesc->pipelinesNum * 2;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = m_descriptorHeapCapacity;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap)));
    m_descriptorHeap->SetName(L"NRD_DescriptorHeap");
    m_descriptorIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ── 6. 상수 버퍼 (upload heap, ring) ──────────────────────────
    //   매 dispatch 마다 다른 256B-aligned offset 에 CB 데이터 쓰기.
    //   파이프라인 수만큼 dispatch 예상 + 256B 정렬 + 여유 2배.
    const uint32_t cbAlignedSize = (m_constantBufferMaxDataSize + 255) & ~255u;
    m_constantBufferCapacity = cbAlignedSize * instDesc->pipelinesNum * 2;

    D3D12_HEAP_PROPERTIES hpUpload{}; hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width            = m_constantBufferCapacity;
    cbDesc.Height           = 1;
    cbDesc.DepthOrArraySize = 1; cbDesc.MipLevels = 1;
    cbDesc.Format           = DXGI_FORMAT_UNKNOWN; cbDesc.SampleDesc = {1,0};
    cbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device->CreateCommittedResource(&hpUpload, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)));
    m_constantBuffer->SetName(L"NRD_ConstantBuffer");
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    ThrowIfFailed(m_constantBuffer->Map(0, &noRead, &mapped));
    m_constantBufferMapped = (uint8_t*)mapped;

    m_available = true;
    std::println("[NRD] Init 완료 — {}x{}, {} 파이프라인 PSO, 디스크립터 힙 {} 슬롯",
                 renderW, renderH, instDesc->pipelinesNum, m_descriptorHeapCapacity);
    return true;
}

// ===================================================================
// Shutdown
// ===================================================================
void NRDDenoiser::Shutdown()
{
    if (m_constantBuffer && m_constantBufferMapped)
    {
        D3D12_RANGE noWrite{0, 0};
        m_constantBuffer->Unmap(0, &noWrite);
        m_constantBufferMapped = nullptr;
    }
    if (m_instance)
    {
        nrd::DestroyInstance(*m_instance);
        m_instance = nullptr;
    }
    m_available = false;
}

// ===================================================================
// Denoise — per-frame: SetCommonSettings → GetComputeDispatches → execute
// ===================================================================
void NRDDenoiser::Denoise(ID3D12GraphicsCommandList* cmdList,
                          const DenoiseInputs& inputs,
                          const FrameSettings& settings)
{
    if (!m_available) return;

    // ── 1. CommonSettings 설정 ─────────────────────────────────────
    nrd::CommonSettings cs{};
    std::memcpy(cs.viewToClipMatrix,         settings.viewToClipMatrix,         sizeof(float) * 16);
    std::memcpy(cs.viewToClipMatrixPrev,     settings.viewToClipMatrixPrev,     sizeof(float) * 16);
    std::memcpy(cs.worldToViewMatrix,        settings.worldToViewMatrix,        sizeof(float) * 16);
    std::memcpy(cs.worldToViewMatrixPrev,    settings.worldToViewMatrixPrev,    sizeof(float) * 16);
    cs.cameraJitter[0]                     = settings.cameraJitter[0];
    cs.cameraJitter[1]                     = settings.cameraJitter[1];
    cs.cameraJitterPrev[0]                 = settings.cameraJitterPrev[0];
    cs.cameraJitterPrev[1]                 = settings.cameraJitterPrev[1];
    cs.resourceSize[0]                     = (uint16_t)m_renderW;
    cs.resourceSize[1]                     = (uint16_t)m_renderH;
    cs.resourceSizePrev[0]                 = (uint16_t)m_renderW;
    cs.resourceSizePrev[1]                 = (uint16_t)m_renderH;
    cs.rectSize[0]                         = (uint16_t)m_renderW;
    cs.rectSize[1]                         = (uint16_t)m_renderH;
    cs.rectSizePrev[0]                     = (uint16_t)m_renderW;
    cs.rectSizePrev[1]                     = (uint16_t)m_renderH;
    cs.frameIndex                          = settings.frameIndex;
    cs.accumulationMode                    = settings.accumulationRestart
                                              ? nrd::AccumulationMode::RESTART
                                              : nrd::AccumulationMode::CONTINUE;
    cs.isMotionVectorInWorldSpace          = false;

    nrd::Result r = nrd::SetCommonSettings(*m_instance, cs);
    if (r != nrd::Result::SUCCESS)
    {
        std::println("[NRD] SetCommonSettings 실패: {}", (uint32_t)r);
        return;
    }

    // ── 2. RELAX_DIFFUSE_SPECULAR 설정 (기본값 사용) ──────────────
    nrd::RelaxSettings rs{};
    nrd::SetDenoiserSettings(*m_instance, 0u, &rs);

    // ── 3. Compute dispatch 목록 받기 ──────────────────────────────
    const nrd::Identifier ids[] = { 0u };
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchesNum = 0;
    r = nrd::GetComputeDispatches(*m_instance, ids, 1, dispatches, dispatchesNum);
    if (r != nrd::Result::SUCCESS)
    {
        std::println("[NRD] GetComputeDispatches 실패: {}", (uint32_t)r);
        return;
    }

    // ── 4. 디스패치 실행 ──────────────────────────────────────────
    // 매 dispatch:
    //   (a) CB 데이터를 upload heap ring 에 쓰고 root CBV 바인딩
    //   (b) 디스크립터 힙에 SRV/UAV 슬롯 채우고 root table 바인딩
    //   (c) PSO 바인딩 + Dispatch
    //   (d) 다음 dispatch 를 위한 UAV barrier (출력 텍스처에 대해)
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    auto getInputResource = [&](nrd::ResourceType t) -> ID3D12Resource*
    {
        using R = nrd::ResourceType;
        switch (t)
        {
            case R::IN_DIFF_RADIANCE_HITDIST: return inputs.diffRadianceHitDist;
            case R::IN_SPEC_RADIANCE_HITDIST: return inputs.specRadianceHitDist;
            case R::IN_NORMAL_ROUGHNESS:      return inputs.normalRoughness;
            case R::IN_VIEWZ:                 return inputs.viewZ;
            case R::IN_MV:                    return inputs.motionVec;
            case R::OUT_DIFF_RADIANCE_HITDIST: return m_outDiffRadHitDist.Get();
            case R::OUT_SPEC_RADIANCE_HITDIST: return m_outSpecRadHitDist.Get();
            default: return nullptr;
        }
    };

    auto getPoolResource = [&](nrd::ResourceType pool, uint16_t idx) -> ID3D12Resource*
    {
        if (pool == nrd::ResourceType::PERMANENT_POOL)
            return (idx < m_permanentTextures.size()) ? m_permanentTextures[idx].Get() : nullptr;
        if (pool == nrd::ResourceType::TRANSIENT_POOL)
            return (idx < m_transientTextures.size()) ? m_transientTextures[idx].Get() : nullptr;
        return nullptr;
    };

    // 매 프레임 ring 재시작
    m_descriptorHeapNext  = 0;
    m_constantBufferNext  = 0;

    const uint32_t cbAlignedSize = (m_constantBufferMaxDataSize + 255) & ~255u;
    const D3D12_GPU_VIRTUAL_ADDRESS cbBase = m_constantBuffer->GetGPUVirtualAddress();
    D3D12_CPU_DESCRIPTOR_HANDLE heapCpuBase = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE heapGpuBase = m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();

    for (uint32_t d = 0; d < dispatchesNum; ++d)
    {
        const auto& dd = dispatches[d];

        // (a) CB 업로드 + 루트 CBV
        if (dd.constantBufferDataSize > 0 && dd.constantBufferData)
        {
            std::memcpy(m_constantBufferMapped + m_constantBufferNext,
                        dd.constantBufferData, dd.constantBufferDataSize);
            cmdList->SetComputeRootConstantBufferView(1, cbBase + m_constantBufferNext);
            m_constantBufferNext += cbAlignedSize;
            if (m_constantBufferNext + cbAlignedSize > m_constantBufferCapacity)
                m_constantBufferNext = 0;  // ring wrap
        }

        // (b) 디스크립터 슬롯 채우기 — SRV 먼저, UAV 다음 (range 순서와 일치)
        const uint32_t slotsForDispatch = m_perSetTexturesMaxNum + m_perSetStorageTexturesMaxNum;
        if (m_descriptorHeapNext + slotsForDispatch > m_descriptorHeapCapacity)
            m_descriptorHeapNext = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE tableCpu = heapCpuBase;
        D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = heapGpuBase;
        tableCpu.ptr += (SIZE_T)m_descriptorHeapNext * m_descriptorIncSize;
        tableGpu.ptr += (UINT64) m_descriptorHeapNext * m_descriptorIncSize;

        uint32_t srvSlot = 0;
        uint32_t uavSlot = 0;
        for (uint32_t ri = 0; ri < dd.resourcesNum; ++ri)
        {
            const auto& rd = dd.resources[ri];
            ID3D12Resource* tex = nullptr;
            if (rd.type == nrd::ResourceType::PERMANENT_POOL ||
                rd.type == nrd::ResourceType::TRANSIENT_POOL)
                tex = getPoolResource(rd.type, rd.indexInPool);
            else
                tex = getInputResource(rd.type);

            // 슬롯 위치 계산: SRV 영역(0..perSetTextures-1) 또는 UAV 영역
            uint32_t slotIndexInTable;
            if (rd.descriptorType == nrd::DescriptorType::TEXTURE)
            {
                slotIndexInTable = srvSlot++;
            }
            else // STORAGE_TEXTURE
            {
                slotIndexInTable = m_perSetTexturesMaxNum + uavSlot++;
            }

            D3D12_CPU_DESCRIPTOR_HANDLE slotCpu = tableCpu;
            slotCpu.ptr += (SIZE_T)slotIndexInTable * m_descriptorIncSize;

            if (!tex)
            {
                // null 바인딩 — null descriptor
                if (rd.descriptorType == nrd::DescriptorType::TEXTURE)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
                    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    nullSrv.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
                    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullSrv.Texture2D.MipLevels = 1;
                    m_device->CreateShaderResourceView(nullptr, &nullSrv, slotCpu);
                }
                else
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
                    nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    nullUav.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
                    m_device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, slotCpu);
                }
                continue;
            }

            D3D12_RESOURCE_DESC td = tex->GetDesc();
            if (rd.descriptorType == nrd::DescriptorType::TEXTURE)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Format              = td.Format;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                m_device->CreateShaderResourceView(tex, &srv, slotCpu);
            }
            else
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Format        = td.Format;
                m_device->CreateUnorderedAccessView(tex, nullptr, &uav, slotCpu);
            }
        }

        cmdList->SetComputeRootDescriptorTable(0, tableGpu);
        m_descriptorHeapNext += slotsForDispatch;

        // (c) PSO + Dispatch
        cmdList->SetPipelineState(m_pipelines[dd.pipelineIndex].Get());
        cmdList->Dispatch(dd.gridWidth, dd.gridHeight, 1);

        // (d) UAV barrier (다음 dispatch 가 이 dispatch 의 UAV 출력을 SRV 로 읽을 수 있음)
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;  // 모든 UAV 보호 (보수적)
        cmdList->ResourceBarrier(1, &uavBarrier);
    }
}
