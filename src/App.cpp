#include "App.h"
#include <print>
#include <cstring>
#include <cmath>
#include <vector>

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

        ThrowIfFailed(m_core.Device()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_sceneCBPass2)));
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

    m_sceneBuilt = true;
    std::println("[App] 초기화 완료 - 씬 0 (야외)");
    std::println("[App] 조작: WASD 이동, IJKL 시점, QE 상하, 1/2/3 씬 전환, D 디노이저 토글, ESC 종료");
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
// 슬롯 2: UAV u2 (g_fresnel,      R32F    – RenderTarget::Init 내부)
// 슬롯 3: UAV u3 (g_depth,        R32F    – RenderTarget::Init 내부)
// 슬롯 4: UAV u4 (g_normal,       RGBA32F – RenderTarget::Init 내부)
// 슬롯 5: UAV u5 (g_accumSq,      R32F    – RenderTarget::Init 내부)
// 슬롯 6: SRV t0 (TLAS)
// 슬롯 7: SRV t1 (plane VB)
// 슬롯 8: SRV t2 (cube VB)
// 슬롯 9: SRV t3 (room VB)
// 슬롯 10: SRV t4 (sphere VB)
// ---------------------------------------------------------------
void App::BuildDescriptors()
{
    auto& heap   = m_core.CbvSrvUavHeap();
    auto* device = m_core.Device();

    // 슬롯 0~4: UAV (렌더 타겟 + GBuffer)
    m_renderTarget.Init(device, heap, m_width, m_height);

    // 슬롯 6: SRV (TLAS)  ← 슬롯 0..5는 RenderTarget::Init에서 UAV로 사용
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
        instances[2].mask         = 0xFF; // 그림자 캐스팅 (반투명)

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
        cb.lightIntensity=7.0f;
        cb.lightColor[0]=1.0f; cb.lightColor[1]=1.0f; cb.lightColor[2]=1.0f;

        // Light 2: 오른쪽 위
        cb.light2Pos[0]=1.5f; cb.light2Pos[1]=3.6f; cb.light2Pos[2]=-1.5f;
        cb.light2Intensity=5.5f;
        cb.light2Color[0]=1.0f; cb.light2Color[1]=1.0f; cb.light2Color[2]=1.0f;

        // mat0: 방 벽/바닥/천장 – 흰색
        cb.matAlbedoRoughness[0][0]=0.85f; cb.matAlbedoRoughness[0][1]=0.85f;
        cb.matAlbedoRoughness[0][2]=0.85f; cb.matAlbedoRoughness[0][3]=0.90f;
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

    // 패스 트레이싱 누적 파라미터
    cb.frameCount        = m_frameCount;
    cb.randomSeed        = m_frameCount;  // 프레임마다 달라지는 시드
    cb.debugMode         = m_debugMode;
    cb.fresnelThreshold  = m_fresnelThreshold;

    // 업로드 버퍼에 직접 기록
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    ThrowIfFailed(m_sceneCB->Map(0, &readRange, &mapped));
    std::memcpy(mapped, &cb, sizeof(SceneCB));
    m_sceneCB->Unmap(0, nullptr);

    // Pass2 CBV: debugMode 비트8=1 (passIndex=1)
    SceneCB cbPass2 = cb;
    cbPass2.debugMode |= (1u << 8u);
    ThrowIfFailed(m_sceneCBPass2->Map(0, &readRange, &mapped));
    std::memcpy(mapped, &cbPass2, sizeof(SceneCB));
    m_sceneCBPass2->Unmap(0, nullptr);
}

// ---------------------------------------------------------------
// 키 다운 이벤트 (WM_KEYDOWN에서 호출)
// ---------------------------------------------------------------
void App::OnKeyDown(uint32_t key)
{
    if (key == '1') SwitchScene(0);
    else if (key == '2') SwitchScene(1);
    else if (key == '3') SwitchScene(2);
    else if (key == 'R')
    {
        m_denoiseEnabled        = !m_denoiseEnabled;
        m_denoiser.enabled      = m_denoiseEnabled;
        m_frameCount            = 0;  // 누적 초기화 (노이즈 기준이 달라지므로)
        m_cameraMoved           = true;
        m_accumDirty            = true;  // 디노이저 상태 변화 → 누적 버퍼 클리어 필요
        std::println("[App] 디노이저 {}", m_denoiseEnabled ? "ON" : "OFF");
    }
    else if (key == 'V')
    {
        m_debugMode = (m_debugMode + 1u) % 6u;
        static constexpr const char* k_names[] = {
            "PT (표준)", "Fresnel map", "Variance", "Depth edge", "Normal edge", "Threshold mask"
        };
        std::println("[App] debugMode → {} ({})", m_debugMode, k_names[m_debugMode]);
    }
    else if (key == VK_OEM_4)  // '[' — 임계값 낮추기
    {
        m_fresnelThreshold = std::max(0.01f, m_fresnelThreshold - 0.05f);
        std::println("[App] fresnelThreshold → {:.2f}", m_fresnelThreshold);
    }
    else if (key == VK_OEM_6)  // ']' — 임계값 높이기
    {
        m_fresnelThreshold = std::min(1.00f, m_fresnelThreshold + 0.05f);
        std::println("[App] fresnelThreshold → {:.2f}", m_fresnelThreshold);
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
        m_cameraMoved = false;
    }
    else
    {
        m_frameCount++;
    }

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

    cmd->DispatchRays(&dispatchDesc);  // Pass 1

    // Pass1 쓰기 완료 보장 (g_fresnel, g_accumulation 등 Pass2가 읽는 리소스)
    m_renderTarget.UAVBarriers(cmd);

    // Pass 2: Fresnel-guided extra samples
    cmd->SetComputeRootConstantBufferView(1, m_sceneCBPass2->GetGPUVirtualAddress());
    cmd->DispatchRays(&dispatchDesc);

    // Pass2 쓰기 완료 보장
    m_renderTarget.UAVBarriers(cmd);

    // 디노이저가 활성화된 경우: g_accumulation → (A-trous 3패스) → g_output 덮어쓰기
    if (m_denoiseEnabled)
    {
        m_denoiser.Apply(cmd,
                         m_renderTarget.AccumResource(),
                         m_renderTarget.Resource());
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
