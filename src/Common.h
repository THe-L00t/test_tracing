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
struct alignas(16) SceneCB
{
    float    camPos[3];      uint32_t sceneID;        // 16
    float    camRight[3];    float    tanHalfFovY;     // 16
    float    camUp[3];       float    aspectRatio;     // 16
    float    camForward[3];  float    _pad0;           // 16
    float    lightPos[3];    float    lightRadius;     // 16
    float    lightColor[3];  float    lightIntensity;  // 16
    // 4개 재질 × (float4 albedoRoughness + float4 metallic)
    float    matAlbedoRoughness[4][4]; // [mat][0..3] = albedo.xyz + roughness  (64)
    float    matMetallic[4][4];        // [mat][0] = metallic                   (64)
    // 96 + 128 = 224, 256까지 패딩
    float    _fill[8];
};
static_assert(sizeof(SceneCB) == 256, "SceneCB must be 256 bytes");
