#include "DLSSIntegration.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_params_dlssd.h"
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
                            D3D12_GPU_VIRTUAL_ADDRESS   tlasGpuVA,
                            ID3D12Resource*             planeVb,   uint32_t planeVertCount,
                            ID3D12Resource*             cubeVb,    uint32_t cubeVertCount,
                            ID3D12Resource*             roomVb,    uint32_t roomVertCount,
                            ID3D12Resource*             sphereVb,  uint32_t sphereVertCount)
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
    m_renderColor  = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_renderAccum  = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R32G32B32A32_FLOAT);
    m_dlssOutput   = MakeUAVTex(device, displayW,  displayH,  DXGI_FORMAT_R8G8B8A8_UNORM);
    m_depth        = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R32_FLOAT);
    m_motionVec    = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16_FLOAT);
    m_renderNormal = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16_FLOAT);

    m_renderColor ->SetName(L"DLSS_RenderColor");
    m_renderAccum ->SetName(L"DLSS_RenderAccum");
    m_dlssOutput  ->SetName(L"DLSS_Output");
    m_renderNormal->SetName(L"DLSS_RenderNormal");

    // ── 4. 공유 힙에 UAV/SRV 등록 ───────────────────────────────
    // shader-visible 힙은 CPU read 불가 → CopyDescriptors 사용 불가
    // 리소스 포인터로 SRV를 직접 생성한다.
    // 비DLSS 슬롯 0..9 선점 후 → DLSS는 슬롯 10부터 시작

    // 슬롯 10: UAV m_renderColor (u0)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateUnorderedAccessView(m_renderColor.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 11: UAV m_renderAccum (u1)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(m_renderAccum.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 12: UAV m_depth (u2)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(m_depth.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 13: UAV m_motionVec (u3)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateUnorderedAccessView(m_motionVec.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 14: UAV m_renderNormal (u4, oct-법선 — A-trous 디노이저 입력)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateUnorderedAccessView(m_renderNormal.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 15: SRV TLAS 미러
    {
        DescriptorHandle h = sharedHeap.Allocate();
        m_tlasMirrorCPU = h.cpu;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension            = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srvDesc.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.RaytracingAccelerationStructure.Location = tlasGpuVA;
        device->CreateShaderResourceView(nullptr, &srvDesc, h.cpu);
    }
    // 슬롯 16..19: SRV VB 미러 (plane, cube, room, sphere)
    auto makeVbSRV = [&](ID3D12Resource* vb, uint32_t cnt)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement        = 0;
        srvDesc.Buffer.NumElements         = cnt;
        srvDesc.Buffer.StructureByteStride = 24; // sizeof(VertexPN): float3 pos + float3 normal
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(vb, &srvDesc, h.cpu);
    };
    makeVbSRV(planeVb,  planeVertCount);
    makeVbSRV(cubeVb,   cubeVertCount);
    makeVbSRV(roomVb,   roomVertCount);
    makeVbSRV(sphereVb, sphereVertCount);

    // ── 5. DLSS Ray Reconstruction 피처 생성 (RR 전용, 폴백 없음) ──
    // 입력: raw 1spp HDR (m_renderAccum RGBA32F) + depth + MV + normals
    // RR은 AI 모델이 denoising + temporal + upscaling을 1패스로 처리
    int rrAvailable = 0;
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rrAvailable);
    if (!rrAvailable)
        std::println("[DLSS-RR] WARNING: Available 플래그 0 — DLL이 로드되지 않으면 CreateFeature 실패");

    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_CreationNodeMask,    1);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_VisibilityNodeMask,  1);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_Width,               m_renderW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_Height,              m_renderH);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_OutWidth,            displayW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_OutHeight,           displayH);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_PerfQualityValue,    NVSDK_NGX_PerfQuality_Value_MaxPerf);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_DLSS_Denoise_Mode,   NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Unpacked);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_Use_HW_Depth,        NVSDK_NGX_DLSS_Depth_Type_Linear);

    res = NVSDK_NGX_D3D12_CreateFeature(cmdList, NVSDK_NGX_Feature_RayReconstruction, m_params, &m_feature);
    if (NVSDK_NGX_FAILED(res))
    {
        std::println("[DLSS-RR] CreateFeature 실패: 0x{:08X}", static_cast<uint32_t>(res));
        std::println("[DLSS-RR] nvngx_dlssd.dll이 실행 파일과 같은 폴더에 있는지 확인");
        NVSDK_NGX_D3D12_DestroyParameters(m_params);
        m_params = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }

    m_available = true;
    std::println("[DLSS-RR] Ray Reconstruction 초기화 완료 ({}x{} → {}x{}) — U 키로 토글",
                 m_renderW, m_renderH, displayW, displayH);
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
                                      D3D12_GPU_VIRTUAL_ADDRESS tlasGpuVA)
{
    if (!m_available) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension            = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = tlasGpuVA;
    device->CreateShaderResourceView(nullptr, &srvDesc, m_tlasMirrorCPU);
}

// ── UAVBarriers ───────────────────────────────────────────────────
void DLSSIntegration::UAVBarriers(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barriers[5]{};
    barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_renderColor.Get();
    barriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_renderAccum.Get();
    barriers[2].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].UAV.pResource = m_depth.Get();
    barriers[3].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[3].UAV.pResource = m_motionVec.Get();
    barriers[4].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[4].UAV.pResource = m_renderNormal.Get();
    cmdList->ResourceBarrier(5, barriers);
}

// ── Evaluate ──────────────────────────────────────────────────────
void DLSSIntegration::Evaluate(ID3D12GraphicsCommandList* cmdList,
                                float jitterX, float jitterY, bool reset)
{
    auto makeTransition = [](ID3D12Resource* res,
                              D3D12_RESOURCE_STATES before,
                              D3D12_RESOURCE_STATES after) -> D3D12_RESOURCE_BARRIER
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return b;
    };

    // RR 입력: raw HDR 1spp (renderAccum), depth, motionVec, normals → SRV
    D3D12_RESOURCE_BARRIER toSRV[4] = {
        makeTransition(m_renderAccum.Get(),  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_depth.Get(),        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_motionVec.Get(),    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_renderNormal.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cmdList->ResourceBarrier(4, toSRV);

    // RR 파라미터: raw 1spp HDR(RGBA32F) + G-Buffer normals
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Color,           m_renderAccum.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Output,          m_dlssOutput.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Depth,           m_depth.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_MotionVectors,   m_motionVec.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Normals, m_renderNormal.Get());
    NVSDK_NGX_Parameter_SetF (m_params, NVSDK_NGX_Parameter_Jitter_Offset_X, jitterX);
    NVSDK_NGX_Parameter_SetF (m_params, NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterY);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_Reset,    reset ? 1 : 0);
    NVSDK_NGX_Parameter_SetF (m_params, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    NVSDK_NGX_Parameter_SetF (m_params, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,  m_renderW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, m_renderH);
    NVSDK_NGX_Parameter_SetF (m_params, NVSDK_NGX_Parameter_DLSS_Pre_Exposure,   1.0f);
    NVSDK_NGX_Parameter_SetF (m_params, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);

    NVSDK_NGX_Result res = NVSDK_NGX_D3D12_EvaluateFeature_C(cmdList, m_feature, m_params, NULL);
    if (NVSDK_NGX_FAILED(res))
        std::println("[DLSS-RR] Evaluate 실패: 0x{:08X}", static_cast<uint32_t>(res));

    // SRV → UAV 복원
    D3D12_RESOURCE_BARRIER toUAV[4] = {
        makeTransition(m_renderAccum.Get(),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_depth.Get(),        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_motionVec.Get(),    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_renderNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmdList->ResourceBarrier(4, toUAV);
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
