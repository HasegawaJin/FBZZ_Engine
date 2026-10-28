# Water appearance authoring

WaterComponent owns the water footprint, waves, flow displacement, ripples, patch LOD and CPU surface query. A project `.mat` chooses its own HLSL shader through the existing `shader` reference. Appearance is open project code, not a built-in list of styles.

`Assets/Shaders/Water/WaterSurface.hlsli` exposes the shared `WaterVSInput`, `WaterPSInput`, `EvaluateWaterVertex`, `VSMain`, wave/detail helpers and engine view/depth resources. Include it once, then declare your `MaterialConstants : register(CB_MATERIAL)` and implement `float4 PSMain(WaterPSInput p) : SV_Target0`. `WaterRealisticExample.hlsl` reuses `WaterStandardAppearance.hlsli`; `WaterToonExample.hlsl` owns its pixel shading and demonstrates depth/light bands, tint colors, highlights, foam and a texture. These are editable examples, not exclusive choices.

The existing `.mat` `[params]` table stores values by reflected name. Float, int, uint, bool, vector, matrix and array values use the existing typed reflection rules. The runtime and material preview rebuild b2 from the shader descriptor; the byte layout is not fixed at the legacy water effect buffer's 48 bytes. The Project Appearance section in both material inspectors edits reflected parameters and supported texture slots. Save the material normally. Shader hot reload reconstructs the layout from the newly loaded descriptor; failed reloads retain the last successful shader. Legacy missing water keys keep their former defaults.

Textures use the engine's existing eight canonical slots, not arbitrary sampler names:

| `.mat` key / Inspector label | HLSL index declaration |
|---|---|
| `albedo` | `texAlbedoIndex` |
| `normal` | `texNormalIndex` |
| `metallic` | `texMetallicIndex` |
| `emissive` | `texEmissiveIndex` |
| `ao` | `texAOIndex` |
| `tex5` | `tex5Index` |
| `tex6` | `tex6Index` |
| `tex7` | `tex7Index` |

Include `Common/MaterialTextures.hlsli`, declare only the needed `FBZZ_MATERIAL_TEX_*` fields and a `uint textureMask` in b2, and sample through `FBZZ_MATERIAL_TEX`. The renderer writes indices and mask; material values must not supply raw GPU indices. Check the mask before sampling an unset slot. All eight slots are available through the existing DX12 bindless path. On a legacy register-sampling path only t0/t1/t2/t7 are supplied for project textures; water reserves t3/t4/t5/t6/t8/t9/t17/t26 and other engine bindings. The example uses `albedo` with a t0 fallback. Appearance does not replace water service resources or introduce another binding system.

Declare this explicit metadata contract only while using the shared unmodified geometry:

```toml
[shader]
version = 1
vertex = 'water_surface_v1'
surface = 'water_v1'
opacity = 'custom'
variants = 'none'
preview = 'water'
```

The declaration enables existing shared indexed/instanced patch and GPU indirect paths without checking shader filenames or substituting the default pixel shader. A mesh-shader render setting currently uses the water indexed fallback in the production WaterRenderPass; arbitrary project mesh-stage routing is not provided. WaterComponent requires both `water_surface_v1` and `water_v1`. Missing or mismatched declarations use the built-in Water shader, shared vertex geometry and legacy effect constants, with a diagnostic once per rejected shader/resource lifetime. Project b2 buffers and appearance texture bindings are not forwarded to this fallback. The saved material reference is preserved so an author can repair its declaration. CPU draw mode does not permit a separate arbitrary vertex simulation.

Keep b0 (camera), b1 (WaterCB), lighting/shadows and other service bindings unchanged. Pixel code should preserve `WaterSurfaceExcluded`, intended footprint clipping and edge alpha/clip behavior; otherwise visible coverage can differ from selection or water-medium coverage. The Toon example preserves those contracts but deliberately implements its own surface lighting rather than the default optical/SSR model. Underwater volume rendering remains the engine service configured by the material.

Arbitrary vertex displacement is outside this shared geometry contract. Metadata is the author's explicit promise to include the unmodified shared vertex implementation, not proof that arbitrary HLSL follows it. A shader that declares the contract but changes `VSMain` or shared geometry violates the contract; the engine cannot infer or automatically synchronize unknown vertex code with CPU physics. A general project-defined vertex/sampling provider is not supplied. Use WaterWavesAsset and the WaterComponent wave/flow parameters for supported shared surface changes: rendering, CPU surface queries, buoyancy, selection and water-medium integration consume those same engine services. A future custom simulation requires an explicit matching CPU sampler and conservative bounds before it can participate safely.

To try an example, assign `WaterRealisticExample.mat` or `WaterToonExample.mat` to a WaterComponent, change Project Appearance values, save, reload and compare. Existing Ocean assignments are left intact.

## Unsupported-material preview and repair

The Water Inspector reports whether the shared geometry contract is active or fallback is in use. A shader declaring `preview = 'water'` but lacking the required geometry/surface pair previews with the same built-in shader and descriptor as the runtime fallback. A shader without a known preview declaration remains an unsupported preview because a standalone material has no WaterComponent context to infer its geometry. Save/reload retains the authored shader and parameters; fallback does not rewrite the asset to a built-in style.

To author supported deformation, duplicate a WaterWavesAsset or edit the WaterComponent wave/flow settings, and keep WaterSurface.hlsli unchanged in the project appearance. The existing Realistic and Toon examples demonstrate different pixel treatments over that shared surface. For a shared-surface appearance example, open `Assets/Samples/SurfaceExamples/SurfaceExamples.scene`; its project-authored CustomWater material keeps the shared vertex implementation unchanged.
