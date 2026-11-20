// license:GPLv3+ (glue only; the filter is AMD FidelityFX FSR 1 RCAS, MIT, see ffx_fsr1.h)

$input v_texcoord0

#include "bgfx_shader.sh"

// w_h_height = (sharpness in stops: 0 = sharpest, unused, image width, image height)
uniform vec4 w_h_height;

SAMPLER2D(tex_fb_unfiltered, 0); // EASU output at display resolution

#include "fsr_common.sh"

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(tex_fb_unfiltered, clamp(p, ASU2(0, 0), ASU2(w_h_height.zw) - ASU2(1, 1)), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

void main()
{
	AU4 con;
	FsrRcasCon(con, w_h_height.x);
	AF1 r, g, b;
	FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), con);
	gl_FragColor = vec4(r, g, b, 1.0);
}
