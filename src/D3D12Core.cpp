#include "D3D12Core.h"
#include <print>
#include <format>

void D3D12Core::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    CreateDevice();
    CreateCommandObjects();
    CreateSwapChain(hwnd);
    CreateRTVs();

    // CBV/SRV/UAV 힙 – UAV(레이트레이싱 출력) + SRV(TLAS) 슬롯 포함
    m_cbvSrvUavHeap.Init(m_device.Get(),
                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                         64, true);

    // 텍스트 오버레이 (D3D11On12 + Direct2D + DirectWrite)
    m_textOverlay.Init(m_device.Get(), m_cmdQueue.Get(),
                       m_swapChain.Get(), width, height);

    // 펜스 생성
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

    std::println("[D3D12Core] 초기화 완료 ({}x{})", width, height);
}

void D3D12Core::Shutdown()
{
    FlushGPU();
    m_textOverlay.Shutdown();
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

void D3D12Core::SubmitAndFlush()
{
    ThrowIfFailed(m_cmdList->Close());
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    FlushGPU();
}

void D3D12Core::CreateDevice()
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug1> debugCtrl;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugCtrl))))
    {
        debugCtrl->EnableDebugLayer();
        debugCtrl->SetEnableGPUBasedValidation(TRUE);
        std::println("[D3D12Core] 디버그 레이어 활성화");
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(
#if defined(_DEBUG)
        DXGI_CREATE_FACTORY_DEBUG,
#else
        0,
#endif
        IID_PPV_ARGS(&m_factory)));

    // DXR 지원 어댑터 탐색
    ComPtr<IDXGIAdapter1> adapter;
    for (uint32_t i = 0;
         m_factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                                        D3D_FEATURE_LEVEL_12_1,
                                        IID_PPV_ARGS(&m_device))))
        {
            // DXR 지원 확인
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
            if (SUCCEEDED(m_device->CheckFeatureSupport(
                    D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5))) &&
                opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0)
            {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                // wprintf는 narrow-mode stdout과 혼용 불가 → UTF-8 변환 후 출력
                int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                             nullptr, 0, nullptr, nullptr);
                std::string adapterName(static_cast<size_t>(len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                    adapterName.data(), len, nullptr, nullptr);
                std::println("[D3D12Core] 어댑터: {}", adapterName);
                return;
            }
            m_device.Reset();
        }
    }
    throw std::runtime_error("DXR을 지원하는 GPU를 찾을 수 없습니다.");
}

void D3D12Core::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_cmdQueue)));

    for (auto i : std::views::iota(0u, k_backBufferCount))
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc[i])));

    ThrowIfFailed(m_device->CreateCommandList1(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&m_cmdList)));
}

void D3D12Core::CreateSwapChain(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = m_width;
    desc.Height      = m_height;
    desc.Format      = k_backBufferFormat;
    desc.SampleDesc  = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = k_backBufferCount;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
        m_cmdQueue.Get(), hwnd, &desc, nullptr, nullptr, &sc1));
    ThrowIfFailed(sc1.As(&m_swapChain));
    m_backBufferIdx = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12Core::CreateRTVs()
{
    m_rtvHeap.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                   k_backBufferCount, false);

    for (auto i : std::views::iota(0u, k_backBufferCount))
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
        m_device->CreateRenderTargetView(
            m_backBuffers[i].Get(), nullptr, m_rtvHeap.GetHandle(i).cpu);
    }
}

void D3D12Core::BeginFrame()
{
    auto& alloc = m_cmdAlloc[m_backBufferIdx];
    ThrowIfFailed(alloc->Reset());
    ThrowIfFailed(m_cmdList->Reset(alloc.Get(), nullptr));
}

void D3D12Core::EndFrame(float fps)
{
    ThrowIfFailed(m_cmdList->Close());

    ID3D12CommandList* lists[] = {m_cmdList.Get()};
    m_cmdQueue->ExecuteCommandLists(1, lists);

    // FPS 텍스트 오버레이: D3D12 Execute 이후, Present 이전에 렌더
    m_textOverlay.DrawFPS(fps, m_backBufferIdx);

    ThrowIfFailed(m_swapChain->Present(1, 0));

    // 펜스 신호
    const uint64_t val = ++m_fenceCounter;
    m_fenceValues[m_backBufferIdx] = val;
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), val));

    m_backBufferIdx = m_swapChain->GetCurrentBackBufferIndex();

    // 다음 프레임 할당자가 사용 가능할 때까지 대기
    if (m_fence->GetCompletedValue() < m_fenceValues[m_backBufferIdx])
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(
            m_fenceValues[m_backBufferIdx], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12Core::FlushGPU()
{
    const uint64_t val = ++m_fenceCounter;
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), val));
    if (m_fence->GetCompletedValue() < val)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(val, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}
