# Fork notes

Changes and findings in this fork on top of [hilals/nhl24pc](https://github.com/hilals/nhl24pc).

## Fixed

### GPU "device lost" crash when entering a mode or mid-game

Symptom: the game crashed with `Device lost during waiting for a frame` when loading
Ones Now / Be a Pro, and occasionally in the middle of a game. Whether it happened depended
on the state of the shader cache (`user/cache/CUSA37933`).

Cause: a guest shader can be compiled into several permutations. The permutations do not always
have the same resource layout. Example: a vertex shader used by an indirect draw takes its base
vertex from the `BaseVertex` built-in, while the same shader used by a direct draw reads it from
user data. The direct-draw permutation therefore needs an extra `srt_flatbuf` storage buffer, and
every binding after it shifts by one. The pipeline cache always handed out the resource info of
the first permutation, so pipelines were built and bound with the wrong layout. The GPU then read
samplers as images and so on, and faulted. With the disk cache, the "first" permutation was
whichever one loaded first, which is why the crash came and went with the cache.

This bug is also present in upstream shadPS4.

Fix: each permutation keeps its own `Shader::Info`. Pipelines use the info of the permutation they
were built with. The disk cache stores it per permutation, and `ShaderMetaVersion` was bumped, so
old caches are rebuilt on the first run. Expect a slower first load and some stutter until the
cache fills again.

Files: `vk_pipeline_cache.cpp/.h`, `vk_pipeline_serialization.cpp/.h`.

Update (2026-11-24): hilals/nhl24pc fixed the same bug independently (per-permutation `own_info`,
plus extra checks that skip cached pipelines whose stages don't line up). When syncing with
hilals, their implementation replaced the one from this fork.

### Crash diagnostic layer ignored its settings

The `output_path` and `sync_after_commands` layer settings were sent under the wrong layer name,
so the LunarG crash diagnostic layer ignored them. Fixed in `vk_platform.cpp`.

## Configuration notes

- `General.redzone_patches: true` is needed to avoid an access violation (`eboot+0x662f83f`)
  during match load. The launcher has a toggle for it.
- Leave `vkcrash_diagnostic_enabled`, `vkguest_markers` and `vkhost_markers` off for normal play.
  They cost performance and are only needed when investigating a GPU crash.

## Investigating GPU crashes

1. Install the Vulkan SDK (it provides the crash diagnostic layer).
2. Set `vkcrash_diagnostic_enabled`, `vkguest_markers` and `vkhost_markers` to `true`.
3. After a crash, read `cdl_dump.yaml` in the newest folder under `%USERPROFILE%\cdl`. It shows
   the in-flight draw, its pipeline (`vs_0x...`, `fs_0x...`), its bound resources and the fault
   address.
4. More detailed dumps: create a `vk_layer_settings.txt` file and point `VK_LAYER_SETTINGS_PATH`
   to its folder before starting the launcher. The `CDL_*` environment variables are not read.
   ```
   lunarg_crash_diagnostic.dump_queue_submits = all
   lunarg_crash_diagnostic.dump_command_buffers = all
   lunarg_crash_diagnostic.dump_commands = all
   lunarg_crash_diagnostic.dump_shaders = on_crash
   ```
5. Shaders: `user/cache/CUSA37933/<hash>_<perm>.spv` can be disassembled with `spirv-dis`.

## Known open issues

- White flashing textures during replays and right before gameplay starts.
- Vulkan validation reports, not yet investigated:
  - indirect-draw vertex shaders bind 2D/3D image views of the wrong type (VUID 07752);
  - a descriptor type mismatch at pipeline creation (07990), possibly the permutation bug above;
  - depth-compare sampling of R16 images (06479);
  - an image created with one mip level too many (00958);
  - a missing 16-bit storage feature;
  - a tessellation interface mismatch (08743).
