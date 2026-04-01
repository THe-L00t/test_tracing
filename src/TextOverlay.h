#pragma once
#include "Common.h"
#include <d3d11on12.h>
#include <d2d1_3.h>
#include <dwrite.h>

// D3D11On12 + Direct2D + DirectWrite 를 이용한 화면 텍스트 오버레이
// D3D12 ExecuteCommandLists() 이후 Present() 이전에 DrawFPS()를 호출한다.
class TextOverlay
{
public:
    // D3D12 장치·큐·스왑체인으로 초기화
    void Init(ID3D12Device*       device,
              ID3D12CommandQueue* cmdQueue,
              IDXGISwapChain3*    swapChain,
              uint32_t            width,
              uint32_t            height);

    void Shutdown();

    // backBufferIdx: 현재 프레임의 백버퍼 인덱스
    void DrawFPS(float fps, uint32_t backBufferIdx);

    bool IsReady() const noexcept { return m_ready; }

private:
    void CreatePerBufferResources(IDXGISwapChain3* swapChain,
                                  uint32_t         width,
                                  uint32_t         height);

    ComPtr<ID3D11On12Device>     m_d3d11On12;
    ComPtr<ID3D11DeviceContext>  m_d3d11Context;
    ComPtr<ID2D1Factory3>        m_d2dFactory;
    ComPtr<ID2D1Device2>         m_d2dDevice;
    ComPtr<ID2D1DeviceContext2>  m_d2dContext;
    ComPtr<IDWriteFactory>       m_dwriteFactory;
    ComPtr<IDWriteTextFormat>    m_textFormat;
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<ID2D1SolidColorBrush> m_shadowBrush;

    // 백버퍼 당 리소스 (스왑체인 버퍼 수만큼)
    ComPtr<ID3D11Resource>       m_wrappedBuffers[k_backBufferCount];
    ComPtr<ID2D1Bitmap1>         m_d2dTargets[k_backBufferCount];

    bool m_ready = false;
};
