#pragma once

#include <cstdint>
#include <string>
#include <dxgi.h>

namespace vrmod {

const char* DxgiFormatName(DXGI_FORMAT format);

// Writes an uncompressed 32bpp bottom-up BMP from top-down 32bpp source data.
// swapRB converts RGBA source data to the BGRA byte order BMP expects; pass
// false if the source is already BGRA.
bool WriteBmp32(const std::string& path, uint32_t width, uint32_t height,
                 const uint8_t* srcData, uint32_t srcRowPitch, bool swapRB);

} // namespace vrmod
