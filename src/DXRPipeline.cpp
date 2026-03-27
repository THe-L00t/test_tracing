#include "DXRPipeline.h"
#include <d3dcompiler.h>
#include <print>
#include <fstream>
#include <filesystem>

namespace
{
    // HLSL 파일을 DXIL로 컴파일 (런타임 컴파일, 개발용)
    ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& path,
                                   const char* target)
    {
        ComPtr<ID3DBlob> code, err;
        UINT flags = D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        HRESULT hr = D3DCompileFromFile(
            path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "", target, flags, 0, &code, &err);

        if (FAILED(hr))
        {
            if (err)
                std::println("[셰이더 컴파일 오류] {}",
                             static_cast<const char*>(err->GetBufferPointer()));
            ThrowIfFailed(hr, "셰이더 컴파일 실패");
        }
        return code;
    }

    // 셰이더 DXIL 파일 읽기
    std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open())
            throw std::runtime_error(std::format("파일 열기 실패: {}", path.string()));
        const auto size = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        f.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }
}

// ---------------------------------------------------------------
// 글로벌 루트 시그니처
// 슬롯: u0 = 출력 UAV, t0 = TLAS SRV
// ---------------------------------------------------------------
ComPtr<ID3D12RootSignature> CreateGlobalRootSignature(ID3D12Device* device)
{
    // 디스크립터 테이블: u0 (UAV) + t0 (SRV)
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};

    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0;
    ranges[0].RegisterSpace                     = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 0;
    ranges[1].RegisterSpace                     = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER1 param{};
    param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges   = ranges;
    param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsDesc.Desc_1_1.NumParameters     = 1;
    rsDesc.Desc_1_1.pParameters       = &param;
    rsDesc.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeVersionedRootSignature(&rsDesc, &blob, &err));

    ComPtr<ID3D12RootSignature> rs;
    ThrowIfFailed(device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    return rs;
}

// ---------------------------------------------------------------
// RTPSO 생성
// ---------------------------------------------------------------
void DXRPipeline::Init(ID3D12Device5* device, ID3D12RootSignature* globalRootSig)
{
    // 셰이더 컴파일 (lib_6_3 타겟)
    auto shaderBlob = CompileShader(L"shaders/Raytracing.hlsl", "lib_6_3");

    // ─── 서브오브젝트 ───────────────────────────────────────────
    // 1. DXIL 라이브러리
    D3D12_EXPORT_DESC exports[] = {
        { L"RayGen",     nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"MissShader", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"ClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE },
    };
    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary = { shaderBlob->GetBufferPointer(),
                            shaderBlob->GetBufferSize() };
    libDesc.NumExports  = static_cast<UINT>(std::size(exports));
    libDesc.pExports    = exports;

    // 2. 히트 그룹
    D3D12_HIT_GROUP_DESC hitGroup{};
    hitGroup.HitGroupExport         = L"HitGroup";
    hitGroup.Type                   = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = L"ClosestHit";

    // 3. 셰이더 설정 (페이로드 = float4 색상, 4바이트 * 4)
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes   = sizeof(float) * 4;
    shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2; // barycentrics

    // 4. 파이프라인 설정 (재귀 없음)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = 1;

    // 5. 글로벌 루트 시그니처
    D3D12_GLOBAL_ROOT_SIGNATURE globalRS{};
    globalRS.pGlobalRootSignature = globalRootSig;

    // ─── 서브오브젝트 배열 ──────────────────────────────────────
    D3D12_STATE_SUBOBJECT subobjects[5]{};
    subobjects[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,         &libDesc        };
    subobjects[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,            &hitGroup       };
    subobjects[2] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig };
    subobjects[3] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig };
    subobjects[4] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRS      };

    D3D12_STATE_OBJECT_DESC soDesc{};
    soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = static_cast<UINT>(std::size(subobjects));
    soDesc.pSubobjects   = subobjects;

    ThrowIfFailed(device->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_pso)));
    ThrowIfFailed(m_pso.As(&m_psoProps));

    std::println("[DXRPipeline] RTPSO 생성 완료");
}

void DXRPipeline::BuildShaderTable(ID3D12Device* device)
{
    ShaderTable::Desc desc{};
    desc.rayGenID   = m_psoProps->GetShaderIdentifier(L"RayGen");
    desc.missID     = m_psoProps->GetShaderIdentifier(L"MissShader");
    desc.hitGroupID = m_psoProps->GetShaderIdentifier(L"HitGroup");
    m_shaderTable.Build(device, desc);
    std::println("[DXRPipeline] 셰이더 테이블 구축 완료");
}
