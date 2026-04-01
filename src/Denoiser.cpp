#include "Denoiser.h"
#include <dxcapi.h>
#include <filesystem>
#include <print>

namespace
{
    // DXC로 컴퓨트 셰이더(cs_6_0) 컴파일
    ComPtr<IDxcBlob> CompileCS(const std::filesystem::path& path)
    {
        ComPtr<IDxcUtils>    utils;
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
                std::println("[Denoiser] 셰이더 컴파일 오류:\n{}", errors->GetStringPointer());
            ThrowIfFailed(hr, "Denoise.hlsl 컴파일 실패");
        }

        ComPtr<IDxcBlob> blob;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
        return blob;
    }
}

// ---------------------------------------------------------------
void Denoiser::Init(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    m_device = device;
    m_width  = width;
    m_height = height;

    // 핑퐁 버퍼 생성
    CreateBuffers(width, height);

    // 루트 시그니처 + 컴퓨트 PSO 생성
    CreatePSO();

    // 디스크립터 힙 (6 슬롯: 3패스 × [SRV, UAV])
    m_heap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, true);

    m_initialized = true;
    std::println("[Denoiser] 초기화 완료 (A-trous 3패스, {}x{})", width, height);
}

// ---------------------------------------------------------------
void Denoiser::CreateBuffers(uint32_t width, uint32_t height)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&m_ping)));
    m_ping->SetName(L"Denoiser_Ping");

    ThrowIfFailed(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&m_pong)));
    m_pong->SetName(L"Denoiser_Pong");
}

// ---------------------------------------------------------------
void Denoiser::CreatePSO()
{
    // ── 루트 시그니처 ──────────────────────────────────────────────
    // Param 0: 루트 상수 5개 (b0): width, height, stepSize, sigmaColor, doTonemap
    // Param 1: 디스크립터 테이블
    //           Range 0: SRV  t0 × 1  (offset 0)
    //           Range 1: UAV  u0 × 1  (offset 1)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        // SRV range (t0)
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 1;
        ranges[0].BaseShaderRegister                = 0;
        ranges[0].RegisterSpace                     = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        // UAV range (u0)
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 1;
        ranges[1].BaseShaderRegister                = 0;
        ranges[1].RegisterSpace                     = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER params[2]{};
        // Param 0: 루트 상수
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace  = 0;
        params[0].Constants.Num32BitValues = 5;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        // Param 1: 디스크립터 테이블
        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges   = ranges;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err),
            "Denoiser 루트 시그니처 직렬화 실패");
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)),
            "Denoiser 루트 시그니처 생성 실패");
    }

    // ── 컴퓨트 PSO ────────────────────────────────────────────────
    {
        auto blob = CompileCS(L"shaders/Denoise.hlsl");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature     = m_rootSig.Get();
        psoDesc.CS.pShaderBytecode = blob->GetBufferPointer();
        psoDesc.CS.BytecodeLength  = blob->GetBufferSize();

        ThrowIfFailed(m_device->CreateComputePipelineState(
            &psoDesc, IID_PPV_ARGS(&m_pso)),
            "Denoiser PSO 생성 실패");
    }
}

// ---------------------------------------------------------------
// 디스크립터 힙 슬롯 구성 (Apply 첫 호출 시 1회 실행)
// [0]=SRV:accum  [1]=UAV:ping   → Pass 0 테이블 (base=slot 0)
// [2]=SRV:ping   [3]=UAV:pong   → Pass 1 테이블 (base=slot 2)
// [4]=SRV:pong   [5]=UAV:output → Pass 2 테이블 (base=slot 4)
void Denoiser::BuildDescriptors(ID3D12Resource* accumResource,
                                ID3D12Resource* outputResource)
{
    // RGBA32F SRV 공통 설정
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                        = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDesc.Texture2D.MipLevels           = 1;
    srvDesc.Texture2D.MostDetailedMip     = 0;
    srvDesc.Texture2D.PlaneSlice          = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    // RGBA32F UAV (ping, pong)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavF32{};
    uavF32.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavF32.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;

    // RGBA8 UAV (g_output)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavR8{};
    uavR8.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavR8.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;

    auto cpu = [&](uint32_t i) { return m_heap.GetHandle(i).cpu; };

    m_device->CreateShaderResourceView(accumResource,   &srvDesc, cpu(0)); // SRV:accum
    m_device->CreateUnorderedAccessView(m_ping.Get(),   nullptr, &uavF32, cpu(1)); // UAV:ping
    m_device->CreateShaderResourceView(m_ping.Get(),    &srvDesc, cpu(2)); // SRV:ping
    m_device->CreateUnorderedAccessView(m_pong.Get(),   nullptr, &uavF32, cpu(3)); // UAV:pong
    m_device->CreateShaderResourceView(m_pong.Get(),    &srvDesc, cpu(4)); // SRV:pong
    m_device->CreateUnorderedAccessView(outputResource, nullptr, &uavR8,  cpu(5)); // UAV:output

    m_descriptorsBuilt = true;
}

// ---------------------------------------------------------------
void Denoiser::Apply(ID3D12GraphicsCommandList4* cmdList,
                     ID3D12Resource*             accumResource,
                     ID3D12Resource*             outputResource)
{
    if (!m_initialized) return;

    // 첫 Apply 호출 시 디스크립터 구성 (리소스 포인터 확정 후)
    if (!m_descriptorsBuilt)
        BuildDescriptors(accumResource, outputResource);

    // ── 디노이저 힙 + 파이프라인 바인딩 ──────────────────────────
    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(m_rootSig.Get());
    cmdList->SetPipelineState(m_pso.Get());

    // 상수 구조 (HLSL DenoiseCB와 32비트 단위 1:1 대응)
    struct CB { uint32_t width, height, step; float sigma; uint32_t tonemap; };

    uint32_t gx = (m_width  + 7) / 8;
    uint32_t gy = (m_height + 7) / 8;

    // ── 리소스 배리어 헬퍼 ────────────────────────────────────────
    auto uavToSrv = [&](ID3D12Resource* res)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    };
    auto srvToUav = [&](ID3D12Resource* res)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    };
    auto uavBarrier = [&](ID3D12Resource* res)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmdList->ResourceBarrier(1, &b);
    };

    // ── Pass 0 (step=1): accum → ping ────────────────────────────
    uavToSrv(accumResource);                // accum: UAV → SRV
    {
        CB cb{ m_width, m_height, 1, 0.3f, 0 };
        cmdList->SetComputeRoot32BitConstants(0, 5, &cb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(0).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(m_ping.Get());
    uavToSrv(m_ping.Get());                 // ping: UAV → SRV

    // ── Pass 1 (step=2): ping → pong ─────────────────────────────
    {
        CB cb{ m_width, m_height, 2, 0.3f, 0 };
        cmdList->SetComputeRoot32BitConstants(0, 5, &cb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(2).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(m_pong.Get());
    uavToSrv(m_pong.Get());                 // pong: UAV → SRV

    // ── Pass 2 (step=4, final): pong → g_output + tonemap ────────
    {
        CB cb{ m_width, m_height, 4, 0.3f, 1 };
        cmdList->SetComputeRoot32BitConstants(0, 5, &cb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(4).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(outputResource);             // g_output UAV 쓰기 완료 보장

    // ── 상태 복원 (다음 프레임 DispatchRays를 위해 모두 UAV로) ───
    srvToUav(accumResource);
    srvToUav(m_ping.Get());
    srvToUav(m_pong.Get());
}

// ---------------------------------------------------------------
void Denoiser::Shutdown()
{
    m_ping.Reset();
    m_pong.Reset();
    m_pso.Reset();
    m_rootSig.Reset();
    m_initialized      = false;
    m_descriptorsBuilt = false;
    m_device           = nullptr;
}
