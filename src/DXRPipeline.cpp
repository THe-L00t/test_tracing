#include "DXRPipeline.h"
#include <dxcapi.h>
#include <print>
#include <filesystem>

namespace
{
    // DXC를 사용한 HLSL lib_6_3 컴파일
    // D3DCompileFromFile(FXC)은 SM6.x를 지원하지 않으므로 DXC 필수
    ComPtr<IDxcBlob> CompileShaderDXC(const std::filesystem::path& path)
    {
        ComPtr<IDxcUtils> utils;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)),
                      "DxcUtils 생성 실패");

        ComPtr<IDxcCompiler3> compiler;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)),
                      "DxcCompiler3 생성 실패");

        // 소스 파일 로드
        ComPtr<IDxcBlobEncoding> sourceBlob;
        ThrowIfFailed(utils->LoadFile(path.c_str(), nullptr, &sourceBlob),
                      std::format("셰이더 파일 열기 실패: {}", path.string()));

        DxcBuffer source{};
        source.Ptr      = sourceBlob->GetBufferPointer();
        source.Size     = sourceBlob->GetBufferSize();
        source.Encoding = DXC_CP_UTF8;

        // 컴파일 인수
        std::wstring filenameStr = path.filename().wstring();
        std::vector<LPCWSTR> args = {
            filenameStr.c_str(),
            L"-T", L"lib_6_3",
            L"-HV", L"2021",  // HLSL 2021
#if defined(_DEBUG)
            L"-Zi",           // 디버그 정보
            L"-Od",           // 최적화 비활성화
#else
            L"-O3",
#endif
        };

        ComPtr<IDxcResult> result;
        ThrowIfFailed(compiler->Compile(
            &source,
            args.data(), static_cast<UINT32>(args.size()),
            nullptr,
            IID_PPV_ARGS(&result)), "DXC Compile 호출 실패");

        // 컴파일 오류 확인
        HRESULT hr = S_OK;
        result->GetStatus(&hr);
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            if (errors && errors->GetStringLength() > 0)
                std::println("[DXC 컴파일 오류]\n{}", errors->GetStringPointer());
            ThrowIfFailed(hr, "셰이더 컴파일 실패");
        }

        ComPtr<IDxcBlob> dxilBlob;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxilBlob), nullptr);
        return dxilBlob;
    }
}

// ---------------------------------------------------------------
// 글로벌 루트 시그니처
// 파라미터 0: 디스크립터 테이블
//   힙 슬롯 0: UAV u0 (g_output,       RGBA8)
//   힙 슬롯 1: UAV u1 (g_accumulation, RGBA32F)
//   힙 슬롯 2: SRV t0 (TLAS)
//   힙 슬롯 3: SRV t1 (plane VB)
//   힙 슬롯 4: SRV t2 (cube VB)
//   힙 슬롯 5: SRV t3 (room VB)
//   힙 슬롯 6: SRV t4 (sphere VB)
// 파라미터 1: 인라인 루트 CBV b0 (씬 상수)
// ---------------------------------------------------------------
ComPtr<ID3D12RootSignature> CreateGlobalRootSignature(ID3D12Device* device)
{
    // ── 디스크립터 테이블 레이아웃 ──────────────────────────────
    // Range 0: UAV u0-u1  (2개,  힙 offset 0)  output + accumulation
    // Range 1: SRV t0-t4  (5개,  힙 offset 2)  TLAS + VBs
    // Range 2: UAV u2-u8  (7개,  힙 offset 7)  G-Buffer(4) + Reservoir(2) + MotionVec(1)
    // Range 3: SRV t5-t12 (8개,  힙 offset 14) LightList(1) + G-Buffer SRV(4) + MotionVec SRV(1) + Reservoir SRV(2)
    //
    // 힙 슬롯 요약:
    //  0: UAV u0 (g_output)        7: UAV u2 (gbuf_worldPos)   14: SRV t5 (lightList)
    //  1: UAV u1 (g_accumulation)  8: UAV u3 (gbuf_normal)     15: SRV t6 (gbuf_worldPos)
    //  2: SRV t0 (TLAS)            9: UAV u4 (gbuf_albedo)     16: SRV t7 (gbuf_normal)
    //  3: SRV t1 (plane VB)       10: UAV u5 (gbuf_matInfo)    17: SRV t8 (gbuf_albedo)
    //  4: SRV t2 (cube VB)        11: UAV u6 (reservoir_cur)   18: SRV t9 (gbuf_matInfo)
    //  5: SRV t3 (room VB)        12: UAV u7 (reservoir_prev)  19: SRV t10(motionVec)
    //  6: SRV t4 (sphere VB)      13: UAV u8 (motionVec)       20: SRV t11(reservoir_in)
    //                                                           21: SRV t12(reservoir_cur SRV)

    D3D12_DESCRIPTOR_RANGE1 ranges[4]{};

    // Range 0: UAV u0-u1 (기존 출력 버퍼)
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors                    = 2;
    ranges[0].BaseShaderRegister                = 0;
    ranges[0].RegisterSpace                     = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[0].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    // Range 1: SRV t0-t4 (기존 TLAS + VBs)
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors                    = 5;
    ranges[1].BaseShaderRegister                = 0;
    ranges[1].RegisterSpace                     = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    ranges[1].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    // Range 2: UAV u2-u8 (ReSTIR G-Buffer + Reservoir + MotionVec)
    ranges[2].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[2].NumDescriptors                    = 7;
    ranges[2].BaseShaderRegister                = 2;   // u2 시작
    ranges[2].RegisterSpace                     = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = 7;   // 힙 슬롯 7
    ranges[2].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    // Range 3: SRV t5-t12 (LightList + G-Buffer SRV + MotionVec SRV + Reservoir SRV)
    ranges[3].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].NumDescriptors                    = 8;
    ranges[3].BaseShaderRegister                = 5;   // t5 시작
    ranges[3].RegisterSpace                     = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = 14;  // 힙 슬롯 14
    ranges[3].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    D3D12_ROOT_PARAMETER1 params[3]{};

    // 파라미터 0: 디스크립터 테이블 (4 ranges)
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 4;
    params[0].DescriptorTable.pDescriptorRanges   = ranges;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    // 파라미터 1: 인라인 루트 CBV b0 (SceneCB, 기존)
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace  = 0;
    params[1].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // 파라미터 2: 인라인 루트 CBV b1 (ReSTIRCB, 신규)
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].Descriptor.RegisterSpace  = 0;
    params[2].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsDesc.Desc_1_1.NumParameters     = 3;
    rsDesc.Desc_1_1.pParameters       = params;
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
// 익스포트: RayGen, MissShader, MissShadow, ClosestHit
// ---------------------------------------------------------------
void DXRPipeline::Init(ID3D12Device5* device, ID3D12RootSignature* globalRootSig)
{
    // DXC로 lib_6_3 셰이더 컴파일
    auto dxilBlob = CompileShaderDXC(L"shaders/Raytracing.hlsl");

    // 1. DXIL 라이브러리 (4개 진입점 익스포트)
    D3D12_EXPORT_DESC exports[] = {
        { L"RayGen",     nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"MissShader", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"MissShadow", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"ClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE },
    };
    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary = { dxilBlob->GetBufferPointer(), dxilBlob->GetBufferSize() };
    libDesc.NumExports  = static_cast<UINT>(std::size(exports));
    libDesc.pExports    = exports;

    // 2. 히트 그룹 (ClosestHit만 포함, MissShadow는 별도 Miss 슬롯)
    D3D12_HIT_GROUP_DESC hitGroup{};
    hitGroup.HitGroupExport         = L"HitGroup";
    hitGroup.Type                   = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = L"ClosestHit";

    // 3. 셰이더 설정
    //    RayPayload: float3×4 + float(scatterPdf) + uint×3 = 48+4+12 = 64 bytes
    //    ShadowPayload: float = 4 bytes
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes   = 64;  // RayPayload 크기
    shaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2;  // 배리센트릭

    // 4. 파이프라인 설정
    //    재귀 깊이 2: RayGen→ClosestHit(1)→ShadowRay(2)
    //    iterative bounce이므로 반사 재귀 없음
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = 2;

    // 5. 글로벌 루트 시그니처
    D3D12_GLOBAL_ROOT_SIGNATURE globalRS{};
    globalRS.pGlobalRootSignature = globalRootSig;

    D3D12_STATE_SUBOBJECT subobjects[5]{};
    subobjects[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,               &libDesc        };
    subobjects[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,                  &hitGroup       };
    subobjects[2] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,   &shaderConfig   };
    subobjects[3] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig };
    subobjects[4] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,      &globalRS       };

    D3D12_STATE_OBJECT_DESC soDesc{};
    soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = static_cast<UINT>(std::size(subobjects));
    soDesc.pSubobjects   = subobjects;

    ThrowIfFailed(device->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_pso)),
                  "RTPSO 생성 실패");
    ThrowIfFailed(m_pso.As(&m_psoProps));

    std::println("[DXRPipeline] RTPSO 생성 완료 (RayGen/MissShader/MissShadow/ClosestHit)");
}

void DXRPipeline::BuildShaderTable(ID3D12Device* device)
{
    ShaderTable::Desc desc{};
    desc.rayGenID   = m_psoProps->GetShaderIdentifier(L"RayGen");
    desc.missID     = m_psoProps->GetShaderIdentifier(L"MissShader");
    desc.missID2    = m_psoProps->GetShaderIdentifier(L"MissShadow");
    desc.hitGroupID = m_psoProps->GetShaderIdentifier(L"HitGroup");
    m_shaderTable.Build(device, desc);
    std::println("[DXRPipeline] 셰이더 테이블 구축 완료 (4 레코드)");
}
