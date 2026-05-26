#pragma once
#include "Common.h"
#include <vector>

// NRD API forward decls (피하기: 헤더에 NRD include 안 함)
namespace nrd { struct Instance; }

// NVIDIA Real-Time Denoiser (NRD) D3D12 native integration
//
// 파이프라인:
//   PathTracer (1spp lobe-separated radiance)
//     → NRDDenoiser::Denoise (RELAX_DIFFUSE_SPECULAR)
//     → 출력: 디노이즈된 diff/spec radiance + hitDist
//     → Composite (denoised * albedo) — 별도 패스
//     → DLSS-RR Evaluate
//
// 입력 (RT 셰이더가 매 프레임 기록):
//   - diffRadianceHitDist (RGBA16F): rgb=raw diff radiance, w=hitDist
//   - specRadianceHitDist (RGBA16F): rgb=raw spec radiance, w=hitDist
//   - normalRoughness    (RGBA8 또는 R10G10B10A2): NRD-encoded normal + roughness
//   - viewZ              (R16F): linear view-space Z
//   - motionVec          (RG16F): pixel-space MV (unjittered)
//
// 출력:
//   - outDiffRadianceHitDist (RGBA16F): 디노이즈된 diff
//   - outSpecRadianceHitDist (RGBA16F): 디노이즈된 spec
class NRDDenoiser
{
public:
    bool Init(ID3D12Device5* device, uint32_t renderW, uint32_t renderH);
    void Shutdown();

    struct DenoiseInputs
    {
        ID3D12Resource* diffRadianceHitDist;
        ID3D12Resource* specRadianceHitDist;
        ID3D12Resource* normalRoughness;   // NRD-encoded format
        ID3D12Resource* viewZ;
        ID3D12Resource* motionVec;
    };

    struct FrameSettings
    {
        float viewToClipMatrix[16];
        float viewToClipMatrixPrev[16];
        float worldToViewMatrix[16];
        float worldToViewMatrixPrev[16];
        float cameraJitter[2];
        float cameraJitterPrev[2];
        uint32_t frameIndex;
        bool     accumulationRestart;
    };

    // 컴퓨트 디스패치를 cmdList 에 기록한다. cmdList 가 열린 상태여야 함.
    // 호출 전: 입력 리소스들은 UAV 상태여야 함 (SRV 로 transition 은 내부 처리)
    void Denoise(ID3D12GraphicsCommandList* cmdList,
                 const DenoiseInputs& inputs,
                 const FrameSettings& settings);

    // 출력 접근 (Composite 패스가 SRV 로 사용)
    ID3D12Resource* DenoisedDiffuse()  const noexcept { return m_outDiffRadHitDist.Get(); }
    ID3D12Resource* DenoisedSpecular() const noexcept { return m_outSpecRadHitDist.Get(); }

    bool IsAvailable() const noexcept { return m_available; }

private:
    nrd::Instance* m_instance = nullptr;
    ID3D12Device5* m_device   = nullptr;

    // Shared compute root signature (모든 NRD 파이프라인이 공통 사용)
    //   param 0: root CBV b0 space1
    //   param 1: descriptor table SRV t0..tN space0 (per-dispatch textures)
    //   param 2: descriptor table UAV u0..uM space0 (per-dispatch storage textures)
    //   static samplers: NEAREST_CLAMP s0 / LINEAR_CLAMP s1 (space1)
    ComPtr<ID3D12RootSignature> m_rootSig;

    // 파이프라인 PSO (InstanceDesc::pipelines 와 1:1 대응)
    std::vector<ComPtr<ID3D12PipelineState>> m_pipelines;

    // NRD 텍스처 풀
    std::vector<ComPtr<ID3D12Resource>> m_permanentTextures;
    std::vector<ComPtr<ID3D12Resource>> m_transientTextures;

    // 디노이즈 출력 (별도 alloc — NRD ResourceType::OUT_DIFF_RADIANCE_HITDIST 매핑)
    ComPtr<ID3D12Resource> m_outDiffRadHitDist;
    ComPtr<ID3D12Resource> m_outSpecRadHitDist;

    // 디스크립터 힙 (NRD 전용, 매 프레임 ring buffer 식으로 재사용)
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    uint32_t                     m_descriptorHeapCapacity = 0;
    uint32_t                     m_descriptorHeapNext     = 0;
    uint32_t                     m_descriptorIncSize      = 0;

    // 상수 버퍼 (upload heap, dispatch 마다 다른 offset 사용)
    ComPtr<ID3D12Resource> m_constantBuffer;
    uint32_t               m_constantBufferCapacity = 0;
    uint32_t               m_constantBufferNext     = 0;
    uint8_t*               m_constantBufferMapped   = nullptr;

    // InstanceDesc 캐시 — Denoise() 에서 resource 매핑에 사용
    uint32_t m_perSetTexturesMaxNum        = 0;
    uint32_t m_perSetStorageTexturesMaxNum = 0;
    uint32_t m_constantBufferMaxDataSize   = 0;

    uint32_t m_renderW = 0;
    uint32_t m_renderH = 0;
    bool     m_available = false;
};
