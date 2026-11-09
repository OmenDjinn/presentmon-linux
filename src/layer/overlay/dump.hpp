// SPDX-License-Identifier: MIT
// PRESENTMON_OVERLAY_DUMP test hook: turns the bytes read back from a presented swapchain image
// into files. Pure CPU code (no Vulkan calls), so it is unit tested.
//
// Raw file ("<path>.raw", written for every format, authoritative for exact encoded values):
//   64-byte header, little endian
//     char     magic[8]        "PMLDUMP1"
//     uint32_t vkFormat        VkFormat
//     uint32_t vkColorSpace    VkColorSpaceKHR
//     uint32_t width, height
//     uint32_t rowBytes        bytes per row in the data that follows (== width * bytesPerPixel)
//     uint32_t bytesPerPixel
//     uint32_t frameIndex      1-based present number on the swapchain
//     uint32_t reserved[7]
//   then height * rowBytes bytes exactly as stored in the image (no channel swizzle)
// Viewable file: PNG (or PPM when the path ends in .ppm), always 8-bit sRGB. 10-bit PQ and scRGB
// images are decoded and mapped so that SDR white (203 nits) becomes 255.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pml::layer::overlay {

struct DumpImage
{
	uint32_t vkFormat = 0;
	uint32_t vkColorSpace = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t frameIndex = 0;
	std::vector<uint8_t> bytes;   // tightly packed rows
};

uint32_t dumpBytesPerPixel(uint32_t vkFormat);   // 0 = format not supported by the hook

// Decodes to tightly packed 8-bit RGB (3 bytes per pixel). False for unsupported formats.
bool dumpToRgb8(const DumpImage &image, std::vector<uint8_t> *rgb);

// Writes <path> (PNG or PPM by extension) and <path>.raw. Returns an error text, empty on success.
std::string writeDump(const DumpImage &image, const std::string &path);

// Exposed for tests: the transfer functions the shader implements, in float.
float pqEncodeNits(float nits);       // ST 2084 inverse EOTF, nits -> [0,1]
float pqDecodeToNits(float signal);   // ST 2084 EOTF
float srgbEncode(float linear);

} // namespace pml::layer::overlay
