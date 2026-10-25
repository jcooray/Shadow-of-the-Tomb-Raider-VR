#include "build_config.h"
#include "backbuffer_capture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>

#include "logger.h"
#include "bmp_writer.h"

#pragma comment(lib, "d3d11.lib")

namespace vrmod {

namespace {

void CaptureBackbuffer(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        modlog::Line("BackbufferCapture: GetBuffer failed, hr=" + std::to_string(hr));
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    std::ostringstream oss;
    oss << "BackbufferCapture: " << desc.Width << "x" << desc.Height
        << " format=" << DxgiFormatName(desc.Format) << " (" << desc.Format << ")";
    modlog::Line(oss.str());

    ID3D11Device* device = nullptr;
    backBuffer->GetDevice(&device);
    if (!device) {
        modlog::Line("BackbufferCapture: GetDevice failed.");
        backBuffer->Release();
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        modlog::Line("BackbufferCapture: CreateTexture2D(staging) failed, hr=" + std::to_string(hr));
        if (context) context->Release();
        device->Release();
        backBuffer->Release();
        return;
    }

    context->CopyResource(staging, backBuffer);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        modlog::Line("BackbufferCapture: Map failed, hr=" + std::to_string(hr));
        staging->Release();
        context->Release();
        device->Release();
        backBuffer->Release();
        return;
    }

    bool isRgba = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    bool isBgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

    if (!isRgba && !isBgra) {
        modlog::Line(std::string("BackbufferCapture: unhandled format ") + DxgiFormatName(desc.Format)
            + " -- can't write a BMP directly (only 32bpp RGBA/BGRA handled), dumping raw bytes to sbs_capture.raw instead.");
        std::ofstream raw("sbs_capture.raw", std::ios::binary);
        if (raw) {
            for (UINT y = 0; y < desc.Height; ++y) {
                raw.write(reinterpret_cast<const char*>(static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch),
                          mapped.RowPitch);
            }
        }
    } else {
        bool swapRB = isRgba; // BMP wants BGRA byte order; RGBA needs swapping, BGRA doesn't.
        if (WriteBmp32("sbs_capture.bmp", desc.Width, desc.Height,
                        static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, swapRB)) {
            modlog::Line("BackbufferCapture: wrote sbs_capture.bmp");
        } else {
            modlog::Line("BackbufferCapture: failed to write sbs_capture.bmp");
        }
    }

    context->Unmap(staging, 0);
    staging->Release();
    context->Release();
    device->Release();
    backBuffer->Release();
}

} // namespace

void CaptureBackbufferIfHotkeyPressed(IDXGISwapChain* swapChain) {
    static bool wasDown = false;
    bool isDown = (VRMOD_DEV_KEY(VK_F5) & 0x8000) != 0;
    bool justPressed = isDown && !wasDown;
    wasDown = isDown;

    if (justPressed && swapChain) {
        CaptureBackbuffer(swapChain);
    }
}

} // namespace vrmod
