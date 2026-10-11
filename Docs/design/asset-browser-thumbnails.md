# AssetBrowser thumbnails and original type icons

AssetBrowser keeps content previews for textures, texture descriptors, sprites, fonts, materials, FBX models, meshes, mesh-bearing prefabs, VFX representative textures, terrain materials, curves and gradients. A successful model or material render takes precedence over a type icon.

## Original bitmap resources

Six original images were created independently with the built-in image generator at the user's request. No reference images, brand logos or third-party icon files were supplied. The common treatment uses ivory, teal and deep navy, transparent backgrounds, generous margins and thick shapes. Category silhouettes are a folded code page, a scene frame, stacked prefab blocks, a speaker, a two-pose animation filmstrip with a play badge and a shader node graph feeding a shaded sphere.

| Category | Extensions | Runtime sizes |
| --- | --- | --- |
| Script | `.cs`, `.py`, `.lua`, `.cpp`, `.hpp`, `.c`, `.h`, `.cc`, `.cxx` | 16, 24, 32, 64, 128, 256 px |
| Scene | `.scene` | same |
| Prefab | `.prefab` fallback; content preview wins | same |
| Audio | `.wav`, `.mp3`, `.ogg`, `.flac`, `.synth` | same |
| Animation | `.anim`, `.animcontroller`, `.animctrl`, `.mask`, `.sequence` | same |
| Shader | `.hlsl`, `.hlsli` | same |

Runtime resources live in `Assets/Icons/AssetBrowser/`. The original RGBA PNGs live in `Docs/media/asset-browser-icons/Source/`. `Assets/Icons/AssetBrowser/Provenance.json` records exact generation prompts, processing and SHA-256 hashes. Raster preparation preserves alpha, fits the mark proportionally within 80% of the canvas and resamples to each runtime size. It does not introduce a vector substitute.

The grid, tree leaves and list rows use these category images. Image selection accounts for ImGui framebuffer scale. Missing resources fall back to existing type labels. Lookup checks SDK `share/fbzz/Assets/Icons/AssetBrowser`, engine-root `Assets/Icons/AssetBrowser`, and the source repository. Existing SDK packaging installs the engine `Assets/` tree; this change does not update an installed SDK.

`Docs/media/asset-browser-icons/SizeProof.png` is a resource contact sheet, inspected at 16/24/32/64 px. The 128/256 columns are fitted into the contact sheet for comparison. This is not an Editor screenshot or evidence of native UI rendering.

## Work and ownership bounds

Raw image previews run one CPU decode job per panel, with a nonblocking completion poll. Renderer-thread code creates the small GPU texture only after the result is accepted. PNG, JPEG, BMP, TGA, DDS and ICO use a 32 MiB encoded-file limit and a 16 Mi-pixel decoded-image limit. Output has a longest edge of 256 px. The original dimensions are retained for aspect ratio and sprite UV calculations. DDS arrays, cubemaps and volumes are explicitly unavailable. ICO frame offsets and dimensions are checked before decoding.

Font preview rasterization runs in the same worker, with a 32 MiB file guard and 128 px output, reusing the existing font preview implementation. It has not been fuzz-tested for malformed font tables. Material, prefab, VFX, terrain, curve and gradient text previews have a 1 MiB foreground parse guard; sprite metadata has the same guard.

Model, prefab model and texture-descriptor requests hold `AssetLease` values from the existing `AssetStreamer`. Drawing uses `TryGet` and does not call synchronous completion for these requests. Decode/upload/resident budgets remain the streaming system's responsibility. Cold 3D thumbnail work is limited to one visible entry per panel per frame; already rendered entries remain available while other requests wait.

The panel tracks at most 192 preview path keys using recent access. Eviction releases owned raw-image textures, render targets, material preview GPU data and streaming demand leases. Type icon textures have a separate fixed maximum of 36 slots. This is a count budget for resources owned by the panel, not a total GPU-byte budget for all referenced assets. Shader-correct material previews still use shared material renderer resources and their existing texture lifetime policy; those shared resources are not covered by the 192-key limit.

## State and update contract

Content preview placeholders distinguish loading (clock), failed (exclamation mark) and unavailable/unsupported (slash), with explanatory tooltips. Shape carries the distinction independently of color. Type-only icons do not imply a content-preview failure.

Raw image jobs carry a monotonically increasing ticket plus source modification time. An evicted, invalidated, changed, renamed, deleted or reimported entry cannot publish an obsolete decode into a new cache entry. Renderer-resource instance/reset versions discard handles from a retired resource pool. File modification times, explicit refresh, live material revisions and streaming publication revisions trigger redraw. Cache invalidation also re-evaluates dependent prefab, VFX and terrain previews. Prefab/terrain reset paths release old render targets before clearing state.

Failed requests retain bounded retries; explicit refresh allows retry again. Large/unsupported content is a terminal placeholder until content changes or refresh. Directory eviction covers model-asset and texture-descriptor caches in addition to the existing preview categories. RevealExplorer behavior and its existing tests were not changed.

## Verification evidence and remaining checks

- Resource verification: 36 runtime PNGs, exact requested square dimensions, RGBA alpha including transparent pixels, provenance SHA-256 matches, inspected contact sheet.
- Static verification: repository AgentLint passed on nine touched C++/CMake files, zero errors and warnings; scoped whitespace check passed.
- CPU regression tests added and registered: six category mappings preserve real-preview types, RGBA downsampling preserves source dimensions/aspect/alpha, missing/corrupt/unsupported/oversize files produce distinct states, huge TGA headers are rejected before pixel allocation.
- Shared Editor compilation initially exposed a missing `AssetStreaming.hpp` include in the panel header. The defining header was added and the Profiler build owner notified. The subsequent shared `build-20261012-010600-53572.log` shows AssetBrowser source compilation and `FBZZEditor.lib` generation without that error. `FBZZTestsEditorAuto.exe` was subsequently generated in the same log, confirming compilation/linking of the four new tests. Their execution was pending when this note was written.
- Editor/GPU validation has not been run by this owner. No SDK install, Editor process launch, user project asset changes, commit or push was performed.

After the build/GPU owners allocate the appropriate slot, verify grid/tree/list icons on dark and light backgrounds at 100/150/200% DPI; texture/material/FBX/prefab real previews; unsupported DDS and large files; corrupt/truncated files; refresh after a failed import; source updates while decoding; metadata updates; rename/delete/reimport during decoding; repeated navigation and more than 192 entries; device reset; RevealExplorer regression. Check UI responsiveness, cache/GPU resource counts and that old imagery is never attached to a newly imported or renamed entry. These scenarios are planned validation, not completed tests.

## Icon revision 2

The user requested clearer Animation and Shader imagery. Only these two categories were regenerated with the built-in image generator and replaced at every runtime resolution. Script, Scene, Prefab and Audio source/runtime bytes remain unchanged. Animation now depicts two poses in a short filmstrip and a prominent play mark; Shader depicts two connected nodes feeding a shaded sphere. No display code changed for this revision.

`Docs/media/asset-browser-icons/Revision2Comparison.png` compares the original and revised designs. `Archive/Revision1/` retains the two original source images, twelve runtime PNGs, complete original provenance and old size proof. The `previous_versions` records in current provenance point to those archived paths and retain the original Library IDs and hashes. Restoring the old pair means restoring the archived source/runtime files plus the corresponding provenance and proof; the archive is data, not an automatic restore command.

The revised original PNGs are also saved separately in Library so the old and new designs remain available for comparison. Verification covered all runtime dimensions, transparent alpha, hashes, unchanged bytes for the other four categories, and a resource proof at native 16/24/32/64 px. This bitmap revision did not start a build or Editor process and does not claim native UI/DPI verification.
