#include "SVGFDenoiser.h"
#include <dxcapi.h>
#include <filesystem>
#include <print>

namespace
{
    ComPtr<IDxcBlob> CompileCS(const std::filesystem::path& path)
    {
        ComPtr<IDxcUtils>     utils;
        ComPtr<IDxcCompiler3> compiler;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils,    IID_PPV_ARGS(&utils)));
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

        ComPtr<IDxcBlobEncoding> src;
        ThrowIfFailed(utils->LoadFile(path.c_str(), nullptr, &src),
                      std::format("셰이더 파일 열기 실패: {}", path.string()));

        DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_UTF8 };

        std::wstring name = path.filename().wstring();
        std::vector<LPCWSTR> args = {
            name.c_str(),
            L"-T", L"cs_6_0",
            L"-E", L"main",
            L"-HV", L"2021",
#if defined(_DEBUG)
            L"-Zi", L"-Od",
#else
            L"-O3",
#endif
        };

        ComPtr<IDxcResult> result;
        ThrowIfFailed(compiler->Compile(&buf,
            args.data(), static_cast<UINT32>(args.size()),
            nullptr, IID_PPV_ARGS(&result)));

        HRESULT hr = S_OK;
        result->GetStatus(&hr);
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            if (errors && errors->GetStringLength() > 0)
                std::println("[SVGFDenoiser] 셰이더 컴파일 오류:\n{}", errors->GetStringPointer());
            ThrowIfFailed(hr, std::format("{} 컴파일 실패", path.string()));
        }

        ComPtr<IDxcBlob> blob;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
        return blob;
    }
}

// ---------------------------------------------------------------
void SVGFDenoiser::Init(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    m_device = device;
    m_width  = width;
    m_height = height;

    CreateBuffers(width, height);
    CreatePSO();

    // 12 슬롯: 3패스 × [SRV:color, SRV:depth, SRV:normal, UAV:output]
    m_heap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12, true);

    m_initialized = true;
    std::println("[SVGFDenoiser] 초기화 완료 (로컬분산 A-trous 3패스, {}x{})", width, height);
}

// ---------------------------------------------------------------
void SVGFDenoiser::CreateBuffers(uint32_t width, uint32_t height)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width            = width;
    d.Height           = height;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc       = {1, 0};
    d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_ping)));
    m_ping->SetName(L"SVGFDenoiser_Ping");

    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_pong)));
    m_pong->SetName(L"SVGFDenoiser_Pong");
}

// ---------------------------------------------------------------
void SVGFDenoiser::CreatePSO()
{
    // Param 0: 루트 상수 8개 (b0: width, height, stepSize, phiColor, doTonemap, sigmaDepth, sigmaNormal, pad)
    // Param 1: 디스크립터 테이블
    //   Range 0: SRV×3 (t0=color, t1=depth, t2=normals, offset 0)
    //   Range 1: UAV×1 (u0, offset 3)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 3;
        ranges[0].BaseShaderRegister                = 0;
        ranges[0].RegisterSpace                     = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 1;
        ranges[1].BaseShaderRegister                = 0;
        ranges[1].RegisterSpace                     = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 3;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace  = 0;
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges   = ranges;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err),
            "SVGFDenoiser 루트 시그니처 직렬화 실패");
        ThrowIfFailed(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)), "SVGFDenoiser 루트 시그니처 생성 실패");
    }
    {
        auto blob = CompileCS(L"shaders/SVGFWavelet.hlsl");
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature     = m_rootSig.Get();
        psoDesc.CS.pShaderBytecode = blob->GetBufferPointer();
        psoDesc.CS.BytecodeLength  = blob->GetBufferSize();
        ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)),
            "SVGFDenoiser PSO 생성 실패");
    }
}

// ---------------------------------------------------------------
void SVGFDenoiser::BuildDescriptors(ID3D12Resource* curIllum,
                                    ID3D12Resource* outputColor,
                                    ID3D12Resource* depth,
                                    ID3D12Resource* normals)
{
    auto makeSRV = [](DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format                  = fmt;
        d.Texture2D.MipLevels     = 1;
        return d;
    };

    auto srvRGBA32 = makeSRV(DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto srvR32    = makeSRV(DXGI_FORMAT_R32_FLOAT);
    auto srvRG16   = makeSRV(DXGI_FORMAT_R16G16_FLOAT);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavF32{};
    uavF32.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavF32.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavR8{};
    uavR8.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavR8.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    auto cpu = [&](uint32_t i) { return m_heap.GetHandle(i).cpu; };
    auto* dv = m_device;

    // Pass 0 (base=0): curIllum → ping
    dv->CreateShaderResourceView(curIllum,        &srvRGBA32, cpu(0));
    dv->CreateShaderResourceView(depth,           &srvR32,    cpu(1));
    dv->CreateShaderResourceView(normals,         &srvRG16,   cpu(2));
    dv->CreateUnorderedAccessView(m_ping.Get(), nullptr, &uavF32, cpu(3));

    // Pass 1 (base=4): ping → pong
    dv->CreateShaderResourceView(m_ping.Get(),    &srvRGBA32, cpu(4));
    dv->CreateShaderResourceView(depth,           &srvR32,    cpu(5));
    dv->CreateShaderResourceView(normals,         &srvRG16,   cpu(6));
    dv->CreateUnorderedAccessView(m_pong.Get(), nullptr, &uavF32, cpu(7));

    // Pass 2 (base=8): pong → outputColor (tonemap, RGBA8)
    dv->CreateShaderResourceView(m_pong.Get(),    &srvRGBA32, cpu(8));
    dv->CreateShaderResourceView(depth,           &srvR32,    cpu(9));
    dv->CreateShaderResourceView(normals,         &srvRG16,   cpu(10));
    dv->CreateUnorderedAccessView(outputColor, nullptr, &uavR8, cpu(11));

    m_descriptorsBuilt = true;
}

// ---------------------------------------------------------------
void SVGFDenoiser::Apply(ID3D12GraphicsCommandList4* cmdList,
                         ID3D12Resource* curIllum,
                         ID3D12Resource* outputColor,
                         ID3D12Resource* depth,
                         ID3D12Resource* normals)
{
    if (!m_initialized) return;

    if (!m_descriptorsBuilt)
        BuildDescriptors(curIllum, outputColor, depth, normals);

    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    cmdList->SetPipelineState(m_pso.Get());

    struct CB { uint32_t w, h, step; float phi; uint32_t tonemap; float sigD, sigN; uint32_t pad; };

    uint32_t gx = (m_width  + 7) / 8;
    uint32_t gy = (m_height + 7) / 8;

    auto uavToSrv = [&](ID3D12Resource* res) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    };
    auto srvToUav = [&](ID3D12Resource* res) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    };
    auto uavBarrier = [&](ID3D12Resource* res) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmdList->ResourceBarrier(1, &b);
    };

    // Pass 0 (step=1): curIllum → ping
    uavToSrv(curIllum);
    uavToSrv(depth);
    uavToSrv(normals);
    {
        CB cb{ m_width, m_height, 1u, 4.0f, 0u, 0.05f, 0.15f, 0u };
        cmdList->SetComputeRoot32BitConstants(0, 8, &cb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(0).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(m_ping.Get());
    uavToSrv(m_ping.Get());

    // Pass 1 (step=2): ping → pong
    {
        CB cb{ m_width, m_height, 2u, 4.0f, 0u, 0.05f, 0.15f, 0u };
        cmdList->SetComputeRoot32BitConstants(0, 8, &cb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(4).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(m_pong.Get());
    uavToSrv(m_pong.Get());

    // Pass 2 (step=4, tonemap): pong → outputColor
    {
        CB cb{ m_width, m_height, 4u, 4.0f, 1u, 0.05f, 0.15f, 0u };
        cmdList->SetComputeRoot32BitConstants(0, 8, &cb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(8).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(outputColor);

    // 상태 복원 (DLSS가 SRV→UAV 직접 처리하지만 다음 프레임 RT 쓰기를 위해 복원)
    srvToUav(curIllum);
    srvToUav(depth);
    srvToUav(normals);
    srvToUav(m_ping.Get());
    srvToUav(m_pong.Get());
}

// ---------------------------------------------------------------
void SVGFDenoiser::Shutdown()
{
    m_ping.Reset();
    m_pong.Reset();
    m_pso.Reset();
    m_rootSig.Reset();
    m_initialized      = false;
    m_descriptorsBuilt = false;
    m_device           = nullptr;
}
