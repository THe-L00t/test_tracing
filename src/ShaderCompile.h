#pragma once
#include "Common.h"
#include <dxcapi.h>
#include <print>

#pragma comment(lib, "dxcompiler.lib")

// HPAR-PT 공통 DXC 컴파일 헬퍼 (Phase 1 +에서 추가된 compute 패스용)
//   기존 DXRPipeline/SVGFDenoiser/Denoiser 의 자체 컴파일 코드는 페이즈 경계 원칙에 따라 그대로 유지.
//   HPAR-PT 페이즈에서 새로 만드는 compute 패스만 이 헬퍼 사용.
//
// 사용 예: auto blob = CompileComputeCS(L"shaders/Foo.hlsl", L"main");
inline ComPtr<IDxcBlob> CompileComputeCS(const wchar_t* path,
                                          const wchar_t* entry  = L"main",
                                          const wchar_t* target = L"cs_6_0")
{
    ComPtr<IDxcUtils>      utils;
    ComPtr<IDxcCompiler3>  compiler;
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils,    IID_PPV_ARGS(&utils)));
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

    ComPtr<IDxcIncludeHandler> inc;
    utils->CreateDefaultIncludeHandler(&inc);

    ComPtr<IDxcBlobEncoding> src;
    HRESULT hr = utils->LoadFile(path, nullptr, &src);
    if (FAILED(hr))
    {
        std::println("[ShaderCompile] 셰이더 파일 로드 실패: {}", (const char*)nullptr);
        return nullptr;
    }

    DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
    LPCWSTR args[] = { L"-E", entry, L"-T", target, L"-Zi", L"-Qembed_debug" };

    ComPtr<IDxcResult> res;
    ThrowIfFailed(compiler->Compile(&buf, args, _countof(args), inc.Get(), IID_PPV_ARGS(&res)));

    ComPtr<IDxcBlobUtf8> errs;
    res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr);
    if (errs && errs->GetStringLength() > 0)
        std::println("[ShaderCompile] {}", (const char*)errs->GetBufferPointer());

    HRESULT status;
    res->GetStatus(&status);
    if (FAILED(status))
        return nullptr;

    ComPtr<IDxcBlob> blob;
    res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
    return blob;
}
