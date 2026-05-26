#include "ImportanceMapPass.h"
#include "ShaderCompile.h"
#include <print>

// ──────────────────────────────────────────────────────────────────
// 상수 버퍼 레이아웃 (256B 정렬)
// ──────────────────────────────────────────────────────────────────
struct HistoCB
{
    uint32_t width;
    uint32_t height;
    float    eMax;
    float    sMax;
    float    mMax;
    float    pad[3];
    float    fill[56];
};
static_assert(sizeof(HistoCB) == 256, "HistoCB 256B");

struct ReduceCB
{
    float eMax;
    float sMax;
    float mMax;
    float pad;
    float fill[60];
};
static_assert(sizeof(ReduceCB) == 256, "ReduceCB 256B");

struct ImportanceCB
{
    uint32_t width;
    uint32_t height;
    float    weightE;
    float    weightD;
    float    weightS;
    float    weightM;
    float    weightV;
    int32_t  metricFilter;  // 0=All, 1=E, 2=D, 3=S, 4=M
    float    fill[56];
};
static_assert(sizeof(ImportanceCB) == 256, "ImportanceCB 256B");

struct EmaCB
{
    uint32_t width;
    uint32_t height;
    float    alpha;
    float    motionThreshold;
    uint32_t firstFrame;
    uint32_t pad[3];
    float    fill[56];
};
static_assert(sizeof(EmaCB) == 256, "EmaCB 256B");

// ──────────────────────────────────────────────────────────────────
// 힙 슬롯 레이아웃 (13 슬롯, Phase 3)
//   0..4 : SRV depth/accum/normals/motionVec/specAlbedo  (Phase 2 입력)
//   5    : SRV percentile                               (Phase 2)
//   6    : UAV histogram                                (Phase 2)
//   7    : UAV percentile                               (Phase 2)
//   8    : UAV importanceRaw                            (Phase 2 output, was 'importance')
//   9    : SRV importanceRaw                            (Phase 3 EMA t0)
//  10    : SRV smooth-history                           (Phase 3 EMA t1, dynamic per-frame)
//  11    : SRV motionVec dup for EMA                    (Phase 3 EMA t2, contiguous SRV table)
//  12    : UAV smooth-output                            (Phase 3 EMA u0, dynamic per-frame)
// ──────────────────────────────────────────────────────────────────
constexpr uint32_t SLOT_SRV_BASE           = 0;
constexpr uint32_t SLOT_SRV_PERCENTILE     = 5;
constexpr uint32_t SLOT_UAV_HISTOGRAM      = 6;
constexpr uint32_t SLOT_UAV_PERCENTILE     = 7;
constexpr uint32_t SLOT_UAV_IMPORTANCE_RAW = 8;
constexpr uint32_t SLOT_SRV_RAW_FOR_EMA    = 9;
constexpr uint32_t SLOT_SRV_EMA_HISTORY    = 10;
constexpr uint32_t SLOT_SRV_EMA_MV         = 11;
constexpr uint32_t SLOT_UAV_EMA_SMOOTH     = 12;
constexpr uint32_t TOTAL_SLOTS             = 13;

constexpr uint32_t HISTOGRAM_UINT_COUNT = 3u * 256u;

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

static ComPtr<ID3D12RootSignature> BuildComputeRS(ID3D12Device* device,
                                                   uint32_t numSrv, uint32_t srvBaseRegister,
                                                   uint32_t numUav, uint32_t uavBaseRegister,
                                                   bool needsCBV = true)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    uint32_t rangeCount = 0;
    if (numSrv > 0)
    {
        ranges[rangeCount].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[rangeCount].NumDescriptors                    = numSrv;
        ranges[rangeCount].BaseShaderRegister                = srvBaseRegister;
        ranges[rangeCount].RegisterSpace                     = 0;
        ranges[rangeCount].OffsetInDescriptorsFromTableStart = 0;
        rangeCount++;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors                    = numUav;
    uavRange.BaseShaderRegister                = uavBaseRegister;
    uavRange.RegisterSpace                     = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    uint32_t paramCount = 0;
    if (numSrv > 0)
    {
        params[paramCount].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
        params[paramCount].DescriptorTable.pDescriptorRanges   = &ranges[0];
        params[paramCount].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        paramCount++;
    }
    if (numUav > 0)
    {
        params[paramCount].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
        params[paramCount].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[paramCount].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        paramCount++;
    }
    if (needsCBV)
    {
        params[paramCount].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[paramCount].Descriptor.ShaderRegister = 0;
        params[paramCount].Descriptor.RegisterSpace  = 0;
        params[paramCount].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        paramCount++;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = paramCount;
    rsDesc.pParameters   = params;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        std::println("[ImportanceMap] RS serialize 실패: {}",
                     err ? (const char*)err->GetBufferPointer() : "?");
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> rs;
    ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                              IID_PPV_ARGS(&rs)));
    return rs;
}

// ──────────────────────────────────────────────────────────────────
// Init
// ──────────────────────────────────────────────────────────────────
bool ImportanceMapPass::Init(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_device = device;
    m_width  = width;
    m_height = height;

    // ── 1. 텍스처 자원 ───────────────────────────────────────────
    m_importanceRaw = CreateUAVTex2D(device, width, height,
                                      DXGI_FORMAT_R16_FLOAT, L"HPAR_ImportanceRaw");
    m_smooth[0]     = CreateUAVTex2D(device, width, height,
                                      DXGI_FORMAT_R16_FLOAT, L"HPAR_ImportanceSmooth[0]");
    m_smooth[1]     = CreateUAVTex2D(device, width, height,
                                      DXGI_FORMAT_R16_FLOAT, L"HPAR_ImportanceSmooth[1]");
    // m_semanticV 는 Phase 16 (NN) 에서 텍스처로 채워질 자리. Phase 3 에서는
    // ImportanceMap.hlsl 내부 상수 0 으로 처리하므로 미할당.

    // ── 2. 히스토그램 + percentile 버퍼 ───────────────────────────
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = HISTOGRAM_UINT_COUNT * sizeof(uint32_t);
        rd.Height           = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_histogram)));
        m_histogram->SetName(L"HPAR_Histogram");
    }
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = sizeof(float) * 4;
        rd.Height           = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_percentile)));
        m_percentile->SetName(L"HPAR_Percentile");
    }

    // ── 3. 디스크립터 힙 (shader-visible, TOTAL_SLOTS) ────────────
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = TOTAL_SLOTS;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_descHeap)));
        m_descIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // ── 4. ClearUnorderedAccessViewUint 용 non-visible 힙 ────────
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 1;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_clearHeap)));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format        = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = HISTOGRAM_UINT_COUNT;
        uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        m_histogramUavCpuShaderInvisible.cpu = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateUnorderedAccessView(m_histogram.Get(), nullptr, &uav,
                                          m_histogramUavCpuShaderInvisible.cpu);
    }

    // ── 5. 정적 디스크립터 슬롯 채우기 ────────────────────────────
    auto cpu = m_descHeap->GetCPUDescriptorHandleForHeapStart();
    auto slotCpu = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += (SIZE_T)m_descIncSize * slot;
        return h;
    };

    // slot 5: SRV percentile
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement        = 0;
        srv.Buffer.NumElements         = 1;
        srv.Buffer.StructureByteStride = sizeof(float) * 4;
        device->CreateShaderResourceView(m_percentile.Get(), &srv, slotCpu(SLOT_SRV_PERCENTILE));
    }
    // slot 6: UAV histogram (RAW)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format        = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = HISTOGRAM_UINT_COUNT;
        uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(m_histogram.Get(), nullptr, &uav, slotCpu(SLOT_UAV_HISTOGRAM));
    }
    // slot 7: UAV percentile
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format        = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements         = 1;
        uav.Buffer.StructureByteStride = sizeof(float) * 4;
        device->CreateUnorderedAccessView(m_percentile.Get(), nullptr, &uav, slotCpu(SLOT_UAV_PERCENTILE));
    }
    // slot 8: UAV importanceRaw
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16_FLOAT;
        device->CreateUnorderedAccessView(m_importanceRaw.Get(), nullptr, &uav, slotCpu(SLOT_UAV_IMPORTANCE_RAW));
    }
    // slot 9: SRV importanceRaw (EMA t0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format              = DXGI_FORMAT_R16_FLOAT;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_importanceRaw.Get(), &srv, slotCpu(SLOT_SRV_RAW_FOR_EMA));
    }
    // slot 10 (SRV history) 와 12 (UAV smooth out) 은 매 frame 동적 업데이트 (ping-pong)
    // slot 11 (SRV motionVec for EMA) 는 매 frame 입력 자원 받아서 채움

    // ── 6. Root signatures ────────────────────────────────────────
    m_rsHisto      = BuildComputeRS(device, /*SRV*/5, 0, /*UAV*/1, 0);
    m_rsReduce     = BuildComputeRS(device, /*SRV*/0, 0, /*UAV*/2, 0);
    m_rsImportance = BuildComputeRS(device, /*SRV*/6, 0, /*UAV*/1, 0);
    m_rsEMA        = BuildComputeRS(device, /*SRV*/3, 0, /*UAV*/1, 0);
    if (!m_rsHisto || !m_rsReduce || !m_rsImportance || !m_rsEMA) return false;
    m_rsHisto     ->SetName(L"HPAR_RS_Histogram");
    m_rsReduce    ->SetName(L"HPAR_RS_Reduce");
    m_rsImportance->SetName(L"HPAR_RS_Importance");
    m_rsEMA       ->SetName(L"HPAR_RS_EMA");

    // ── 7. Compute PSOs ───────────────────────────────────────────
    auto makePSO = [&](ID3D12RootSignature* rs, const wchar_t* path, const char* label) -> ComPtr<ID3D12PipelineState>
    {
        auto blob = CompileComputeCS(path);
        if (!blob) { std::println("[ImportanceMap] {} 컴파일 실패", label); return nullptr; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature       = rs;
        d.CS.pShaderBytecode   = blob->GetBufferPointer();
        d.CS.BytecodeLength    = blob->GetBufferSize();
        ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
        return pso;
    };
    m_psoHisto      = makePSO(m_rsHisto.Get(),      L"shaders/MetricHistogram.hlsl", "MetricHistogram");
    m_psoReduce     = makePSO(m_rsReduce.Get(),     L"shaders/MetricReduce.hlsl",    "MetricReduce");
    m_psoImportance = makePSO(m_rsImportance.Get(), L"shaders/ImportanceMap.hlsl",   "ImportanceMap");
    m_psoEMA        = makePSO(m_rsEMA.Get(),        L"shaders/ImportanceEMA.hlsl",   "ImportanceEMA");
    if (!m_psoHisto || !m_psoReduce || !m_psoImportance || !m_psoEMA) return false;

    // ── 8. 상수 버퍼 ──────────────────────────────────────────────
    m_cbHisto      = CreateUploadCB(device, sizeof(HistoCB),      &m_cbHistoMapped);
    m_cbReduce     = CreateUploadCB(device, sizeof(ReduceCB),     &m_cbReduceMapped);
    m_cbImportance = CreateUploadCB(device, sizeof(ImportanceCB), &m_cbImportanceMapped);
    m_cbEMA        = CreateUploadCB(device, sizeof(EmaCB),        &m_cbEMAMapped);

    std::println("[ImportanceMap] Init 완료 — {}x{}, 4-pass pipeline (histogram+reduce+importance+EMA), ping-pong smooth buffers",
                 width, height);
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Shutdown
// ──────────────────────────────────────────────────────────────────
void ImportanceMapPass::Shutdown()
{
    auto unmap = [](ComPtr<ID3D12Resource>& res, void*& mapped)
    {
        if (res && mapped) { D3D12_RANGE nw{0,0}; res->Unmap(0, &nw); mapped = nullptr; }
    };
    unmap(m_cbHisto,      m_cbHistoMapped);
    unmap(m_cbReduce,     m_cbReduceMapped);
    unmap(m_cbImportance, m_cbImportanceMapped);
    unmap(m_cbEMA,        m_cbEMAMapped);
}

// ──────────────────────────────────────────────────────────────────
// Apply — 매 프레임 4 dispatch
// ──────────────────────────────────────────────────────────────────
void ImportanceMapPass::Apply(ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* depthRes,
                              ID3D12Resource* accumRes,
                              ID3D12Resource* normalsRes,
                              ID3D12Resource* motionVecRes,
                              ID3D12Resource* specAlbedoRes)
{
    // ── 1. CB 업데이트 ────────────────────────────────────────────
    HistoCB cbH{};
    cbH.width = m_width; cbH.height = m_height;
    cbH.eMax  = m_eMax;  cbH.sMax = m_sMax; cbH.mMax = m_mMax;
    std::memcpy(m_cbHistoMapped, &cbH, sizeof(HistoCB));

    ReduceCB cbR{};
    cbR.eMax = m_eMax; cbR.sMax = m_sMax; cbR.mMax = m_mMax;
    std::memcpy(m_cbReduceMapped, &cbR, sizeof(ReduceCB));

    ImportanceCB cbI{};
    cbI.width  = m_width; cbI.height = m_height;
    cbI.weightE = m_weightE; cbI.weightD = m_weightD;
    cbI.weightS = m_weightS; cbI.weightM = m_weightM; cbI.weightV = m_weightV;
    cbI.metricFilter = m_metricFilter;
    std::memcpy(m_cbImportanceMapped, &cbI, sizeof(ImportanceCB));

    EmaCB cbE{};
    cbE.width = m_width; cbE.height = m_height;
    cbE.alpha = m_alpha;
    cbE.motionThreshold = m_motionThreshold;
    cbE.firstFrame = m_firstFrameFlag ? 1u : 0u;
    std::memcpy(m_cbEMAMapped, &cbE, sizeof(EmaCB));

    // ── 2. 매 frame 변하는 SRV 채우기 ────────────────────────────
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
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format        = fmt;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(res, &srv, slotCpu(slot));
    };
    writeSrv(0, depthRes,     DXGI_FORMAT_R32_FLOAT);
    writeSrv(1, accumRes,     DXGI_FORMAT_R32G32B32A32_FLOAT);
    writeSrv(2, normalsRes,   DXGI_FORMAT_R16G16B16A16_FLOAT);
    writeSrv(3, motionVecRes, DXGI_FORMAT_R16G16_FLOAT);
    writeSrv(4, specAlbedoRes,DXGI_FORMAT_R16G16B16A16_FLOAT);

    // EMA 의 SRV t2 (motionVec) 도 자체 슬롯에 동일 자원으로 채움 (table contiguity)
    writeSrv(SLOT_SRV_EMA_MV, motionVecRes, DXGI_FORMAT_R16G16_FLOAT);

    // ping-pong: EMA history (read) 와 smooth (write) 슬롯 동적 업데이트
    const uint32_t readIdx  = 1u - m_writeIdx;
    writeSrv(SLOT_SRV_EMA_HISTORY, m_smooth[readIdx].Get(), DXGI_FORMAT_R16_FLOAT);
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16_FLOAT;
        m_device->CreateUnorderedAccessView(m_smooth[m_writeIdx].Get(), nullptr, &uav,
                                            slotCpu(SLOT_UAV_EMA_SMOOTH));
    }

    // ── 3. 입력 텍스처 UAV → SRV transition (5개 + smooth[readIdx]) ──
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

    D3D12_RESOURCE_BARRIER toSRV_inputs[6]{};
    toSRV_inputs[0] = makeTrans(depthRes,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV_inputs[1] = makeTrans(accumRes,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV_inputs[2] = makeTrans(normalsRes,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV_inputs[3] = makeTrans(motionVecRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV_inputs[4] = makeTrans(specAlbedoRes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV_inputs[5] = makeTrans(m_smooth[readIdx].Get(),
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(6, toSRV_inputs);

    // ── 4. 히스토그램 clear ───────────────────────────────────────
    ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    auto gpuBase = m_descHeap->GetGPUDescriptorHandleForHeapStart();
    auto slotGpu = [&](uint32_t slot)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = gpuBase;
        h.ptr += (UINT64)m_descIncSize * slot;
        return h;
    };

    const uint32_t clearVals[4] = {0u, 0u, 0u, 0u};
    cmdList->ClearUnorderedAccessViewUint(
        slotGpu(SLOT_UAV_HISTOGRAM),
        m_histogramUavCpuShaderInvisible.cpu,
        m_histogram.Get(),
        clearVals, 0, nullptr);

    // ── 5. PASS A — MetricHistogram dispatch ─────────────────────
    cmdList->SetComputeRootSignature(m_rsHisto.Get());
    cmdList->SetComputeRootDescriptorTable(0, gpuBase);                    // SRV table @ slot 0
    cmdList->SetComputeRootDescriptorTable(1, slotGpu(SLOT_UAV_HISTOGRAM));// UAV table @ slot 6
    cmdList->SetComputeRootConstantBufferView(2, m_cbHisto->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_psoHisto.Get());
    const uint32_t gx = (m_width  + 7) / 8;
    const uint32_t gy = (m_height + 7) / 8;
    cmdList->Dispatch(gx, gy, 1);

    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = m_histogram.Get();
    cmdList->ResourceBarrier(1, &uavBar);

    // ── 6. PASS B — MetricReduce dispatch ────────────────────────
    cmdList->SetComputeRootSignature(m_rsReduce.Get());
    cmdList->SetComputeRootDescriptorTable(0, slotGpu(SLOT_UAV_HISTOGRAM));// UAV table @ slot 6,7
    cmdList->SetComputeRootConstantBufferView(1, m_cbReduce->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_psoReduce.Get());
    cmdList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER pctToSRV = makeTrans(m_percentile.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &pctToSRV);

    // ── 7. PASS C — ImportanceMap dispatch ───────────────────────
    cmdList->SetComputeRootSignature(m_rsImportance.Get());
    cmdList->SetComputeRootDescriptorTable(0, gpuBase);                          // SRV table @ slot 0..5
    cmdList->SetComputeRootDescriptorTable(1, slotGpu(SLOT_UAV_IMPORTANCE_RAW)); // UAV @ slot 8
    cmdList->SetComputeRootConstantBufferView(2, m_cbImportance->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_psoImportance.Get());
    cmdList->Dispatch(gx, gy, 1);

    // importanceRaw 의 UAV write 완료 보장 (EMA 가 SRV 로 읽음 — slot 9)
    D3D12_RESOURCE_BARRIER rawToSRV = makeTrans(m_importanceRaw.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &rawToSRV);

    // ── 8. PASS D — ImportanceEMA dispatch ───────────────────────
    cmdList->SetComputeRootSignature(m_rsEMA.Get());
    cmdList->SetComputeRootDescriptorTable(0, slotGpu(SLOT_SRV_RAW_FOR_EMA));    // SRV table @ slot 9..11 (raw, history, motionVec)
    cmdList->SetComputeRootDescriptorTable(1, slotGpu(SLOT_UAV_EMA_SMOOTH));     // UAV @ slot 12 (smooth out)
    cmdList->SetComputeRootConstantBufferView(2, m_cbEMA->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_psoEMA.Get());
    cmdList->Dispatch(gx, gy, 1);

    // ── 9. 복원 ──────────────────────────────────────────────────
    D3D12_RESOURCE_BARRIER restore[8]{};
    int n = 0;
    // 입력 5개 SRV → UAV
    for (int i = 0; i < 5; ++i)
    {
        restore[n] = toSRV_inputs[i];
        restore[n].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        restore[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ++n;
    }
    // smooth history SRV → UAV
    restore[n++] = makeTrans(m_smooth[readIdx].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // importanceRaw SRV → UAV
    restore[n++] = makeTrans(m_importanceRaw.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // percentile SRV → UAV
    restore[n++] = makeTrans(m_percentile.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(n, restore);

    // ── 10. Ping-pong 토글 + first-frame 해제 ────────────────────
    m_writeIdx = 1u - m_writeIdx;
    m_firstFrameFlag = false;
}
