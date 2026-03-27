#include "App.h"
#include <print>

void App::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    m_core.Init(hwnd, width, height);

    m_globalRS = CreateGlobalRootSignature(m_core.Device());
    m_pipeline.Init(m_core.Device(), m_globalRS.Get());

    // 씬과 디스크립터는 첫 렌더 전에 한 번만 빌드
    BuildScene();
    BuildDescriptors();

    m_pipeline.BuildShaderTable(m_core.Device());

    std::println("[App] 초기화 완료");
}

void App::Shutdown()
{
    m_core.Shutdown();
}

void App::BuildScene()
{
    // 간단한 삼각형 하나
    static constexpr float verts[][3] = {
        {  0.0f,  0.7f, 1.0f },
        {  0.7f, -0.7f, 1.0f },
        { -0.7f, -0.7f, 1.0f },
    };

    // AS 빌드용: Present 없이 제출 후 GPU 대기
    ThrowIfFailed(m_core.CmdAlloc(0)->Reset());
    ThrowIfFailed(m_core.CmdList()->Reset(m_core.CmdAlloc(0), nullptr));

    m_blas.Build(m_core.Device(), m_core.CmdList(),
                 std::span{ verts });
    m_tlas.Build(m_core.Device(), m_core.CmdList(),
                 m_blas.Resource());

    m_core.SubmitAndFlush();

    m_sceneBuilt = true;
    std::println("[App] 가속 구조 빌드 완료");
}

void App::BuildDescriptors()
{
    auto& heap = m_core.CbvSrvUavHeap();

    // 슬롯 0: UAV (렌더 타겟)
    m_renderTarget.Init(m_core.Device(), heap, m_width, m_height);

    // 슬롯 1: SRV (TLAS)
    m_tlasSRV = heap.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension                            = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping                  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = m_tlas.Resource()->GetGPUVirtualAddress();
    m_core.Device()->CreateShaderResourceView(nullptr, &srvDesc, m_tlasSRV.cpu);
}

void App::OnRender()
{
    m_core.BeginFrame();

    auto* cmd = m_core.CmdList();

    // 디스크립터 힙 바인딩
    ID3D12DescriptorHeap* heaps[] = { m_core.CbvSrvUavHeap().Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_globalRS.Get());

    // 힙 시작 = UAV(슬롯0)와 SRV(슬롯1) 모두 포함하는 테이블
    cmd->SetComputeRootDescriptorTable(0, m_core.CbvSrvUavHeap().GetHandle(0).gpu);

    // RTPSO 설정 및 레이 디스패치
    cmd->SetPipelineState1(m_pipeline.PSO());

    const auto& st = m_pipeline.GetShaderTable();
    D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
    dispatchDesc.RayGenerationShaderRecord = st.RayGenRange();
    dispatchDesc.MissShaderTable           = st.MissRange();
    dispatchDesc.HitGroupTable             = st.HitGroupRange();
    dispatchDesc.Width                     = m_width;
    dispatchDesc.Height                    = m_height;
    dispatchDesc.Depth                     = 1;

    cmd->DispatchRays(&dispatchDesc);

    // 결과를 백버퍼로 복사 후 Present
    m_renderTarget.CopyToBackBuffer(cmd, m_core.BackBuffer());

    m_core.EndFrame();
}

void App::OnResize(uint32_t width, uint32_t height)
{
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    // 리사이즈 지원 시 스왑체인/렌더타겟 재생성 필요 (현재 미구현)
    std::println("[App] 리사이즈: {}x{}", width, height);
}
