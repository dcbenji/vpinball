// license:GPLv3+

// Lean temporal upscaler (Upscaler = 6), pass 2 of 2, at DISPLAY size, RGBA16F history = output (linear HDR):
// rgb = the accumulated image, a = its accumulated sample weight.
// One pass, 9 point taps of the render (current frame reconstruction + colour box), 1 tap of pass 1, 5 bilinear taps of
// the history (Catmull-Rom), so it stays in the cost class of a plain TAA resolve:
// 1. current frame at this output pixel: Gaussian (Blackman-Harris fit) of the 3x3 jittered render samples around it
// 2. history reprojected with the dilated motion, Catmull-Rom from 5 bilinear taps (corners dropped)
// 3. history clipped (not clamped) toward the mean of a variance box of the 3x3 in YCoCg, wider on still pixels
// 4. blend by accumulated weight: each frame adds its nearest sample's closeness to this output pixel (sharp
//    accumulation of the jittered sub-pixel positions), the history weight is capped (lower in motion), zero on
//    disocclusion / off screen / reset
// Blending and clipping run on reversibly tonemapped values (c / (1 + max(c))): bright HDR highlights do not dominate.
// Sources (public): Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014 (YCoCg box, Blackman-Harris weights,
// tonemapped blend); Salvi, "An Excursion in Temporal Supersampling", GDC 2016 (variance clipping); Pedersen, "Temporal
// Reprojection Anti-Aliasing in INSIDE", GDC 2016 (clip toward the box centre); Jimenez, "Filmic SMAA", SIGGRAPH 2016
// (5-tap Catmull-Rom); Yang et al., "A Survey of Temporal Antialiasing Techniques", 2020 (accumulated weight).
// Moving pixels (little history to accumulate: a flipper mid stroke, a ball leaving) take their current frame estimate from an edge
// directed upscale instead (AMD FidelityFX FSR 1 EASU, MIT, ffx_fsr1.h in this tree; see LICENSE-FidelityFX-FSR.txt), its sample
// positions shifted by the jitter: smooth edges where the 3x3 Gaussian of a half resolution render drew 2 px stair steps.

$input v_texcoord0

#include "common.sh"

// [0] xy: render size, zw: jitter (render pixels, +y down the texture rows; the sample of render pixel i sits at i + 0.5 - jitter)
// [1] xy: display size, z: 1 on a reset, w: weight loss of clipped history (its weight x (1 / clip ratio)^w)
// [2] x: variance box scale on still pixels, y: in motion, z: history weight cap still, w: in motion
// [3] x: history weight at which the current frame kernel is one output pixel wide (0 = always one render pixel), y: 1 = use the
//     pass 1 activity for the still / moving rules, z: 1 = no kernel narrowing on moving pixels, w: edge directed (EASU) estimate on moving
//     pixels, strength (0 = off)
uniform vec4 leanTaa[4];

SAMPLER2D(tex_fb_unfiltered, 0); // Render (linear HDR, render size)
SAMPLER2D(tex_bloom,         1); // History (display size, bilinear)
SAMPLER2D(tex_normals,       6); // Pass 1 (render size)

vec3 Tonemap(vec3 c) { return c * (1.0 / (1.0 + max(c.r, max(c.g, c.b)))); }
vec3 InvTonemap(vec3 c) { return c * (1.0 / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4)); }
vec3 ToYCoCg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 FromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// isnan() / isinf() have no translation in the WGSL path (as in fs_pp_ssr.sc): there the exponent bits are tested instead
#if BGFX_SHADER_LANGUAGE_WGSL
#define LEAN_EXP_ALL_ONES(x) ((floatBitsToUint(x) & 0x7F800000u) == 0x7F800000u)
#define LeanNotFinite(v) (LEAN_EXP_ALL_ONES((v).r) || LEAN_EXP_ALL_ONES((v).g) || LEAN_EXP_ALL_ONES((v).b) || LEAN_EXP_ALL_ONES((v).a))
#else
#define LeanNotFinite(v) (any(isnan(v)) || any(isinf(v)))
#endif

#ifdef EASU
// FSR 1 EASU on the reversibly tonemapped render (texel fetches, gather emulated as in fs_pp_fsr_easu.sc). Only in the EASU variant:
// a second pass that discards every still pixel, so the main resolve keeps its lean register budget (EASU in it cost every pixel)
#include "fsr_common.sh"
vec3 LeanEasuTap(ASU2 q) { return Tonemap(clamp(texelFetch(tex_fb_unfiltered, q, 0).rgb, vec3(0.0, 0.0, 0.0), vec3(65000.0, 65000.0, 65000.0))); }
#define LEAN_EASU_GATHER(C) \
	ASU2 sz = ASU2(leanTaa[0].xy) - ASU2(1, 1); \
	ASU2 b = ASU2(floor(p * leanTaa[0].xy - AF2(0.5, 0.5))); \
	ASU2 b0 = clamp(b, ASU2(0, 0), sz); \
	ASU2 b1 = clamp(b + ASU2(1, 1), ASU2(0, 0), sz); \
	return AF4(LeanEasuTap(ASU2(b0.x, b1.y)).C, LeanEasuTap(ASU2(b1.x, b1.y)).C, LeanEasuTap(ASU2(b1.x, b0.y)).C, LeanEasuTap(ASU2(b0.x, b0.y)).C);
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { LEAN_EASU_GATHER(r) }
AF4 FsrEasuGF(AF2 p) { LEAN_EASU_GATHER(g) }
AF4 FsrEasuBF(AF2 p) { LEAN_EASU_GATHER(b) }
#include "ffx_fsr1.h"
#endif

vec4 History(vec2 uv, vec2 size)
{
	const vec2 pos = uv * size;
	const vec2 tc1 = floor(pos - 0.5) + 0.5;
	const vec2 f = pos - tc1;
	const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	const vec2 w3 = f * f * (-0.5 + 0.5 * f);
	const vec2 w12 = w1 + w2;
	const vec2 inv = 1.0 / size;
	const vec2 t0 = (tc1 - 1.0) * inv, t3 = (tc1 + 2.0) * inv, t12 = (tc1 + w2 / w12) * inv;
	const float a = w12.x * w0.y, b = w0.x * w12.y, c = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
	const vec4 r = texture2DLod(tex_bloom, vec2(t12.x, t0.y), 0.0) * a + texture2DLod(tex_bloom, vec2(t0.x, t12.y), 0.0) * b
		+ texture2DLod(tex_bloom, t12, 0.0) * c + texture2DLod(tex_bloom, vec2(t3.x, t12.y), 0.0) * d + texture2DLod(tex_bloom, vec2(t12.x, t3.y), 0.0) * e;
	return r * (1.0 / (a + b + c + d + e));
}

void main()
{
	const vec2 rs = leanTaa[0].xy, jitter = leanTaa[0].zw, ds = leanTaa[1].xy;
	const vec2 uv = v_texcoord0;
	const vec2 pr = uv * rs; // this output pixel's centre in render pixels
	const ivec2 c = ivec2(floor(pr + jitter)); // the render pixel whose jittered sample is nearest
	const ivec2 maxp = ivec2(rs) - ivec2(1, 1);
	const vec4 a = texelFetch(tex_normals, clamp(c, ivec2(0, 0), maxp), 0);
	// 0 still .. 1 moving: from the pass 1 activity (full at 0.5 output pixel per frame, so slow movers and the area they
	// leave get the moving rules), or (activity off) from this pixel's motion (full at 2 output pixels per frame)
	const float speed = leanTaa[3].y != 0.0 ? saturate(a.w * (ds.x / rs.x) * 2.0) : saturate(length(a.xy * ds) * 0.5);
#ifdef EASU
	// Still pixels are left to the main pass. Not "== 0": static pixels carry a rounding residue of the jitter cancellation (~1e-3 px)
	if (speed <= 0.02)
		discard;
#endif

	// History first: its weight sets the width of this frame's reconstruction kernel
	const vec2 prevUv = uv + a.xy;
	float W = 0.0;
	vec4 h = vec4(0.0, 0.0, 0.0, 0.0);
	if (leanTaa[1].z == 0.0 && prevUv.x >= 0.0 && prevUv.y >= 0.0 && prevUv.x <= 1.0 && prevUv.y <= 1.0)
	{
		h = History(prevUv, ds);
		// A non finite history (an Inf / NaN that reached the render once) would stay forever: treated as no history
		if (!LeanNotFinite(h))
			W = min(max(h.a, 0.0), mix(leanTaa[2].z, leanTaa[2].w, speed)) * (1.0 - a.z);
	}

	// Current frame: a Gaussian (Blackman-Harris fit) of the 3x3 jittered samples. Its width is one render pixel where the
	// history is empty and shrinks toward one output pixel as the history weight grows to leanTaa[3].x (0 = never), so the
	// accumulated image converges to the output grid's sharpness instead of the render grid's
	// leanTaa[3].z > 0: only still pixels narrow it (a moving pixel holds little history: a narrow kernel there is close to point
	// sampling the render, stair steps along moving edges at low scales)
	const float shrink = leanTaa[3].x > 0.0 ? saturate(W / leanTaa[3].x) * (leanTaa[3].z > 0.0 ? 1.0 - speed : 1.0) : 0.0;
	const vec2 kscale = mix(vec2(1.0, 1.0), ds / rs, shrink);
	vec3 sum = vec3(0.0, 0.0, 0.0), m1 = vec3(0.0, 0.0, 0.0), m2 = vec3(0.0, 0.0, 0.0);
	float wsum = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			const ivec2 q = c + ivec2(x, y);
			const vec3 s = ToYCoCg(Tonemap(clamp(texelFetch(tex_fb_unfiltered, clamp(q, ivec2(0, 0), maxp), 0).rgb, vec3(0.0, 0.0, 0.0), vec3(65000.0, 65000.0, 65000.0))));
			const vec2 d = (vec2(q) + 0.5 - jitter - pr) * kscale;
			const float w = exp(-2.29 * dot(d, d)) + 1e-6;
			sum += w * s;
			wsum += w;
			m1 += s;
			m2 += s * s;
		}
	vec3 current = sum / wsum;
#ifdef EASU
	// Moving pixels: the edge directed estimate weighted by the speed (0 at the border of the moving area, where this pass stops: no seam)
	const float easu = leanTaa[3].w * saturate((speed - 0.02) * 5.0); // 0 where this pass stops (no seam), full from 0.22
	BRANCH if (easu > 0.0)
	{
		AU4 con0, con1, con2, con3;
		FsrEasuCon(con0, con1, con2, con3, rs.x, rs.y, rs.x, rs.y, ds.x, ds.y);
		// EASU puts input texel i at i; here its sample sits at i - jitter (render pixels, texture rows)
		con0[2] = AU1_AF1(AF1_AU1(con0[2]) + jitter.x);
		con0[3] = AU1_AF1(AF1_AU1(con0[3]) + jitter.y);
		AF3 e;
		FsrEasuF(e, AU2(uv * ds), con0, con1, con2, con3);
		current = mix(current, ToYCoCg(clamp(e, vec3(0.0, 0.0, 0.0), vec3(0.999, 0.999, 0.999))), saturate(easu));
	}
#endif
	// Closeness of the nearest sample to this output pixel, in output pixels: the weight this frame adds to the accumulation
	const vec2 dn = (vec2(c) + 0.5 - jitter - pr) * (ds / rs);
	const float conf = max(exp(-2.0 * dot(dn, dn)), 0.05);

	vec3 hist = current;
	if (W > 0.0)
	{
		hist = ToYCoCg(Tonemap(max(h.rgb, vec3(0.0, 0.0, 0.0))));
		// Variance clip toward the box centre
		const vec3 mean = m1 * (1.0 / 9.0);
		const vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mean * mean, vec3(0.0, 0.0, 0.0)));
		const vec3 ext = max(mix(leanTaa[2].x, leanTaa[2].y, speed) * sigma, vec3(1e-4, 1e-4, 1e-4));
		const vec3 dh = hist - mean;
		const vec3 r = abs(dh) / ext;
		const float m = max(r.x, max(r.y, r.z));
		if (m > 1.0)
		{
			hist = mean + dh * (1.0 / m);
			W *= pow(1.0 / m, leanTaa[1].w); // history far outside the box (a lamp switching) is replaced faster
		}
	}
	const float alpha = conf / (W + conf);
	const vec3 res = InvTonemap(max(FromYCoCg(mix(hist, current, alpha)), vec3(0.0, 0.0, 0.0)));
	gl_FragColor = vec4(res, W + conf);
}
