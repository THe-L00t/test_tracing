#include "ImportanceMapPass.h"
#include <d3dcompiler.h>
#include <print>
#include <filesystem>

#pragma comment(lib, "dxcompiler.lib")
#include <dxcapi.h>

// CB 레이아웃 (16-byte aligned, 256-byte 정렬은 D3D12 에서 강제)
struct ImportanceCB
{
    uint32_t width;
    uint32_t height;
    float    weightE;
    float    weightD;
    float    _pad[60];  // 256B 정렬
};
static_assert(sizeof(ImportanceCB) == 256, "ImportanceCB must be 256-byte aligned");

static ComPtr<IDxcBlob> CompileShader(const wchar_t* path, const wchar_t* entry, const wchar_t* target)
{
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

    ComPtr<IDxcIncludeHandler> includeHandler;
    utils->CreateDefaultIncludeHandler(&includeHandler);

    ComPtr<IDxcBlobEncoding> sourceBlob;
    ThrowIfFailed(utils->LoadFile(path, nullptr, &sourceBlob));
    DxcBuffer source{};
    source.Ptr      = sourceBlob->GetBufferPointer();
    source.Size     = sourceBlob->GetBufferSize();
    source.Encoding = DXC_CP_ACP;

    LPCWSTR args[] = {
        L"-E", entry,
        L"-T", target,
        L"-Zi",
        L"-Qembed_debug",
    };
    ComPtr<IDxcResult> result;
    ThrowIfFailed(compiler->Compile(&source, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result)));

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0)
    {
        std::println("[ImportanceMap] 셰이더 컴파일 메시지: {}", (const char*)errors->GetBufferPointer());
    }

    HRESULT status;
    result->GetStatus(&status);
    if (FAILED(status))
    {
        std::println("[ImportanceMap] 셰이더 컴파일 실패: 0x{:08X}", (uint32_t)status);
        return nullptr;
    }

    ComPtr<IDxcBlob> shader;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr);
    return shader;
}

bool ImportanceMapPass::Init(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_device = device;
    m_width  = width;
    m_height = height;

    // ── 1. importance 텍스처 생성 (R16F) ──────────────────────────
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;  rd.Height = height;
        rd.DepthOrArraySize = 1;      rd.MipLevels = 1;
        rd.Format           = DXGI_FORMAT_R16_FLOAT;
        rd.SampleDesc       = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_importance)));
        m_importance->SetName(L"HPAR_ImportanceMap");
    }

    // ── 2. Root Signature ─────────────────────────────────────────
    //   param 0: descriptor table — SRV×2 (depth, accum) + UAV×1 (importance)
    //   param 1: root CBV b0
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 2;       // t0, t1
        ranges[0].BaseShaderRegister                = 0;
        ranges[0].RegisterSpace                     = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 1;       // u0
        ranges[1].BaseShaderRegister                = 0;
        ranges[1].RegisterSpace                     = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges   = ranges;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace  = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr))
        {
            std::println("[ImportanceMap] RootSig serialize 실패: {}",
                         err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        ThrowIfFailed(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_rootSig)));
        m_rootSig->SetName(L"HPAR_ImportanceMap_RootSig");
    }

    // ── 3. 컴퓨트 PSO ─────────────────────────────────────────────
    {
        // DXRPipeline 과 동일한 패턴: 작업 디렉토리 기준 상대 경로
        auto blob = CompileShader(L"shaders/ImportanceMap.hlsl", L"main", L"cs_6_0");
        if (!blob)
        {
            std::println("[ImportanceMap] shaders/ImportanceMap.hlsl 컴파일 실패");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature        = m_rootSig.Get();
        psoDesc.CS.pShaderBytecode    = blob->GetBufferPointer();
        psoDesc.CS.BytecodeLength     = blob->GetBufferSize();
        ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));
        m_pso->SetName(L"HPAR_ImportanceMap_PSO");
    }

    // ── 4. 디스크립터 힙 (shader-visible, 3 슬롯) ──────────────────
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 3;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descHeap)));
        m_descHeap->SetName(L"HPAR_ImportanceMap_DescHeap");
        m_descIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // ── 5. 상수 버퍼 (upload heap) ────────────────────────────────
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = sizeof(ImportanceCB);
        rd.Height           = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)));
        D3D12_RANGE noRead{0, 0};
        ThrowIfFailed(m_constantBuffer->Map(0, &noRead, &m_cbMapped));
    }

    std::println("[ImportanceMap] Init 완료 — {}x{}, R16F UAV, compute PSO 컴파일됨", width, height);
    return true;
}

void ImportanceMapPass::Shutdown()
{
    if (m_constantBuffer && m_cbMapped)
    {
        D3D12_RANGE noWrite{0, 0};
        m_constantBuffer->Unmap(0, &noWrite);
        m_cbMapped = nullptr;
    }
}

void ImportanceMapPass::Apply(ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* depthRes,
                              ID3D12Resource* accumRes)
{
    // ── 1. CB 업데이트 ────────────────────────────────────────────
    ImportanceCB cb{};
    cb.width   = m_width;
    cb.height  = m_height;
    cb.weightE = m_weightE;
    cb.weightD = m_weightD;
    std::memcpy(m_cbMapped, &cb, sizeof(ImportanceCB));

    // ── 2. 디스크립터 슬롯 채우기 ────────────────────────────────
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_descHeap->GetGPUDescriptorHandleForHeapStart();

    // slot 0: SRV g_depth (depth 텍스처 — R32F 또는 R16F 둘 다 R_FLOAT 로 view)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format              = DXGI_FORMAT_R32_FLOAT;  // App.cpp 가 R32_FLOAT 로 m_gbufferDepth 생성
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(depthRes, &srv, cpu);
    }
    // slot 1: SRV g_accumulation (RGBA32F)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += m_descIncSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(accumRes, &srv, h);
    }
    // slot 2: UAV g_importance
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += m_descIncSize * 2;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16_FLOAT;
        m_device->CreateUnorderedAccessView(m_importance.Get(), nullptr, &uav, h);
    }

    // ── 3. 입력 리소스 transition: UAV → SRV ──────────────────────
    //   depthRes / accumRes 는 호출자가 UAV 상태로 두었음 (RT shader 가 마지막에 write)
    D3D12_RESOURCE_BARRIER toSRV[2]{};
    toSRV[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSRV[0].Transition.pResource   = depthRes;
    toSRV[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSRV[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toSRV[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSRV[1] = toSRV[0];
    toSRV[1].Transition.pResource   = accumRes;
    cmdList->ResourceBarrier(2, toSRV);

    // ── 4. dispatch ──────────────────────────────────────────────
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootDescriptorTable(0, gpu);
    cmdList->SetComputeRootConstantBufferView(1, m_constantBuffer->GetGPUVirtualAddress());
    cmdList->SetPipelineState(m_pso.Get());

    const uint32_t gridX = (m_width  + 7) / 8;
    const uint32_t gridY = (m_height + 7) / 8;
    cmdList->Dispatch(gridX, gridY, 1);

    // ── 5. 복원: SRV → UAV ───────────────────────────────────────
    D3D12_RESOURCE_BARRIER toUAV[2]{};
    toUAV[0] = toSRV[0];
    toUAV[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toUAV[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toUAV[1] = toUAV[0];
    toUAV[1].Transition.pResource   = accumRes;
    cmdList->ResourceBarrier(2, toUAV);
}
