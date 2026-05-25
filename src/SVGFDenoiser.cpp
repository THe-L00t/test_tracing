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
    CreateTemporalPSO();
    CreateWaveletPSO();

    // 25 슬롯: Temporal(10) + Wavelet×3(5×3)
    m_heap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 25, true);

    m_initialized = true;
    std::println("[SVGFDenoiser] 초기화 완료 (Temporal + Wavelet×3, {}x{})", width, height);
}

// ---------------------------------------------------------------
void SVGFDenoiser::CreateBuffers(uint32_t w, uint32_t h)
{
    auto make = [&](DXGI_FORMAT fmt, const wchar_t* dbgName) -> ComPtr<ID3D12Resource>
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width            = w;
        d.Height           = h;
        d.DepthOrArraySize = 1;
        d.MipLevels        = 1;
        d.Format           = fmt;
        d.SampleDesc       = {1, 0};
        d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r)));
        r->SetName(dbgName);
        return r;
    };

    m_accumPing   = make(DXGI_FORMAT_R32G32B32A32_FLOAT, L"SVGF_AccumPing");
    m_accumPong   = make(DXGI_FORMAT_R32G32B32A32_FLOAT, L"SVGF_AccumPong");
    m_momentsCur  = make(DXGI_FORMAT_R32G32B32A32_FLOAT, L"SVGF_MomentsCur");
    m_momentsPrev = make(DXGI_FORMAT_R32G32B32A32_FLOAT, L"SVGF_MomentsPrev");
    m_prevDepth   = make(DXGI_FORMAT_R32_FLOAT,           L"SVGF_PrevDepth");
    m_prevNormal  = make(DXGI_FORMAT_R16G16_FLOAT,        L"SVGF_PrevNormal");
    m_waveletPing = make(DXGI_FORMAT_R32G32B32A32_FLOAT, L"SVGF_WaveletPing");
    m_waveletPong = make(DXGI_FORMAT_R32G32B32A32_FLOAT, L"SVGF_WaveletPong");
}

// ---------------------------------------------------------------
void SVGFDenoiser::CreateTemporalPSO()
{
    // Param 0: 루트 상수 8개 (b0: width, height, alpha, depthThresh, normalThresh, pad×3)
    // Param 1: 디스크립터 테이블
    //   Range 0: SRV×8 (t0-t7, offset 0)
    //   Range 1: UAV×2 (u0-u1, offset 8)
    // Static sampler: s0 linear clamp
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 8;
        ranges[0].BaseShaderRegister                = 0;
        ranges[0].RegisterSpace                     = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 2;
        ranges[1].BaseShaderRegister                = 0;
        ranges[1].RegisterSpace                     = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 8;

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

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister   = 0;
        sampler.RegisterSpace    = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = 2;
        rsDesc.pParameters       = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers   = &sampler;
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err),
            "SVGF Temporal 루트 시그니처 직렬화 실패");
        ThrowIfFailed(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_temporalRS)), "SVGF Temporal 루트 시그니처 생성 실패");
    }
    {
        auto blob = CompileCS(L"shaders/SVGFTemporalAccum.hlsl");
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature     = m_temporalRS.Get();
        psoDesc.CS.pShaderBytecode = blob->GetBufferPointer();
        psoDesc.CS.BytecodeLength  = blob->GetBufferSize();
        ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_temporalPSO)),
            "SVGF Temporal PSO 생성 실패");
    }
}

// ---------------------------------------------------------------
void SVGFDenoiser::CreateWaveletPSO()
{
    // Param 0: 루트 상수 8개 (b0: width, height, stepSize, phiColor, doTonemap, sigmaDepth, sigmaNormal, pad)
    // Param 1: 디스크립터 테이블
    //   Range 0: SRV×4 (t0-t3, offset 0)
    //   Range 1: UAV×1 (u0, offset 4)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 4;
        ranges[0].BaseShaderRegister                = 0;
        ranges[0].RegisterSpace                     = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 1;
        ranges[1].BaseShaderRegister                = 0;
        ranges[1].RegisterSpace                     = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 4;

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
            "SVGF Wavelet 루트 시그니처 직렬화 실패");
        ThrowIfFailed(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_waveletRS)), "SVGF Wavelet 루트 시그니처 생성 실패");
    }
    {
        auto blob = CompileCS(L"shaders/SVGFWavelet.hlsl");
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature     = m_waveletRS.Get();
        psoDesc.CS.pShaderBytecode = blob->GetBufferPointer();
        psoDesc.CS.BytecodeLength  = blob->GetBufferSize();
        ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_waveletPSO)),
            "SVGF Wavelet PSO 생성 실패");
    }
}

// ---------------------------------------------------------------
// 힙 슬롯 구성 (Apply 첫 호출 시 1회)
// [0-9]:   Temporal: SRV t0-t7, UAV u0-u1
// [10-14]: Wavelet 0: SRV t0-t3, UAV u0
// [15-19]: Wavelet 1: SRV t0-t3, UAV u0
// [20-24]: Wavelet 2: SRV t0-t3, UAV u0 (RGBA8)
void SVGFDenoiser::BuildDescriptors(ID3D12Resource* curIllum,
                                    ID3D12Resource* outputColor,
                                    ID3D12Resource* depth,
                                    ID3D12Resource* normals,
                                    ID3D12Resource* motionVec)
{
    auto makeSRV = [](DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format                  = fmt;
        d.Texture2D.MipLevels     = 1;
        return d;
    };
    auto makeUAV = [](DXGI_FORMAT fmt) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Format        = fmt;
        return d;
    };

    auto srvRGBA32 = makeSRV(DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto srvR32    = makeSRV(DXGI_FORMAT_R32_FLOAT);
    auto srvRG16   = makeSRV(DXGI_FORMAT_R16G16_FLOAT);
    auto uavRGBA32 = makeUAV(DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto uavRGBA8  = makeUAV(DXGI_FORMAT_R8G8B8A8_UNORM);

    auto cpu = [&](uint32_t i) { return m_heap.GetHandle(i).cpu; };
    auto* d  = m_device;

    // ── Temporal pass [0-9] ─────────────────────────────────────
    d->CreateShaderResourceView(curIllum,          &srvRGBA32, cpu(0));  // t0: curIllum
    d->CreateShaderResourceView(m_accumPong.Get(), &srvRGBA32, cpu(1));  // t1: prevAccum
    d->CreateShaderResourceView(m_momentsPrev.Get(),&srvRGBA32,cpu(2));  // t2: prevMoments
    d->CreateShaderResourceView(motionVec,         &srvRG16,   cpu(3));  // t3: motionVec
    d->CreateShaderResourceView(depth,             &srvR32,    cpu(4));  // t4: depth
    d->CreateShaderResourceView(m_prevDepth.Get(), &srvR32,    cpu(5));  // t5: prevDepth
    d->CreateShaderResourceView(normals,           &srvRG16,   cpu(6));  // t6: normals
    d->CreateShaderResourceView(m_prevNormal.Get(),&srvRG16,   cpu(7));  // t7: prevNormals
    d->CreateUnorderedAccessView(m_accumPing.Get(),  nullptr, &uavRGBA32, cpu(8));  // u0: accumOut
    d->CreateUnorderedAccessView(m_momentsCur.Get(), nullptr, &uavRGBA32, cpu(9));  // u1: momentsOut

    // ── Wavelet pass 0 [10-14]: accumPing → waveletPing (step=1) ─
    d->CreateShaderResourceView(m_accumPing.Get(), &srvRGBA32, cpu(10)); // t0
    d->CreateShaderResourceView(depth,             &srvR32,    cpu(11)); // t1
    d->CreateShaderResourceView(normals,           &srvRG16,   cpu(12)); // t2
    d->CreateShaderResourceView(m_momentsCur.Get(),&srvRGBA32, cpu(13)); // t3
    d->CreateUnorderedAccessView(m_waveletPing.Get(), nullptr, &uavRGBA32, cpu(14)); // u0

    // ── Wavelet pass 1 [15-19]: waveletPing → waveletPong (step=2)
    d->CreateShaderResourceView(m_waveletPing.Get(),&srvRGBA32, cpu(15)); // t0
    d->CreateShaderResourceView(depth,              &srvR32,    cpu(16)); // t1
    d->CreateShaderResourceView(normals,            &srvRG16,   cpu(17)); // t2
    d->CreateShaderResourceView(m_momentsCur.Get(), &srvRGBA32, cpu(18)); // t3
    d->CreateUnorderedAccessView(m_waveletPong.Get(), nullptr, &uavRGBA32, cpu(19)); // u0

    // ── Wavelet pass 2 [20-24]: waveletPong → outputColor (step=4, tonemap)
    d->CreateShaderResourceView(m_waveletPong.Get(),&srvRGBA32, cpu(20)); // t0
    d->CreateShaderResourceView(depth,              &srvR32,    cpu(21)); // t1
    d->CreateShaderResourceView(normals,            &srvRG16,   cpu(22)); // t2
    d->CreateShaderResourceView(m_momentsCur.Get(), &srvRGBA32, cpu(23)); // t3
    d->CreateUnorderedAccessView(outputColor, nullptr, &uavRGBA8, cpu(24)); // u0: RGBA8 출력

    m_descriptorsBuilt = true;
}

// ---------------------------------------------------------------
void SVGFDenoiser::Apply(ID3D12GraphicsCommandList4* cmdList,
                         ID3D12Resource* curIllum,
                         ID3D12Resource* outputColor,
                         ID3D12Resource* depth,
                         ID3D12Resource* normals,
                         ID3D12Resource* motionVec,
                         bool reset)
{
    if (!m_initialized) return;

    if (!m_descriptorsBuilt)
        BuildDescriptors(curIllum, outputColor, depth, normals, motionVec);

    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    uint32_t gx = (m_width  + 7) / 8;
    uint32_t gy = (m_height + 7) / 8;

    // ── 배리어 헬퍼 ───────────────────────────────────────────────
    auto transition = [&](ID3D12Resource* res,
                          D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
    };
    auto uavBarrier = [&](ID3D12Resource* res) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmdList->ResourceBarrier(1, &b);
    };

    constexpr auto kUAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto kSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto kSRC = D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr auto kDST = D3D12_RESOURCE_STATE_COPY_DEST;

    // ── Temporal pass ─────────────────────────────────────────────
    cmdList->SetComputeRootSignature(m_temporalRS.Get());
    cmdList->SetPipelineState(m_temporalPSO.Get());

    // 시간적 입력 UAV → SRV (8개 배치)
    {
        D3D12_RESOURCE_BARRIER bars[8]{};
        ID3D12Resource* inputs[8] = {
            curIllum, m_accumPong.Get(), m_momentsPrev.Get(), motionVec,
            depth, m_prevDepth.Get(), normals, m_prevNormal.Get()
        };
        for (int i = 0; i < 8; ++i)
        {
            bars[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bars[i].Transition.pResource   = inputs[i];
            bars[i].Transition.StateBefore = kUAV;
            bars[i].Transition.StateAfter  = kSRV;
            bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmdList->ResourceBarrier(8, bars);
    }

    // Temporal CB: { width, height, alpha, depthThresh, normalThresh, pad×3 }
    struct TemporalCB { uint32_t w, h; float alpha, depthThresh, normalThresh; uint32_t pad[3]; };
    float alpha = (reset || m_firstFrame) ? 1.0f : 0.1f;
    TemporalCB tcb{ m_width, m_height, alpha, 0.05f, 0.9f, {0, 0, 0} };
    cmdList->SetComputeRoot32BitConstants(0, 8, &tcb, 0);
    cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(0).gpu);
    cmdList->Dispatch(gx, gy, 1);

    uavBarrier(m_accumPing.Get());
    uavBarrier(m_momentsCur.Get());

    // ── Wavelet passes ────────────────────────────────────────────
    cmdList->SetComputeRootSignature(m_waveletRS.Get());
    cmdList->SetPipelineState(m_waveletPSO.Get());

    // accumPing, momentsCur: UAV → SRV (웨이블릿 입력)
    transition(m_accumPing.Get(),  kUAV, kSRV);
    transition(m_momentsCur.Get(), kUAV, kSRV);
    // depth, normals 이미 SRV 상태

    // Wavelet CB 구조
    struct WaveletCB { uint32_t w, h, step; float phi; uint32_t tonemap; float sigmaD, sigmaN; uint32_t pad; };

    // Pass 0 (step=1): accumPing → waveletPing
    {
        WaveletCB wcb{ m_width, m_height, 1u, 4.0f, 0u, 0.05f, 0.15f, 0u };
        cmdList->SetComputeRoot32BitConstants(0, 8, &wcb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(10).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(m_waveletPing.Get());
    transition(m_waveletPing.Get(), kUAV, kSRV);

    // Pass 1 (step=2): waveletPing → waveletPong
    {
        WaveletCB wcb{ m_width, m_height, 2u, 4.0f, 0u, 0.05f, 0.15f, 0u };
        cmdList->SetComputeRoot32BitConstants(0, 8, &wcb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(15).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(m_waveletPong.Get());
    transition(m_waveletPong.Get(), kUAV, kSRV);

    // Pass 2 (step=4, tonemap): waveletPong → outputColor
    {
        WaveletCB wcb{ m_width, m_height, 4u, 4.0f, 1u, 0.05f, 0.15f, 0u };
        cmdList->SetComputeRoot32BitConstants(0, 8, &wcb, 0);
        cmdList->SetComputeRootDescriptorTable(1, m_heap.GetHandle(20).gpu);
        cmdList->Dispatch(gx, gy, 1);
    }
    uavBarrier(outputColor);

    // ── 모든 SRV → UAV 복원 (배치) ───────────────────────────────
    {
        ID3D12Resource* srvResources[12] = {
            curIllum, motionVec, depth, normals,
            m_accumPong.Get(), m_momentsPrev.Get(), m_prevDepth.Get(), m_prevNormal.Get(),
            m_accumPing.Get(), m_momentsCur.Get(), m_waveletPing.Get(), m_waveletPong.Get()
        };
        D3D12_RESOURCE_BARRIER bars[12]{};
        for (int i = 0; i < 12; ++i)
        {
            bars[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bars[i].Transition.pResource   = srvResources[i];
            bars[i].Transition.StateBefore = kSRV;
            bars[i].Transition.StateAfter  = kUAV;
            bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmdList->ResourceBarrier(12, bars);
    }

    // ── 히스토리 버퍼 업데이트 (CopyResource) ────────────────────
    // UAV→COPY 배치, 복사 4개, COPY→UAV 배치
    {
        D3D12_RESOURCE_BARRIER toSrc[4]{}, toDst[4]{};
        ID3D12Resource* srcRes[4] = { m_accumPing.Get(), m_momentsCur.Get(), depth, normals };
        ID3D12Resource* dstRes[4] = { m_accumPong.Get(), m_momentsPrev.Get(), m_prevDepth.Get(), m_prevNormal.Get() };

        for (int i = 0; i < 4; ++i)
        {
            toSrc[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toSrc[i].Transition.pResource   = srcRes[i];
            toSrc[i].Transition.StateBefore = kUAV;
            toSrc[i].Transition.StateAfter  = kSRC;
            toSrc[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            toDst[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toDst[i].Transition.pResource   = dstRes[i];
            toDst[i].Transition.StateBefore = kUAV;
            toDst[i].Transition.StateAfter  = kDST;
            toDst[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }

        // 전환: src×4 → COPY_SOURCE, dst×4 → COPY_DEST
        D3D12_RESOURCE_BARRIER copyBars[8]{};
        for (int i = 0; i < 4; ++i) { copyBars[i]     = toSrc[i]; copyBars[4 + i] = toDst[i]; }
        cmdList->ResourceBarrier(8, copyBars);

        for (int i = 0; i < 4; ++i)
            cmdList->CopyResource(dstRes[i], srcRes[i]);

        // 복원: COPY_SOURCE/COPY_DEST → UAV
        for (int i = 0; i < 4; ++i)
        {
            copyBars[i].Transition.StateBefore     = kSRC;
            copyBars[i].Transition.StateAfter      = kUAV;
            copyBars[4 + i].Transition.StateBefore = kDST;
            copyBars[4 + i].Transition.StateAfter  = kUAV;
        }
        cmdList->ResourceBarrier(8, copyBars);
    }

    m_firstFrame = false;
}

// ---------------------------------------------------------------
void SVGFDenoiser::Shutdown()
{
    m_accumPing.Reset();
    m_accumPong.Reset();
    m_momentsCur.Reset();
    m_momentsPrev.Reset();
    m_prevDepth.Reset();
    m_prevNormal.Reset();
    m_waveletPing.Reset();
    m_waveletPong.Reset();
    m_temporalPSO.Reset();
    m_temporalRS.Reset();
    m_waveletPSO.Reset();
    m_waveletRS.Reset();
    m_initialized      = false;
    m_descriptorsBuilt = false;
    m_firstFrame       = true;
    m_device           = nullptr;
}
