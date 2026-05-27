#include "DLSSIntegration.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_params_dlssd.h"
#include <print>
#include <filesystem>
#include <string>
#include <windows.h>

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

    // ── 1. NGX 초기화 (EXE 디렉터리를 DLL 검색 경로로 명시) ───────
    // CWD가 EXE 디렉터리와 다를 수 있어 (VS 디버그시 $(ProjectDir)) 명시적으로 전달
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir = exeDir.substr(0, slash);
    std::println("[DLSS] EXE 디렉터리: {}",
                 std::filesystem::path(exeDir).string());

    const wchar_t* searchPaths[] = { exeDir.c_str() };
    NVSDK_NGX_FeatureCommonInfo featureInfo{};
    featureInfo.PathListInfo.Path   = searchPaths;
    featureInfo.PathListInfo.Length = 1;

    // ProjectID 기반 초기화 — 임의의 App ID(0x534C5344 'DLSS')는 NVIDIA 화이트리스트
    // 미등록 상태라 RR Available=0 으로 차단된다. ProjectID + ENGINE_TYPE_CUSTOM 경로는
    // 연구/개발용으로 NVIDIA가 공식 지원하므로 RTX 20xx+ GPU에서 RR이 정상 활성화된다.
    // ProjectID는 GUID 형식 문자열이면 충분 (NVIDIA 등록 불필요).
    NVSDK_NGX_Result res = NVSDK_NGX_D3D12_Init_with_ProjectID(
        "a1b2c3d4-e5f6-7890-abcd-ef1234567890",   // ProjectID (개발용 GUID)
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "1.0.0",                                   // EngineVersion
        exeDir.c_str(), device, &featureInfo, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(res))
    {
        std::println("[DLSS] NGX_D3D12_Init_with_ProjectID 실패: 0x{:08X} (비-NVIDIA GPU 또는 구형 드라이버)",
                     static_cast<uint32_t>(res));
        return false;
    }
    std::println("[DLSS] NGX_D3D12_Init_with_ProjectID OK (ENGINE_TYPE_CUSTOM)");

    res = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_params);
    if (NVSDK_NGX_FAILED(res))
    {
        std::println("[DLSS] GetCapabilityParameters 실패: 0x{:08X}", static_cast<uint32_t>(res));
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }

    // ── 1.5. 기능별 가용성 진단 출력 ──────────────────────────────
    int srAvail = 0, rrAvail = 0;
    int srNeedsUpdate = 0, rrNeedsUpdate = 0;
    int srInitResult  = 0, rrInitResult  = 0;
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_Available,          &srAvail);
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rrAvail);
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,          &srNeedsUpdate);
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &rrNeedsUpdate);
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,          &srInitResult);
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &rrInitResult);
    std::println("[DLSS] 진단: SR(avail={}, needsDrv={}, init=0x{:08X}) RR(avail={}, needsDrv={}, init=0x{:08X})",
                 srAvail, srNeedsUpdate, (uint32_t)srInitResult,
                 rrAvail, rrNeedsUpdate, (uint32_t)rrInitResult);
    if (rrAvail == 0)
    {
        std::println("[DLSS-RR] RR Available=0: 드라이버가 너무 구버전이거나 GPU가 RR을 지원하지 않음");
        std::println("[DLSS-RR] 요구사항: RTX 20xx 이상 + 드라이버 537.58+ (DLSS 3.5 출시)");
    }

    // ── 2. 렌더 해상도 쿼리 (Quality 모드 ≈ 67% 스케일 = 약 44% 픽셀) ─────
    uint32_t optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0.0f;
    res = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_params,
        displayW, displayH, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        &optW, &optH, &maxW, &maxH, &minW, &minH, &sharpness);

    if (NVSDK_NGX_FAILED(res) || optW == 0 || optH == 0)
    {
        std::println("[DLSS] OPTIMAL_SETTINGS 실패: 0x{:08X}, optW={} optH={}",
                     static_cast<uint32_t>(res), optW, optH);
        NVSDK_NGX_D3D12_DestroyParameters(m_params);
        m_params = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }
    m_renderW = optW;
    m_renderH = optH;

    const float scale = 100.0f * float(m_renderW * m_renderH) / float(displayW * displayH);
    std::println("[DLSS] 렌더: {}x{} → 출력: {}x{} (Quality, {:.0f}% 픽셀)",
                 m_renderW, m_renderH, displayW, displayH, scale);

    // ── 3. 리소스 생성 ────────────────────────────────────────────
    m_renderColor  = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_renderAccum  = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R32G32B32A32_FLOAT);
    m_dlssOutput   = MakeUAVTex(device, displayW,  displayH,  DXGI_FORMAT_R8G8B8A8_UNORM);
    m_depth        = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R32_FLOAT);
    m_motionVec    = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16_FLOAT);
    // oct-법선(xy) + 0(z) + roughness(w) — DLSS-RR Packed Roughness 모드 + A-trous 공용
    m_renderNormal = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // DLSS-RR 필수 입력: DiffuseAlbedo + SpecularAlbedo (없으면 Evaluate 0xBAD0000A)
    m_diffuseAlbedo  = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_specularAlbedo = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // NRD 입력 (path tracer 가 RT 셰이더에서 u7/u8/u9 로 기록)
    m_nrdDiffRadiance = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_nrdSpecRadiance = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_nrdViewZ        = MakeUAVTex(device, m_renderW, m_renderH, DXGI_FORMAT_R16_FLOAT);

    m_renderColor    ->SetName(L"DLSS_RenderColor");
    m_renderAccum    ->SetName(L"DLSS_RenderAccum");
    m_dlssOutput     ->SetName(L"DLSS_Output");
    m_renderNormal   ->SetName(L"DLSS_RenderNormal");
    m_diffuseAlbedo  ->SetName(L"DLSS_DiffuseAlbedo");
    m_specularAlbedo ->SetName(L"DLSS_SpecularAlbedo");
    m_nrdDiffRadiance->SetName(L"NRD_DiffRadianceHitDist");
    m_nrdSpecRadiance->SetName(L"NRD_SpecRadianceHitDist");
    m_nrdViewZ       ->SetName(L"NRD_ViewZ");

    // ── 4. 공유 힙에 UAV/SRV 등록 ───────────────────────────────
    // shader-visible 힙은 CPU read 불가 → CopyDescriptors 사용 불가
    // 리소스 포인터로 SRV를 직접 생성한다.
    // 비DLSS 슬롯 0..9 선점 후 → DLSS는 슬롯 10부터 시작

    // 슬롯 16: UAV m_renderColor (u0)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateUnorderedAccessView(m_renderColor.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 17: UAV m_renderAccum (u1)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(m_renderAccum.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 18: UAV m_depth (u2)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(m_depth.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 19: UAV m_motionVec (u3)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateUnorderedAccessView(m_motionVec.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 20: UAV m_renderNormal (u4, world-법선 xyz + roughness w — DLSS-RR + A-trous 공용)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_renderNormal.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 21: UAV m_diffuseAlbedo (u5, DLSS-RR DiffuseAlbedo)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_diffuseAlbedo.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 22: UAV m_specularAlbedo (u6, DLSS-RR SpecularAlbedo F0)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_specularAlbedo.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 23: UAV m_nrdDiffRadiance (u7, NRD diff radiance + hitDist)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_nrdDiffRadiance.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 24: UAV m_nrdSpecRadiance (u8, NRD spec radiance + hitDist)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_nrdSpecRadiance.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 25: UAV m_nrdViewZ (u9, NRD linear view Z)
    {
        DescriptorHandle h = sharedHeap.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format        = DXGI_FORMAT_R16_FLOAT;
        device->CreateUnorderedAccessView(m_nrdViewZ.Get(), nullptr, &uav, h.cpu);
    }
    // 슬롯 26: SRV TLAS 미러
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

    // Phase 4 — SRV t5 importance (R16F, render-res). 매 frame App 이 갱신.
    //   Init 시점엔 null SRV 로 자리만 잡음.
    {
        DescriptorHandle h = sharedHeap.Allocate();
        m_importanceMirrorCPU = h.cpu;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Format        = DXGI_FORMAT_R16_FLOAT;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &srvDesc, h.cpu);
    }

    // ── 5. DLSS Ray Reconstruction 피처 생성 (RR 전용, 폴백 없음) ──
    // 입력: raw 1spp HDR (m_renderAccum RGBA32F) + depth + MV + normals
    // RR은 AI 모델이 denoising + temporal + upscaling을 1패스로 처리
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_CreationNodeMask,    1);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_VisibilityNodeMask,  1);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_Width,               m_renderW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_Height,              m_renderH);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_OutWidth,            displayW);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_OutHeight,           displayH);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_PerfQualityValue,    NVSDK_NGX_PerfQuality_Value_MaxQuality);
    // 입력은 path tracer raw HDR(RGBA32F, >1.0 가능) + render-res MV
    //  - IsHDR    : color 입력이 HDR linear 임을 알림 (필수, 없으면 InvalidParameter)
    //  - MVLowRes : 모션벡터를 render-res 로 제공 (현재 셰이더가 render-res 단위로 기록)
    const int featureFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                           | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, featureFlags);
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_DLSS_Denoise_Mode,   NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
    // Packed: roughness 가 normals.w 채널에 패킹 — 별도 GBuffer.Roughness 텍스처 불필요
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Packed);
    // 셰이더가 NDC depth 를 g_depth 에 기록하므로 HW depth 모드를 알린다 (Linear 로 두면 InvalidParameter)
    NVSDK_NGX_Parameter_SetI (m_params, NVSDK_NGX_Parameter_Use_HW_Depth,        NVSDK_NGX_DLSS_Depth_Type_HW);

    // RR Render Preset = E (최신 transformer 모델)
    //   기본 모델(D) 보다 노이지 입력에 robust 하며 path-traced HDR 의 분산을 더 잘 처리.
    //   모든 PerfQuality 레벨에 같은 preset 을 명시 (사용자가 모드 바꿔도 일관 적용).
    const uint32_t presetE = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E;
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,             presetE);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,          presetE);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,         presetE);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,      presetE);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, presetE);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality,     presetE);

    res = NVSDK_NGX_D3D12_CreateFeature(cmdList, NVSDK_NGX_Feature_RayReconstruction, m_params, &m_feature);
    if (NVSDK_NGX_FAILED(res))
    {
        const char* errName = "Unknown";
        switch (res)
        {
            case NVSDK_NGX_Result_FAIL_FeatureNotSupported:      errName = "FeatureNotSupported";      break;
            case NVSDK_NGX_Result_FAIL_PlatformError:            errName = "PlatformError";            break;
            case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:     errName = "FeatureAlreadyExists";     break;
            case NVSDK_NGX_Result_FAIL_FeatureNotFound:          errName = "FeatureNotFound";          break;
            case NVSDK_NGX_Result_FAIL_InvalidParameter:         errName = "InvalidParameter";         break;
            case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:    errName = "ScratchBufferTooSmall";    break;
            case NVSDK_NGX_Result_FAIL_NotInitialized:           errName = "NotInitialized";           break;
            case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:   errName = "UnsupportedInputFormat";   break;
            case NVSDK_NGX_Result_FAIL_RWFlagMissing:            errName = "RWFlagMissing";            break;
            case NVSDK_NGX_Result_FAIL_MissingInput:             errName = "MissingInput";             break;
            case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:errName = "UnableToInitializeFeature";break;
            case NVSDK_NGX_Result_FAIL_OutOfDate:                errName = "OutOfDate";                break;
            case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:           errName = "OutOfGPUMemory";           break;
            case NVSDK_NGX_Result_FAIL_UnsupportedFormat:        errName = "UnsupportedFormat";        break;
            case NVSDK_NGX_Result_FAIL_UnsupportedParameter:     errName = "UnsupportedParameter";     break;
            case NVSDK_NGX_Result_FAIL_Denied:                   errName = "Denied";                   break;
            case NVSDK_NGX_Result_FAIL_NotImplemented:           errName = "NotImplemented";           break;
        }
        std::println("[DLSS-RR] CreateFeature 실패: 0x{:08X} ({})", static_cast<uint32_t>(res), errName);
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
    D3D12_RESOURCE_BARRIER barriers[7]{};
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
    barriers[5].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[5].UAV.pResource = m_diffuseAlbedo.Get();
    barriers[6].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[6].UAV.pResource = m_specularAlbedo.Get();
    cmdList->ResourceBarrier(7, barriers);
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

    // RR 입력: raw HDR 1spp (renderAccum), depth, motionVec, normals, diffuse/specular albedo → SRV
    D3D12_RESOURCE_BARRIER toSRV[6] = {
        makeTransition(m_renderAccum.Get(),    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_depth.Get(),          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_motionVec.Get(),      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_renderNormal.Get(),   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_diffuseAlbedo.Get(),  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        makeTransition(m_specularAlbedo.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cmdList->ResourceBarrier(6, toSRV);

    // RR 파라미터: raw 1spp HDR(RGBA32F) + G-Buffer normals/diffuse/specular
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Color,           m_renderAccum.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Output,          m_dlssOutput.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_Depth,           m_depth.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_MotionVectors,   m_motionVec.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_GBuffer_Normals, m_renderNormal.Get());
    // DLSS-RR 필수: 표면 albedo 분리 입력 (diffuse·specular)
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_DiffuseAlbedo,   m_diffuseAlbedo.Get());
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, NVSDK_NGX_Parameter_SpecularAlbedo,  m_specularAlbedo.Get());
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
    D3D12_RESOURCE_BARRIER toUAV[6] = {
        makeTransition(m_renderAccum.Get(),    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_depth.Get(),          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_motionVec.Get(),      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_renderNormal.Get(),   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_diffuseAlbedo.Get(),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        makeTransition(m_specularAlbedo.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmdList->ResourceBarrier(6, toUAV);
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
