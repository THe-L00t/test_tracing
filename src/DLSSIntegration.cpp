#include "DLSSIntegration.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include <print>

// ── 텍스처 생성 헬퍼 ─────────────────────────────────────────────
static ComPtr<ID3D12Resource> MakeUAVTex(ID3D12Device* dev,
    uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format           = fmt; rd.SampleDesc = {1, 0};
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

static ComPtr<ID3D12Resource> MakeSRVTex(ID3D12Device* dev,
    uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format           = fmt; rd.SampleDesc = {1, 0};
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

// ── Init ─────────────────────────────────────────────────────────
bool DLSSIntegration::Init(ID3D12Device*               device,
                            ID3D12GraphicsCommandList*  cmdList,
                            uint32_t                    displayW,
                            uint32_t                    displayH,
                            DescriptorHeap&             sharedHeap,
                            D3D12_CPU_DESCRIPTOR_HANDLE tlasCPU,
                            D3D12_CPU_DESCRIPTOR_HANDLE planeVbCPU,
                            D3D12_CPU_DESCRIPTOR_HANDLE cubeVbCPU,
                            D3D12_CPU_DESCRIPTOR_HANDLE roomVbCPU,
                            D3D12_CPU_DESCRIPTOR_HANDLE sphereVbCPU)
{
    m_displayW = displayW;
    m_displayH = displayH;

    // ── 1. NGX 초기화 ─────────────────────────────────────────────
    NVSDK_NGX_Result res = NVSDK_NGX_D3D12_Init(0x534C5344 /*'DLSS'*/, L".", device);
    if (NVSDK_NGX_FAILED(res))
    {
        std::println("[DLSS] NGX 초기화 실패 (비-NVIDIA GPU 또는 구형 드라이버): 0x{:08X}",
                     static_cast<uint32_t>(res));
        return false;
    }

    res = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_params);
    if (NVSDK_NGX_FAILED(res))
    {
        std::println("[DLSS] GetCapabilityParameters 실패: 0x{:08X}",
                     static_cast<uint32_t>(res));
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }

    // ── 2. 렌더 해상도 쿼리 (Performance 모드 ≈ 50%) ─────────────
    uint32_t optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0.0f;
    res = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_params,
        displayW, displayH, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        &optW, &optH, &maxW, &maxH, &minW, &minH, &sharpness);

    if (NVSDK_NGX_FAILED(res) || optW == 0 || optH == 0)
    {
        // DLSS 미지원 GPU (AMD, Intel, 구형 NVIDIA)
        std::println("[DLSS] DLSS 미지원: 최적 해상도 쿼리 실패");
        NVSDK_NGX_D3D12_DestroyParameters(m_params);
        m_params = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }
    m_renderW = optW;
    m_renderH = optH;

    const float scale = 100.0f * float(m_renderW * m_renderH) / float(displayW * displayH);
    std::println("[DLSS] 렌더: {}x{} → 출력: {}x{} (Performance, {:.0f}% 픽셀)",
                 m_renderW, m_renderH, displayW, displayH, scale);

    // ── 3. 리소스 생성 ────────────────────────────────────────────
    m_renderColor = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_renderAccum = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R32G32B32A32_FLOAT);
    m_dlssOutput  = MakeUAVTex(device, displayW,  displayH,  DXGI_FORMAT_R8G8B8A8_UNORM);
    m_depth       = MakeSRVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R32_FLOAT);
    m_motionVec   = MakeSRVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16_FLOAT);

    m_renderColor->SetName(L"DLSS_RenderColor");
    m_renderAccum->SetName(L"DLSS_RenderAccum");
    m_dlssOutput ->SetName(L"DLSS_Output");

    // ── 4. 공유 힙에 UAV/SRV 등록 ───────────────────────────────
    // 슬롯 7: UAV m_renderColor (u0 용)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateUnorderedAccessView(m_renderColor.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 8: UAV m_renderAccum (u1 용)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(m_renderAccum.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 9..13: SRV 미러 (TLAS, plane, cube, room, sphere)
    const D3D12_CPU_DESCRIPTOR_HANDLE srcs[5] = {
        tlasCPU, planeVbCPU, cubeVbCPU, roomVbCPU, sphereVbCPU
    };
    for (int i = 0; i < 5; ++i)
    {
        DescriptorHandle dst = sharedHeap.Allocate();
        if (i == 0) m_tlasMirrorCPU = dst.cpu;
        device->CopyDescriptorsSimple(1, dst.cpu, srcs[i],
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // ── 5. DLSS 피처 생성 ─────────────────────────────────────────
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth           = m_renderW;
    cp.Feature.InHeight          = m_renderH;
    cp.Feature.InTargetWidth     = displayW;
    cp.Feature.InTargetHeight    = displayH;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;
    cp.InFeatureCreateFlags      = 0;  // LDR (RGBA8 톤맵 완료 입력)

    res = NGX_D3D12_CREATE_DLSS_EXT(cmdList, 1, 1, &m_feature, m_params, &cp);
    if (NVSDK_NGX_FAILED(res))
    {
        std::println("[DLSS] CreateFeature 실패: 0x{:08X}", static_cast<uint32_t>(res));
        NVSDK_NGX_D3D12_DestroyParameters(m_params);
        m_params = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }

    m_available = true;
    std::println("[DLSS] 초기화 완료 — U 키로 온/오프 토글");
    return true;
}

// ── Shutdown ──────────────────────────────────────────────────────
void DLSSIntegration::Shutdown(ID3D12Device* device)
{
    if (!m_available) return;
    if (m_feature)  { NVSDK_NGX_D3D12_ReleaseFeature(m_feature); m_feature = nullptr; }
    if (m_params)   { NVSDK_NGX_D3D12_DestroyParameters(m_params); m_params = nullptr; }
    NVSDK_NGX_D3D12_Shutdown1(device);
    m_available = false;
}

// ── RefreshTLASSRV ────────────────────────────────────────────────
void DLSSIntegration::RefreshTLASSRV(ID3D12Device* device,
                                      D3D12_CPU_DESCRIPTOR_HANDLE tlasCPU)
{
    if (!m_available) return;
    device->CopyDescriptorsSimple(1, m_tlasMirrorCPU, tlasCPU,
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// ── UAVBarriers ───────────────────────────────────────────────────
void DLSSIntegration::UAVBarriers(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_renderColor.Get();
    barriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_renderAccum.Get();
    cmdList->ResourceBarrier(2, barriers);
}

// ── Evaluate ──────────────────────────────────────────────────────
void DLSSIntegration::Evaluate(ID3D12GraphicsCommandList* cmdList,
                                float jitterX, float jitterY, bool reset)
{
    // m_renderColor: UAV → SRV (DLSS 가 컴퓨트로 읽음)
    D3D12_RESOURCE_BARRIER toSRV{};
    toSRV.Type                          = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSRV.Transition.pResource          = m_renderColor.Get();
    toSRV.Transition.StateBefore        = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSRV.Transition.StateAfter         = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toSRV.Transition.Subresource        = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toSRV);

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor            = m_renderColor.Get();
    ep.Feature.pInOutput           = m_dlssOutput.Get();
    ep.Feature.InSharpness         = 0.0f;
    ep.pInDepth                    = m_depth.Get();
    ep.pInMotionVectors            = m_motionVec.Get();
    ep.InJitterOffsetX             = jitterX;
    ep.InJitterOffsetY             = jitterY;
    ep.InReset                     = reset ? 1 : 0;
    ep.InMVScaleX                  = 1.0f;
    ep.InMVScaleY                  = 1.0f;
    ep.InRenderSubrectDimensions   = { m_renderW, m_renderH };

    NVSDK_NGX_Result res = NGX_D3D12_EVALUATE_DLSS_EXT(cmdList, m_feature, m_params, &ep);
    if (NVSDK_NGX_FAILED(res))
        std::println("[DLSS] Evaluate 실패: 0x{:08X}", static_cast<uint32_t>(res));

    // m_renderColor: SRV → UAV (다음 프레임 DispatchRays 에서 씀)
    D3D12_RESOURCE_BARRIER toUAV = toSRV;
    toUAV.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toUAV.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmdList->ResourceBarrier(1, &toUAV);
}

// ── CopyOutputToBackBuffer ────────────────────────────────────────
void DLSSIntegration::CopyOutputToBackBuffer(ID3D12GraphicsCommandList* cmdList,
                                              ID3D12Resource*             backBuffer)
{
    D3D12_RESOURCE_BARRIER barriers[2]{};

    // m_dlssOutput: UAV → COPY_SRC
    barriers[0].Type                          = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource          = m_dlssOutput.Get();
    barriers[0].Transition.StateBefore        = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter         = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource        = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // backBuffer: PRESENT → COPY_DEST
    barriers[1].Type                          = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource          = backBuffer;
    barriers[1].Transition.StateBefore        = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter         = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.Subresource        = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(2, barriers);
    cmdList->CopyResource(backBuffer, m_dlssOutput.Get());

    // 복원
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(2, barriers);
}
