#include "WindowHelpers.h"
#include <iostream>
#include <stdexcept>
#include <winrt/Windows.Foundation.h>
#include <windows.graphics.capture.interop.h> // IGraphicsCaptureItemInterop

HWND fetchForegroundWindow()
{
    try
    {
        HWND foreground = ::GetForegroundWindow();
        if (!foreground)
        {
            std::wcerr << L"[fetchForegroundWindow] Returned NULL.\n";
        }
        return foreground;
    }
    catch (...)
    {
        std::wcerr << L"[fetchForegroundWindow] Exception.\n";
        return nullptr;
    }
}

uint64_t GetWindowIdFromHWND(HWND hwnd)
{
    if (!hwnd)
    {
        std::wcerr << L"[GetWindowIdFromHWND] Invalid.\n";
        return 0;
    }
    return reinterpret_cast<uint64_t>(hwnd);
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
CreateCaptureItemForWindow(HWND hwnd)
{
    using namespace winrt::Windows::Graphics::Capture;

    if (!hwnd || !::IsWindow(hwnd))
    {
        throw std::invalid_argument("Invalid HWND passed to CreateCaptureItemForWindow.");
    }

    auto interopFactory = winrt::get_activation_factory<
        GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item{ nullptr };

    HRESULT hr = interopFactory->CreateForWindow(
        hwnd,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );

    if (FAILED(hr))
    {
        std::wcerr << L"[CreateCaptureItemForWindow] Failed. HRESULT=0x"
            << std::hex << hr << std::endl;
        return nullptr;
    }

    return item;
}
