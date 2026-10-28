<!-- @file    fiber-project-appearance.md -->
<!-- @brief   Project-authored Grass/Fur appearance families on the shared Fiber geometry path. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-21 -->

# Project-authored Grass and Fur

A Fiber component still owns placement, shell/blade quality, distance LOD and the material reference. A project material can now select an appearance family through its existing `shader` field. No extra material format is used.

The supplied `Assets/Materials/Examples/Fiber/MeadowBlade.mat` and `StripeFur.mat` are editable examples. Their HLSL files live under `Assets/Shaders/Examples/Fiber/`. Duplicate a material and its shader family, then edit the project files. The material Inspector exposes the reflected numeric fields and supported texture slots; Save .mat persists them. The ordinary material Preview and Asset Browser thumbnail use the custom appearance. Reload shaders after changing source. Source-defined object macros used by the shared appearance/material include hooks are tracked through nested includes and aliases. Unsupported include expressions conservatively rebuild instead of silently reusing stale bytecode; this is not a full HLSL preprocessor. Ordinary literal include tracking is preserved across conditional guards.

## Shared geometry contract

Each family member's adjacent HLSL `.meta` declares `vertex = 'fiber_blade_v1'`, `fiber_shell_v1`, or `fiber_fin_v1`. The preview field is respectively `fiber_blade`, `fiber_shell`, or `fiber_fin`. These are geometry/ABI capabilities, not appearance choices.

A primary `MyGrass.hlsl` has these sibling files:

- `MyGrassGBuffer.hlsl`
- `MyGrassVelocity.hlsl`
- `MyGrassShadow.hlsl`
- `MyGrassSelection.hlsl`

Shell and Fin families also require the five `MyGrassSkinned*.hlsl` siblings (the primary skinned filename is `MyGrassSkinned.hlsl`). Blade currently supports static surfaces. Each wrapper sets FIBER_FIN, FIBER_PASS and FIBER_SKINNED as the examples do, and includes `Fiber/FiberSurface.hlsli`.

All passes retain the same engine vertex deformation, wind and contact bend, skinning, shell offsets, coverage, LOD dither and material mask. Color and GBuffer can run the project's appearance hooks. Shadows, selection and velocity retain the shared geometry/coverage. Existing page submission and culling remain in use.

The family declaration must match the component mode. A custom Shell family can be used as Hybrid only when it supplies the Fin companion below; it cannot be used as Blade. Missing members, wrong declarations and incompatible material layout produce a diagnostic and use the compatible built-in family. There is no silent switch to a separate slow geometry renderer. Hybrid custom families use the same engine Shell and Fin generation, batching and bounds as built-in Hybrid.

## Hybrid companion family

A Shell primary `MyFur.hlsl` may additionally supply `MyFurFin.hlsl`, `MyFurFinGBuffer.hlsl`, `MyFurFinVelocity.hlsl`, `MyFurFinShadow.hlsl`, `MyFurFinSelection.hlsl` and their `MyFurFinSkinned*.hlsl` equivalents. Fin wrappers declare `fiber_fin_v1` and use `FIBER_FIN 1`. All twenty members share exactly the same reflected material fields, packing and texture-index bindings. The runtime validates that complete layout, not only the engine-owned prefix.

Assign the Shell primary material to a Fiber component and choose Hybrid. `StripeFur.mat` includes a complete companion family and can be used in Shell or Hybrid mode. The same Fin primary may also be assigned to a Fin-only material. No new material format or style selector is introduced. Hybrid shadows retain the existing Shell-only shadow rule. Missing/wrong Fin members or layout mismatches produce an explicit diagnostic and the entire component uses the compatible built-in family, so it never mixes incompatible custom Shell and built-in Fin appearance.

## Typed parameters and textures

The first 128 bytes of MaterialConstants are engine-owned. `Fiber/FiberMaterialFields.hlsli` defines their layout. The `FBZZ_FIBER_MATERIAL_FIELDS` include appends project-defined fields after that prefix. Keep all wrappers on the same field declaration.

Numeric values use the existing `.mat` `[params]` table and HLSL reflection. Float, vector, integer, unsigned integer, boolean, supported arrays and matrices follow the common material binding rules. The examples add accentColor, patternScale, patternStrength, colorBands and enablePattern. The prefix remains sanitized and incorporates the Fiber component's per-instance color/length/density/clumping/mask overrides.

Texture names are the existing eight material slots: albedo, normal, metallic, emissive, ao, tex5, tex6 and tex7, with their corresponding tex*Index HLSL fields. These are a fixed binding vocabulary, not arbitrary named texture bindings. tex5 is reserved for the engine Fiber mask; its R/G/B/A values control density/length/clumping/tip mix, and geometry-mask values must stay within [0,1]. Other declared slots can provide project appearance textures. Missing appearance textures have an invalid descriptor index and must be guarded before sampling; the geometry mask retains its white fallback.

The `FBZZ_FIBER_APPEARANCE` include implements:

```hlsl
float3 FiberAppearanceAlbedo(FiberPixel p, FiberStrand strand, float3 engineColor);
float3 FiberAppearanceLighting(FiberPixel p, FiberStrand strand, float3 engineLighting);
```

The first function supplies GBuffer albedo. The second supplies final Forward/Deferred Fiber lighting and can call the first to keep both representations consistent, as the examples do. FiberPixel exposes world position, root, normal, UV, normalized height, local flow and the engine coverage data. Existing light/shadow helpers are available. This hook deliberately does not add a new node editor or shader graph.

## Deformation limits and compatibility

The renderer expands bounds for the engine-managed maximum length and bend, including per-instance settings. The appearance hook runs after vertex deformation and cannot add a vertex offset through this API. Do not add a separate vertex displacement, alpha clip or different wind in a family wrapper and still declare the shared contract. Such code would require matching camera/shadow/velocity implementations, conservative bounds and the necessary CPU queries; arbitrary deformation cannot be inferred from shader source.

Legacy Fiber materials keep their built-in family and defaults. The shared material prefix, geometry buffers and draw path are unchanged for those materials. Custom appearances add their own shader/material costs; no performance equivalence is claimed without measurement.

Focused regressions cover typed material save/reload, geometry-mode matching, all static/skinned family shader compilations, custom preview rendering and changed color with unchanged coverage. Performance measurements belong to the separate Fiber benchmark; a CPU placement count is not a rendering-performance result.

