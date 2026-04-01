#include "TextOverlay.h"
#include <print>
#include <cstdio>

// D3D11On12 / D2D / DWrite 라이브러리 자동 링크
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

void TextOverlay::Init(ID3D12Device*       device,
                       ID3D12CommandQueue* cmdQueue,
                       IDXGISwapChain3*    swapChain,
                       uint32_t            width,
                       uint32_t            height)
{
    // ── 1. D3D11On12 디바이스 생성 ──────────────────────────────
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        ComPtr<ID3D11Device> d3d11Dev;
        IUnknown* queues[] = { cmdQueue };
        HRESULT hr = D3D11On12CreateDevice(
            device, flags,
            nullptr, 0,
            queues, 1, 0,
            &d3d11Dev, &m_d3d11Context, nullptr);
        if (FAILED(hr))
        {
            std::println("[TextOverlay] D3D11On12 디바이스 생성 실패 (0x{:08X})", static_cast<uint32_t>(hr));
            return;
        }
        ThrowIfFailed(d3d11Dev.As(&m_d3d11On12));
    }

    // ── 2. Direct2D 팩토리 + 디바이스 + 컨텍스트 ───────────────
    {
        D2D1_FACTORY_OPTIONS opts{};
#if defined(_DEBUG)
        opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        ThrowIfFailed(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            opts,
            m_d2dFactory.GetAddressOf()));

        ComPtr<IDXGIDevice> dxgiDevice;
        ThrowIfFailed(m_d3d11On12.As(&dxgiDevice));

        ComPtr<ID2D1Device> d2dDev;
        ThrowIfFailed(m_d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDev));
        ThrowIfFailed(d2dDev.As(&m_d2dDevice));
        ThrowIfFailed(m_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext));
    }

    // ── 3. DirectWrite 팩토리 + 텍스트 포맷 ─────────────────────
    {
        ThrowIfFailed(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())));

        // Consolas 18pt, 굵게
        ThrowIfFailed(m_dwriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            18.0f, L"ko-KR",
            &m_textFormat));

        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    // ── 4. 브러시 생성 ──────────────────────────────────────────
    {
        // 흰색 본문
        ThrowIfFailed(m_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), &m_textBrush));
        // 검은색 그림자
        ThrowIfFailed(m_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f), &m_shadowBrush));
    }

    // ── 5. 백버퍼 당 래핑 리소스 생성 ──────────────────────────
    CreatePerBufferResources(swapChain, width, height);

    m_ready = true;
    std::println("[TextOverlay] 초기화 완료 (FPS 오버레이 활성화)");
}

void TextOverlay::CreatePerBufferResources(IDXGISwapChain3* swapChain,
                                           uint32_t         /*width*/,
                                           uint32_t         /*height*/)
{
    for (uint32_t i = 0; i < k_backBufferCount; ++i)
    {
        // 스왑체인 백버퍼 참조 획득
        ComPtr<ID3D12Resource> bb;
        ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb)));

        // D3D11On12 래핑 리소스 생성
        // InState  = PRESENT: D3D12가 Copy 후 PRESENT 상태로 남겨둠
        // OutState = PRESENT: D2D 작업 후 다시 PRESENT 상태로 반환
        D3D11_RESOURCE_FLAGS d3d11Flags{ D3D11_BIND_RENDER_TARGET };
        ThrowIfFailed(m_d3d11On12->CreateWrappedResource(
            bb.Get(), &d3d11Flags,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_PRESENT,
            IID_PPV_ARGS(&m_wrappedBuffers[i])));

        // DXGI 서피스 → D2D 비트맵(렌더 타겟) 생성
        ComPtr<IDXGISurface> surface;
        ThrowIfFailed(m_wrappedBuffers[i].As(&surface));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

        ThrowIfFailed(m_d2dContext->CreateBitmapFromDxgiSurface(
            surface.Get(), &bmpProps, &m_d2dTargets[i]));
    }
}

void TextOverlay::DrawFPS(float fps, uint32_t backBufferIdx, bool denoiserOn)
{
    if (!m_ready) return;

    // D3D11On12: 백버퍼 리소스 획득 (D3D12 GPU 작업 완료 대기 포함)
    ID3D11Resource* res[] = { m_wrappedBuffers[backBufferIdx].Get() };
    m_d3d11On12->AcquireWrappedResources(res, 1);

    // D2D 렌더 타겟 설정
    m_d2dContext->SetTarget(m_d2dTargets[backBufferIdx].Get());
    m_d2dContext->BeginDraw();
    m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());

    // 텍스트 조합: "FPS: X.X | DNSR: ON" 또는 "FPS: X.X"
    wchar_t buf[128];
    if (denoiserOn)
        swprintf_s(buf, L"FPS: %.1f  |  DNSR: ON", fps);
    else
        swprintf_s(buf, L"FPS: %.1f", fps);
    UINT32 len = static_cast<UINT32>(wcslen(buf));

    // 그림자 (+1px 오프셋 검정)
    D2D1_RECT_F shadow = D2D1::RectF(11.0f, 11.0f, 420.0f, 40.0f);
    m_d2dContext->DrawText(buf, len, m_textFormat.Get(), shadow, m_shadowBrush.Get());

    // 본문 (흰색)
    D2D1_RECT_F text = D2D1::RectF(10.0f, 10.0f, 420.0f, 40.0f);
    m_d2dContext->DrawText(buf, len, m_textFormat.Get(), text, m_textBrush.Get());

    m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);

    // D3D11On12: 리소스 반환 (OutState=PRESENT 로 전환)
    m_d3d11On12->ReleaseWrappedResources(res, 1);

    // D3D11 커맨드를 D3D12 큐에 제출
    m_d3d11Context->Flush();
}

void TextOverlay::Shutdown()
{
    for (auto& t : m_d2dTargets)   t.Reset();
    for (auto& w : m_wrappedBuffers) w.Reset();
    m_shadowBrush.Reset();
    m_textBrush.Reset();
    m_textFormat.Reset();
    m_dwriteFactory.Reset();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_d3d11Context.Reset();
    m_d3d11On12.Reset();
    m_ready = false;
}
