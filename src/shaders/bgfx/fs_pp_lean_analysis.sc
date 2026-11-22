// license:GPLv3+

// Lean temporal upscaler (Upscaler = 6), pass 1 of 2, at RENDER size, RGBA16F:
// xy = dilated motion: offset from this pixel's UV to its previous frame UV, taken from the nearest depth of the 3x3 (so
//      edges of moving parts carry their motion over the pixels they leave), jitter cancelled
// z  = disocclusion: the motion stored at the reprojected position last frame differs from this one (the pixel was
//      covered by something else, e.g. the playfield a flipper or the ball just uncovered) or it comes from off screen.
//      Last frame's motion is scaled by the frame time ratio first, so uneven frame pacing does not flag a steady mover
// w  = activity, render pixels per frame: this pixel's motion, or half of last frame's activity here, whichever is larger
//      (something moved over this spot recently: the area a slow ball leaves gets the moving-pixel rules, not the still ones)
// Sources (public): Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014 (nearest depth dilation); Yang et al.,
// "A Survey of Temporal Antialiasing Techniques", 2020 (velocity based disocclusion). Motion vectors as the depth
// prepass writes them (fs_depth.sc).

$input v_texcoord0

#include "common.sh"

// [0] xy: render size, zw: jitter cancellation (previous minus current jitter, UV)
// [2] x: disocclusion tolerance as a fraction of this pixel's motion (0 = fixed 0.5 render px)
// [1] x: 1 on a reset, y: this frame's duration / last frame's (1 = off), z: 1 = activity on, w: > 0 = last frame's prepass depth bound, relative depth step that counts
uniform vec4 leanTaa[4];

SAMPLER2D(tex_fb_unfiltered, 0); // Depth prepass depth (device depth, 0 near)
SAMPLER2D(tex_depth,         4); // Last frame's depth prepass depth (the other scene target's prepass)
SAMPLER2D(tex_ao_dither,     5); // This pass, last frame
SAMPLER2D(tex_normals,       6); // Depth prepass attachment 0: zw = current minus previous UV (jittered projections)

void main()
{
	const vec2 size = leanTaa[0].xy;
	const ivec2 maxp = ivec2(size) - ivec2(1, 1);
	const ivec2 p = ivec2(v_texcoord0 * size);
	float dmin = 2.0;
	ivec2 best = p;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			const ivec2 q = clamp(p + ivec2(x, y), ivec2(0, 0), maxp);
			const float d = texelFetch(tex_fb_unfiltered, q, 0).x;
			if (d < dmin)
			{
				dmin = d;
				best = q;
			}
		}
	// Pixels the prepass did not cover keep the cleared (0, 0): static, no jitter cancellation
	const vec2 zw = texelFetch(tex_normals, best, 0).zw;
	vec2 mv = (zw.x == 0.0 && zw.y == 0.0) ? vec2(0.0, 0.0) : -zw - leanTaa[0].zw;
	// The dilation carries a MOVING nearer edge over the pixels it leaves. A still nearer part (a wire ramp over a rolling ball) must not
	// take a moving pixel's motion away: that ball pixel would reproject as still (full history, narrow kernel) and break into blocks
	float d = dmin;
	const vec2 zc = texelFetch(tex_normals, p, 0).zw;
	const vec2 mc = (zc.x == 0.0 && zc.y == 0.0) ? vec2(0.0, 0.0) : -zc - leanTaa[0].zw;
	if (length(mv * size) < 0.05 && length(mc * size) >= 0.05)
	{
		mv = mc;
		d = texelFetch(tex_fb_unfiltered, p, 0).x;
	}

	const vec2 prevUv = v_texcoord0 + mv;
	float disocc = 1.0;
	if (leanTaa[1].x == 0.0 && prevUv.x >= 0.0 && prevUv.y >= 0.0 && prevUv.x <= 1.0 && prevUv.y <= 1.0)
	{
		const vec2 prevMv = texelFetch(tex_ao_dither, clamp(ivec2(prevUv * size), ivec2(0, 0), maxp), 0).xy * leanTaa[1].y;
		// Render pixels of motion difference: below 0.5 the same surface, above 2 another one
		// Tolerance grows with the speed when leanTaa[2].x > 0 (that fraction of this pixel's motion): a rotating, accelerating part (a
		// flipper mid stroke) keeps its history; an uncovered pixel (no motion now, some before) still trips the base 0.5 px
		const float tol = 0.5 + leanTaa[2].x * length(mv * size);
		disocc = saturate((length((mv - prevMv) * size) - tol) * (1.0 / 1.5));
		// Something nearer stood here last frame and is gone (a ball leaving from rest has no motion where it sat, so the velocity test
		// misses it): the depth at the reprojected position last frame is in front of this pixel's own depth
		if (leanTaa[1].w != 0.0)
		{
			// farthest of last frame's 3x3 against the nearest of this frame's 3x3 (own depth on a moving pixel the still nearest left alone): only inside a departed object, not on static depth edges
			// that the jitter shifts by a sub pixel, nor in gaps thinner than a pixel (rails) that the jitter shows in some frames only
			const ivec2 q = ivec2(prevUv * size);
			float dPrev = 0.0;
			for (int y = -1; y <= 1; y++)
				for (int x = -1; x <= 1; x++)
					dPrev = max(dPrev, texelFetch(tex_depth, clamp(q + ivec2(x, y), ivec2(0, 0), maxp), 0).x);
			if (dPrev < d - leanTaa[1].w * max(d, 1e-3))
				disocc = 1.0;
		}
	}
	float activity = length(mv * size);
	if (leanTaa[1].z != 0.0 && leanTaa[1].x == 0.0)
		activity = max(activity, 0.5 * texelFetch(tex_ao_dither, p, 0).w);
	gl_FragColor = vec4(mv, disocc, activity);
}
