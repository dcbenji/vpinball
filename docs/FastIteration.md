# Fast iteration on a running table

**Sessions on the ben-pc rig:** one player at a time, no restart between edits.

1. Start, waits its turn for the rig: `ssh ben@ben-pc '~/Games/VPX-fastiter/start.sh edit'` (`play` for a running game; a pack dir as 2nd argument; `FI_ENGINE=<dir>` for your own build with these features).
2. Call tools: `ssh ben@ben-pc '~/Games/VPX-fastiter/mcp vpx_view path=/tmp/a.png'`; `mcp` alone lists them, `mcp vpx_guide` to begin. Field names: `mcp vpx_table action=part name=<part>`.
3. Edit shaders in `~/Games/VPX-fastiter/shaders/` (reset to the engine's at each start; `FI_KEEP_SHADERS=1` keeps them): they swap in by themselves, `mcp vpx_engine_shaders action=status` tells the time or the error.
4. Compare only captures from `vpx_view`: right after an edit the shadows are wrong for about 40 frames, it waits for that in an edit session. A still table moves up to 3% of its pixels by itself.
5. Always finish with `ssh ben@ben-pc '~/Games/VPX-fastiter/stop.sh'`: it frees the rig. Nothing is saved, the pack on disk stays as it was.

Two development features, both off unless their environment variables are set, for people and AI agents working on shaders or table look. Linux and macOS, BGFX only; other builds compile them out.

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

## MCP server

```bash
VPX_MCP_PORT=9077 VPX_MCP_TOKEN=<secret> ./VPinballX_BGFX -LiveEdit table.vpz
claude mcp add --transport http vpx-engine http://127.0.0.1:9077/mcp --header "Authorization: Bearer <secret>"
```

HTTP on `127.0.0.1` only, bearer token required, Host and Origin checked. It speaks plain JSON responses (no server stream). From a shell: `tools/dev/vpxmcp.py`.

Tools shared in name and shape with the VPX Editor's MCP server:

- `vpx_guide`: start here.
- `vpx_table`: `overview`, `parts` (`type`, `nameRegex`, `limit`), `part` (`name`).
- `vpx_part`: `modify` with `part:{partName, <fields>}`. Applies unless `preview:true`.
- `vpx_material`: `list`, `modify` with `name` + `patch`. A preview until `confirm:true`.
- `vpx_history`: `undo`, `redo`. One history with the live editor.
- `vpx_view`: a PNG of the 3D view, inline (`max_width`) or saved (`path`). `area:"window"` includes the editor panels.

Only in the engine:

- `vpx_engine_status`: mode, frame, playing, shader reload state.
- `vpx_engine_camera`: `eye`, `target`, `fov`, `shading` (`pbr`, `solid`, `wireframe`), or `release:true`.
- `vpx_engine_lights`: hold a lamp or flasher at `level`; no `level` releases it.
- `vpx_engine_run`: `pause`, `resume`, `step` (`ms`, or one physics step), `wait_frames` (`n`).
- `vpx_engine_timing`: fps, frame and GPU ms over the last frames (`last`, `reset`).
- `vpx_engine_shaders`: `status`, `reload` (`file`).
- `vpx_engine_reload`: one `part` or `material` from the pack folder; `mesh:true` also reloads a primitive's mesh, not undoable.
- `vpx_engine_quit`: closes the player. Unsaved edits are lost.

Worth knowing:

- Field names are the keys of the pack files (`parts/*.json`, `materials/*.json`), read through the same serializers. `vpx_table action:"part"` shows them. An unknown key or a wrong type is refused before anything changes.
- Nothing here saves. Edits are on the live table until you save in the editor.
- Undo needs an edit session (`-LiveEdit`). In play or inspection, edits go to the live copy and the reply says `"undo":false`.
- A part edit runs the code undo runs: `Load` over the saved state, then a rebuild of the renderable and colliders. Pane setters with side effects beyond the part's data do not run.
- In play, changing a part with colliders pauses the game (the physics engine does that); the reply's `playing` says so, `vpx_engine_run action:"resume"` goes on.
- In play, `vpx_engine_camera` stays until the layout changes (a resize, for example): set it again then.
- After an edit the table's shadows rebuild over some frames with wrong light on the way. In an edit session `vpx_view` waits for that (at most 5 s, `settled` in the reply), which also makes it slower than a bare capture.
- A capture stalls the frame it reads back by a few hundred ms. Do not capture while measuring frame time.
- In an edit session the capture shows the view gizmo and editor overlays that sit on the 3D view.
- Any local process with the token can drive the player and write captures to `.png` or `.jpg` paths. Keep the token out of shared logs.

### Example: change a material, capture before and after

```bash
export VPX_MCP_PORT=9077 VPX_MCP_TOKEN=<secret>
tools/dev/vpxmcp.py vpx_engine_camera eye='[476,1900,520]' target='[560,950,60]' fov=45
tools/dev/vpxmcp.py vpx_view path=/tmp/before.png
tools/dev/vpxmcp.py vpx_material action=modify name=PF_Playfield patch='{"roughness":0.03,"metallic":0.9}'   # a preview
tools/dev/vpxmcp.py vpx_material action=modify name=PF_Playfield patch='{"roughness":0.03,"metallic":0.9}' confirm=true
tools/dev/vpxmcp.py vpx_view path=/tmp/after.png
tools/dev/vpxmcp.py vpx_history action=undo
```

On the rig above: 4.1 s for all of it with an undo and a third capture, same process throughout. A restart alone takes about 8 s to the first capture. Before and after differ on 10 to 12% of the pixels, before and undone on 0.4 to 2%. With no edit at all, the table's own light animation moves up to 3% between two captures.
