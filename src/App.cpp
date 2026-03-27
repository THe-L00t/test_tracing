#include "App.h"
#include <print>
#include <cstring>

// ---------------------------------------------------------------
// 씬 지오메트리 정의
// ---------------------------------------------------------------

// 평면: y=0, xz ±10, 법선 위쪽
static const VertexPN g_planeVerts[6] = {
    {{-10.0f, 0.0f, -10.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 10.0f, 0.0f, -10.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 10.0f, 0.0f,  10.0f}, {0.0f, 1.0f, 0.0f}},
    {{-10.0f, 0.0f, -10.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 10.0f, 0.0f,  10.0f}, {0.0f, 1.0f, 0.0f}},
    {{-10.0f, 0.0f,  10.0f}, {0.0f, 1.0f, 0.0f}},
};

// 단위 큐브 [-0.5, 0.5]^3, 36 정점 (6면 × 2삼각형 × 3정점)
// 각 면: 외부 법선 방향
static const VertexPN g_cubeVerts[36] = {
    // Front (z=+0.5, n=(0,0,1))
    {{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},

    // Back (z=-0.5, n=(0,0,-1))
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},

    // Left (x=-0.5, n=(-1,0,0))
    {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},

    // Right (x=+0.5, n=(1,0,0))
    {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},

    // Top (y=+0.5, n=(0,1,0))
    {{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},

    // Bottom (y=-0.5, n=(0,-1,0))
    {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}},
};

// 방 지오메트리: x∈[-3,3], y∈[0,4], z∈[-3,3]
// 법선은 내부를 향함
static const VertexPN g_roomVerts[36] = {
    // 바닥 (y=0, n=(0,1,0) - 내부 위쪽)
    {{-3.0f, 0.0f, -3.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 3.0f, 0.0f, -3.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 3.0f, 0.0f,  3.0f}, {0.0f, 1.0f, 0.0f}},
    {{-3.0f, 0.0f, -3.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 3.0f, 0.0f,  3.0f}, {0.0f, 1.0f, 0.0f}},
    {{-3.0f, 0.0f,  3.0f}, {0.0f, 1.0f, 0.0f}},

    // 천장 (y=4, n=(0,-1,0) - 내부 아래쪽)
    {{-3.0f, 4.0f,  3.0f}, {0.0f, -1.0f, 0.0f}},
    {{ 3.0f, 4.0f,  3.0f}, {0.0f, -1.0f, 0.0f}},
    {{ 3.0f, 4.0f, -3.0f}, {0.0f, -1.0f, 0.0f}},
    {{-3.0f, 4.0f,  3.0f}, {0.0f, -1.0f, 0.0f}},
    {{ 3.0f, 4.0f, -3.0f}, {0.0f, -1.0f, 0.0f}},
    {{-3.0f, 4.0f, -3.0f}, {0.0f, -1.0f, 0.0f}},

    // 왼쪽 벽 (x=-3, n=(1,0,0) - 내부 오른쪽)
    {{-3.0f, 0.0f,  3.0f}, {1.0f, 0.0f, 0.0f}},
    {{-3.0f, 0.0f, -3.0f}, {1.0f, 0.0f, 0.0f}},
    {{-3.0f, 4.0f, -3.0f}, {1.0f, 0.0f, 0.0f}},
    {{-3.0f, 0.0f,  3.0f}, {1.0f, 0.0f, 0.0f}},
    {{-3.0f, 4.0f, -3.0f}, {1.0f, 0.0f, 0.0f}},
    {{-3.0f, 4.0f,  3.0f}, {1.0f, 0.0f, 0.0f}},

    // 오른쪽 벽 (x=3, n=(-1,0,0) - 내부 왼쪽)
    {{ 3.0f, 0.0f, -3.0f}, {-1.0f, 0.0f, 0.0f}},
    {{ 3.0f, 0.0f,  3.0f}, {-1.0f, 0.0f, 0.0f}},
    {{ 3.0f, 4.0f,  3.0f}, {-1.0f, 0.0f, 0.0f}},
    {{ 3.0f, 0.0f, -3.0f}, {-1.0f, 0.0f, 0.0f}},
    {{ 3.0f, 4.0f,  3.0f}, {-1.0f, 0.0f, 0.0f}},
    {{ 3.0f, 4.0f, -3.0f}, {-1.0f, 0.0f, 0.0f}},

    // 앞쪽 벽 (z=-3, n=(0,0,1) - 내부 뒤쪽)
    {{-3.0f, 0.0f, -3.0f}, {0.0f, 0.0f, 1.0f}},
    {{-3.0f, 4.0f, -3.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 3.0f, 4.0f, -3.0f}, {0.0f, 0.0f, 1.0f}},
    {{-3.0f, 0.0f, -3.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 3.0f, 4.0f, -3.0f}, {0.0f, 0.0f, 1.0f}},
    {{ 3.0f, 0.0f, -3.0f}, {0.0f, 0.0f, 1.0f}},

    // 뒤쪽 벽 (z=3, n=(0,0,-1) - 내부 앞쪽)
    {{ 3.0f, 0.0f,  3.0f}, {0.0f, 0.0f, -1.0f}},
    {{ 3.0f, 4.0f,  3.0f}, {0.0f, 0.0f, -1.0f}},
    {{-3.0f, 4.0f,  3.0f}, {0.0f, 0.0f, -1.0f}},
    {{ 3.0f, 0.0f,  3.0f}, {0.0f, 0.0f, -1.0f}},
    {{-3.0f, 4.0f,  3.0f}, {0.0f, 0.0f, -1.0f}},
    {{-3.0f, 0.0f,  3.0f}, {0.0f, 0.0f, -1.0f}},
};

// ---------------------------------------------------------------
// 변환 행렬 헬퍼: 단위 행렬 + 이동
// ---------------------------------------------------------------
static void MakeIdentityTransform(float out[3][4])
{
    // 단위 행렬 (row-major 3x4)
    out[0][0] = 1.0f; out[0][1] = 0.0f; out[0][2] = 0.0f; out[0][3] = 0.0f;
    out[1][0] = 0.0f; out[1][1] = 1.0f; out[1][2] = 0.0f; out[1][3] = 0.0f;
    out[2][0] = 0.0f; out[2][1] = 0.0f; out[2][2] = 1.0f; out[2][3] = 0.0f;
}

static void MakeTranslateTransform(float out[3][4], float tx, float ty, float tz)
{
    MakeIdentityTransform(out);
    out[0][3] = tx;
    out[1][3] = ty;
    out[2][3] = tz;
}

// ---------------------------------------------------------------
// App::Init
// ---------------------------------------------------------------
void App::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    m_core.Init(hwnd, width, height);
    std::println("[App] D3D12Core 완료");

    m_globalRS = CreateGlobalRootSignature(m_core.Device());
    std::println("[App] 글로벌 루트시그니처 완료");

    m_pipeline.Init(m_core.Device(), m_globalRS.Get());
    std::println("[App] RTPSO 완료");

    // 씬 상수 버퍼 생성 (업로드 힙, 256바이트, 매 프레임 기록)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width            = 256;
        cbDesc.Height           = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels        = 1;
        cbDesc.SampleDesc       = {1, 0};
        cbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(m_core.Device()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_sceneCB)));
    }
    std::println("[App] 씬 상수 버퍼 완료");

    // BLAS 빌드 (AS 빌드용: Present 없이 제출 후 GPU 대기)
    ThrowIfFailed(m_core.CmdAlloc(0)->Reset());
    ThrowIfFailed(m_core.CmdList()->Reset(m_core.CmdAlloc(0), nullptr));

    BuildBLASes();
    std::println("[App] BLAS 빌드 완료");

    // 초기 씬 (Scene 0: 야외)
    m_sceneID = 0;
    m_camera.Init(0.0f, 1.5f, -5.0f, 0.0f, 0.0f);

    // Scene 0 TLAS 빌드
    {
        TLASInstance instances[2]{};
        MakeIdentityTransform(instances[0].transform);
        instances[0].blasResource = m_planeBLAS.Resource();
        instances[0].instanceID   = 0;

        MakeTranslateTransform(instances[1].transform, 0.0f, 0.5f, 3.0f);
        instances[1].blasResource = m_cubeBLAS.Resource();
        instances[1].instanceID   = 1;

        m_tlas.Build(m_core.Device(), m_core.CmdList(),
                     std::span{ instances });
    }

    m_core.SubmitAndFlush();
    std::println("[App] 초기 TLAS 빌드 완료 (Scene 0)");

    // 디스크립터 힙 설정
    BuildDescriptors();
    std::println("[App] 디스크립터 완료");

    // 셰이더 테이블 빌드
    m_pipeline.BuildShaderTable(m_core.Device());

    m_sceneBuilt = true;
    std::println("[App] 초기화 완료 - 씬 0 (야외)");
    std::println("[App] 조작: WASD 이동, IJKL 시점, QE 상하, 1/2 씬 전환, ESC 종료");
}

void App::Shutdown()
{
    m_core.Shutdown();
}

// ---------------------------------------------------------------
// BLAS 빌드 (cmdList가 이미 열려 있어야 함)
// ---------------------------------------------------------------
void App::BuildBLASes()
{
    m_planeBLAS.Build(m_core.Device(), m_core.CmdList(),
                      std::span{ g_planeVerts });
    m_cubeBLAS.Build(m_core.Device(), m_core.CmdList(),
                     std::span{ g_cubeVerts });
    m_roomBLAS.Build(m_core.Device(), m_core.CmdList(),
                     std::span{ g_roomVerts });
}

// ---------------------------------------------------------------
// 디스크립터 힙 구성
// 슬롯 0: UAV (렌더 타겟)
// 슬롯 1: SRV (TLAS)
// 슬롯 2: SRV (plane VB, StructuredBuffer<VertexPN>)
// 슬롯 3: SRV (cube VB)
// 슬롯 4: SRV (room VB)
// ---------------------------------------------------------------
void App::BuildDescriptors()
{
    auto& heap   = m_core.CbvSrvUavHeap();
    auto* device = m_core.Device();

    // 슬롯 0: UAV (렌더 타겟)
    m_renderTarget.Init(device, heap, m_width, m_height);

    // 슬롯 1: SRV (TLAS)
    m_tlasSRV = heap.Allocate();
    RebuildTLASSRV();

    // 슬롯 2: SRV (plane VB)
    m_planeVbSRV = heap.Allocate();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement        = 0;
        srvDesc.Buffer.NumElements         = m_planeBLAS.VertexCount();
        srvDesc.Buffer.StructureByteStride = sizeof(VertexPN);  // 24
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(m_planeBLAS.VertexBuffer(), &srvDesc, m_planeVbSRV.cpu);
    }

    // 슬롯 3: SRV (cube VB)
    m_cubeVbSRV = heap.Allocate();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement        = 0;
        srvDesc.Buffer.NumElements         = m_cubeBLAS.VertexCount();
        srvDesc.Buffer.StructureByteStride = sizeof(VertexPN);
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(m_cubeBLAS.VertexBuffer(), &srvDesc, m_cubeVbSRV.cpu);
    }

    // 슬롯 4: SRV (room VB)
    m_roomVbSRV = heap.Allocate();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement        = 0;
        srvDesc.Buffer.NumElements         = m_roomBLAS.VertexCount();
        srvDesc.Buffer.StructureByteStride = sizeof(VertexPN);
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(m_roomBLAS.VertexBuffer(), &srvDesc, m_roomVbSRV.cpu);
    }
}

// ---------------------------------------------------------------
// TLAS SRV 재생성 (힙 슬롯 1에 덮어씀)
// ---------------------------------------------------------------
void App::RebuildTLASSRV()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension                            = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping                  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = m_tlas.Resource()->GetGPUVirtualAddress();
    m_core.Device()->CreateShaderResourceView(nullptr, &srvDesc, m_tlasSRV.cpu);
}

// ---------------------------------------------------------------
// 씬 전환 (GPU 동기화 후 TLAS 재빌드)
// ---------------------------------------------------------------
void App::SwitchScene(uint32_t id)
{
    if (id == m_sceneID) return;
    m_sceneID = id;

    // GPU 완전 대기
    m_core.FlushGPU();

    // 커맨드 리스트 열기
    ThrowIfFailed(m_core.CmdAlloc(0)->Reset());
    ThrowIfFailed(m_core.CmdList()->Reset(m_core.CmdAlloc(0), nullptr));

    if (m_sceneID == 0)
    {
        // 씬 0 (야외): 평면 + 큐브
        std::println("[App] 씬 0 (야외) 전환");
        m_camera.Init(0.0f, 1.5f, -5.0f, 0.0f, 0.0f);

        TLASInstance instances[2]{};
        MakeIdentityTransform(instances[0].transform);
        instances[0].blasResource = m_planeBLAS.Resource();
        instances[0].instanceID   = 0;

        MakeTranslateTransform(instances[1].transform, 0.0f, 0.5f, 3.0f);
        instances[1].blasResource = m_cubeBLAS.Resource();
        instances[1].instanceID   = 1;

        m_tlas.Build(m_core.Device(), m_core.CmdList(),
                     std::span{ instances });
    }
    else
    {
        // 씬 1 (실내): 방 + 큐브
        std::println("[App] 씬 1 (실내) 전환");
        m_camera.Init(0.0f, 1.7f, -2.0f, 0.0f, 0.0f);

        TLASInstance instances[2]{};
        MakeIdentityTransform(instances[0].transform);
        instances[0].blasResource = m_roomBLAS.Resource();
        instances[0].instanceID   = 2;

        MakeTranslateTransform(instances[1].transform, 0.0f, 0.5f, 0.5f);
        instances[1].blasResource = m_cubeBLAS.Resource();
        instances[1].instanceID   = 3;

        m_tlas.Build(m_core.Device(), m_core.CmdList(),
                     std::span{ instances });
    }

    m_core.SubmitAndFlush();

    // TLAS SRV 갱신
    RebuildTLASSRV();
}

// ---------------------------------------------------------------
// 입력 처리 (GetAsyncKeyState, dt = 고정 0.016f)
// ---------------------------------------------------------------
void App::ProcessInput(float dt)
{
    constexpr float k_moveSpeed = 5.0f;   // 단위/초
    constexpr float k_rotSpeed  = 1.5f;   // 라디안/초

    const float move = k_moveSpeed * dt;
    const float rot  = k_rotSpeed  * dt;

    // 전진/후진
    if (GetAsyncKeyState('W') & 0x8000) m_camera.MoveForward( move);
    if (GetAsyncKeyState('S') & 0x8000) m_camera.MoveForward(-move);

    // 좌/우 스트레이프
    if (GetAsyncKeyState('A') & 0x8000) m_camera.MoveRight(-move);
    if (GetAsyncKeyState('D') & 0x8000) m_camera.MoveRight( move);

    // 상/하 이동 (월드 Y)
    if (GetAsyncKeyState('Q') & 0x8000) m_camera.MoveUp( move);
    if (GetAsyncKeyState('E') & 0x8000) m_camera.MoveUp(-move);

    // 피치 (I=올려다보기, K=내려다보기)
    if (GetAsyncKeyState('I') & 0x8000) m_camera.AddPitch(-rot);
    if (GetAsyncKeyState('K') & 0x8000) m_camera.AddPitch( rot);

    // 요 (J=왼쪽, L=오른쪽)
    if (GetAsyncKeyState('J') & 0x8000) m_camera.AddYaw(-rot);
    if (GetAsyncKeyState('L') & 0x8000) m_camera.AddYaw( rot);
}

// ---------------------------------------------------------------
// 씬 상수 버퍼 업데이트 (매 프레임)
// ---------------------------------------------------------------
void App::UpdateSceneCB()
{
    SceneCB cb{};

    // 카메라
    const float* pos     = m_camera.Pos();
    const float* right   = m_camera.Right();
    const float* up      = m_camera.Up();
    const float* forward = m_camera.Forward();

    cb.camPos[0] = pos[0];     cb.camPos[1] = pos[1];     cb.camPos[2] = pos[2];
    cb.camRight[0] = right[0]; cb.camRight[1] = right[1]; cb.camRight[2] = right[2];
    cb.camUp[0] = up[0];       cb.camUp[1] = up[1];       cb.camUp[2] = up[2];
    cb.camForward[0] = forward[0]; cb.camForward[1] = forward[1]; cb.camForward[2] = forward[2];

    // FOV 60도 기준: tanHalfFovY = tan(30°) ≈ 0.57735
    cb.tanHalfFovY = 0.57735f;
    cb.aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
    cb.sceneID     = m_sceneID;
    cb._pad0       = 0.0f;

    if (m_sceneID == 0)
    {
        // Scene 0 (야외): 태양 방향광 (방향 벡터를 위치 대신 저장)
        // 방향광이므로 lightPos에 정규화된 방향 저장
        float sx = 1.0f, sy = 2.0f, sz = -0.5f;
        float slen = sqrtf(sx*sx + sy*sy + sz*sz);
        cb.lightPos[0]     = sx / slen;
        cb.lightPos[1]     = sy / slen;
        cb.lightPos[2]     = sz / slen;
        cb.lightRadius     = 0.0f;
        cb.lightColor[0]   = 1.0f;
        cb.lightColor[1]   = 0.95f;
        cb.lightColor[2]   = 0.8f;
        cb.lightIntensity  = 3.0f;
    }
    else
    {
        // Scene 1 (실내): 포인트 라이트
        cb.lightPos[0]    = 0.0f;
        cb.lightPos[1]    = 3.8f;
        cb.lightPos[2]    = 0.0f;
        cb.lightRadius    = 0.0f;
        cb.lightColor[0]  = 1.0f;
        cb.lightColor[1]  = 1.0f;
        cb.lightColor[2]  = 1.0f;
        cb.lightIntensity = 15.0f;
    }

    // 재질 0: 초록 바닥 (albedo=(0.15,0.6,0.1), roughness=0.85, metallic=0)
    cb.matAlbedoRoughness[0][0] = 0.15f;
    cb.matAlbedoRoughness[0][1] = 0.60f;
    cb.matAlbedoRoughness[0][2] = 0.10f;
    cb.matAlbedoRoughness[0][3] = 0.85f;
    cb.matMetallic[0][0] = 0.0f;

    // 재질 1: 빨간 메탈릭 큐브 (albedo=(0.8,0.05,0.05), roughness=0.05, metallic=0.95)
    cb.matAlbedoRoughness[1][0] = 0.80f;
    cb.matAlbedoRoughness[1][1] = 0.05f;
    cb.matAlbedoRoughness[1][2] = 0.05f;
    cb.matAlbedoRoughness[1][3] = 0.05f;
    cb.matMetallic[1][0] = 0.95f;

    // 재질 2: 흰색 벽 (albedo=(0.85,0.85,0.85), roughness=0.9, metallic=0)
    cb.matAlbedoRoughness[2][0] = 0.85f;
    cb.matAlbedoRoughness[2][1] = 0.85f;
    cb.matAlbedoRoughness[2][2] = 0.85f;
    cb.matAlbedoRoughness[2][3] = 0.90f;
    cb.matMetallic[2][0] = 0.0f;

    // 재질 3: 파란 매트 큐브 (albedo=(0.1,0.15,0.8), roughness=0.95, metallic=0)
    cb.matAlbedoRoughness[3][0] = 0.10f;
    cb.matAlbedoRoughness[3][1] = 0.15f;
    cb.matAlbedoRoughness[3][2] = 0.80f;
    cb.matAlbedoRoughness[3][3] = 0.95f;
    cb.matMetallic[3][0] = 0.0f;

    // 업로드 버퍼에 직접 기록
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    ThrowIfFailed(m_sceneCB->Map(0, &readRange, &mapped));
    std::memcpy(mapped, &cb, sizeof(SceneCB));
    m_sceneCB->Unmap(0, nullptr);
}

// ---------------------------------------------------------------
// 키 다운 이벤트 (WM_KEYDOWN에서 호출)
// ---------------------------------------------------------------
void App::OnKeyDown(uint32_t key)
{
    if (key == '1') SwitchScene(0);
    else if (key == '2') SwitchScene(1);
}

// ---------------------------------------------------------------
// OnRender (매 프레임)
// ---------------------------------------------------------------
void App::OnRender()
{
    constexpr float k_dt = 0.016f;  // 고정 델타타임

    ProcessInput(k_dt);
    UpdateSceneCB();

    m_core.BeginFrame();

    auto* cmd = m_core.CmdList();

    // 디스크립터 힙 바인딩
    ID3D12DescriptorHeap* heaps[] = { m_core.CbvSrvUavHeap().Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_globalRS.Get());

    // 파라미터 0: 디스크립터 테이블 (힙 슬롯 0부터 - UAV + SRVs)
    cmd->SetComputeRootDescriptorTable(0, m_core.CbvSrvUavHeap().GetHandle(0).gpu);

    // 파라미터 1: 인라인 루트 CBV (씬 상수 버퍼)
    cmd->SetComputeRootConstantBufferView(1, m_sceneCB->GetGPUVirtualAddress());

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

    // UAV 쓰기 완료 보장 (DispatchRays → CopyResource 순서 강제)
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_renderTarget.Resource();
    cmd->ResourceBarrier(1, &uavBarrier);

    // 결과를 백버퍼로 복사 후 Present
    m_renderTarget.CopyToBackBuffer(cmd, m_core.BackBuffer());

    m_core.EndFrame();
}

void App::OnResize(uint32_t width, uint32_t height)
{
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    std::println("[App] 리사이즈: {}x{}", width, height);
}
