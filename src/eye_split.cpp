#include "build_config.h"
#include "eye_split.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>
#include <sstream>

#include "logger.h"
#include "bmp_writer.h"

#pragma comment(lib, "d3d11.lib")

namespace vrmod {

namespace {

ID3D11Texture2D* g_leftEyeTexture = nullptr;
ID3D11Texture2D* g_rightEyeTexture = nullptr;
UINT g_eyeWidth = 0;
UINT g_eyeHeight = 0;
DXGI_FORMAT g_eyeFormat = DXGI_FORMAT_UNKNOWN;
unsigned long long g_updateCount = 0;

void ReleaseEyeTextures() {
    if (g_leftEyeTexture) {
        g_leftEyeTexture->Release();
        g_leftEyeTexture = nullptr;
    }
    if (g_rightEyeTexture) {
        g_rightEyeTexture->Release();
        g_rightEyeTexture = nullptr;
    }
}

bool EnsureEyeTextures(ID3D11Device* device, UINT eyeWidth, UINT eyeHeight, DXGI_FORMAT format) {
    if (g_leftEyeTexture && g_rightEyeTexture
        && g_eyeWidth == eyeWidth && g_eyeHeight == eyeHeight && g_eyeFormat == format) {
        return true;
    }

    ReleaseEyeTextures();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = eyeWidth;
    desc.Height = eyeHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_leftEyeTexture);
    if (FAILED(hr)) {
        modlog::Line("EyeSplit: CreateTexture2D(left) failed, hr=" + std::to_string(hr));
        return false;
    }
    hr = device->CreateTexture2D(&desc, nullptr, &g_rightEyeTexture);
    if (FAILED(hr)) {
        modlog::Line("EyeSplit: CreateTexture2D(right) failed, hr=" + std::to_string(hr));
        ReleaseEyeTextures();
        return false;
    }

    g_eyeWidth = eyeWidth;
    g_eyeHeight = eyeHeight;
    g_eyeFormat = format;

    std::ostringstream oss;
    oss << "EyeSplit: (re)created eye textures " << eyeWidth << "x" << eyeHeight
        << " format=" << DxgiFormatName(format);
    modlog::Line(oss.str());
    return true;
}

// Dumps one eye texture to a BMP via a staging (CPU-readable) copy -- same
// technique as BackbufferCapture, just against our own persistent texture
// instead of the swap chain's.
void DumpEyeTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                     ID3D11Texture2D* eyeTexture, const std::string& path) {
    if (!eyeTexture) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    eyeTexture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        modlog::Line("EyeSplit: CreateTexture2D(staging dump) failed, hr=" + std::to_string(hr));
        return;
    }

    context->CopyResource(staging, eyeTexture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        modlog::Line("EyeSplit: Map(staging dump) failed, hr=" + std::to_string(hr));
        staging->Release();
        return;
    }

    bool isRgba = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    bool swapRB = isRgba; // BMP wants BGRA byte order; RGBA needs swapping, BGRA doesn't.
    if (WriteBmp32(path, desc.Width, desc.Height, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, swapRB)) {
        modlog::Line("EyeSplit: wrote " + path);
    } else {
        modlog::Line("EyeSplit: failed to write " + path);
    }

    context->Unmap(staging, 0);
    staging->Release();
}

} // namespace

void UpdateEyeSplitTextures(IDXGISwapChain* swapChain) {
    if (!swapChain) {
        return;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    UINT eyeWidth = desc.Width / 2;
    UINT eyeHeight = desc.Height;

    ID3D11Device* device = nullptr;
    backBuffer->GetDevice(&device);
    if (!device) {
        backBuffer->Release();
        return;
    }

    if (!EnsureEyeTextures(device, eyeWidth, eyeHeight, desc.Format)) {
        device->Release();
        backBuffer->Release();
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    D3D11_BOX leftBox{};
    leftBox.left = 0;
    leftBox.right = eyeWidth;
    leftBox.top = 0;
    leftBox.bottom = eyeHeight;
    leftBox.front = 0;
    leftBox.back = 1;
    context->CopySubresourceRegion(g_leftEyeTexture, 0, 0, 0, 0, backBuffer, 0, &leftBox);

    D3D11_BOX rightBox{};
    rightBox.left = eyeWidth;
    rightBox.right = eyeWidth * 2;
    rightBox.top = 0;
    rightBox.bottom = eyeHeight;
    rightBox.front = 0;
    rightBox.back = 1;
    context->CopySubresourceRegion(g_rightEyeTexture, 0, 0, 0, 0, backBuffer, 0, &rightBox);

    auto count = ++g_updateCount;
    if (count == 1 || count % 300 == 0) {
        modlog::Line("EyeSplit: updated eye textures, count=" + std::to_string(count));
    }

    context->Release();
    device->Release();
    backBuffer->Release();
}

ID3D11Texture2D* GetLeftEyeTexture() {
    return g_leftEyeTexture;
}

ID3D11Texture2D* GetRightEyeTexture() {
    return g_rightEyeTexture;
}

void DumpEyeTexturesIfHotkeyPressed() {
    static bool wasDown = false;
    bool isDown = (VRMOD_DEV_KEY(VK_F4) & 0x8000) != 0;
    bool justPressed = isDown && !wasDown;
    wasDown = isDown;

    if (!justPressed) {
        return;
    }

    if (!g_leftEyeTexture || !g_rightEyeTexture) {
        modlog::Line("EyeSplit: dump requested but eye textures don't exist yet.");
        return;
    }

    ID3D11Device* device = nullptr;
    g_leftEyeTexture->GetDevice(&device);
    if (!device) {
        return;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    DumpEyeTexture(device, context, g_leftEyeTexture, "eye_left.bmp");
    DumpEyeTexture(device, context, g_rightEyeTexture, "eye_right.bmp");

    context->Release();
    device->Release();
}

} // namespace vrmod
