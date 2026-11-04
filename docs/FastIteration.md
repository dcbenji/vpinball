# Fast iteration on a running table

A development feature, off unless its environment variable is set, for people and AI agents working on shaders or table look. Linux and macOS, BGFX only; other builds compile it out.

## Shader hot reload

```bash
VPX_SHADER_HOTRELOAD=/path/to/src/shaders/bgfx ./VPinballX_BGFX -LiveEdit table.vpz
```

Edit a file in that folder and save: the player recompiles what depends on it and swaps it in. The camera, lights, table state and undo history stay as they were.

- `VPX_SHADERC=<path>` when shaderc is not at `<repo>/third-party/tools/<platform>/shaderc`.
- The folder must match the build: a shader that uses a uniform this shader object did not have at startup needs C++ and a restart. It is refused with that message.
- Only variants this run loaded are compiled, following `#include`s; a stereo or VR variant is compiled only in a stereo or VR run.
- A compile error keeps the old shaders. It shows on screen for 10 s, in the log, and in the `vpx_engine_shaders` status.
- Edits are not embedded in the binary: a build is still what ships them.

Measured on an RTX 4070 with Vulkan on Police Force, 1080p, from file saved to the first presented frame with the new shader:

- `fs_flasher_dome.sc`, 2 variants: 0.47 s. `material.sh`, 20 variants: 0.43 s.
- `fs_pbr.sc`, 25 variants: 4.4 s. `common.sh`, 214 variants: 6.2 s.
- For scale, a rebuild of the same `fs_pbr.sc` edit: 134 s for the shader step alone, then compile and link, a copy, and about 8 s to restart the table.

Metal uses the same code path with `--platform osx -p metal21-11`. It has not been run yet: the new source files pass a syntax-only compile with the Mac flags and shaderc produces Metal blobs on a Mac (0.28 s per `fs_pbr` variant), nothing more. A shader that shaderc accepts but Metal's own compiler rejects at load time is the untested case.

