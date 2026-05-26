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
        cbDesc.Width            = 512;  // sizeof(SceneCB)=320, D3D12 256바이트 정렬 → 512
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

    // DLSS 초기화 (NVIDIA GPU + 지원 드라이버 필요)
    // NGX_D3D12_CREATE_DLSS_EXT 가 GPU 커맨드를 기록하므로 열린 cmdList 필요
    ThrowIfFailed(m_core.CmdAlloc(0)->Reset());
    ThrowIfFailed(m_core.CmdList()->Reset(m_core.CmdAlloc(0), nullptr));

    // BuildDescriptors() 이후에 호출 → 힙 슬롯 7..13 사용
    // shader-visible 힙은 CopyDescriptors src 불가 → 리소스 직접 전달
    if (m_dlss.Init(m_core.Device(), m_core.CmdList(),
                    width, height,
                    m_core.CbvSrvUavHeap(),
                    m_tlas.Resource()->GetGPUVirtualAddress(),
                    m_planeBLAS.VertexBuffer(), m_planeBLAS.VertexCount(),
                    m_cubeBLAS.VertexBuffer(),  m_cubeBLAS.VertexCount(),
                    m_roomBLAS.VertexBuffer(),  m_roomBLAS.VertexCount(),
                    m_sphereBLAS.VertexBuffer(), m_sphereBLAS.VertexCount()))
    {
        m_dlssEnabled = true;
        std::println("[App] DLSS 기본 활성화 — U 키로 토글");

        // NRD 디노이저 초기화 (DLSS 렌더 해상도 기준)
        //   파이프라인: PT(1spp lobe-separated) → NRDDenoiser → Composite → DLSS-RR
        if (m_nrdDenoiser.Init(m_core.Device(), m_dlss.RenderWidth(), m_dlss.RenderHeight()))
        {
            std::println("[App] NRD 디노이저 활성화");
        }
        else
        {
            std::println("[App] NRD 디노이저 초기화 실패 — DLSS-RR 만 사용");
        }
    }
    else
    {
        std::println("[App] DLSS 비활성 (비NVIDIA GPU 또는 미지원 드라이버)");
    }

    // DLSS 피처 생성 커맨드를 GPU에 제출
    m_core.SubmitAndFlush();

    m_sceneBuilt = true;
    std::println("[App] 초기화 완료 - 씬 0 (야외)");
    std::println("[App] 조작: WASD 이동, IJKL 시점, QE 상하, 1/2/3 씬 전환, R 디노이저, U DLSS, ESC 종료");
}

void App::Shutdown()
{
    m_core.FlushGPU();
    m_nrdDenoiser.Shutdown();
    m_dlss.Shutdown(m_core.Device());
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
// 디스크립터 힙 구성 (일반 모드)
// 슬롯 0: UAV u0 (g_output,       RGBA8        – RenderTarget::Init 내부)
// 슬롯 1: UAV u1 (g_accumulation, RGBA32F      – RenderTarget::Init 내부)
// 슬롯 2: UAV u2 (g_depth,        R32_FLOAT       full-res — NDC depth)
// 슬롯 3: UAV u3 (g_motionVec,    RGBA16F         full-res — 비DLSS: oct-법선+roughness / DLSS: 모션벡터)
// 슬롯 4: UAV u4 (g_normals,      RGBA16F         full-res — world-법선(xyz)+roughness(w))
// 슬롯 5: UAV u5 (g_diffAlbedo,   RGBA16F         full-res — DLSS-RR diffuse albedo)
// 슬롯 6: UAV u6 (g_specAlbedo,   RGBA16F         full-res — DLSS-RR specular F0)
// 슬롯 7: UAV u7 (g_diffRadHit,   RGBA16F         full-res — NRD diff radiance + hitDist)
// 슬롯 8: UAV u8 (g_specRadHit,   RGBA16F         full-res — NRD spec radiance + hitDist)
// 슬롯 9: UAV u9 (g_viewZ,        R16_FLOAT       full-res — NRD linear view Z)
// 슬롯10: SRV t0 (TLAS)
// 슬롯11: SRV t1 (plane VB)
// 슬롯12: SRV t2 (cube VB)
// 슬롯13: SRV t3 (room VB)
// 슬롯14: SRV t4 (sphere VB)
// DLSS 모드 추가 슬롯 (DLSSIntegration::Init 에서 할당, 슬롯 15부터):
// 슬롯15: UAV m_renderColor, 슬롯16: UAV m_renderAccum
// 슬롯17: UAV m_depth,       슬롯18: UAV m_motionVec
// 슬롯19: UAV m_renderNormal, 슬롯20: UAV m_diffuseAlbedo, 슬롯21: UAV m_specularAlbedo
// 슬롯22: UAV m_nrdDiffRad, 슬롯23: UAV m_nrdSpecRad, 슬롯24: UAV m_nrdViewZ
// 슬롯25: SRV TLAS 미러, 슬롯26-29: SRV VB 미러
// ---------------------------------------------------------------
void App::BuildDescriptors()
{
    auto& heap   = m_core.CbvSrvUavHeap();
    auto* device = m_core.Device();

    // 슬롯 0,1: UAV u0(g_output), u1(g_accumulation)
    m_renderTarget.Init(device, heap, m_width, m_height);

    // 슬롯 2,3: G-Buffer UAV (full-res) — RT 셰이더가 항상 기록, A-trous 디노이저가 읽음
    auto makeUAVTexFull = [&](DXGI_FORMAT fmt, uint32_t w, uint32_t h) -> ComPtr<ID3D12Resource>
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc = {1, 0};
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> res;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)));
        return res;
    };
    if (!m_gbufferDepth)      m_gbufferDepth      = makeUAVTexFull(DXGI_FORMAT_R32_FLOAT,           m_width, m_height);
    if (!m_gbufferNormal)     m_gbufferNormal     = makeUAVTexFull(DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height);
    if (!m_gbufferDiffAlbedo) m_gbufferDiffAlbedo = makeUAVTexFull(DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height);
    if (!m_gbufferSpecAlbedo) m_gbufferSpecAlbedo = makeUAVTexFull(DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height);
    if (!m_nrdDiffRadiance)   m_nrdDiffRadiance   = makeUAVTexFull(DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height);
    if (!m_nrdSpecRadiance)   m_nrdSpecRadiance   = makeUAVTexFull(DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height);
    if (!m_nrdViewZ)          m_nrdViewZ          = makeUAVTexFull(DXGI_FORMAT_R16_FLOAT,          m_width, m_height);
    m_gbufferDepth     ->SetName(L"GBuffer_Depth");
    m_gbufferNormal    ->SetName(L"GBuffer_Normal");
    m_gbufferDiffAlbedo->SetName(L"GBuffer_DiffAlbedo");
    m_gbufferSpecAlbedo->SetName(L"GBuffer_SpecAlbedo");
    m_nrdDiffRadiance  ->SetName(L"NRD_DiffRadianceHitDist");
    m_nrdSpecRadiance  ->SetName(L"NRD_SpecRadianceHitDist");
    m_nrdViewZ         ->SetName(L"NRD_ViewZ");

    {
        DescriptorHandle h = heap.Allocate();  // slot 2
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(m_gbufferDepth.Get(), nullptr, &uav, h.cpu);
    }
    {
        DescriptorHandle h = heap.Allocate();  // slot 3
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_gbufferNormal.Get(), nullptr, &uav, h.cpu);
    }
    {
        DescriptorHandle h = heap.Allocate();  // slot 4: u4 (g_normals) — oct-법선(xy)+roughness(w)
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_gbufferNormal.Get(), nullptr, &uav, h.cpu);
    }
    {
        DescriptorHandle h = heap.Allocate();  // slot 5: u5 (g_diffAlbedo) — DLSS-RR diffuse albedo
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_gbufferDiffAlbedo.Get(), nullptr, &uav, h.cpu);
    }
    {
        DescriptorHandle h = heap.Allocate();  // slot 6: u6 (g_specAlbedo) — DLSS-RR specular F0
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_gbufferSpecAlbedo.Get(), nullptr, &uav, h.cpu);
    }
    {
        DescriptorHandle h = heap.Allocate();  // slot 7: u7 (g_diffRadHit) — NRD diff radiance + hitDist
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_nrdDiffRadiance.Get(), nullptr, &uav, h.cpu);
    }
    {
        DescriptorHandle h = heap.Allocate();  // slot 8: u8 (g_specRadHit) — NRD spec radiance + hitDist
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(m_nrdSpecRadiance.Get(), nullptr, &uav, h.cpu);
    }
    {
        DescriptorHandle h = heap.Allocate();  // slot 9: u9 (g_viewZ) — NRD linear view Z
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        device->CreateUnorderedAccessView(m_nrdViewZ.Get(), nullptr, &uav, h.cpu);
    }

    // 슬롯 10: SRV (TLAS)  ← 슬롯 0..9는 UAV
    m_tlasSRV = heap.Allocate();
    RebuildTLASSRV();

    // 슬롯 6: SRV (plane VB)
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

    // 슬롯 7: SRV (cube VB)
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

    // 슬롯 8: SRV (room VB)
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

    // 슬롯 9: SRV (sphere VB)
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

    // 씬 전환 후 이전 카메라를 현재 카메라로 초기화 — 첫 프레임 쓰레기 MV 방지
    {
        const float* pos     = m_camera.Pos();
        const float* right   = m_camera.Right();
        const float* up      = m_camera.Up();
        const float* forward = m_camera.Forward();
        std::memcpy(m_prevCamPos,     pos,     sizeof(float) * 3);
        std::memcpy(m_prevCamRight,   right,   sizeof(float) * 3);
        std::memcpy(m_prevCamUp,      up,      sizeof(float) * 3);
        std::memcpy(m_prevCamForward, forward, sizeof(float) * 3);
        m_prevTanHalfFovY = 0.57735f;
        m_prevAspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
    }

    m_core.SubmitAndFlush();

    // TLAS SRV 갱신 (메인 슬롯 + DLSS 미러 슬롯)
    RebuildTLASSRV();
    m_dlss.RefreshTLASSRV(m_core.Device(), m_tlas.Resource()->GetGPUVirtualAddress());
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
    // DLSS 모드: 프레임당 1spp 사용 (DLSS가 시간적 누적 담당)
    cb.frameCount = (m_dlssEnabled && m_dlss.IsAvailable()) ? 0u : m_frameCount;
    cb.randomSeed = m_frameCount;  // 프레임마다 달라지는 시드

    // DLSS Halton 지터 (비DLSS 시 0 → 셰이더에서 무효)
    if (m_dlssEnabled && m_dlss.IsAvailable())
    {
        // Halton(base2, base3), 1-indexed, 32 프레임 주기
        auto halton = [](int i, int b) -> float {
            float f = 1.0f, r = 0.0f;
            while (i > 0) { f /= b; r += f * (i % b); i /= b; }
            return r;
        };
        int hi         = int(m_dlssFrameIdx % 32) + 1;
        m_dlssJitterX  = halton(hi, 2) - 0.5f;
        m_dlssJitterY  = halton(hi, 3) - 0.5f;
        cb.jitterX     = m_dlssJitterX;
        cb.jitterY     = m_dlssJitterY;
    }
    else
    {
        m_dlssJitterX = cb.jitterX = 0.0f;
        m_dlssJitterY = cb.jitterY = 0.0f;
    }

    // 이전 프레임 카메라 (DLSS 모션벡터 계산용)
    std::memcpy(cb.prevCamPos,     m_prevCamPos,     sizeof(float) * 3);
    cb.isDLSSMode = (m_dlssEnabled && m_dlss.IsAvailable()) ? 1u : 0u;
    std::memcpy(cb.prevCamRight,   m_prevCamRight,   sizeof(float) * 3);
    cb.prevTanHalfFovY = m_prevTanHalfFovY;
    std::memcpy(cb.prevCamUp,      m_prevCamUp,      sizeof(float) * 3);
    cb.prevAspectRatio  = m_prevAspectRatio;
    std::memcpy(cb.prevCamForward, m_prevCamForward, sizeof(float) * 3);

    // 현재 카메라를 다음 프레임의 "이전 카메라"로 저장
    std::memcpy(m_prevCamPos,     pos,     sizeof(float) * 3);
    std::memcpy(m_prevCamRight,   right,   sizeof(float) * 3);
    std::memcpy(m_prevCamUp,      up,      sizeof(float) * 3);
    std::memcpy(m_prevCamForward, forward, sizeof(float) * 3);
    m_prevTanHalfFovY = cb.tanHalfFovY;
    m_prevAspectRatio  = cb.aspectRatio;

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
    else if (key == 'R')
    {
        m_denoiseEnabled   = !m_denoiseEnabled;
        m_denoiser.enabled = m_denoiseEnabled;
        m_frameCount       = 0;
        m_cameraMoved      = true;
        m_accumDirty       = true;
        if (m_dlssEnabled && m_dlss.IsAvailable())
            std::println("[App] DLSS 모드: denoiser는 DLSS 내부 처리 (R키 무효)");
        else
            std::println("[App] A-trous 디노이저 {}", m_denoiseEnabled ? "ON" : "OFF");
    }
    else if (key == 'U')
    {
        if (m_dlss.IsAvailable())
        {
            m_dlssEnabled  = !m_dlssEnabled;
            m_dlssFrameIdx = 0;
            m_frameCount   = 0;
            m_cameraMoved  = true;
            m_accumDirty   = true;
            std::println("[App] DLSS {} ({}x{} → {}x{}){}",
                         m_dlssEnabled ? "ON" : "OFF",
                         m_dlss.RenderWidth(), m_dlss.RenderHeight(),
                         m_width, m_height,
                         (m_dlssEnabled && m_denoiseEnabled) ? " + A-trous render-res" : "");
        }
        else
        {
            std::println("[App] DLSS 미지원 GPU");
        }
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

    // DLSS 시간적 히스토리 리셋 여부를 누적 버퍼 클리어 전에 캡처
    bool dlssReset = m_accumDirty;

    // 씬 전환 / 디노이저 토글 시: 이전 씬 잔상 제거
    if (m_accumDirty)
    {
        m_renderTarget.ClearAccumulation(cmd);
        m_accumDirty = false;
    }

    cmd->SetComputeRootSignature(m_globalRS.Get());
    cmd->SetComputeRootConstantBufferView(1, m_sceneCB->GetGPUVirtualAddress());
    cmd->SetPipelineState1(m_pipeline.PSO());

    const auto& st = m_pipeline.GetShaderTable();

    if (m_dlssEnabled && m_dlss.IsAvailable())
    {
        // ── DLSS 경로 ─────────────────────────────────────────────
        // 디스크립터 테이블 베이스를 힙 슬롯 15 으로 (render-res UAV u0..u9 + SRV 미러)
        cmd->SetComputeRootDescriptorTable(0,
            m_core.CbvSrvUavHeap().GetHandle(15).gpu);

        D3D12_DISPATCH_RAYS_DESC dr{};
        dr.RayGenerationShaderRecord = st.RayGenRange();
        dr.MissShaderTable           = st.MissRange();
        dr.HitGroupTable             = st.HitGroupRange();
        dr.Width                     = m_dlss.RenderWidth();
        dr.Height                    = m_dlss.RenderHeight();
        dr.Depth                     = 1;
        cmd->DispatchRays(&dr);

        // RT 셰이더 쓰기 완료 보장 (renderColor, renderAccum, depth, motionVec, renderNormal)
        m_dlss.UAVBarriers(cmd);

        // DLSS-RR: AI 기반 denoising + temporal accumulation + upscaling 통합 1패스
        // 입력: raw 1spp HDR(renderAccum), depth, motionVec, normals
        m_dlss.Evaluate(cmd, m_dlssJitterX, m_dlssJitterY, dlssReset);

        ++m_dlssFrameIdx;

        m_dlss.CopyOutputToBackBuffer(cmd, m_core.BackBuffer());
    }
    else
    {
        // ── 일반 경로 (누적 + A-trous 디노이저) ───────────────────
        cmd->SetComputeRootDescriptorTable(0,
            m_core.CbvSrvUavHeap().GetHandle(0).gpu);

        D3D12_DISPATCH_RAYS_DESC dr{};
        dr.RayGenerationShaderRecord = st.RayGenRange();
        dr.MissShaderTable           = st.MissRange();
        dr.HitGroupTable             = st.HitGroupRange();
        dr.Width                     = m_width;
        dr.Height                    = m_height;
        dr.Depth                     = 1;
        cmd->DispatchRays(&dr);

        m_renderTarget.UAVBarriers(cmd);

        // G-Buffer depth/normal UAV 배리어 (RT 셰이더 쓰기 완료 보장)
        {
            D3D12_RESOURCE_BARRIER gbBarriers[2]{};
            gbBarriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            gbBarriers[0].UAV.pResource = m_gbufferDepth.Get();
            gbBarriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            gbBarriers[1].UAV.pResource = m_gbufferNormal.Get();
            cmd->ResourceBarrier(2, gbBarriers);
        }

        if (m_denoiseEnabled)
        {
            m_denoiser.Apply(cmd,
                             m_renderTarget.AccumResource(),
                             m_renderTarget.Resource(),
                             m_gbufferDepth.Get(),
                             m_gbufferNormal.Get());
        }

        m_renderTarget.CopyToBackBuffer(cmd, m_core.BackBuffer());
    }

    m_core.EndFrame(m_smoothFps, m_denoiseEnabled);
}

void App::OnResize(uint32_t width, uint32_t height)
{
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;

    // DLSS 텍스처는 초기 해상도에 고정된다.
    // 리사이즈 시 그대로 두면 render-res / display-res 불일치로 Evaluate가 잘못된
    // 해상도 버퍼를 읽어 크래시 또는 아티팩트가 발생하므로 DLSS를 비활성화한다.
    // (완전한 resize 지원은 FlushGPU → Shutdown → 힙 슬롯 9+ 재초기화 → Init 필요)
    if (m_dlssEnabled)
    {
        m_dlssEnabled = false;
        m_frameCount  = 0;
        m_accumDirty  = true;
        std::println("[App] 리사이즈 {}x{} — DLSS 비활성화 (재시작 시 복원)", width, height);
    }
    else
    {
        std::println("[App] 리사이즈: {}x{}", width, height);
    }
}
