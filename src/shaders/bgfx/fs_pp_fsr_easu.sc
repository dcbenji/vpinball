// license:GPLv3+ (glue only; the filter is AMD FidelityFX FSR 1 EASU, MIT, see ffx_fsr1.h)

$input v_texcoord0

#include "bgfx_shader.sh"

// w_h_height = (input width, input height, output width, output height) in pixels
uniform vec4 w_h_height;

SAMPLER2D(tex_fb_unfiltered, 0); // Tonemapped (perceptual) render at render resolution

#include "fsr_common.sh"

// EASU wants textureGather: emulated with 4 texel fetches (gather order x=(0,1) y=(1,1) z=(1,0) w=(0,0)),
// clamped to the image. Shared by the 3 channel callbacks, the compiler merges the identical fetches.
#define FSR_EASU_GATHER(C) \
	ASU2 sz = ASU2(w_h_height.xy) - ASU2(1, 1); \
	ASU2 b = ASU2(floor(p * w_h_height.xy - AF2(0.5, 0.5))); \
	ASU2 b0 = clamp(b, ASU2(0, 0), sz); \
	ASU2 b1 = clamp(b + ASU2(1, 1), ASU2(0, 0), sz); \
	return AF4(texelFetch(tex_fb_unfiltered, ASU2(b0.x, b1.y), 0).C, texelFetch(tex_fb_unfiltered, ASU2(b1.x, b1.y), 0).C, \
	           texelFetch(tex_fb_unfiltered, ASU2(b1.x, b0.y), 0).C, texelFetch(tex_fb_unfiltered, ASU2(b0.x, b0.y), 0).C);

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { FSR_EASU_GATHER(r) }
AF4 FsrEasuGF(AF2 p) { FSR_EASU_GATHER(g) }
AF4 FsrEasuBF(AF2 p) { FSR_EASU_GATHER(b) }

#include "ffx_fsr1.h"

void main()
{
	// Constants computed per pixel (bgfx has no uint uniforms), as bgfx's own FSR example does
	AU4 con0, con1, con2, con3;
	FsrEasuCon(con0, con1, con2, con3, w_h_height.x, w_h_height.y, w_h_height.x, w_h_height.y, w_h_height.z, w_h_height.w);
	AF3 c;
	FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
	gl_FragColor = vec4(c, 1.0);
}
