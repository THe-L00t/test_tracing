#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
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
