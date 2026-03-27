#pragma once
#include "Common.h"
#include "DescriptorHeap.h"

// D3D12 디바이스, 커맨드 큐, 스왑체인, 동기화 원시체 관리
class D3D12Core
{
public:
    void Init(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    // 프레임 시작 / 끝
    void BeginFrame();
    void EndFrame();

    // 커맨드 리스트 제출 후 GPU 완전 대기 (Present 없음, 초기화용)
    void SubmitAndFlush();

    // GPU 완전 대기
    void FlushGPU();

    // 접근자
    ID3D12Device5*              Device()       const noexcept { return m_device.Get(); }
    ID3D12CommandAllocator*     CmdAlloc(uint32_t i) const noexcept { return m_cmdAlloc[i].Get(); }
    ID3D12GraphicsCommandList4* CmdList()      const noexcept { return m_cmdList.Get(); }
    ID3D12CommandQueue*         CmdQueue()     const noexcept { return m_cmdQueue.Get(); }
    IDXGISwapChain3*            SwapChain()    const noexcept { return m_swapChain.Get(); }
    DescriptorHeap&             RtvHeap()      noexcept       { return m_rtvHeap; }
    DescriptorHeap&             CbvSrvUavHeap()noexcept       { return m_cbvSrvUavHeap; }
    uint32_t                    BackBufferIdx()const noexcept { return m_backBufferIdx; }
    uint32_t                    Width()        const noexcept { return m_width; }
    uint32_t                    Height()       const noexcept { return m_height; }
    ID3D12Resource*             BackBuffer()   const noexcept { return m_backBuffers[m_backBufferIdx].Get(); }

private:
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd);
    void CreateRTVs();

    ComPtr<IDXGIFactory6>             m_factory;
    ComPtr<ID3D12Device5>             m_device;
    ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    ComPtr<ID3D12CommandAllocator>    m_cmdAlloc[k_backBufferCount];
    ComPtr<ID3D12GraphicsCommandList4>m_cmdList;
    ComPtr<IDXGISwapChain3>           m_swapChain;
    ComPtr<ID3D12Resource>            m_backBuffers[k_backBufferCount];

    DescriptorHeap m_rtvHeap;
    DescriptorHeap m_cbvSrvUavHeap;

    ComPtr<ID3D12Fence> m_fence;
    HANDLE              m_fenceEvent = nullptr;
    uint64_t            m_fenceValues[k_backBufferCount]{};
    uint64_t            m_fenceCounter = 0;

    uint32_t m_backBufferIdx = 0;
    uint32_t m_width  = k_defaultWidth;
    uint32_t m_height = k_defaultHeight;
};
