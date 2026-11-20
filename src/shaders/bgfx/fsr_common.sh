// license:GPLv3+ (glue only; ffx_a.h and ffx_fsr1.h are AMD FidelityFX FSR 1, MIT, see LICENSE-FidelityFX-FSR.txt)

// AMD FidelityFX Super Resolution 1 setup for the VPX fragment post process passes.
// Full precision math on every backend (bgfx's example keeps fp16 off for GLSL too).
#define A_GPU 1
#if BGFX_SHADER_LANGUAGE_GLSL
	#define A_GLSL 1
	#define A_SKIP_EXT 1
#else
	#define A_HLSL 1
#endif
#include "ffx_a.h"
