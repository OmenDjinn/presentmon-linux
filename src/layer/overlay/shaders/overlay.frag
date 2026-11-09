// SPDX-License-Identifier: MIT
// Fragment shader for the in-game overlay: Dear ImGui's stock fragment shader plus a colour
// transform, because ImGui colours are authored as sRGB-encoded values but swapchains differ.
// One SPIR-V module per MODE (see overlay.cpp, colorModeFor()):
//   0  8/10-bit UNORM, SRGB_NONLINEAR       write the sRGB-encoded value as is
//   1  *_SRGB formats                        the hardware encodes, so write linear light
//   2  HDR10_ST2084 (10-bit)                 sRGB -> linear -> BT.2020, SDR white at 203 nits, PQ encode
//   3  EXTENDED_SRGB_LINEAR (FP16)           sRGB -> linear, scaled so that white = 203 / 80
// Alpha blending runs on the transformed values (linear for modes 1 and 3, PQ for mode 2).
#version 450 core
layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform texture2D _Texture;
layout(set = 1, binding = 0) uniform sampler _Sampler;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

const float kSdrWhiteNits = 203.0;   // ITU-R BT.2408 reference white for HDR10 graphics
const float kScRgbUnitNits = 80.0;   // scRGB: 1.0 = 80 nits

vec3 srgbToLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// SMPTE ST 2084 inverse EOTF: absolute luminance in nits -> signal in [0, 1].
vec3 pqEncode(vec3 nits)
{
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
	return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

void main()
{
	vec4 c = In.Color * texture(sampler2D(_Texture, _Sampler), In.UV.st);
#if MODE == 0
	fColor = c;
#elif MODE == 1
	fColor = vec4(srgbToLinear(c.rgb), c.a);
#elif MODE == 2
	// BT.709 -> BT.2020 primaries (ITU-R BT.2087), GLSL matrices are column major.
	const mat3 toBt2020 = mat3(0.6274, 0.0691, 0.0164,
	                           0.3293, 0.9195, 0.0880,
	                           0.0433, 0.0114, 0.8956);
	vec3 lin = toBt2020 * srgbToLinear(c.rgb);
	fColor = vec4(pqEncode(lin * kSdrWhiteNits), c.a);
#elif MODE == 3
	fColor = vec4(srgbToLinear(c.rgb) * (kSdrWhiteNits / kScRgbUnitNits), c.a);
#else
#error unknown MODE
#endif
}
