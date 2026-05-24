#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

// HRESULT 체크 매크로
inline void ThrowIfFailed(HRESULT hr, std::string_view msg = "")
{
    if (FAILED(hr))
    {
        char* winMsg = nullptr;
        FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(hr),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<char*>(&winMsg), 0, nullptr);

        std::string desc = winMsg ? winMsg : "";
        if (winMsg) LocalFree(winMsg);
        // 줄바꿈 제거
        if (!desc.empty() && desc.back() == '\n') desc.pop_back();
        if (!desc.empty() && desc.back() == '\r') desc.pop_back();

        auto err = std::format("HRESULT 0x{:08X} ({}){}", static_cast<uint32_t>(hr),
                               desc, msg.empty() ? "" : std::format(" [{}]", msg));
        throw std::runtime_error(err);
    }
}

// DXR을 위한 D3D12 확장 인터페이스
using ID3D12Device5Ptr        = ComPtr<ID3D12Device5>;
using ID3D12GraphicsCommandList4Ptr = ComPtr<ID3D12GraphicsCommandList4>;
using ID3D12Resource1Ptr      = ComPtr<ID3D12Resource>;

inline constexpr uint32_t k_backBufferCount = 2;
inline constexpr uint32_t k_defaultWidth    = 1280;
inline constexpr uint32_t k_defaultHeight   = 720;
inline constexpr DXGI_FORMAT k_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// 크기 정렬 헬퍼
inline constexpr uint64_t AlignUp(uint64_t size, uint64_t alignment)
{
    return (size + alignment - 1) & ~(alignment - 1);
}

// 정점 포맷: 위치 + 노멀 (24바이트)
struct VertexPN
{
    float pos[3];
    float normal[3];
};

// 씬 상수 버퍼 (256바이트, 16바이트 정렬)
// InstanceID 인코딩: 하위4비트=geomType(0=plane,1=cube,2=room), 상위비트=matIdx
struct alignas(16) SceneCB
{
    // 카메라 (64)
    float    camPos[3];       uint32_t sceneID;
    float    camRight[3];     float    tanHalfFovY;
    float    camUp[3];        float    aspectRatio;
    float    camForward[3];   float    _pad0;

    // 광원 1 (32)
    float    lightPos[3];     float    lightRadius;
    float    lightColor[3];   float    lightIntensity;

    // 광원 2 – 씬2 전용 (32)
    float    light2Pos[3];    float    light2Radius;
    float    light2Color[3];  float    light2Intensity;

    // 재질 4개 (albedo.xyz + roughness) (64)
    float    matAlbedoRoughness[4][4];

    // 재질 4개의 metallic을 float4 하나로 압축 (16)
    float    matMetallic[4];
    // 발광 강도 per 재질 (0=비발광, >0이면 albedo×강도 방출) (16)
    float    matEmissive[4];

    // 패스 트레이싱 + 발광 박스 면광원 (32)
    uint32_t frameCount;
    uint32_t randomSeed;
    float    emissBoxHalfSize;    // 발광 박스 반크기 (0=없음)
    float    jitterX;             // DLSS Halton 지터 X [-0.5, 0.5] (비DLSS 시 0)
    float    emissBoxCenter[3];   // 발광 박스 중심
    float    jitterY;             // DLSS Halton 지터 Y [-0.5, 0.5] (비DLSS 시 0)
};
// 검증: 64+32+32+64+16+16+16+16 = 256
static_assert(sizeof(SceneCB) == 256, "SceneCB must be 256 bytes");
