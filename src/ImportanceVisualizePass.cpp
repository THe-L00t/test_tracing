#include "ImportanceVisualizePass.h"
#include "ShaderCompile.h"
#include <print>

struct VisCB
{
    uint32_t displayW;
    uint32_t displayH;
    float    renderW;
    float    renderH;
    float    _pad[60];
};
static_assert(sizeof(VisCB) == 256, "VisCB must be 256-byte aligned");

bool ImportanceVisualizePass::Init(ID3D12Device* device,
                                    uint32_t displayW, uint32_t displayH,
                                    uint32_t renderW, uint32_t renderH)
{
    m_device   = device;
    m_displayW = displayW; m_displayH = displayH;
    m_renderW  = (float)renderW; m_renderH = (float)renderH;

    // 출력 텍스처 (RGBA8 display-res, UAV+COPY_SOURCE)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = displayW; rd.Height = displayH;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc = {1, 0};
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_output)));
        m_output->SetName(L"HPAR_ImportanceVis_Output");
    }

    // Root signature: SRV×1 + UAV×1 + CBV + static linear sampler
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0; ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0; ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0; params[1].Descriptor.RegisterSpace = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxAnisotropy = 1;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MinLOD = 0.0f; samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0; samp.RegisterSpace = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &samp;

        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            std::println("[ImportanceVis] RootSig 직렬화 실패");
            return false;
        }
        ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_rootSig)));
    }

    // PSO
    {
        auto blob = CompileComputeCS(L"shaders/ImportanceVisualize.hlsl");
        if (!blob) { std::println("[ImportanceVis] 셰이더 컴파일 실패"); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_rootSig.Get();
        pd.CS.pShaderBytecode = blob->GetBufferPointer();
        pd.CS.BytecodeLength  = blob->GetBufferSize();
        ThrowIfFailed(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso)));
    }

    // Descriptor heap (2 슬롯)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 2; d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_descHeap)));
        m_descIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // CBV upload buffer
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(VisCB);
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = {1, 0};
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)));
        D3D12_RANGE nr{0, 0};
        ThrowIfFailed(m_constantBuffer->Map(0, &nr, &m_cbMapped));
    }

    std::println("[ImportanceVis] Init 완료 — {}x{} display-res 출력", displayW, displayH);
    return true;
}

void ImportanceVisualizePass::Shutdown()
{
    if (m_constantBuffer && m_cbMapped)
    {
        D3D12_RANGE nw{0, 0};
        m_constantBuffer->Unmap(0, &nw);
        m_cbMapped = nullptr;
    }
}

void ImportanceVisualizePass::Apply(ID3D12GraphicsCommandList* cmdList,
                                     ID3D12Resource* importance)
{
    // CB update
    VisCB cb{};
    cb.displayW = m_displayW;
    cb.displayH = m_displayH;
    cb.renderW  = m_renderW;
    cb.renderH  = m_renderH;
    std::memcpy(m_cbMapped, &cb, sizeof(VisCB));

    // Descriptor 채우기
    auto cpu = m_descHeap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_descHeap->GetGPUDescriptorHandleForHeapStart();

    // slot 0 SRV importance
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(importance, &srv, cpu);
    }
    // slot 1 UAV output
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += m_descIncSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        m_device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uav, h);
    }

    // importance UAV → SRV
    D3D12_RESOURCE_BARRIER toSRV{};
    toSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSRV.Transition.pResource = importance;
    toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSRV.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toSRV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toSRV);

    // Dispatch
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootDescriptorTable(0, gpu);
    cmdList->SetComputeRootConstantBufferView(1, m_constantBuffer->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_pso.Get());
    const uint32_t gx = (m_displayW + 7) / 8;
    const uint32_t gy = (m_displayH + 7) / 8;
    cmdList->Dispatch(gx, gy, 1);

    // 복원: importance SRV → UAV
    D3D12_RESOURCE_BARRIER toUAV = toSRV;
    toUAV.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toUAV.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmdList->ResourceBarrier(1, &toUAV);
}
