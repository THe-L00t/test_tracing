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
    float    _cbPad1;
    float    emissBoxCenter[3];   // 발광 박스 중심
    float    _cbPad2;
};
// 검증: 64+32+32+64+16+16+16+16 = 256
static_assert(sizeof(SceneCB) == 256, "SceneCB must be 256 bytes");

// ──────────────────────────────────────────────────────────────
//  ReSTIR 전용 상수 버퍼 (b1, 128바이트)
//  SceneCB가 꽉 차 있으므로 ReSTIR 파라미터는 별도 CBV로 분리
// ──────────────────────────────────────────────────────────────
struct alignas(16) ReSTIRCB
{
    // 이전 프레임 카메라 (재투영용) (64)
    float    prevCamPos[3];      float _p0;
    float    prevCamRight[3];    float prevTanHalfFovY;
    float    prevCamUp[3];       float prevAspectRatio;
    float    prevCamForward[3];  float _p1;

    // ReSTIR 파라미터 (32)
    uint32_t lightCount;         // 씬 광원 총 개수
    uint32_t candidateCount;     // RIS 후보 수 M (기본 32)
    uint32_t screenW;
    uint32_t screenH;
    uint32_t frameIndex;
    float    temporalMaxM;       // 시간적 누적 M 클램프 (ghosting 방지, 기본 30)
    uint32_t spatialRadius;      // 공간 재사용 반경 (픽셀, 기본 30)
    uint32_t spatialSamples;     // 공간 재사용 이웃 샘플 수 (기본 5)

    // 패딩 (32)
    float    _pad[8];
};
static_assert(sizeof(ReSTIRCB) == 128, "ReSTIRCB must be 128 bytes");

// ──────────────────────────────────────────────────────────────
//  광원 데이터 (StructuredBuffer<LightData>, t5)
//  SceneCB의 하드코딩 광원을 대체 – ReSTIR은 동적 광원 리스트 필요
// ──────────────────────────────────────────────────────────────
struct LightData
{
    float    pos[3];       // 포인트/스팟: 위치, 방향광: 정규화 방향
    float    intensity;
    float    color[3];
    uint32_t type;         // 0=point, 1=directional, 2=area(box)
    // area light 전용
    float    halfSize;     // box half-extent (type==2일 때)
    float    center[3];    // box center      (type==2일 때)
};
// 32 bytes, StructuredBuffer stride = 32

// ──────────────────────────────────────────────────────────────
//  Reservoir (GPU StructuredBuffer, 16바이트 정렬)
//  RIS 가중치 합산 및 선택된 광원 샘플 저장
// ──────────────────────────────────────────────────────────────
struct alignas(16) Reservoir
{
    uint32_t lightIdx;   // 선택된 광원 인덱스 (UINT_MAX = 유효하지 않음)
    float    wSum;       // 누적 가중치 Σw_i
    float    W;          // 비편향 기여 가중치 = wSum / (M * p_hat(y))
    uint32_t M;          // 시도한 후보 수
};
static_assert(sizeof(Reservoir) == 16, "Reservoir must be 16 bytes");
