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
    int32_t  metricFilter;  // 0=All, 1=E only, 2=D only, 3=S only, 4=M only
    float    fill[56];
};
static_assert(sizeof(ImportanceCB) == 256, "ImportanceCB 256B");

// ──────────────────────────────────────────────────────────────────
// 힙 슬롯 레이아웃 (9 슬롯)
//   0..4 : SRV depth/accum/normals/motionVec/specAlbedo
//   5    : SRV percentile (StructuredBuffer<float4>, ImportanceMap 만 사용)
//   6    : UAV histogram
//   7    : UAV percentile
//   8    : UAV importance
// ──────────────────────────────────────────────────────────────────
constexpr uint32_t SLOT_SRV_BASE       = 0;
constexpr uint32_t SLOT_SRV_PERCENTILE = 5;
constexpr uint32_t SLOT_UAV_HISTOGRAM  = 6;
constexpr uint32_t SLOT_UAV_PERCENTILE = 7;
constexpr uint32_t SLOT_UAV_IMPORTANCE = 8;
constexpr uint32_t TOTAL_SLOTS         = 9;

// ──────────────────────────────────────────────────────────────────
// 버퍼 정보
// ──────────────────────────────────────────────────────────────────
constexpr uint32_t HISTOGRAM_UINT_COUNT = 3u * 256u;   // 3 metrics × 256 bins

// ──────────────────────────────────────────────────────────────────
// 헬퍼: upload CB 생성
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

// ──────────────────────────────────────────────────────────────────
// 헬퍼: compute root signature 빌더
//   SRV table (numSrv) + UAV table (numUav) + 1 root CBV (b0)
// ──────────────────────────────────────────────────────────────────
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

    // ── 1. 출력 importance 텍스처 ─────────────────────────────────
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;  rd.Height = height;
        rd.DepthOrArraySize = 1;      rd.MipLevels = 1;
        rd.Format           = DXGI_FORMAT_R16_FLOAT; rd.SampleDesc = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_importance)));
        m_importance->SetName(L"HPAR_ImportanceMap");
    }

    // ── 2. 히스토그램 버퍼 (3 × 256 uint = 3072 bytes, raw) ──────
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

    // ── 3. percentile 버퍼 (1 × float4) ──────────────────────────
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

    // ── 4. 디스크립터 힙 (shader-visible, 9 슬롯) ─────────────────
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = TOTAL_SLOTS;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_descHeap)));
        m_descIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // ── 5. ClearUnorderedAccessViewUint 용 non-visible 힙 (slot 1) ─
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 1;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_clearHeap)));

        // 히스토그램 UAV (RAW byte address buffer) — non-visible
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format        = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = HISTOGRAM_UINT_COUNT;
        uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        m_histogramUavCpuShaderInvisible.cpu = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateUnorderedAccessView(m_histogram.Get(), nullptr, &uav,
                                          m_histogramUavCpuShaderInvisible.cpu);
    }

    // ── 6. 정적 디스크립터 슬롯 채우기 ────────────────────────────
    //   리소스 의존 슬롯 (depth/accum/normals/mv/spec) 는 매 frame Apply 에서 채움
    //   slot 5 SRV percentile, slot 6 UAV histogram, slot 7 UAV percentile, slot 8 UAV importance
    //   는 자체 보유 리소스라 Init 에서 한 번만 채움
    auto cpu = m_descHeap->GetCPUDescriptorHandleForHeapStart();

    // slot 5: SRV percentile (StructuredBuffer<float4>)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += m_descIncSize * SLOT_SRV_PERCENTILE;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement        = 0;
        srv.Buffer.NumElements         = 1;
        srv.Buffer.StructureByteStride = sizeof(float) * 4;
        device->CreateShaderResourceView(m_percentile.Get(), &srv, h);
    }
    // slot 6: UAV histogram (RAW)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += m_descIncSize * SLOT_UAV_HISTOGRAM;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format        = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = HISTOGRAM_UINT_COUNT;
        uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(m_histogram.Get(), nullptr, &uav, h);
    }
    // slot 7: UAV percentile (StructuredBuffer)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += m_descIncSize * SLOT_UAV_PERCENTILE;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format        = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements         = 1;
        uav.Buffer.StructureByteStride = sizeof(float) * 4;
        device->CreateUnorderedAccessView(m_percentile.Get(), nullptr, &uav, h);
    }
    // slot 8: UAV importance
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += m_descIncSize * SLOT_UAV_IMPORTANCE;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16_FLOAT;
        device->CreateUnorderedAccessView(m_importance.Get(), nullptr, &uav, h);
    }

    // ── 7. Root signatures ────────────────────────────────────────
    m_rsHisto      = BuildComputeRS(device, /*SRV*/5, 0, /*UAV*/1, 0);
    m_rsReduce     = BuildComputeRS(device, /*SRV*/0, 0, /*UAV*/2, 0);
    m_rsImportance = BuildComputeRS(device, /*SRV*/6, 0, /*UAV*/1, 0);
    if (!m_rsHisto || !m_rsReduce || !m_rsImportance) return false;
    m_rsHisto     ->SetName(L"HPAR_RS_Histogram");
    m_rsReduce    ->SetName(L"HPAR_RS_Reduce");
    m_rsImportance->SetName(L"HPAR_RS_Importance");

    // ── 8. Compute PSOs ───────────────────────────────────────────
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
    if (!m_psoHisto || !m_psoReduce || !m_psoImportance) return false;

    // ── 9. 상수 버퍼 ──────────────────────────────────────────────
    m_cbHisto      = CreateUploadCB(device, sizeof(HistoCB),      &m_cbHistoMapped);
    m_cbReduce     = CreateUploadCB(device, sizeof(ReduceCB),     &m_cbReduceMapped);
    m_cbImportance = CreateUploadCB(device, sizeof(ImportanceCB), &m_cbImportanceMapped);

    std::println("[ImportanceMap] Init 완료 — {}x{}, 3-pass pipeline (histogram + reduce + importance)",
                 width, height);
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Shutdown
// ──────────────────────────────────────────────────────────────────
void ImportanceMapPass::Shutdown()
{
    if (m_cbHisto      && m_cbHistoMapped)      { D3D12_RANGE nw{0,0}; m_cbHisto->Unmap(0, &nw);      m_cbHistoMapped     = nullptr; }
    if (m_cbReduce     && m_cbReduceMapped)     { D3D12_RANGE nw{0,0}; m_cbReduce->Unmap(0, &nw);     m_cbReduceMapped    = nullptr; }
    if (m_cbImportance && m_cbImportanceMapped) { D3D12_RANGE nw{0,0}; m_cbImportance->Unmap(0, &nw); m_cbImportanceMapped = nullptr; }
}

// ──────────────────────────────────────────────────────────────────
// Apply — 매 프레임 3 dispatch
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
    cbH.width  = m_width;  cbH.height = m_height;
    cbH.eMax   = m_eMax;   cbH.sMax = m_sMax; cbH.mMax = m_mMax;
    std::memcpy(m_cbHistoMapped, &cbH, sizeof(HistoCB));

    ReduceCB cbR{};
    cbR.eMax = m_eMax; cbR.sMax = m_sMax; cbR.mMax = m_mMax;
    std::memcpy(m_cbReduceMapped, &cbR, sizeof(ReduceCB));

    ImportanceCB cbI{};
    cbI.width   = m_width; cbI.height = m_height;
    cbI.weightE = m_weightE; cbI.weightD = m_weightD;
    cbI.weightS = m_weightS; cbI.weightM = m_weightM; cbI.weightV = m_weightV;
    cbI.metricFilter = m_metricFilter;
    std::memcpy(m_cbImportanceMapped, &cbI, sizeof(ImportanceCB));

    // ── 2. 매 frame 변하는 SRV (slot 0..4) 채우기 ─────────────────
    auto cpuBase = m_descHeap->GetCPUDescriptorHandleForHeapStart();

    auto writeSrv = [&](uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += m_descIncSize * slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format        = fmt;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(res, &srv, h);
    };
    writeSrv(0, depthRes,     DXGI_FORMAT_R32_FLOAT);
    writeSrv(1, accumRes,     DXGI_FORMAT_R32G32B32A32_FLOAT);
    writeSrv(2, normalsRes,   DXGI_FORMAT_R16G16B16A16_FLOAT);
    writeSrv(3, motionVecRes, DXGI_FORMAT_R16G16_FLOAT);
    writeSrv(4, specAlbedoRes,DXGI_FORMAT_R16G16B16A16_FLOAT);

    // ── 3. 입력 텍스처 UAV → SRV transition ──────────────────────
    auto makeTrans = [](ID3D12Resource* res,
                         D3D12_RESOURCE_STATES before,
                         D3D12_RESOURCE_STATES after) -> D3D12_RESOURCE_BARRIER
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return b;
    };
    D3D12_RESOURCE_BARRIER toSRV[5]{};
    toSRV[0] = makeTrans(depthRes,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[1] = makeTrans(accumRes,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[2] = makeTrans(normalsRes,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[3] = makeTrans(motionVecRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    toSRV[4] = makeTrans(specAlbedoRes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(5, toSRV);

    // ── 4. 히스토그램 clear ───────────────────────────────────────
    ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    auto gpuBase = m_descHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHistUAV = gpuBase;
    gpuHistUAV.ptr += (UINT64)m_descIncSize * SLOT_UAV_HISTOGRAM;

    const uint32_t clearVals[4] = {0u, 0u, 0u, 0u};
    cmdList->ClearUnorderedAccessViewUint(
        gpuHistUAV,
        m_histogramUavCpuShaderInvisible.cpu,
        m_histogram.Get(),
        clearVals, 0, nullptr);

    // ── 5. PASS A — MetricHistogram dispatch ─────────────────────
    cmdList->SetComputeRootSignature(m_rsHisto.Get());
    // SRV table → slot 0
    cmdList->SetComputeRootDescriptorTable(0, gpuBase);
    // UAV table → slot 6 (histogram)
    cmdList->SetComputeRootDescriptorTable(1, gpuHistUAV);
    // CBV
    cmdList->SetComputeRootConstantBufferView(2, m_cbHisto->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_psoHisto.Get());
    const uint32_t gx = (m_width  + 7) / 8;
    const uint32_t gy = (m_height + 7) / 8;
    cmdList->Dispatch(gx, gy, 1);

    // UAV barrier on histogram
    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = m_histogram.Get();
    cmdList->ResourceBarrier(1, &uavBar);

    // ── 6. PASS B — MetricReduce dispatch ────────────────────────
    cmdList->SetComputeRootSignature(m_rsReduce.Get());
    // UAV table → slot 6 (histogram + percentile, consecutive)
    cmdList->SetComputeRootDescriptorTable(0, gpuHistUAV);
    // CBV
    cmdList->SetComputeRootConstantBufferView(1, m_cbReduce->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_psoReduce.Get());
    cmdList->Dispatch(1, 1, 1);  // 단일 스레드

    // percentile 을 UAV → SRV 로 transition (importance pass 가 SRV 로 읽음)
    D3D12_RESOURCE_BARRIER pctToSRV = makeTrans(m_percentile.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &pctToSRV);

    // ── 7. PASS C — ImportanceMap dispatch ───────────────────────
    cmdList->SetComputeRootSignature(m_rsImportance.Get());
    // SRV table → slot 0 (depth..specAlbedo + percentile slot 5)
    cmdList->SetComputeRootDescriptorTable(0, gpuBase);
    // UAV table → slot 8 (importance)
    D3D12_GPU_DESCRIPTOR_HANDLE gpuImpUAV = gpuBase;
    gpuImpUAV.ptr += (UINT64)m_descIncSize * SLOT_UAV_IMPORTANCE;
    cmdList->SetComputeRootDescriptorTable(1, gpuImpUAV);
    cmdList->SetComputeRootConstantBufferView(2, m_cbImportance->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_psoImportance.Get());
    cmdList->Dispatch(gx, gy, 1);

    // ── 8. 복원: percentile SRV → UAV ────────────────────────────
    D3D12_RESOURCE_BARRIER pctToUAV = makeTrans(m_percentile.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &pctToUAV);

    // 입력 텍스처 SRV → UAV 복원
    D3D12_RESOURCE_BARRIER toUAV[5]{};
    for (int i = 0; i < 5; ++i)
    {
        toUAV[i] = toSRV[i];
        toUAV[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toUAV[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    cmdList->ResourceBarrier(5, toUAV);
}
