#include "App.h"
#include <dxcapi.h>
#include <print>
#include <cstring>
#include <cmath>
#include <vector>
#include <filesystem>

// ---------------------------------------------------------------
// 구 메시 생성 (UV 구, 삼각형 리스트, 단위 구 반지름 1)
// stacks: 위아래 분할 수, slices: 수평 분할 수
// ---------------------------------------------------------------
static std::vector<VertexPN> GenerateSphereVerts(int stacks = 20, int slices = 20)
{
    std::vector<VertexPN> verts;
    constexpr float pi = 3.14159265358979323846f;

    // 구 표면 위의 한 점 계산: normal = position (단위 구)
    auto makeVert = [](float theta, float phi) -> VertexPN
    {
        float st = sinf(theta), ct = cosf(theta);
        float sp = sinf(phi),   cp = cosf(phi);
        float x = st * cp, y = ct, z = st * sp;
        return VertexPN{ {x, y, z}, {x, y, z} };
    };

    for (int i = 0; i < stacks; ++i)
    {
        float t0 = pi * float(i)     / float(stacks);
        float t1 = pi * float(i + 1) / float(stacks);

        for (int j = 0; j < slices; ++j)
        {
            float p0 = 2.0f * pi * float(j)     / float(slices);
            float p1 = 2.0f * pi * float(j + 1) / float(slices);

            VertexPN v00 = makeVert(t0, p0);
            VertexPN v10 = makeVert(t1, p0);
            VertexPN v01 = makeVert(t0, p1);
            VertexPN v11 = makeVert(t1, p1);

            if (i == 0) // 상단 캡: 삼각형
            {
                verts.push_back(v00);
                verts.push_back(v11);
                verts.push_back(v10);
            }
            else if (i == stacks - 1) // 하단 캡: 삼각형
            {
                verts.push_back(v00);
                verts.push_back(v01);
                verts.push_back(v11);
            }
            else // 중간 밴드: 쿼드 = 삼각형 2개
            {
                verts.push_back(v00);
                verts.push_back(v11);
                verts.push_back(v10);

                verts.push_back(v00);
                verts.push_back(v01);
                verts.push_back(v11);
            }
        }
    }
    return verts;
}

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

// 스케일 + 이동: worldPos = scale*localPos + translate
// 큐브 BLAS는 [-0.5,0.5]^3이므로 center = translate 위치, 크기 = scale
static void MakeScaleTranslateTransform(float out[3][4],
    float sx, float sy, float sz,
    float tx, float ty, float tz)
{
    out[0][0] = sx;   out[0][1] = 0.0f; out[0][2] = 0.0f; out[0][3] = tx;
    out[1][0] = 0.0f; out[1][1] = sy;   out[1][2] = 0.0f; out[1][3] = ty;
    out[2][0] = 0.0f; out[2][1] = 0.0f; out[2][2] = sz;   out[2][3] = tz;
}

// InstanceID 인코딩: 하위4비트=geomType(0=plane,1=cube,2=room), 상위비트=matIdx
static constexpr uint32_t EncodeID(uint32_t geom, uint32_t mat)
{
    return geom | (mat << 4);
}

// ---------------------------------------------------------------
// App::Init
// ---------------------------------------------------------------
void App::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_hwnd   = hwnd;
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
        instances[0].instanceID   = EncodeID(0, 0); // plane, mat0 (초록 바닥)

        MakeTranslateTransform(instances[1].transform, 0.0f, 0.5f, 3.0f);
        instances[1].blasResource = m_cubeBLAS.Resource();
        instances[1].instanceID   = EncodeID(1, 1); // cube, mat1 (빨간 메탈릭)

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

    // 디노이저 초기화
    m_denoiser.Init(m_core.Device(), width, height);

    // ReSTIR 초기화
    m_restir.Init(m_core.Device(), m_core.CbvSrvUavHeap(),
                  m_globalRS.Get(), width, height);

    // GBuffer / Shade DXR PSO 빌드
    BuildGBufferPSO();
    BuildShadePSO();

    // 초기 씬(0) 광원 업로드
    {
        ThrowIfFailed(m_core.CmdAlloc(0)->Reset());
        ThrowIfFailed(m_core.CmdList()->Reset(m_core.CmdAlloc(0), nullptr));
        auto lights = BuildLightList(m_sceneID);
        m_restir.UploadLights(m_core.CmdList(), lights);
        m_restir.UpdateCB(ReSTIRCB{});  // 기본값으로 초기화
        m_core.SubmitAndFlush();
    }

    m_prevCamera = m_camera;  // 이전 프레임 카메라 초기화

    m_sceneBuilt = true;
    std::println("[App] 초기화 완료 - 씬 0 (야외)");
    std::println("[App] 조작: WASD 이동, IJKL 시점, QE 상하, 1/2/3 씬 전환, F 디노이저 토글, ESC 종료");
}

void App::Shutdown()
{
    m_denoiser.Shutdown();
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

    // 구 BLAS: UV 구 메시 생성 후 빌드 (stacks=20, slices=20)
    auto sphereVerts = GenerateSphereVerts(20, 20);
    m_sphereBLAS.Build(m_core.Device(), m_core.CmdList(),
                       std::span{ sphereVerts });
}

// ---------------------------------------------------------------
// 디스크립터 힙 구성
// 슬롯 0: UAV u0 (g_output,       RGBA8   – RenderTarget::Init 내부)
// 슬롯 1: UAV u1 (g_accumulation, RGBA32F – RenderTarget::Init 내부)
// 슬롯 2: SRV t0 (TLAS)
// 슬롯 3: SRV t1 (plane VB)
// 슬롯 4: SRV t2 (cube VB)
// 슬롯 5: SRV t3 (room VB)
// ---------------------------------------------------------------
void App::BuildDescriptors()
{
    auto& heap   = m_core.CbvSrvUavHeap();
    auto* device = m_core.Device();

    // 슬롯 0: UAV (렌더 타겟)
    m_renderTarget.Init(device, heap, m_width, m_height);

    // 슬롯 2: SRV (TLAS)  ← 슬롯 0,1은 RenderTarget::Init에서 UAV로 사용
    m_tlasSRV = heap.Allocate();
    RebuildTLASSRV();

    // 슬롯 3: SRV (plane VB)
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

    // 슬롯 4: SRV (cube VB)
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

    // 슬롯 5: SRV (room VB)
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

    // 슬롯 6: SRV (sphere VB)
    m_sphereVbSRV = heap.Allocate();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement        = 0;
        srvDesc.Buffer.NumElements         = m_sphereBLAS.VertexCount();
        srvDesc.Buffer.StructureByteStride = sizeof(VertexPN);
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(m_sphereBLAS.VertexBuffer(), &srvDesc, m_sphereVbSRV.cpu);
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
    m_frameCount  = 0;
    m_cameraMoved = true;
    m_accumDirty  = true;  // 씬 내용이 바뀌므로 누적 버퍼 클리어 필요

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
        instances[0].instanceID   = EncodeID(0, 0); // plane, mat0

        MakeTranslateTransform(instances[1].transform, 0.0f, 0.5f, 3.0f);
        instances[1].blasResource = m_cubeBLAS.Resource();
        instances[1].instanceID   = EncodeID(1, 1); // cube, mat1

        m_tlas.Build(m_core.Device(), m_core.CmdList(),
                     std::span{ instances });
    }
    else if (m_sceneID == 2)
    {
        // 씬 2 (투명/반투명 구 쇼케이스): 방 + 골드 큐브 + 반투명 구 + 유리 구
        std::println("[App] 씬 2 (구 쇼케이스) 전환");
        m_camera.Init(0.0f, 1.8f, -2.5f, 0.0f, 0.05f);

        TLASInstance instances[4]{};

        // 방 (mat0 = 흰색 벽)
        MakeIdentityTransform(instances[0].transform);
        instances[0].blasResource = m_roomBLAS.Resource();
        instances[0].instanceID   = EncodeID(2, 0);
        instances[0].mask         = 0xFF;

        // 골드 메탈릭 큐브 – 왼쪽 기둥
        MakeScaleTranslateTransform(instances[1].transform,
            0.8f, 2.0f, 0.8f, -1.8f, 1.0f, 0.5f);
        instances[1].blasResource = m_cubeBLAS.Resource();
        instances[1].instanceID   = EncodeID(1, 1); // cube, mat1
        instances[1].mask         = 0xFF;

        // 반투명 구 (mat2) – 왼쪽 중앙, 반지름 0.7
        // geomType=3 (sphere), matIdx=2
        MakeScaleTranslateTransform(instances[2].transform,
            0.7f, 0.7f, 0.7f, -0.5f, 0.7f, 0.5f);
        instances[2].blasResource = m_sphereBLAS.Resource();
        instances[2].instanceID   = EncodeID(3, 2);
        instances[2].mask         = 0x02; // 그림자 레이 제외 (유리계열)

        // 투명 유리 구 (mat3) – 오른쪽, 반지름 0.8
        // geomType=3 (sphere), matIdx=3
        MakeScaleTranslateTransform(instances[3].transform,
            0.8f, 0.8f, 0.8f, 0.9f, 0.8f, 0.3f);
        instances[3].blasResource = m_sphereBLAS.Resource();
        instances[3].instanceID   = EncodeID(3, 3);
        instances[3].mask         = 0x02; // 그림자 레이 제외 (유리)

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
        instances[0].instanceID   = EncodeID(2, 2); // room, mat2 (흰색 벽)

        MakeTranslateTransform(instances[1].transform, 0.0f, 0.5f, 0.5f);
        instances[1].blasResource = m_cubeBLAS.Resource();
        instances[1].instanceID   = EncodeID(1, 3); // cube, mat3 (파란 매트)

        m_tlas.Build(m_core.Device(), m_core.CmdList(),
                     std::span{ instances });
    }

    m_core.SubmitAndFlush();

    // TLAS SRV 갱신
    RebuildTLASSRV();

    // ReSTIR: G-Buffer 클리어 + 광원 리스트 갱신
    {
        ThrowIfFailed(m_core.CmdAlloc(0)->Reset());
        ThrowIfFailed(m_core.CmdList()->Reset(m_core.CmdAlloc(0), nullptr));
        m_restir.ClearGBuffer(m_core.CmdList());
        auto lights = BuildLightList(m_sceneID);
        m_restir.UploadLights(m_core.CmdList(), lights);
        m_core.SubmitAndFlush();
    }
    m_prevCamera = m_camera;  // 씬 전환 시 이전 카메라 리셋
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

    // 카메라 변경 여부 추적 (이동/회전 시 누적 초기화)
    bool moved = false;

    // 전진/후진
    if (GetAsyncKeyState('W') & 0x8000) { m_camera.MoveForward( move); moved = true; }
    if (GetAsyncKeyState('S') & 0x8000) { m_camera.MoveForward(-move); moved = true; }

    // 좌/우 스트레이프
    if (GetAsyncKeyState('A') & 0x8000) { m_camera.MoveRight(-move); moved = true; }
    if (GetAsyncKeyState('D') & 0x8000) { m_camera.MoveRight( move); moved = true; }

    // 상/하 이동 (월드 Y)
    if (GetAsyncKeyState('Q') & 0x8000) { m_camera.MoveUp( move); moved = true; }
    if (GetAsyncKeyState('E') & 0x8000) { m_camera.MoveUp(-move); moved = true; }

    // 피치 (I=올려다보기, K=내려다보기)
    if (GetAsyncKeyState('I') & 0x8000) { m_camera.AddPitch(-rot); moved = true; }
    if (GetAsyncKeyState('K') & 0x8000) { m_camera.AddPitch( rot); moved = true; }

    // 요 (J=왼쪽, L=오른쪽)
    if (GetAsyncKeyState('J') & 0x8000) { m_camera.AddYaw(-rot); moved = true; }
    if (GetAsyncKeyState('L') & 0x8000) { m_camera.AddYaw( rot); moved = true; }

    // 마우스 델타 처리 (캡처 중일 때 센터 락)
    if (m_mouseCaptured)
    {
        POINT center = { static_cast<LONG>(m_width / 2), static_cast<LONG>(m_height / 2) };
        POINT screenCenter = center;
        ClientToScreen(m_hwnd, &screenCenter);

        POINT cur;
        GetCursorPos(&cur);
        int dx = cur.x - screenCenter.x;
        int dy = cur.y - screenCenter.y;

        if (dx != 0 || dy != 0)
        {
            constexpr float k_sensitivity = 0.002f;
            m_camera.AddYaw(  static_cast<float>(dx) * k_sensitivity);
            m_camera.AddPitch(static_cast<float>(dy) * k_sensitivity);
            moved = true;
            SetCursorPos(screenCenter.x, screenCenter.y);
        }
    }

    if (moved) m_cameraMoved = true;
}

// ---------------------------------------------------------------
// 마우스 캡처 토글 (좌클릭)
// ---------------------------------------------------------------
void App::ToggleMouseCapture()
{
    if (!m_mouseCaptured)
    {
        // 캡처 시작: 커서 숨기고 클라이언트 영역에 가두기
        m_mouseCaptured = true;
        ShowCursor(FALSE);

        RECT clientRect;
        GetClientRect(m_hwnd, &clientRect);
        MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&clientRect), 2);
        ClipCursor(&clientRect);

        // 커서를 창 중앙으로 이동 (첫 프레임 점프 방지)
        POINT center = { static_cast<LONG>(m_width / 2), static_cast<LONG>(m_height / 2) };
        ClientToScreen(m_hwnd, &center);
        SetCursorPos(center.x, center.y);
    }
    else
    {
        ReleaseMouseCapture();
    }
}

// ---------------------------------------------------------------
// 마우스 캡처 해제 (ESC / 포커스 이탈)
// ---------------------------------------------------------------
void App::ReleaseMouseCapture()
{
    if (m_mouseCaptured)
    {
        m_mouseCaptured = false;
        ClipCursor(nullptr);
        ShowCursor(TRUE);
    }
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
        // ── 씬 0 (야외): 태양 방향광 ──
        float sx = 1.0f, sy = 2.0f, sz = -0.5f;
        float slen = sqrtf(sx*sx + sy*sy + sz*sz);
        cb.lightPos[0] = sx/slen; cb.lightPos[1] = sy/slen; cb.lightPos[2] = sz/slen;
        cb.lightIntensity = 3.0f;
        cb.lightColor[0] = 1.0f; cb.lightColor[1] = 1.0f; cb.lightColor[2] = 1.0f;

        // mat0: 초록 바닥
        cb.matAlbedoRoughness[0][0]=0.15f; cb.matAlbedoRoughness[0][1]=0.60f;
        cb.matAlbedoRoughness[0][2]=0.10f; cb.matAlbedoRoughness[0][3]=0.85f;
        cb.matMetallic[0] = 0.0f;
        // mat1: 빨간 메탈릭 큐브
        cb.matAlbedoRoughness[1][0]=0.80f; cb.matAlbedoRoughness[1][1]=0.05f;
        cb.matAlbedoRoughness[1][2]=0.05f; cb.matAlbedoRoughness[1][3]=0.05f;
        cb.matMetallic[1] = 0.95f;
        cb.emissBoxHalfSize = 0.0f;
    }
    else if (m_sceneID == 1)
    {
        // ── 씬 1 (실내): 포인트 라이트 1개 ──
        cb.lightPos[0]=0.0f; cb.lightPos[1]=3.8f; cb.lightPos[2]=0.0f;
        cb.lightIntensity=8.0f;  // dist≈4m → atten=8/16=0.5, 적정 밝기
        cb.lightColor[0]=1.0f; cb.lightColor[1]=1.0f; cb.lightColor[2]=1.0f;

        // mat2: 흰색 벽
        cb.matAlbedoRoughness[2][0]=0.85f; cb.matAlbedoRoughness[2][1]=0.85f;
        cb.matAlbedoRoughness[2][2]=0.85f; cb.matAlbedoRoughness[2][3]=0.90f;
        cb.matMetallic[2] = 0.0f;
        // mat3: 파란 매트 큐브
        cb.matAlbedoRoughness[3][0]=0.10f; cb.matAlbedoRoughness[3][1]=0.15f;
        cb.matAlbedoRoughness[3][2]=0.80f; cb.matAlbedoRoughness[3][3]=0.95f;
        cb.matMetallic[3] = 0.0f;
        cb.emissBoxHalfSize = 0.0f;
    }
    else
    {
        // ── 씬 2 (투명/반투명 구 쇼케이스): 포인트 라이트 2개 ──
        // Light 1: 왼쪽 위
        cb.lightPos[0]=-1.5f; cb.lightPos[1]=3.6f; cb.lightPos[2]=0.0f;
        cb.lightIntensity=8.4f;
        cb.lightColor[0]=1.0f; cb.lightColor[1]=1.0f; cb.lightColor[2]=1.0f;

        // Light 2: 오른쪽 위
        cb.light2Pos[0]=1.5f; cb.light2Pos[1]=3.6f; cb.light2Pos[2]=-1.5f;
        cb.light2Intensity=6.6f;
        cb.light2Color[0]=1.0f; cb.light2Color[1]=1.0f; cb.light2Color[2]=1.0f;

        // mat0: 방 벽/바닥/천장 – 따뜻한 크림색 (굴절 이미지 구분 가시성 확보)
        cb.matAlbedoRoughness[0][0]=0.92f; cb.matAlbedoRoughness[0][1]=0.80f;
        cb.matAlbedoRoughness[0][2]=0.65f; cb.matAlbedoRoughness[0][3]=0.90f;
        cb.matMetallic[0] = 0.0f;
        cb.matEmissive[0] = 0.0f;

        // mat1: 골드 메탈릭 큐브
        cb.matAlbedoRoughness[1][0]=1.00f; cb.matAlbedoRoughness[1][1]=0.71f;
        cb.matAlbedoRoughness[1][2]=0.29f; cb.matAlbedoRoughness[1][3]=0.12f;
        cb.matMetallic[1] = 1.0f;
        cb.matEmissive[1] = 0.0f;

        // mat2: 반투명 구 (옅은 하늘색)
        // matEmissive = -1.0: 반투명 재질 신호
        cb.matAlbedoRoughness[2][0]=0.65f; cb.matAlbedoRoughness[2][1]=0.85f;
        cb.matAlbedoRoughness[2][2]=1.00f; cb.matAlbedoRoughness[2][3]=0.20f;
        cb.matMetallic[2] = 0.0f;
        cb.matEmissive[2] = -1.0f; // 반투명 신호

        // mat3: 투명 유리 구 (맑은 유리, IOR=1.5)
        // matEmissive = -2.0: 유리 재질 신호
        cb.matAlbedoRoughness[3][0]=0.95f; cb.matAlbedoRoughness[3][1]=0.98f;
        cb.matAlbedoRoughness[3][2]=1.00f; cb.matAlbedoRoughness[3][3]=0.02f;
        cb.matMetallic[3] = 0.0f;
        cb.matEmissive[3] = -2.0f; // 유리 신호 (IOR=1.5)

        cb.emissBoxHalfSize = 0.0f; // 발광 박스 없음
    }

    // 누적 파라미터
    cb.frameCount  = m_frameCount;
    cb.randomSeed  = m_shadeAccumCount;

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
    else if (key == '3') SwitchScene(2);
    else if (key == 'F')
    {
        m_denoiseEnabled        = !m_denoiseEnabled;
        m_denoiser.enabled      = m_denoiseEnabled;
        m_frameCount            = 0;
        m_cameraMoved           = true;
        m_accumDirty            = true;
        std::println("[App] 디노이저 {}", m_denoiseEnabled ? "ON" : "OFF");
    }
}

// ---------------------------------------------------------------
// OnRender (매 프레임)
// ---------------------------------------------------------------
void App::OnRender()
{
    // ── 실제 프레임 시간 측정 + FPS 계산 (지수이동평균, α=0.1) ──
    auto now = Clock::now();
    if (m_lastFrameTime.time_since_epoch().count() != 0)
    {
        float dt = std::chrono::duration<float>(now - m_lastFrameTime).count();
        float instantFps = (dt > 0.0f) ? 1.0f / dt : 0.0f;
        if (m_smoothFps < 1.0f)
            m_smoothFps = instantFps;   // 첫 프레임: 초기화
        else
            m_smoothFps = m_smoothFps * 0.9f + instantFps * 0.1f;
    }
    m_lastFrameTime = now;

    constexpr float k_dt = 0.016f;  // 입력 처리용 고정 델타타임

    ProcessInput(k_dt);

    // 카메라 이동 시 누적 초기화, 정지 시 프레임 누적
    if (m_cameraMoved)
    {
        m_frameCount  = 0;
        m_accumDirty  = true;   // g_accumulation 클리어 + m_shadeAccumCount 리셋
                                // (이동 중 old_accum * 0.8^N → 0 암전 현상 방지)
        m_cameraMoved = false;
    }
    else
    {
        m_frameCount++;
        // 정지 직후 첫 프레임(frameIndex==1): 이전 이동 프레임의 stale g_accumulation을
        // 그대로 lerp하면 검은화면이 될 수 있으므로 한 프레임 더 클리어
        if (m_frameCount == 1)
            m_accumDirty = true;
    }

    // ReSTIR_Shade 전용 누적 카운터:
    //   m_accumDirty(씬 전환/토글/카메라 이동) 시 0 리셋 → alpha=1.0 → 1spp 직접 출력
    //   정지: 무한 증가 → alpha 계속 감소 → 깨끗한 수렴
    if (m_accumDirty)
        m_shadeAccumCount = 0;
    else
        m_shadeAccumCount = std::min(m_shadeAccumCount + 1u, 64u);  // alpha≥1/65≈1.5% 보장

    UpdateSceneCB();

    m_core.BeginFrame();

    auto* cmd = m_core.CmdList();

    // 디스크립터 힙 바인딩
    ID3D12DescriptorHeap* heaps[] = { m_core.CbvSrvUavHeap().Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    // 씬 전환 / 디노이저 토글 시: 이전 씬 잔상 제거
    if (m_accumDirty)
    {
        m_renderTarget.ClearAccumulation(cmd);
        m_accumDirty = false;
    }

    cmd->SetComputeRootSignature(m_globalRS.Get());

    // 파라미터 0: 디스크립터 테이블 (힙 슬롯 0부터)
    cmd->SetComputeRootDescriptorTable(0, m_core.CbvSrvUavHeap().GetHandle(0).gpu);

    // 파라미터 1: SceneCB
    cmd->SetComputeRootConstantBufferView(1, m_sceneCB->GetGPUVirtualAddress());

    {
        // ── ReSTIR 5 패스 ───────────────────────────────────
        UpdateReSTIRCB();
        cmd->SetComputeRootConstantBufferView(2, m_restir.RestirCBAddress());

        // Pass 1: G-Buffer
        m_restir.DispatchGBuffer(cmd, m_gbufferPSO.Get(),
                                 m_gbufferShaderTable, m_width, m_height);

        // Pass 2: Initial (RIS 후보 생성)
        m_restir.DispatchInitial(cmd, m_width, m_height);

        // Pass 3: Temporal (이전 프레임 재사용)
        m_restir.DispatchTemporal(cmd, m_width, m_height);

        // Pass 4: Spatial (이웃 픽셀 재사용 x2)
        m_restir.DispatchSpatial(cmd, m_width, m_height);

        // Pass 5: Shade (그림자 레이 + GGX BRDF → g_output)
        m_restir.DispatchShade(cmd, m_shadePSO.Get(),
                               m_shadeShaderTable, m_width, m_height);

        m_renderTarget.UAVBarriers(cmd);

        // 현재 G-Buffer → prev 복사 (다음 프레임 Temporal surface validation용)
        // G-Buffer가 이 시점에 SRV 상태이므로 안전하게 복사 가능
        m_restir.CopyGBufferToPrev(cmd);

        // 다음 프레임을 위해 이전 카메라 저장 및 Reservoir 교환
        m_prevCamera = m_camera;
        m_restir.SwapReservoirs();
    }

    // 결과를 백버퍼로 복사 후 Present
    m_renderTarget.CopyToBackBuffer(cmd, m_core.BackBuffer());

    m_core.EndFrame(m_smoothFps, m_denoiseEnabled);
}

void App::OnResize(uint32_t width, uint32_t height)
{
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    std::println("[App] 리사이즈: {}x{}", width, height);
}

// ---------------------------------------------------------------
//  DXR PSO 빌더 헬퍼 (DXC lib_6_3 컴파일 → StateObject 생성)
// ---------------------------------------------------------------
namespace
{
    // DXC lib_6_3 컴파일 (DXRPipeline.cpp의 CompileShaderDXC와 동일 패턴)
    ComPtr<IDxcBlob> CompileDXRLib(const std::filesystem::path& path)
    {
        ComPtr<IDxcUtils>    utils;
        ComPtr<IDxcCompiler3> compiler;
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils,    IID_PPV_ARGS(&utils)));
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

        ComPtr<IDxcBlobEncoding> src;
        ThrowIfFailed(utils->LoadFile(path.c_str(), nullptr, &src));
        DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_UTF8 };

        std::wstring fname      = path.wstring();
        std::wstring includeDir = path.parent_path().wstring();
        std::vector<LPCWSTR> args = {
            fname.c_str(), L"-T", L"lib_6_3", L"-HV", L"2021",
            L"-I", includeDir.c_str(),
#if defined(_DEBUG)
            L"-Zi", L"-Od",
#else
            L"-O3",
#endif
        };

        ComPtr<IDxcIncludeHandler> includeHandler;
        ThrowIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler));

        ComPtr<IDxcResult> result;
        ThrowIfFailed(compiler->Compile(&buf, args.data(),
            static_cast<UINT32>(args.size()), includeHandler.Get(), IID_PPV_ARGS(&result)));

        HRESULT hr = S_OK;
        result->GetStatus(&hr);
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobUtf8> err;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
            if (err && err->GetStringLength() > 0)
                std::println("[DXC] {}", err->GetStringPointer());
            ThrowIfFailed(hr, path.string());
        }
        ComPtr<IDxcBlob> dxil;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), nullptr);
        return dxil;
    }
}

// ---------------------------------------------------------------
//  GBuffer PSO 빌드
//  셰이더: RayGen_GB, Miss_GB, ClosestHit_GB (GBuffer.hlsl)
//  페이로드: GBPayload = float3+float+float3+float = 32 bytes
// ---------------------------------------------------------------
void App::BuildGBufferPSO()
{
    auto dxil = CompileDXRLib(L"shaders/GBuffer.hlsl");

    D3D12_EXPORT_DESC exports[] = {
        { L"RayGen_GB",     nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"Miss_GB",       nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"ClosestHit_GB", nullptr, D3D12_EXPORT_FLAG_NONE },
    };
    D3D12_DXIL_LIBRARY_DESC lib{};
    lib.DXILLibrary = { dxil->GetBufferPointer(), dxil->GetBufferSize() };
    lib.NumExports  = static_cast<UINT>(std::size(exports));
    lib.pExports    = exports;

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport         = L"HitGroup_GB";
    hg.Type                   = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = L"ClosestHit_GB";

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    sc.MaxPayloadSizeInBytes   = 32;   // GBPayload: float3+float+float3+float
    sc.MaxAttributeSizeInBytes = 8;    // barycentrics

    D3D12_RAYTRACING_PIPELINE_CONFIG pc{};
    pc.MaxTraceRecursionDepth = 1;     // GBuffer: 기본 레이만, 그림자 없음

    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = m_globalRS.Get();

    D3D12_STATE_SUBOBJECT subs[5]{};
    subs[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,               &lib };
    subs[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,                  &hg  };
    subs[2] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,   &sc  };
    subs[3] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc  };
    subs[4] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,      &grs };

    D3D12_STATE_OBJECT_DESC soDesc{};
    soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = static_cast<UINT>(std::size(subs));
    soDesc.pSubobjects   = subs;

    ThrowIfFailed(m_core.Device()->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_gbufferPSO)));
    ThrowIfFailed(m_gbufferPSO.As(&m_gbufferPSOProps));

    ShaderTable::Desc stDesc{};
    stDesc.rayGenID   = m_gbufferPSOProps->GetShaderIdentifier(L"RayGen_GB");
    stDesc.missID     = m_gbufferPSOProps->GetShaderIdentifier(L"Miss_GB");
    stDesc.missID2    = nullptr;   // GBuffer에 그림자 레이 없음
    stDesc.hitGroupID = m_gbufferPSOProps->GetShaderIdentifier(L"HitGroup_GB");
    m_gbufferShaderTable.Build(m_core.Device(), stDesc);

    std::println("[App] GBuffer PSO 완료");
}

// ---------------------------------------------------------------
//  Shade PSO 빌드
//  셰이더: RayGen_Shade, MissIndirect_Shade, MissShadow_Shade,
//          ClosestHit_Shade  (ReSTIR_Shade.hlsl)
//
//  레이 인덱스:
//    Miss[0] = MissIndirect_Shade  – 간접광/굴절 레이 미스
//    Miss[1] = MissShadow_Shade    – 그림자 레이 미스
//    HitGroup[0] = HitGroup_Shade  – ClosestHit_Shade (간접광 bounce)
//
//  페이로드 최대: IndirectPayload (float3 + uint = 16 bytes)
//  재귀 깊이: 1 (RayGen에서만 TraceRay, ClosestHit_Shade 내 추가 레이 없음)
// ---------------------------------------------------------------
void App::BuildShadePSO()
{
    auto dxil = CompileDXRLib(L"shaders/ReSTIR_Shade.hlsl");

    D3D12_EXPORT_DESC exports[] = {
        { L"RayGen_Shade",       nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"MissIndirect_Shade", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"MissShadow_Shade",   nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"ClosestHit_Shade",   nullptr, D3D12_EXPORT_FLAG_NONE },
    };
    D3D12_DXIL_LIBRARY_DESC lib{};
    lib.DXILLibrary = { dxil->GetBufferPointer(), dxil->GetBufferSize() };
    lib.NumExports  = static_cast<UINT>(std::size(exports));
    lib.pExports    = exports;

    // 간접광/굴절 레이 ClosestHit 히트 그룹
    D3D12_HIT_GROUP_DESC hitGroup{};
    hitGroup.HitGroupExport         = L"HitGroup_Shade";
    hitGroup.Type                   = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = L"ClosestHit_Shade";

    // IndirectPayload: float3×4(48) + float(4) + uint×2(8) = 60 bytes
    // MaxPayloadSizeInBytes = 64 (ShadowPayload 4 포함하여 최대값)
    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    sc.MaxPayloadSizeInBytes   = 64;
    sc.MaxAttributeSizeInBytes = 8;

    // Depth 2: RayGen(0) → ClosestHit_Shade(1) → ShadowVis(2)
    // ClosestHit_Shade 내부에서 NEE shadow ray를 발사하므로 depth=2 필요
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{};
    pc.MaxTraceRecursionDepth = 2;

    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = m_globalRS.Get();

    D3D12_STATE_SUBOBJECT subs[5]{};
    subs[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,               &lib      };
    subs[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,                  &hitGroup };
    subs[2] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,   &sc       };
    subs[3] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc       };
    subs[4] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,      &grs      };

    D3D12_STATE_OBJECT_DESC soDesc{};
    soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = static_cast<UINT>(std::size(subs));
    soDesc.pSubobjects   = subs;

    ThrowIfFailed(m_core.Device()->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_shadePSO)));
    ThrowIfFailed(m_shadePSO.As(&m_shadePSOProps));

    ShaderTable::Desc stDesc{};
    stDesc.rayGenID   = m_shadePSOProps->GetShaderIdentifier(L"RayGen_Shade");
    stDesc.missID     = m_shadePSOProps->GetShaderIdentifier(L"MissIndirect_Shade");
    stDesc.missID2    = m_shadePSOProps->GetShaderIdentifier(L"MissShadow_Shade");
    stDesc.hitGroupID = m_shadePSOProps->GetShaderIdentifier(L"HitGroup_Shade");
    m_shadeShaderTable.Build(m_core.Device(), stDesc);

    std::println("[App] Shade PSO 완료 (직접광 ReSTIR + 1-bounce 간접광 + 유리 굴절)");
}

// ---------------------------------------------------------------
//  씬별 광원 리스트 생성
// ---------------------------------------------------------------
std::vector<LightData> App::BuildLightList(uint32_t sceneID) const
{
    std::vector<LightData> lights;
    LightData ld{};

    if (sceneID == 0)
    {
        // 씬 0 (야외): 태양 방향광
        float sx = 1.0f, sy = 2.0f, sz = -0.5f;
        float slen = sqrtf(sx*sx + sy*sy + sz*sz);
        ld.pos[0] = sx/slen; ld.pos[1] = sy/slen; ld.pos[2] = sz/slen;
        ld.intensity = 3.0f;
        ld.color[0] = 1.0f; ld.color[1] = 1.0f; ld.color[2] = 1.0f;
        ld.type = 1u;  // directional
        lights.push_back(ld);
    }
    else if (sceneID == 1)
    {
        // 씬 1 (실내): 포인트 라이트
        ld.pos[0] = 0.0f; ld.pos[1] = 3.8f; ld.pos[2] = 0.0f;
        ld.intensity = 8.0f;
        ld.color[0] = 1.0f; ld.color[1] = 1.0f; ld.color[2] = 1.0f;
        ld.type = 0u;  // point
        lights.push_back(ld);
    }
    else
    {
        // 씬 2 (구 쇼케이스): 포인트 라이트 2개
        ld.pos[0] = -1.5f; ld.pos[1] = 3.6f; ld.pos[2] = 0.0f;
        ld.intensity = 8.4f;
        ld.color[0] = 1.0f; ld.color[1] = 1.0f; ld.color[2] = 1.0f;
        ld.type = 0u;
        lights.push_back(ld);

        LightData ld2{};
        ld2.pos[0] = 1.5f; ld2.pos[1] = 3.6f; ld2.pos[2] = -1.5f;
        ld2.intensity = 6.6f;
        ld2.color[0] = 1.0f; ld2.color[1] = 1.0f; ld2.color[2] = 1.0f;
        ld2.type = 0u;
        lights.push_back(ld2);
    }

    return lights;
}

// ---------------------------------------------------------------
//  ReSTIRCB 업데이트 (매 프레임 OnRender에서 호출)
// ---------------------------------------------------------------
void App::UpdateReSTIRCB()
{
    ReSTIRCB cb{};

    // 이전 프레임 카메라
    const float* pp = m_prevCamera.Pos();
    const float* pr = m_prevCamera.Right();
    const float* pu = m_prevCamera.Up();
    const float* pf = m_prevCamera.Forward();

    cb.prevCamPos[0]  = pp[0]; cb.prevCamPos[1]  = pp[1]; cb.prevCamPos[2]  = pp[2];
    cb.prevCamRight[0]= pr[0]; cb.prevCamRight[1]= pr[1]; cb.prevCamRight[2]= pr[2];
    cb.prevCamUp[0]   = pu[0]; cb.prevCamUp[1]   = pu[1]; cb.prevCamUp[2]   = pu[2];
    cb.prevCamForward[0]=pf[0]; cb.prevCamForward[1]=pf[1]; cb.prevCamForward[2]=pf[2];
    cb.prevTanHalfFovY = 0.57735f;
    cb.prevAspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);

    // ReSTIR 파라미터
    auto lights = BuildLightList(m_sceneID);
    cb.lightCount      = static_cast<uint32_t>(lights.size());
    cb.candidateCount  = 32u;           // RIS 후보 수 (논문 권장)
    cb.screenW         = m_width;
    cb.screenH         = m_height;
    cb.frameIndex      = m_frameCount;
    cb.temporalMaxM    = 20.0f;         // 고스팅 방지 M 클램프 (Bitterli 2020 권장값)
    cb.spatialRadius   = 30u;           // 공간 재사용 반경 (픽셀)
    cb.spatialSamples  = 5u;            // 공간 이웃 샘플 수

    m_restir.UpdateCB(cb);
}
