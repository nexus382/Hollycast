// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from the project's OpenGL reconstruction kernels in rend/gles/gles.cpp.
// MMPX is the MIT-licensed magnifier by Morgan McGuire and Mara Gagiu; the
// implementation uses the same project-local RGBA8 comparison and edge rules.
#pragma once
#define METAL_RECONSTRUCTION_SOURCE R"RECON(
#define REC_PARAMS texture2d<float> tex, sampler samp, texture2d<float> palette, uint mode, bool ignoreAlpha, uint paletteMode, uint paletteBase
#define REC_ARGS tex, samp, palette, mode, ignoreAlpha, paletteMode, paletteBase
float4 sampleSourceTexel(REC_PARAMS, float2 uv)
{
    float4 color = tex.sample(samp, uv, level(0.0));
    if (paletteMode != 0) {
        uint index = uint(floor(color.r * 255.0 + 0.5)) + paletteBase;
        color = palette.read(uint2(min(index, 1023u), 0));
    }
    if (ignoreAlpha) color.a = 1.0;
    return color;
}
float jincDistance(float2 a, float2 b)
{
	float2 v = b - a;
	return sqrt(dot(v, v));
}

float4 jincResampler(float4 x)
{
	const float wa = 1.382300768;
	const float wb = 2.576105976;
	const float wawb = wa * wb;
	float4 a = sin(x * wa) * sin(x * wb) / (x * x);
	bool4 zero = (x == float4(0.0));
	return float4(zero.x ? wawb : a.x, zero.y ? wawb : a.y, zero.z ? wawb : a.z, zero.w ? wawb : a.w);
}

float4 alphaWeightedBilinear(REC_PARAMS, float2 uv, bool noEdgeBlend)
{
	float2 sourceTexSize = float2(tex.get_width(), tex.get_height());
	float2 pixCoord = uv * sourceTexSize - 0.5;
	float2 originPixCoord = floor(pixCoord);
	float2 sampleUV = (originPixCoord + 0.5) / sourceTexSize;
	float2 texelStep = float2(1.0) / sourceTexSize;

	float4 c00 = sampleSourceTexel(REC_ARGS, sampleUV);
	float4 c10 = sampleSourceTexel(REC_ARGS, sampleUV + float2(texelStep.x, 0.0));
	float4 c01 = sampleSourceTexel(REC_ARGS, sampleUV + float2(0.0, texelStep.y));
	float4 c11 = sampleSourceTexel(REC_ARGS, sampleUV + texelStep);

	float2 weight = pixCoord - originPixCoord;
	float4 color = mix(mix(float4(c00.rgb * c00.a, c00.a), float4(c10.rgb * c10.a, c10.a), weight.x),
			mix(float4(c01.rgb * c01.a, c01.a), float4(c11.rgb * c11.a, c11.a), weight.x), weight.y);

	if (color.a > 0.0)
		color.rgb /= color.a;
	if (noEdgeBlend)
		color.a = color.a >= 0.5 ? 1.0 : 0.0;
	return color;
}

float4 jinc2Sample(REC_PARAMS, float2 uv, bool noEdgeBlend)
{
	const float arStrength = 0.8;
	float2 sourceTexSize = float2(tex.get_width(), tex.get_height());
	float2 pc = uv * sourceTexSize;
	float2 tc = floor(pc - float2(0.5)) + float2(0.5);
	float2 dx = float2(1.0, 0.0);
	float2 dy = float2(0.0, 1.0);

	float4 w0 = jincResampler(float4(jincDistance(pc, tc - dx - dy), jincDistance(pc, tc - dy), jincDistance(pc, tc + dx - dy), jincDistance(pc, tc + dx * 2.0 - dy)));
	float4 w1 = jincResampler(float4(jincDistance(pc, tc - dx), jincDistance(pc, tc), jincDistance(pc, tc + dx), jincDistance(pc, tc + dx * 2.0)));
	float4 w2 = jincResampler(float4(jincDistance(pc, tc - dx + dy), jincDistance(pc, tc + dy), jincDistance(pc, tc + dx + dy), jincDistance(pc, tc + dx * 2.0 + dy)));
	float4 w3 = jincResampler(float4(jincDistance(pc, tc - dx + dy * 2.0), jincDistance(pc, tc + dy * 2.0), jincDistance(pc, tc + dx + dy * 2.0), jincDistance(pc, tc + dx * 2.0 + dy * 2.0)));

	float2 baseUV = tc / sourceTexSize;
	dx /= sourceTexSize;
	dy /= sourceTexSize;

	float4 c00 = sampleSourceTexel(REC_ARGS, baseUV - dx - dy);
	float4 c10 = sampleSourceTexel(REC_ARGS, baseUV - dy);
	float4 c20 = sampleSourceTexel(REC_ARGS, baseUV + dx - dy);
	float4 c30 = sampleSourceTexel(REC_ARGS, baseUV + dx * 2.0 - dy);
	float4 c01 = sampleSourceTexel(REC_ARGS, baseUV - dx);
	float4 c11 = sampleSourceTexel(REC_ARGS, baseUV);
	float4 c21 = sampleSourceTexel(REC_ARGS, baseUV + dx);
	float4 c31 = sampleSourceTexel(REC_ARGS, baseUV + dx * 2.0);
	float4 c02 = sampleSourceTexel(REC_ARGS, baseUV - dx + dy);
	float4 c12 = sampleSourceTexel(REC_ARGS, baseUV + dy);
	float4 c22 = sampleSourceTexel(REC_ARGS, baseUV + dx + dy);
	float4 c32 = sampleSourceTexel(REC_ARGS, baseUV + dx * 2.0 + dy);
	float4 c03 = sampleSourceTexel(REC_ARGS, baseUV - dx + dy * 2.0);
	float4 c13 = sampleSourceTexel(REC_ARGS, baseUV + dy * 2.0);
	float4 c23 = sampleSourceTexel(REC_ARGS, baseUV + dx + dy * 2.0);
	float4 c33 = sampleSourceTexel(REC_ARGS, baseUV + dx * 2.0 + dy * 2.0);

	c00.rgb *= c00.a; c10.rgb *= c10.a; c20.rgb *= c20.a; c30.rgb *= c30.a;
	c01.rgb *= c01.a; c11.rgb *= c11.a; c21.rgb *= c21.a; c31.rgb *= c31.a;
	c02.rgb *= c02.a; c12.rgb *= c12.a; c22.rgb *= c22.a; c32.rgb *= c32.a;
	c03.rgb *= c03.a; c13.rgb *= c13.a; c23.rgb *= c23.a; c33.rgb *= c33.a;

	float wsum = max(dot(w0, float4(1.0)) + dot(w1, float4(1.0)) + dot(w2, float4(1.0)) + dot(w3, float4(1.0)), 0.000001);
	float4 color = float4(
		dot(w0, float4(c00.r, c10.r, c20.r, c30.r)) + dot(w1, float4(c01.r, c11.r, c21.r, c31.r)) + dot(w2, float4(c02.r, c12.r, c22.r, c32.r)) + dot(w3, float4(c03.r, c13.r, c23.r, c33.r)),
		dot(w0, float4(c00.g, c10.g, c20.g, c30.g)) + dot(w1, float4(c01.g, c11.g, c21.g, c31.g)) + dot(w2, float4(c02.g, c12.g, c22.g, c32.g)) + dot(w3, float4(c03.g, c13.g, c23.g, c33.g)),
		dot(w0, float4(c00.b, c10.b, c20.b, c30.b)) + dot(w1, float4(c01.b, c11.b, c21.b, c31.b)) + dot(w2, float4(c02.b, c12.b, c22.b, c32.b)) + dot(w3, float4(c03.b, c13.b, c23.b, c33.b)),
		dot(w0, float4(c00.a, c10.a, c20.a, c30.a)) + dot(w1, float4(c01.a, c11.a, c21.a, c31.a)) + dot(w2, float4(c02.a, c12.a, c22.a, c32.a)) + dot(w3, float4(c03.a, c13.a, c23.a, c33.a))) / wsum;

	color = mix(color, clamp(color, min(min(c11, c21), min(c12, c22)), max(max(c11, c21), max(c12, c22))), arStrength);
	if (color.a > 0.0)
		color.rgb /= color.a;
	if (noEdgeBlend)
		color.a = color.a >= 0.5 ? 1.0 : 0.0;
	return color;
}

// MMPX compares decoded RGBA8 values, including adjacent byte values.
bool colorEq(float4 a, float4 b) { return all((floor(clamp(a, 0.0, 1.0) * 255.0 + 0.5) == floor(clamp(b, 0.0, 1.0) * 255.0 + 0.5))); }
bool mmpxAllEq2(float4 value, float4 a, float4 b) { return colorEq(value, a) && colorEq(value, b); }
bool mmpxAllEq4(float4 value, float4 a, float4 b, float4 c, float4 d) { return colorEq(value, a) && colorEq(value, b) && colorEq(value, c) && colorEq(value, d); }
bool mmpxAnyEq3(float4 value, float4 a, float4 b, float4 c) { return colorEq(value, a) || colorEq(value, b) || colorEq(value, c); }
bool mmpxNoneEq4(float4 value, float4 a, float4 b, float4 c, float4 d) { return !colorEq(value, a) && !colorEq(value, b) && !colorEq(value, c) && !colorEq(value, d); }
float mmpxLuma(float4 color)
{
	// Match the packed RGBA8 rule, including nonzero luma for opaque texels.
	float4 bytes = floor(clamp(color, 0.0, 1.0) * 255.0 + 0.5);
	return (bytes.r + bytes.g + bytes.b + 1.0) * (256.0 - bytes.a);
}
float4 mmpxResultColor(float4 result, float4 center, uint mode) { return mode == 6 && !colorEq(result, center) ? mix(result, center, 0.25) : result; }

float4 mmpxSample(REC_PARAMS, float2 uv)
{
	float2 sourceTexSize = float2(tex.get_width(), tex.get_height());
	float2 bcoords = floor(uv * sourceTexSize);
	float2 baseUV = (bcoords + 0.5) / sourceTexSize;
	float2 invTexSize = float2(1.0) / sourceTexSize;

	// MMPX is a MIT-licensed style-preserving pixel-art magnifier by Morgan McGuire and Mara Gagiu.
	float4 A = sampleSourceTexel(REC_ARGS, baseUV + float2(-1.0, -1.0) * invTexSize);
	float4 B = sampleSourceTexel(REC_ARGS, baseUV + float2( 0.0, -1.0) * invTexSize);
	float4 C = sampleSourceTexel(REC_ARGS, baseUV + float2( 1.0, -1.0) * invTexSize);
	float4 D = sampleSourceTexel(REC_ARGS, baseUV + float2(-1.0,  0.0) * invTexSize);
	float4 E = sampleSourceTexel(REC_ARGS, baseUV);
	float4 F = sampleSourceTexel(REC_ARGS, baseUV + float2( 1.0,  0.0) * invTexSize);
	float4 G = sampleSourceTexel(REC_ARGS, baseUV + float2(-1.0,  1.0) * invTexSize);
	float4 H = sampleSourceTexel(REC_ARGS, baseUV + float2( 0.0,  1.0) * invTexSize);
	float4 I = sampleSourceTexel(REC_ARGS, baseUV + float2( 1.0,  1.0) * invTexSize);
	float4 J = E, K = E, L = E, M = E;

	if (!mmpxAllEq4(E, A, B, C, D) || !mmpxAllEq4(E, F, G, H, I))
	{
		float4 P = sampleSourceTexel(REC_ARGS, baseUV + float2( 0.0, -2.0) * invTexSize);
		float4 Q = sampleSourceTexel(REC_ARGS, baseUV + float2(-2.0,  0.0) * invTexSize);
		float4 R = sampleSourceTexel(REC_ARGS, baseUV + float2( 2.0,  0.0) * invTexSize);
		float4 S = sampleSourceTexel(REC_ARGS, baseUV + float2( 0.0,  2.0) * invTexSize);
		float Bl = mmpxLuma(B), Dl = mmpxLuma(D), El = mmpxLuma(E), Fl = mmpxLuma(F), Hl = mmpxLuma(H);

		if ((colorEq(D, B) && !colorEq(D, H) && !colorEq(D, F)) && (El >= Dl || colorEq(E, A)) && mmpxAnyEq3(E, A, C, G) && ((El < Dl) || !colorEq(A, D) || !colorEq(E, P) || !colorEq(E, Q))) J = D;
		if ((colorEq(B, F) && !colorEq(B, D) && !colorEq(B, H)) && (El >= Bl || colorEq(E, C)) && mmpxAnyEq3(E, A, C, I) && ((El < Bl) || !colorEq(C, B) || !colorEq(E, P) || !colorEq(E, R))) K = B;
		if ((colorEq(H, D) && !colorEq(H, F) && !colorEq(H, B)) && (El >= Hl || colorEq(E, G)) && mmpxAnyEq3(E, A, G, I) && ((El < Hl) || !colorEq(G, H) || !colorEq(E, S) || !colorEq(E, Q))) L = H;
		if ((colorEq(F, H) && !colorEq(F, B) && !colorEq(F, D)) && (El >= Fl || colorEq(E, I)) && mmpxAnyEq3(E, C, G, I) && ((El < Fl) || !colorEq(I, H) || !colorEq(E, R) || !colorEq(E, S))) M = F;

		if (mode == 7)
		{
			if ((!colorEq(E, F) && mmpxAllEq4(E, C, I, D, Q) && mmpxAllEq2(F, B, H)) && !colorEq(F, sampleSourceTexel(REC_ARGS, baseUV + float2( 3.0,  0.0) * invTexSize))) K = M = F;
			if ((!colorEq(E, D) && mmpxAllEq4(E, A, G, F, R) && mmpxAllEq2(D, B, H)) && !colorEq(D, sampleSourceTexel(REC_ARGS, baseUV + float2(-3.0,  0.0) * invTexSize))) J = L = D;
			if ((!colorEq(E, H) && mmpxAllEq4(E, G, I, B, P) && mmpxAllEq2(H, D, F)) && !colorEq(H, sampleSourceTexel(REC_ARGS, baseUV + float2( 0.0,  3.0) * invTexSize))) L = M = H;
			if ((!colorEq(E, B) && mmpxAllEq4(E, A, C, H, S) && mmpxAllEq2(B, D, F)) && !colorEq(B, sampleSourceTexel(REC_ARGS, baseUV + float2( 0.0, -3.0) * invTexSize))) J = K = B;
			if (Bl < El && mmpxAllEq4(E, G, H, I, S) && mmpxNoneEq4(E, A, D, C, F)) J = K = B;
			if (Hl < El && mmpxAllEq4(E, A, B, C, P) && mmpxNoneEq4(E, D, G, I, F)) L = M = H;
			if (Fl < El && mmpxAllEq4(E, A, D, G, Q) && mmpxNoneEq4(E, B, C, I, H)) K = M = F;
			if (Dl < El && mmpxAllEq4(E, C, F, I, R) && mmpxNoneEq4(E, B, A, G, H)) J = L = D;
		}
	}

	float2 fpart = fract(uv * sourceTexSize);
	float4 result = (fpart.x < 0.5) ? ((fpart.y < 0.5) ? J : L) : ((fpart.y < 0.5) ? K : M);
	return mmpxResultColor(result, E, mode);
}

float4 clampToSourceNeighborhood(REC_PARAMS, float2 uv, float4 color)
{
	float2 sourceTexSize = float2(tex.get_width(), tex.get_height());
	float2 base = floor(uv * sourceTexSize);
	float2 invTexSize = float2(1.0) / sourceTexSize;
	float4 minColor = float4(1.0);
	float4 maxColor = float4(0.0);

	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			float4 sampleColor = sampleSourceTexel(REC_ARGS, (base + float2(x, y) + 0.5) * invTexSize);
			minColor = min(minColor, sampleColor);
			maxColor = max(maxColor, sampleColor);
		}
	return clamp(color, minColor, maxColor);
}

float4 sharpenAgainstCenter(REC_PARAMS, float2 uv, float4 color, float strength)
{
	float2 sourceTexSize = float2(tex.get_width(), tex.get_height());
	float2 centerUV = (floor(uv * sourceTexSize) + 0.5) / sourceTexSize;
	float4 center = sampleSourceTexel(REC_ARGS, centerUV);
	color.rgb += (color.rgb - center.rgb) * strength;
	return color;
}

float4 jinc2AntiRingSample(REC_PARAMS, float2 uv, float sharpness)
{
	float4 color = jinc2Sample(REC_ARGS, uv, false);
	if (sharpness > 0.0)
		color = sharpenAgainstCenter(REC_ARGS, uv, color, sharpness);
	return clampToSourceNeighborhood(REC_ARGS, uv, color);
}

float sincWeight(float x)
{
	x = abs(x);
	if (x < 0.00001)
		return 1.0;
	float pix = M_PI_F * x;
	return sin(pix) / pix;
}

float lanczosWeight(float x, float radius)
{
	x = abs(x);
	if (x >= radius)
		return 0.0;
	return sincWeight(x) * sincWeight(x / radius);
}

float cubicWeight(float x, float b, float c)
{
	x = abs(x);
	if (x < 1.0)
		return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) / 6.0;
	if (x < 2.0)
		return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x + (-12.0 * b - 48.0 * c) * x + (8.0 * b + 24.0 * c)) / 6.0;
	return 0.0;
}

float reconstructionWeight(float x, int filterMode)
{
	if (filterMode == 0) return lanczosWeight(x, 2.0);
	if (filterMode == 1) return lanczosWeight(x, 3.0);
	if (filterMode == 2) return cubicWeight(x, 0.378215755, 0.310892122);
	if (filterMode == 3) return cubicWeight(x, 0.262014512, 0.368992744);
	if (filterMode == 4) return cubicWeight(x, 0.0, 0.5);
	return cubicWeight(x, 1.0 / 3.0, 1.0 / 3.0);
}

float4 separableReconstructionSample(REC_PARAMS, float2 uv, int filterMode, bool antiRing)
{
	float2 sourceTexSize = float2(tex.get_width(), tex.get_height());
	float2 pc = uv * sourceTexSize - 0.5;
	float2 base = floor(pc);
	float2 f = pc - base;
	float2 invTexSize = float2(1.0) / sourceTexSize;
	float4 color = float4(0.0);
	float weightSum = 0.0;

	// Only Lanczos3 needs six taps per axis; the other kernels have radius two.
	int radius = filterMode == 1 ? 3 : 2;
	for (int y = 1 - radius; y <= radius; y++)
	{
		float wy = reconstructionWeight(float(y) - f.y, filterMode);
		for (int x = 1 - radius; x <= radius; x++)
		{
			float weight = reconstructionWeight(float(x) - f.x, filterMode) * wy;
			float4 sampleColor = sampleSourceTexel(REC_ARGS, (base + float2(x, y) + 0.5) * invTexSize);
			sampleColor.rgb *= sampleColor.a;
			color += sampleColor * weight;
			weightSum += weight;
		}
	}

	if (abs(weightSum) > 0.00001)
		color /= weightSum;
	if (color.a > 0.0)
		color.rgb /= color.a;
	color = clamp(color, 0.0, 1.0);
	if (antiRing)
		color = clampToSourceNeighborhood(REC_ARGS, uv, color);
	return color;
}


float4 metalSampleReconstruction(REC_PARAMS, float2 uv)
{
    if (mode == 19) return sampleSourceTexel(REC_ARGS, uv);
    if (mode == 1 || mode == 2) return alphaWeightedBilinear(REC_ARGS, uv, mode == 2);
    if (mode == 3 || mode == 4) return jinc2Sample(REC_ARGS, uv, mode == 4);
    if (mode >= 5 && mode <= 7) return mmpxSample(REC_ARGS, uv);
    if (mode >= 8 && mode <= 11) return jinc2AntiRingSample(REC_ARGS, uv,
            mode == 8 ? 0.0 : mode == 9 ? 0.18 : mode == 10 ? 0.08 : 0.22);
    if (mode == 12 || mode == 13) return separableReconstructionSample(REC_ARGS, uv, mode - 12, true);
    if (mode >= 15 && mode <= 18) return separableReconstructionSample(REC_ARGS, uv, mode - 13, false);
    return sampleSourceTexel(REC_ARGS, uv);
}
#undef REC_PARAMS
#undef REC_ARGS
)RECON"
