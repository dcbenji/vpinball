// license:GPLv3+

// Traced insert: an additive lightmap primitive whose glow comes from an offline path-traced render of the insert
// (a bulb under the real plastic, seen from four view elevations, one per channel of the base image), instead of a
// single view-independent lightmap. The primitive is a flat card at the top of the insert, its UV the bake's square.
//
// - tex_base_color (stage 0): the traced light, 4 elevations in RGBA, linear HDR, normalised to its brightest texel.
// - tex_base_normalmap (stage 4, optional): the bake's facet normals, x and 'up the table' in RG, faceted floor in A.
// - tex_traced_env (stage 7, optional): the lamp-off look, the 4 elevations in a 2x2 atlas (15 | 45 over 60 | 70), linear HDR.
// - tex_pbr_orm (stage 8, optional): the table's print map over the table rectangle: rgb the playfield art (sRGB), alpha 1 where
//   the print leaves an insert clear, 0 where it covers it.
// - staticColor_Alpha: the lamp's colour x level, as for any additive lightmap primitive.
//
// tracedParams (insert space in mm, angles in degrees):
// [0] the 4 traced elevations, off vertical
// [1] depths: x cover slab whose refraction is undone, y facets under the top, z glow parallax, w rim fade
// [2] x glow gain, y glow spread, z IOR, w debug (1 = alignment view)
// [3] x glint strength, y glint sharpness, z facet tilt, w colour fringe
// [4] x glint keys, y glint turn (incl. the per-insert turn), z facet waviness, w waviness wavelength
// [5] x per-insert seed, y has facets, z mm per table unit, w frost (a sanded cap: 0 clear, 1 heavily sanded)
// [6] rgb: room light glinting in the facets, lamp on or off, w: lamp-off look gain (0 = no lamp-off atlas)
// [7] rgb: the plastic's colour, tinting the (neutral) lamp-off look by [11].y, w: facet blur for the glints in mips (hides a hard-edged facet map)
// [8] x 1 = spill pass (the card scaled up about its centre), y spill size in card widths, z spill strength,
//     w spill reach as a print map mip (-1: no print map, a round falloff)
// [9] table position to print map UV: xy scale, zw offset (x = 0: no print map)
// [10] xy the print map's texel (UV) at the spill mip, z white leak at the glow's peak (the share of the lamp's white light the
//      plastic lets through: a hot insert's core goes white, as on camera, while its body, spill and lamp-off look keep the colour),
//      w how much the plastic's colour tints the room glints (0 white, 1 the plastic's colour)
// [11] x print map tint (inktint): an insert under printed art (Police Force's jackpot car and character patches) shows
//      through the print by its alpha (the print's transmission) tinted by its hue; 0 = alpha only, as a cut-out window
//      y how much the plastic's colour tints the lamp-off look (plastictint: 1 = fully, for a neutral bake; 0 = not at all,
//      the atlas keeps the colour baked into it: a bake that already shows the plastic's colour is not filtered twice)
//      z facet glow (facetglow: the bulb seen through the facets, its peak in glow gains; 0 = off), w its sharpness (facetlobe)
// [12] parallax hot spot: x the bulb's depth below the insert top in mm (0 = off: the block is skipped, output unchanged),
//      y spot size (1 = a soft spot ~19 mm across at half height), z brightness (1 = +80 % of the glow's peak at its centre),
//      w how much the glints' highlights turn with the eye's bearing (glintturn: 1 = fully, 0 = they hold still relative to the
//      table and glints only shift by refraction parallax; not read by the hot spot)
// [13] parallax hot spot: xy stretch along the card's u and v (1 = round), zw the spot's offset from the card's centre in mm
//      (along u and v: for an unturned card x across, y down the table)

$input v_worldPos, v_tablePos, v_normal, v_texcoord0
#ifdef STEREO
	$input v_eye
#endif
#ifdef CLIP
	$input v_clipDistance
#endif

#include "common.sh"

uniform vec4 staticColor_Alpha;
uniform vec4 tracedParams[17];

#ifdef STEREO
	uniform vec4 cameraPosWorld[2];
	#define cCameraPosWorld cameraPosWorld[int(round(v_eye))].xyz
#else
	uniform vec4 cameraPosWorld;
	#define cCameraPosWorld cameraPosWorld.xyz
#endif

SAMPLER2D(tex_base_color, 0);     // traced light
SAMPLER2D(tex_base_normalmap, 4); // traced facets
SAMPLER2D(tex_traced_env, 7);     // traced lamp-off atlas
SAMPLER2D(tex_pbr_orm, 8);        // the table's print map: art in rgb, clear insert windows in alpha

// Live trace: what a ray leaving the insert's floor downward (from p, mm about the bulb's axis, z up from the insert top)
// sees: the bulb, a glowing sphere of radius r at depth (hotter toward its middle, as the filament inside it), or else the
// cavity under the insert lit by it (the pocket and the board, falling off away from the bulb). soft: a slightly rough
// underside spreads each ray into a cone, as wide as soft x the way down: the bulb's image widens by it (same light), so a
// facet fades in and out of the bulb as the eye moves instead of snapping.
float liveTraceBelow(vec3 p, vec3 d, float depth, float rBulb, float cavity, float soft)
{
	const float down = (p.z + depth) / max(-d.z, 0.05); // the way down to the bulb's level
	const float r = sqrt(rBulb * rBulb + soft * soft * down * down);
	const vec3 oc = p - vec3(0.0, 0.0, -depth);
	const float b = dot(oc, d);
	const float miss2 = max(dot(oc, oc) - b * b, 0.0); // the ray's closest approach to the bulb's centre, squared
	const float edge = saturate(1.0 - miss2 / (r * r));
	// the sphere's face: brightest straight on at its middle (a filament glow inside the glass), a bright rim to its edge
	const float bulb = b < 0.0 ? edge * (0.6 + 2.4 * edge * edge * edge) * (rBulb * rBulb) / (r * r) : 0.0;
	const vec2 q = p.xy + d.xy * down; // where the ray reaches the bulb's level
	const float cavityLight = cavity / (1.0 + dot(q, q) / (depth * depth));
	return bulb + (1.0 - saturate(edge * 4.0)) * cavityLight;
}

// How much each of the four traced elevations counts at this one: linear between neighbours, held at the ends
vec4 tracedAngleWeights(float elevation)
{
	const vec4 a = tracedParams[0];
	vec4 w = vec4(0.0, 0.0, 0.0, 0.0);
	if (elevation <= a.x)
		w.x = 1.0;
	else if (elevation <= a.y)
	{
		const float t = (elevation - a.x) / max(a.y - a.x, 1e-3);
		w.xy = vec2(1.0 - t, t);
	}
	else if (elevation <= a.z)
	{
		const float t = (elevation - a.y) / max(a.z - a.y, 1e-3);
		w.yz = vec2(1.0 - t, t);
	}
	else if (elevation <= a.w)
	{
		const float t = (elevation - a.z) / max(a.w - a.z, 1e-3);
		w.zw = vec2(1.0 - t, t);
	}
	else
		w.w = 1.0;
	return w;
}

// The facets catching a few fixed highlights that turn with the eye's bearing (turn takes some of that back), each colour channel tilted a little apart
float tracedGlint(vec3 n, vec3 v, float turn, float sharp, float channel)
{
	const int keys = int(clamp(tracedParams[4].x, 1.0, 8.0));
	const float tilt = radians(tracedParams[3].z + (channel - 1.0) * tracedParams[3].w);
	const float st = sin(tilt);
	const float ct = cos(tilt);
	const float azimuth = atan2(-v.y, v.x) + radians(tracedParams[4].y + turn); // as the bake's frame: y up the table
	float sum = 0.0;
	for (int j = 0; j < 8; j++)
	{
		if (j >= keys)
			break;
		const float a = azimuth + 6.28318530718 * float(j) / float(keys);
		sum += pow(saturate(dot(n, vec3(st * cos(a), -st * sin(a), ct))), sharp);
	}
	// Phong normalisation: a wide glint is a dim one, not a flood into the bloom. Faded out straight under the eye where the bearing spins.
	return sum * ((sharp + 2.0) / 602.0) * smoothstep(0.05, 0.25, length(v.xy));
}

// The lamp-off look at a card point: the four elevations of the atlas, each read at the mip the insert's screen size asks for
// (or the frost's blur, if wider), half a texel of that mip in from its slice's edge so no slice bleeds into its neighbour
vec3 tracedEnv(vec2 uv, vec4 weights, float minLod)
{
	const vec2 slice = vec2(512.0, 512.0);
	const vec2 dx = dFdx(uv) * slice, dy = dFdy(uv) * slice;
	const float lod = clamp(max(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8)), minLod), 0.0, 8.0);
	const vec2 margin = exp2(lod) / (2.0 * slice);
	const vec2 q = clamp(uv, margin, 1.0 - margin) * 0.5;
	return texture2DLod(tex_traced_env, q, lod).rgb * weights.x
		+ texture2DLod(tex_traced_env, q + vec2(0.5, 0.0), lod).rgb * weights.y
		+ texture2DLod(tex_traced_env, q + vec2(0.0, 0.5), lod).rgb * weights.z
		+ texture2DLod(tex_traced_env, q + vec2(0.5, 0.5), lod).rgb * weights.w;
}

// A cubic B-spline read folded into 4 bilinear taps, for a texture n texels across at the mip read: the taps' coordinates
// (xy the first along u and v, zw the second) and their weights (xy along u and v for the first taps, zw for the second).
// A high mip read plainly is a box (big bilinear blocks); this is smooth.
void bsplineTaps(vec2 uv, float n, out vec4 at, out vec4 g)
{
	const vec2 p = uv * n - 0.5;
	const vec2 i = floor(p);
	const vec2 f = p - i;
	const vec2 f2 = f * f;
	const vec2 f3 = f2 * f;
	const vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	const vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	const vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	const vec2 w3 = f3 / 6.0;
	g = vec4(w0 + w1, w2 + w3);
	at = vec4((i - 0.5 + w1 / g.xy) / n, (i + 1.5 + w3 / g.zw) / n);
}

// One slice of the lamp-off atlas (o = its corner), B-spline smoothed at a mip
vec3 tracedEnvSlice(vec4 at, vec4 g, vec2 o, float lod)
{
	return g.y * (g.x * texture2DLod(tex_traced_env, at.xy + o, lod).rgb + g.z * texture2DLod(tex_traced_env, at.zy + o, lod).rgb)
		+ g.w * (g.x * texture2DLod(tex_traced_env, at.xw + o, lod).rgb + g.z * texture2DLod(tex_traced_env, at.zw + o, lod).rgb);
}

// The lamp-off look blurred smoothly: the four elevations, each B-spline read at a mip, 1.5 texels of it in from the slice's
// edge so no slice bleeds into its neighbour
vec3 tracedEnvSmooth(vec2 uv, vec4 weights, float lod)
{
	const float n = 512.0 * exp2(-lod);
	vec4 at, g;
	bsplineTaps(clamp(uv, 1.5 / n, 1.0 - 1.5 / n), n, at, g);
	at *= 0.5; // a slice is a quarter of the atlas
	return tracedEnvSlice(at, g, vec2(0.0, 0.0), lod) * weights.x + tracedEnvSlice(at, g, vec2(0.5, 0.0), lod) * weights.y
		+ tracedEnvSlice(at, g, vec2(0.0, 0.5), lod) * weights.z + tracedEnvSlice(at, g, vec2(0.5, 0.5), lod) * weights.w;
}

#ifndef CLIP
EARLY_DEPTH_STENCIL
#endif
void main()
{
	#ifdef CLIP
	if (v_clipDistance < 0.0)
		discard;
	#endif

	// Spill pass: the lit insert's light on the art around it. It leaves the window's edges and falls off fast (the window
	// mask blurred over the reach, three mips for a soft tail), lights the art under it (dark ink stays dark), never the
	// windows themselves, and fades out before the scaled card's rim. Without a print map: the old round falloff.
	if (tracedParams[8].x > 0.5)
	{
		const float r = length(v_texcoord0 * 2.0 - 1.0);
		if (tracedParams[9].x > 0.0 && tracedParams[8].w >= 0.0)
		{
			const vec2 t = v_tablePos.xy * tracedParams[9].xy + tracedParams[9].zw;
			const float lod = tracedParams[8].w;
			const vec4 here = texture2DLod(tex_pbr_orm, t, 0.0);
			// each mip read at 4 points half its texel apart (a mip alone is a box: blocky)
			float spread = 0.0;
			for (int i = 0; i < 3; i++)
			{
				const vec2 o = tracedParams[10].xy * (0.5 * exp2(float(i)));
				const float l = lod + float(i);
				const float m = texture2DLod(tex_pbr_orm, t + vec2(o.x, o.y), l).a + texture2DLod(tex_pbr_orm, t + vec2(-o.x, o.y), l).a
					+ texture2DLod(tex_pbr_orm, t + vec2(o.x, -o.y), l).a + texture2DLod(tex_pbr_orm, t - o, l).a;
				spread += (i == 0 ? 0.5 : i == 1 ? 0.3 : 0.2) * 0.25 * m;
			}
			const float spill = spread * (1.0 - here.a) * saturate((1.0 - r) / 0.4);
			gl_FragColor = vec4(staticColor_Alpha.rgb * here.rgb * (tracedParams[8].z * spill), 1.0);
		}
		else
		{
			const float h = exp(-(r / 0.38) * (r / 0.38)) * pow(saturate((1.0 - r) / 0.35), 1.5);
			gl_FragColor = vec4(staticColor_Alpha.rgb * (tracedParams[8].z * pow(h, 2.2)), 1.0);
		}
		return;
	}

	// Insert frame from the card's UV derivatives: table units per UV along u and v, so the card may be placed, turned
	// and scaled freely. Table space is z up; the card's u and v are its own x and y.
	const vec2 uv = v_texcoord0;
	const vec3 dpx = dFdx(v_tablePos);
	const vec3 dpy = dFdy(v_tablePos);
	const vec2 dux = dFdx(uv);
	const vec2 duy = dFdy(uv);
	const float det = dux.x * duy.y - dux.y * duy.x;
	const vec3 dpdu = (dpx * duy.y - dpy * dux.y) / det;
	const vec3 dpdv = (dpy * dux.x - dpx * duy.x) / det;
	const float mmPerUnit = tracedParams[5].z;
	const float size = length(dpdu) * mmPerUnit; // the bake square's width in mm
	const vec3 tu = normalize(dpdu);
	const vec3 tv = normalize(dpdv - tu * dot(dpdv, tu));

	// The eye in the card frame: x along u, y along v (down the table for an unturned card), z up
	const vec3 vt = normalize(cCameraPosWorld - v_tablePos);
	const vec3 v = normalize(vec3(dot(vt, tu), dot(vt, tv), max(vt.z, 1e-5)));
	const float eta = max(tracedParams[2].z, 1.001);
	const vec3 inside = refract(-v, vec3(0.0, 0.0, 1.0), 1.0 / eta);
	const float steep = max(v.z, 0.05);
	const float steepInside = max(-inside.z, 0.05);

	// Frost: a sanded cap (0 clear, 1 heavily sanded). Its rough top scatters what passes it over a cone about the refracted
	// ray (so the refraction itself stays): the light under it blurs over a disc up to ~1.2 mm across (in mm, whatever the view
	// distance) and evens out a little toward the insert's mean glow, the view response flattens a little, the pocket wall's
	// fade softens, and the glints and the hot spot go broad and soft. The print over the cap stays sharp.
	const float frost = saturate(tracedParams[5].w);
	const float blurMm = 1.2 * frost;

	// The card point the cover shows here, its refraction through a slab undone
	const vec2 card = uv - (tracedParams[1].x / size) * (v.xy / steep + inside.xy / steepInside);

	// The traced elevation weights, and how far the inside of the plastic slides against the traced direction
	// (the bake looks up the table from the player's side: its eye is along +v)
	const float elevation = degrees(acos(saturate(v.z)));
	// (the live trace below draws the lit insert; its frost must not mix the four traced elevations of the lamp-off image
	// together: each sees the insert foreshortened differently, so they stack as nested, smaller copies)
	const bool liveOn = tracedParams[16].x > 0.0 && tracedParams[12].x > 0.0 && tracedParams[5].y > 0.5;
	const vec4 weights = mix(tracedAngleWeights(elevation), vec4(0.25, 0.25, 0.25, 0.25), liveOn ? 0.0 : 0.3 * frost);
	const vec3 tracedView = vec3(0.0, sin(radians(elevation)), cos(radians(elevation)));
	const vec3 tracedInside = refract(-tracedView, vec3(0.0, 0.0, 1.0), 1.0 / eta);
	const vec2 slide = (inside.xy / max(-inside.z, 0.25) - tracedInside.xy / max(-tracedInside.z, 0.25)) * (tracedParams[1].z / size);

	const vec2 lightUv = card + slide;
	float light;
	if (frost > 0.0)
	{
		// Blurred over a disc blurMm across: 4 taps on a rotated grid, each at the mip half the disc wide (or the pixel's
		// footprint, if wider; a mip alone is a box: blocky), then mixed a little toward a milky fill: the broad local glow and
		// the insert's mean glow, spread evenly to the outline (the bake's mean over its square, over the share of the square
		// the outline covers)
		const float footprint = max(length(dFdx(lightUv)), length(dFdy(lightUv))) * 1024.0;
		const float r = 0.5 * blurMm / size; // the disc's radius in UV
		const float lod = log2(max(max(footprint, r * 1024.0), 1.0));
		const vec2 disc[4] = { vec2(-0.25, -0.75), vec2(0.75, -0.25), vec2(0.25, 0.75), vec2(-0.75, 0.25) };
		float blurred = 0.0;
		for (int i = 0; i < 4; i++)
			blurred += dot(texture2DLod(tex_base_color, lightUv + disc[i] * r, lod), weights);
		const float broad = dot(texture2DLod(tex_base_color, lightUv, 8.0), weights);
		const float mean = dot(texture2DLod(tex_base_color, vec2(0.5, 0.5), 10.0), weights) / max(tracedParams[1].w > 0.0 ? 0.62 : 1.0, 0.05);
		light = mix(0.25 * blurred, 0.5 * (broad + mean), 0.3 * frost);
	}
	else
		light = dot(texture2DGrad(tex_base_color, lightUv, dFdx(lightUv), dFdy(lightUv)), weights);
	light = pow(max(light, 0.0), 1.0 / max(tracedParams[2].y, 0.05));

	const bool hasFacets = tracedParams[5].y > 0.5;
	vec3 glint = vec3(0.0, 0.0, 0.0);
	const float fgAmount = tracedParams[11].z;
	const float fgSharp = tracedParams[11].w;
	float facetGlow = 0.0;
	if (hasFacets)
	{
		// Fade the light over the last millimetres inside the outline (the render's pocket wall seen at a slant): the floor
		// mask blurred over that width is a half on the outline
		float outline = 1.0;
		if (tracedParams[1].w > 0.0)
		{
			const float texel = size / 1024.0;
			const float mip = log2(max(2.0 * tracedParams[1].w / texel, 1.0));
			outline = mix(smoothstep(0.5, 0.95, texture2DLod(tex_base_normalmap, card, mip).a), 1.0, 0.5 * frost);
			light *= outline;
		}

		// The facets under the card, where the refracted eye ray meets them. The map's green is 'up the table', -v.
		const vec2 floorUv = card + inside.xy * (tracedParams[1].y / size / steepInside);
		const vec2 gx = dFdx(floorUv), gy = dFdy(floorUv);

		// No two inserts glint alike: a per-insert seed turns the highlights and phases the moulding's waviness
		const float seed = tracedParams[5].x;
		const float seedB = fract(seed * 17.13 + 0.37);
		const vec2 wave = card * size * (6.28318530718 / max(tracedParams[4].w, 1e-3));
		const vec2 wobble = vec2(sin(wave.x * 0.83 + wave.y * 0.57 + seed * 6.283), sin(wave.y * 0.91 - wave.x * 0.41 + seedB * 6.283));
		const vec2 lean = wobble * tan(radians(clamp(tracedParams[4].z, 0.0, 30.0)));

		// Specular anti-aliasing: widen the highlight by how much the filtered facet normal changes across the pixel
		const vec2 filtered = texture2DGrad(tex_base_normalmap, floorUv, gx, gy).rg;
		const float variance = max(dot(dFdx(filtered), dFdx(filtered)), dot(dFdy(filtered), dFdy(filtered)));
		const float alpha2 = 2.0 / (max(tracedParams[3].y, 1.0) + 2.0) + min(variance, 0.25);
		// ...and frost turns the sparkle into a soft sheen
		const float sharp = max((2.0 / alpha2 - 2.0) * (1.0 - 0.6 * frost), 1.0);

		// The highlights are sharp and the facet seams hard, so one sample per pixel switches whole pixels on and off along
		// every seam (stair-stepped dashes, with no temporal AA to hide them): average 4 samples on a rotated grid inside
		// the pixel, each reading the facets over a quarter of its footprint
		const vec2 taps[4] = { vec2(-0.125, -0.375), vec2(0.375, -0.125), vec2(0.125, 0.375), vec2(-0.375, 0.125) };
		// (with the live trace, a sanded cap also spreads the facets the glints are seen in, the lamp's and the room's: up to 6x wider at full frost)
		const float footprint = 0.5 * exp2(max(tracedParams[7].w, 0.0) + (liveOn ? 2.5 * frost : 0.0));
		// Undoes part of the highlights' turn with the eye's bearing, as the eye's bearing in table space (0 from the player's side, so its seam is behind the backbox)
		const float glintTurn = -(1.0 - tracedParams[12].w) * degrees(atan2(vt.x, vt.y));
		// Facet glow: the bulb seen through the facets. Its light travels up from the bulb (bulb_depth below the top, under the
		// facets) to the facet floor, where each facet refracts it: a facet sends it to the eye when its normal is the half
		// vector of that refraction (eta * the ray on to the eye - the ray from the bulb). The lobe is wrapped (a facet turned
		// a right angle away still passes a quarter at sharpness 2), so a low sharpness is a milky plastic whose whole body
		// glows with soft rays, a high one a clear plastic where only the facets turned to the eye light up. Brightest over
		// the bulb (an inverse-square falloff: softer than a bare point source, as the pocket walls fill some back in), up to
		// the window's edge. 0 = off.
		const bool fgOn = fgAmount > 0.0 && tracedParams[12].x > 0.0;
		vec3 fgHalf = vec3(0.0, 0.0, 1.0);
		float fgFall = 0.0;
		if (fgOn)
		{
			const vec2 pmm = (floorUv - 0.5) * size - tracedParams[13].zw;
			const float below = max(tracedParams[12].x - tracedParams[1].y, 1.0);
			const vec3 fromBulb = normalize(vec3(pmm, below));
			fgHalf = normalize(-eta * inside - fromBulb);
			fgFall = pow(below / length(vec3(pmm, below)), 2.0);
		}
		const float fgS = max(fgSharp * (1.0 - 0.6 * frost), 1.0);
		// The glow fills the insert's window, as the bake sees it: wherever the traced light reaches the top (the faceted floor
		// and the smooth margin round it), with a crisp edge there, as a real insert's light stops at its window. The bake's
		// window is read at the card point (the top) from its 8-texel mip, 4 taps on a rotated grid so the edge is smooth;
		// the bake is dark between its wedges, but on Police Force's square and triangle bakes the window's cells stay above
		// about 0.002 of the peak at that scale and every cell outside it is 0
		float fgMask = 0.0;
		if (fgOn)
		{
			const vec2 mo = vec2_splat(4.0 / 1024.0);
			const vec4 m4 = texture2DLod(tex_base_color, card + vec2(-0.25 * mo.x, -0.75 * mo.y), 3.0) + texture2DLod(tex_base_color, card + vec2(0.75 * mo.x, -0.25 * mo.y), 3.0)
				+ texture2DLod(tex_base_color, card + vec2(0.25 * mo.x, 0.75 * mo.y), 3.0) + texture2DLod(tex_base_color, card + vec2(-0.75 * mo.x, 0.25 * mo.y), 3.0);
			fgMask = smoothstep(0.0003, 0.002, 0.25 * dot(m4, weights));
		}
		for (int i = 0; i < 4; i++)
		{
			const vec4 f = texture2DGrad(tex_base_normalmap, floorUv + gx * taps[i].x + gy * taps[i].y, gx * footprint, gy * footprint);
			const vec2 nxy = vec2(f.r * 2.0 - 1.0, 1.0 - f.g * 2.0);
			const vec3 wavy = normalize(vec3(nxy + lean, sqrt(saturate(1.0 - dot(nxy, nxy)))));
			glint += vec3(tracedGlint(wavy, v, glintTurn, sharp, 0.0), tracedGlint(wavy, v, glintTurn, sharp, 1.0), tracedGlint(wavy, v, glintTurn, sharp, 2.0)) * f.a;
			if (fgOn)
				facetGlow += pow(saturate(dot(wavy, fgHalf) * 0.5 + 0.5), fgS);
		}
		glint *= 0.25 * (1.0 - (liveOn ? 0.85 : 0.5) * frost); // a sanded top all but hides the facets' highlights (with the live trace)
		facetGlow *= 0.25 * fgFall * fgMask * fgAmount;
	}

	// Live trace (a clear starburst insert, traced through for real per pixel): the eye ray refracts in at the top, falls to
	// the faceted floor (a plane floordepth mm under the top, its facets from the facet map), and splits there by Fresnel:
	// what leaves through the facet goes on down to the bulb (a glowing sphere, bulbradius mm, at bulb_depth,
	// moved by the bulb offset) or the lit cavity round it; what the facet reflects goes back up, where the top reflects it
	// again (all of it past the critical angle: total internal reflection) to a second facet, and leaves there. So the facets
	// aimed at the bulb carry its image at its real depth, the others go dark or carry its second bounce: the diamond look.
	// Where the floor is flat (the skirt round the starburst, a flat bottom) the ray looks onto the pocket's wall and ledge
	// more than the lit cavity: skirt = its share. Eight rays per pixel (the facets' edges are sharp and there is no
	// temporal AA). Frost (a sanded top) turns them a little about the refracted ray and hands the light over to the body
	// glow below (the light scattered in the whole body, smooth): from a clear diamond at 0 to a milky glow at about 0.7.
	// softness = the underside's roughness (see liveTraceBelow). spokes = how differently the two slopes of a spoke couple
	// the bulb's light (moulded starbursts do: one slope reads bright, the other dark, even straight from above, where a
	// clear trace alone would light both alike), after VPE Insert Hybrid's facet contrast: one fixed direction per insert
	// (its seed) plus the eye's, so it changes smoothly with the view. It replaces the baked glow (keepbake = the share
	// kept). 0 = off.
	float live = 0.0;
	const float spokes = tracedParams[15].z;
	const float turnS = tracedParams[5].x * 6.28318530718;
	const vec2 biasRaw = v.xy + 0.45 * vec2(cos(turnS), sin(turnS));
	const vec2 bias = biasRaw / max(length(biasRaw), 0.45); // fades through zero rather than flipping where the eye cancels the seed's direction
	float liveWindow = 0.0;
	if (liveOn)
	{
		const float depth = tracedParams[12].x;
		const float plate = max(min(tracedParams[16].z, depth - 0.5), 0.2);
		const float r = max(tracedParams[15].y, 0.5) * (1.0 + 0.5 * frost);
		const float cav = tracedParams[16].y;
		const float r0 = (eta - 1.0) * (eta - 1.0) / ((eta + 1.0) * (eta + 1.0));
		const vec2 cx = dFdx(card), cy = dFdy(card);
		const float soft = tracedParams[15].x;
		// 8 rays: a rotated 8-rook pattern over the pixel, each turned by frost a different way
		const vec2 sub[8] = { vec2(-0.4375, 0.0625), vec2(-0.3125, -0.3125), vec2(-0.1875, 0.3125), vec2(-0.0625, -0.0625),
			vec2(0.0625, 0.4375), vec2(0.1875, -0.4375), vec2(0.3125, 0.1875), vec2(0.4375, -0.1875) };
		const vec2 cone[8] = { vec2(0.7, 0.2), vec2(-0.2, 0.7), vec2(-0.7, -0.2), vec2(0.2, -0.7), vec2(0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5), vec2(-0.5, -0.5) };
		float sum = 0.0;
		for (int i = 0; i < 8; i++)
		{
			const vec2 c = card + cx * sub[i].x + cy * sub[i].y;
			const vec3 nTop = normalize(vec3(cone[i] * (0.4 * frost), 1.0));
			vec3 d = refract(-v, nTop, 1.0 / eta);
			const float cosV = saturate(dot(v, nTop));
			float w = 1.0 - (r0 + (1.0 - r0) * pow(1.0 - cosV, 5.0));
			vec3 p = vec3((c - 0.5) * size - tracedParams[13].zw, 0.0);
			float seen = 0.0;
			for (int bounce = 0; bounce < 2; bounce++)
			{
				p += d * ((plate + p.z) / max(-d.z, 0.02));
				const vec2 fuv = (p.xy + tracedParams[13].zw) / size + 0.5;
				const vec4 f = texture2DGrad(tex_base_normalmap, fuv, cx * 0.35, cy * 0.35);
				const vec2 nxy = vec2(f.r * 2.0 - 1.0, 1.0 - f.g * 2.0) * step(0.5, f.a); // off the faceted floor: a flat bottom
				const vec3 n = vec3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
				const float cosI = saturate(dot(-d, n));
				const float k = 1.0 - eta * eta * (1.0 - cosI * cosI);
				const float F = k <= 0.0 ? 1.0 : r0 + (1.0 - r0) * pow(1.0 - sqrt(k), 5.0); // Schlick on the air side: ramps to 1 at the critical angle
				if (k > 0.0)
				{
					const float onFacet = smoothstep(0.03, 0.12, length(nxy));
					const float coupling = saturate(0.5 + 1.8 * dot(nxy, bias));
					// over its mean (about 0.85), so the spokes change the pattern, not the insert's brightness
					const float faceGain = mix(1.0, (0.12 + 1.8 * pow(coupling, 1.3)) / 0.85, spokes * onFacet);
					seen += w * (1.0 - F) * liveTraceBelow(p, refract(d, n, eta), depth, r, cav, soft) * mix(tracedParams[15].w, 1.0, onFacet) * faceGain;
				}
				// back up to the top, and down again off it
				d = reflect(d, n);
				w *= F;
				if (d.z <= 0.0)
					break;
				p += d * (-p.z / d.z);
				const float cosT = d.z;
				const float kT = 1.0 - eta * eta * (1.0 - cosT * cosT);
				w *= kT <= 0.0 ? 1.0 : r0 + (1.0 - r0) * pow(1.0 - sqrt(kT), 5.0);
				d.z = -d.z;
			}
			sum += seen;
		}
		// only inside the insert's window (as the bake sees it, the skirt included: its 8-texel mip, as Facet Glow reads it),
		// so no light leaks onto the card's square round the window where no print map covers it
		const vec2 mo = vec2_splat(4.0 / 1024.0);
		const vec4 m4 = texture2DLod(tex_base_color, card + vec2(-0.25 * mo.x, -0.75 * mo.y), 3.0) + texture2DLod(tex_base_color, card + vec2(0.75 * mo.x, -0.25 * mo.y), 3.0)
			+ texture2DLod(tex_base_color, card + vec2(0.25 * mo.x, 0.75 * mo.y), 3.0) + texture2DLod(tex_base_color, card + vec2(-0.75 * mo.x, 0.25 * mo.y), 3.0);
		liveWindow = smoothstep(0.0003, 0.002, 0.25 * dot(m4, weights));
		live = 0.125 * sum * tracedParams[16].x * (1.0 - 0.8 * frost) * liveWindow;
		// the baked glow goes (keepbake keeps a share); with it go the parallax hot spot's 0.7 share and the white leak's
		// share over it, which follow the baked glow
		light *= tracedParams[16].w;
	}

	// The lamp-off look, always there under the lamp, at the same card point and weights as the traced light, filtered by
	// the plastic's colour by plastictint: mix(1, colour, plastictint), written so that 1 gives exactly the colour
	vec3 lampOff = vec3(0.0, 0.0, 0.0);
	if (tracedParams[6].w > 0.0)
	{
		vec3 env;
		vec3 body = vec3(0.0, 0.0, 0.0);
		if (liveOn && frost > 0.0)
		{
			// With the live trace, frost is a sanded cap (the top the spokes are fused to). The room's light entering it is
			// scattered every way, so the facets under it are lit evenly instead of mirroring a few room lights, and what comes
			// back up is spread again on the way out. The lamp-off image is only those few mirrored lights (a few hot wedges,
			// black elsewhere): sanding spreads them (a smooth read over the blur, never a coarse mip's blocks; whole mips, as
			// a B-spline is smooth only at one) and they fade into the body, while the room lights the whole body: the plastic's
			// colour inside the window, its spokes shaded faintly as the lit ones are (spokes), seen through the same blur
			// (the cap is ~2.5 mm over the facets)
			const float offBlurMm = 0.4 + 2.2 * frost;
			const vec2 sx = dFdx(lightUv) * 512.0, sy = dFdy(lightUv) * 512.0;
			const float screenLod = 0.5 * log2(max(max(dot(sx, sx), dot(sy, sy)), 1e-8));
			// (the blur's lod rounded, constant over the insert; the pixel's footprint, if wider, left as it is: no seam where it crosses a mip)
			const vec3 spread = tracedEnvSmooth(lightUv, weights, clamp(max(floor(log2(offBlurMm / size * 512.0) + 0.5), screenLod), 0.0, 5.0));
			if (frost < 0.25)
				env = mix(tracedEnv(lightUv, weights, 0.0), spread, 4.0 * frost);
			else
				env = spread;
			env *= (1.0 - frost) * (1.0 - frost);
			const float nLod = clamp(floor(log2(offBlurMm / size * 1024.0) + 0.5), 0.0, 7.0);
			const vec2 fuv = card + inside.xy * (tracedParams[1].y / size / steepInside);
			vec4 at, g;
			bsplineTaps(fuv, 1024.0 * exp2(-nLod), at, g); // (no margin: not an atlas, and clamp addressing holds the edge)
			const vec4 f = g.y * (g.x * texture2DLod(tex_base_normalmap, at.xy, nLod) + g.z * texture2DLod(tex_base_normalmap, at.zy, nLod))
				+ g.w * (g.x * texture2DLod(tex_base_normalmap, at.xw, nLod) + g.z * texture2DLod(tex_base_normalmap, at.zw, nLod));
			const vec2 nxy = vec2(f.r * 2.0 - 1.0, 1.0 - f.g * 2.0);
			const float coupling = saturate(0.5 + 1.8 * dot(nxy, bias));
			const float shade = mix(1.0, (0.12 + 1.8 * pow(coupling, 1.3)) / 0.85, 0.6 * spokes * saturate(f.a));
			// lit by the room (its colour, its level by the lamp-off gain), always the plastic's colour: the body is not in the
			// lamp-off image, so plastictint (which only says whether that image already has the colour) does not apply
			body = tracedParams[6].rgb / max(tracedParams[6].g, 1e-4) * tracedParams[7].rgb * (tracedParams[6].w * 0.3 * frost * (2.0 - frost) * shade * liveWindow);
		}
		else
		{
			const float offLod = log2(max(0.5 * blurMm / size * 512.0, 1.0));
			env = tracedEnv(lightUv, weights, offLod);
		}
		lampOff = tracedParams[6].w * (tracedParams[7].rgb * tracedParams[11].y + (1.0 - tracedParams[11].y)) * env + body;
	}

	// The lamp's glow and glints, plus the room glinting in the facets whatever the lamp does
	// (the white share grows with the glow: the bake's peak (1) gets all of it, the dim body almost none)
	// The room's light glints off the facets under the plastic, so it comes back through it: tinted by its colour (roomtint)
	const vec3 roomGlint = tracedParams[6].rgb * mix(vec3_splat(1.0), tracedParams[7].rgb, tracedParams[10].w);
	const vec3 glowTint = mix(staticColor_Alpha.rgb, vec3_splat(staticColor_Alpha.a), tracedParams[10].z * saturate(light));
	vec3 result = glowTint * (tracedParams[2].x * light) + staticColor_Alpha.rgb * (10.0 * tracedParams[3].x * glint) + roomGlint * (10.0 * tracedParams[3].x) * glint + lampOff;
	// the facet glow, whiter where it is bright by the same white-leak share
	result += mix(staticColor_Alpha.rgb, vec3_splat(staticColor_Alpha.a), tracedParams[10].z * saturate(facetGlow)) * (tracedParams[2].x * facetGlow);

	// the live trace, whitened by the white-leak share where it is bright
	result += mix(staticColor_Alpha.rgb, vec3_splat(staticColor_Alpha.a), tracedParams[10].z * saturate(live)) * (tracedParams[2].x * live);

	// Body glow: the plastic is a little milky, so the bulb's light scatters inside the whole body and the glow sits in it,
	// under the top. The body is a slab bodythick mm deep under the top; the bulb (a small sphere, bodybulb mm) sits
	// bulb_depth mm below the top, moved by the bulb offset. Along the refracted eye ray through the slab, the light scattered
	// toward the eye is the bulb's 1/r^2 summed along the ray: in closed form, (atan(b - t0) - atan(a - t0)) / h over a segment
	// [a, b] (h = the ray's closest approach to the bulb, t0 = where it is). A forward-scattering lobe (bodyfwd) makes it
	// hottest where the eye looks down the bulb's beam: a core that slides with the view as a thing below the top does, and
	// HDR-bright there, so the tonemapper and the bloom blow it out. The ray is split in 8 and each piece counts only inside
	// the window (the bake's window under it, the skirt included), so where the eye ray leaves the body through its wall the glow stops early: the
	// far side of the window gets darker, the near wall lit. Scaled by bulb_depth, so a straight-down view right over the bulb
	// is about 1 before the lobe at the default thickness (the lobe adds up to (1+g)/(1-g) there). 0 = off.
	// with the live trace on, a little of the body glow always fills the plastic (light scattered in it: the gaps between the
	// rays keep the plastic's colour), and frost hands more of the light to it
	const float bodyAmount = tracedParams[14].x + (tracedParams[16].x > 0.0 && hasFacets ? (0.04 + 0.15 * frost) * tracedParams[16].x : 0.0);
	if (bodyAmount > 0.0 && tracedParams[12].x > 0.0)
	{
		const float depth = tracedParams[12].x;
		const float thick = max(min(tracedParams[14].y, depth - 0.5), 0.2);
		const float g = tracedParams[14].z;
		const vec3 p0 = vec3((card - 0.5) * size - tracedParams[13].zw, 0.0); // the top, in mm about the bulb's spot
		const vec3 toBulb = vec3(0.0, 0.0, -depth) - p0;
		const float tEnd = min(thick / steepInside, 4.0 * thick);
		const float t0 = dot(toBulb, inside);
		const float h = sqrt(max(dot(toBulb, toBulb) - t0 * t0, tracedParams[14].w * tracedParams[14].w));
		float sum = 0.0;
		float prev = atan((0.0 - t0) / h);
		for (int i = 1; i <= 8; i++)
		{
			const float b = tEnd * (float(i) * 0.125);
			const float next = atan((b - t0) / h);
			const vec3 mid = p0 + inside * (b - 0.0625 * tEnd);
			const vec2 muv = (mid.xy + tracedParams[13].zw) / size + 0.5;
			// the wall, softened (a blurred mip, a wide ramp) so the pieces' edges blend instead of stepping
			const float inWindow = smoothstep(0.0003, 0.002, dot(texture2DLod(tex_base_color, muv, 3.5), weights)); // the bake's whole window, the skirt included
			sum += (next - prev) * inWindow;
			prev = next;
		}
		// The lobe at the ray's closest point: the angle between the bulb's beam there and the way on to the eye
		const vec3 closest = p0 + inside * clamp(t0, 0.0, tEnd);
		const float cosA = dot(normalize(closest - vec3(0.0, 0.0, -depth)), -inside);
		const float lobe = (1.0 - g * g) / max((1.0 - g * cosA) * (1.0 - g * cosA), 1e-3);
		const float body = bodyAmount * depth * sum / h * lobe;
		result += mix(staticColor_Alpha.rgb, vec3_splat(staticColor_Alpha.a), tracedParams[10].z * saturate(body)) * (tracedParams[2].x * body);
	}

	// Parallax hot spot: the lamp's light inside the lit insert is brightest over the bulb, which sits bulb_depth mm below the
	// top; the refracted eye ray travels that far down before it meets it, so the soft spot shifts against the top (and the
	// traced light, which parallaxes over its own baked depth) as the eye moves. A broad gaussian, no bulb detail; whiter at
	// its core by the white-leak share; only where the insert is lit; nothing with the lamp off. A frosted cap spreads it out.
	if (tracedParams[12].x > 0.0)
	{
		const vec2 spotUv = card + inside.xy * (tracedParams[12].x / size / steepInside);
		// mm about the bake square's centre, moved by the offset and squashed by the stretch (both along the card's u and v)
		const vec2 p = ((spotUv - 0.5) * size - tracedParams[13].zw) / (max(tracedParams[12].y, 0.05) * max(tracedParams[13].xy, vec2_splat(0.05)));
		const float sigma = 8.0 + 8.0 * frost;
		const float spot = exp(-dot(p, p) / (2.0 * sigma * sigma)) * (0.3 + 0.7 * saturate(light));
		const vec3 hot = mix(staticColor_Alpha.rgb, vec3_splat(staticColor_Alpha.a), tracedParams[10].z * spot);
		result += hot * (0.8 * tracedParams[2].x * tracedParams[12].z * spot);
	}

	// The print on the playfield covers the insert (its glow, the lamp-off look and the facets alike)
	if (tracedParams[9].x > 0.0)
	{
		const vec4 ink = texture2DLod(tex_pbr_orm, v_tablePos.xy * tracedParams[9].xy + tracedParams[9].zw, 0.0);
		// the print as a colour filter: its hue (rgb over its brightest channel), its strength in the alpha
		const vec3 hue = ink.rgb / max(max(ink.r, ink.g), max(ink.b, 1e-3));
		result *= ink.a * mix(vec3_splat(1.0), mix(hue, vec3_splat(1.0), ink.a), tracedParams[11].x);
	}

	// Alignment view: the bake's square in a 2 mm grid, red outside it, the traced light in green
	if (tracedParams[2].w > 0.5)
	{
		const vec2 mm = fract(card * size * 0.5);
		const float grid = step(0.9, max(mm.x, mm.y));
		const float outside = (card.x < 0.0 || card.y < 0.0 || card.x > 1.0 || card.y > 1.0) ? 1.0 : 0.0;
		result = vec3(outside, light, grid);
	}

	gl_FragColor = vec4(result, 1.0);
}
