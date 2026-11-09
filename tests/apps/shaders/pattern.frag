// SPDX-License-Identifier: MIT
#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC
{
	vec2 res;
	vec2 offset;   // pixels; input-reactive mode shifts the pattern by the mouse motion
	uint frame;
	uint mode;     // 0 srgb (HW encodes), 1 unorm (shader encodes), 2 hdr10 PQ, 3 scRGB linear
	float time;
} pc;

const float kSdrWhiteNits = 200.0;

vec3 srgbEncode(vec3 c)
{
	vec3 lo = c * 12.92;
	vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
	return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

vec3 bt709To2020(vec3 c)
{
	return vec3(dot(vec3(0.627404, 0.329283, 0.043313), c),
	            dot(vec3(0.069097, 0.919540, 0.011362), c),
	            dot(vec3(0.016391, 0.088013, 0.895595), c));
}

vec3 pqEncode(vec3 nits)
{
	const float m1 = 0.1593017578125, m2 = 78.84375;
	const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
	return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

// Scene color in linear sRGB primaries where 1.0 is SDR white.
vec3 scene()
{
	vec2 p = gl_FragCoord.xy;
	float h = pc.res.y;
	float barH = max(16.0, floor(h / 20.0));

	// Row 1: the frame index as 32 cells, MSB first. Row 2: the same bits inverted, so a
	// reader can tell a good decode from a blown-out one.
	if (p.y < 2.0 * barH) {
		float cell = floor(p.x / (pc.res.x / 32.0));
		uint bit = (pc.frame >> (31u - uint(clamp(cell, 0.0, 31.0)))) & 1u;
		float v = float(bit);
		if (p.y >= barH)
			v = 1.0 - v;
		return vec3(v);
	}

	// A white block that jumps 12 px per frame: tearing and dropped frames show up at once.
	float sq = max(24.0, barH * 1.5);
	float sx = float((pc.frame * 12u) % uint(max(pc.res.x - sq, 1.0)));
	if (p.y >= h - 2.0 * sq && p.y < h - sq && p.x >= sx && p.x < sx + sq)
		return vec3(1.0);

	// Moving, high-detail background: interference rings, fine stripes and an 8 px grid.
	vec2 q = p + pc.offset;
	float t = float(pc.frame) * 0.04;
	float r = length(q - 0.5 * pc.res - vec2(sin(t) * 300.0, cos(t * 0.7) * 200.0));
	vec3 base = 0.45 + 0.35 * cos(vec3(0.0, 2.1, 4.2) + r * 0.045 - t * 3.0 + q.x * 0.004);
	float stripes = 0.75 + 0.25 * sin((q.x + q.y) * 0.9 + t * 8.0);
	vec2 g = abs(fract(q / 8.0) - 0.5);
	float grid = (min(g.x, g.y) > 0.46) ? 0.35 : 1.0;
	return clamp(base * stripes * grid, 0.0, 0.9);
}

void main()
{
	vec3 c = scene();
	vec3 o;
	if (pc.mode == 0u)
		o = c;
	else if (pc.mode == 1u)
		o = srgbEncode(c);
	else if (pc.mode == 2u)
		o = pqEncode(bt709To2020(c) * kSdrWhiteNits);
	else
		o = c * (kSdrWhiteNits / 80.0);   // scRGB: 1.0 = 80 nits
	outColor = vec4(o, 1.0);
}
