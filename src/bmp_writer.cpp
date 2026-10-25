#include "bmp_writer.h"

#include <fstream>
#include <vector>

namespace vrmod {

namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type = 0x4D42; // 'BM'
    uint32_t size;
    uint16_t reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t offBits;
};
struct BmpInfoHeader {
    uint32_t size = 40;
    int32_t width;
    int32_t height; // positive = bottom-up
    uint16_t planes = 1;
    uint16_t bitCount = 32;
    uint32_t compression = 0;
    uint32_t sizeImage = 0;
    int32_t xPelsPerMeter = 0;
    int32_t yPelsPerMeter = 0;
    uint32_t clrUsed = 0;
    uint32_t clrImportant = 0;
};
#pragma pack(pop)

} // namespace

const char* DxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        default: return "OTHER";
    }
}

bool WriteBmp32(const std::string& path, uint32_t width, uint32_t height,
                 const uint8_t* srcData, uint32_t srcRowPitch, bool swapRB) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    uint32_t rowBytes = width * 4;
    uint32_t imageSize = rowBytes * height;

    BmpFileHeader fileHeader{};
    fileHeader.offBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fileHeader.size = fileHeader.offBits + imageSize;

    BmpInfoHeader infoHeader{};
    infoHeader.width = static_cast<int32_t>(width);
    infoHeader.height = static_cast<int32_t>(height);
    infoHeader.sizeImage = imageSize;

    file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));

    std::vector<uint8_t> row(rowBytes);
    // BMP is bottom-up; the captured texture data is top-down, so rows are
    // written in reverse order.
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcData + static_cast<size_t>(height - 1 - y) * srcRowPitch;
        if (swapRB) {
            for (uint32_t x = 0; x < width; ++x) {
                row[x * 4 + 0] = srcRow[x * 4 + 2]; // B
                row[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                row[x * 4 + 2] = srcRow[x * 4 + 0]; // R
                row[x * 4 + 3] = srcRow[x * 4 + 3]; // A
            }
            file.write(reinterpret_cast<const char*>(row.data()), rowBytes);
        } else {
            file.write(reinterpret_cast<const char*>(srcRow), rowBytes);
        }
    }

    return true;
}

} // namespace vrmod
