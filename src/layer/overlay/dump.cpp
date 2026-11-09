// SPDX-License-Identifier: MIT
#include "dump.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <stb/stb_image_write.h>
#pragma GCC diagnostic pop

namespace pml::layer::overlay {

namespace {

constexpr float kSdrWhiteNits = 203.0f;
constexpr float kScRgbUnitNits = 80.0f;

float halfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
	float v;
	if (exp == 0)
		v = std::ldexp(float(man), -24);
	else if (exp == 31)
		v = man ? NAN : INFINITY;
	else
		v = std::ldexp(float(man | 0x400), int(exp) - 25);
	return sign ? -v : v;
}

uint8_t to8(float v)
{
	return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// BT.2020 -> BT.709 linear RGB (inverse of the matrix the shader uses, ITU-R BT.2087).
void bt2020To709(float r, float g, float b, float *out)
{
	out[0] = 1.6605f * r - 0.5876f * g - 0.0728f * b;
	out[1] = -0.1246f * r + 1.1329f * g - 0.0083f * b;
	out[2] = -0.0182f * r - 0.1006f * g + 1.1187f * b;
}

} // namespace

float pqEncodeNits(float nits)
{
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
	return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

float pqDecodeToNits(float signal)
{
	const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
	const float p = std::pow(std::clamp(signal, 0.0f, 1.0f), 1.0f / m2);
	const float num = std::max(p - c1, 0.0f);
	const float den = c2 - c3 * p;
	return 10000.0f * std::pow(num / den, 1.0f / m1);
}

float srgbEncode(float l)
{
	l = std::clamp(l, 0.0f, 1.0f);
	return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

uint32_t dumpBytesPerPixel(uint32_t f)
{
	switch (VkFormat(f))
	{
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		return 4;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return 8;
	default:
		return 0;
	}
}

bool dumpToRgb8(const DumpImage &img, std::vector<uint8_t> *rgb)
{
	const uint32_t bpp = dumpBytesPerPixel(img.vkFormat);
	if (!bpp || img.bytes.size() < size_t(img.width) * img.height * bpp)
		return false;
	rgb->resize(size_t(img.width) * img.height * 3);
	const bool hdr10 = img.vkColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
	const bool scRgb = img.vkColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
	const size_t count = size_t(img.width) * img.height;
	for (size_t i = 0; i < count; i++)
	{
		const uint8_t *p = img.bytes.data() + i * bpp;
		uint8_t *o = rgb->data() + i * 3;
		switch (VkFormat(img.vkFormat))
		{
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
			o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
			break;
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
			o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
			break;
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		{
			uint32_t v;
			std::memcpy(&v, p, 4);
			uint32_t c0 = v & 0x3ff, c1 = (v >> 10) & 0x3ff, c2 = (v >> 20) & 0x3ff;
			uint32_t r = c0, g = c1, b = c2;
			if (VkFormat(img.vkFormat) == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
				std::swap(r, b);
			if (hdr10)
			{
				const float n2020[3] = {pqDecodeToNits(float(r) / 1023.0f) / kSdrWhiteNits,
				                        pqDecodeToNits(float(g) / 1023.0f) / kSdrWhiteNits,
				                        pqDecodeToNits(float(b) / 1023.0f) / kSdrWhiteNits};
				float lin[3];
				bt2020To709(n2020[0], n2020[1], n2020[2], lin);
				o[0] = to8(srgbEncode(lin[0])); o[1] = to8(srgbEncode(lin[1])); o[2] = to8(srgbEncode(lin[2]));
			}
			else
			{
				o[0] = uint8_t(r >> 2); o[1] = uint8_t(g >> 2); o[2] = uint8_t(b >> 2);
			}
			break;
		}
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		{
			uint16_t h[3];
			std::memcpy(h, p, 6);
			for (int c = 0; c < 3; c++)
			{
				float v = halfToFloat(h[c]);
				if (scRgb)
					v = srgbEncode(v * kScRgbUnitNits / kSdrWhiteNits);
				o[c] = to8(v);
			}
			break;
		}
		default:
			return false;
		}
	}
	return true;
}

std::string writeDump(const DumpImage &img, const std::string &path)
{
	std::vector<uint8_t> rgb;
	if (!dumpToRgb8(img, &rgb))
		return "unsupported format " + std::to_string(img.vkFormat);

	const bool ppm = path.size() >= 4 && path.compare(path.size() - 4, 4, ".ppm") == 0;
	if (ppm)
	{
		FILE *f = std::fopen(path.c_str(), "wb");
		if (!f)
			return "cannot open " + path;
		std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
		std::fwrite(rgb.data(), 1, rgb.size(), f);
		std::fclose(f);
	}
	else if (!stbi_write_png(path.c_str(), int(img.width), int(img.height), 3, rgb.data(), int(img.width) * 3))
	{
		return "cannot write " + path;
	}

	const std::string rawPath = path + ".raw";
	FILE *f = std::fopen(rawPath.c_str(), "wb");
	if (!f)
		return "cannot open " + rawPath;
	const uint32_t bpp = dumpBytesPerPixel(img.vkFormat);
	uint8_t header[64] = {};
	std::memcpy(header, "PMLDUMP1", 8);
	const uint32_t fields[7] = {img.vkFormat, img.vkColorSpace, img.width, img.height, img.width * bpp, bpp,
	                            img.frameIndex};
	std::memcpy(header + 8, fields, sizeof(fields));
	std::fwrite(header, 1, sizeof(header), f);
	std::fwrite(img.bytes.data(), 1, size_t(img.width) * img.height * bpp, f);
	std::fclose(f);
	return {};
}

} // namespace pml::layer::overlay
