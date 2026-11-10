<!-- @file    README.md -->
<!-- @brief   Existing-asset sample for PCG preview, generation and world-cell baking. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-07 -->
# Forest clearing

`ForestClearing.pcg` uses the existing spruce, birch and alpine rock models and materials. The 128 × 128 m area is split into four 64 m cells. The fixed seed is 20261107, with a 512-instance / 1024-candidate budget and a small central clearing.

1. Open **World / PCG** and copy this profile into the project's `Assets` if the sample is coming from shared Engine assets.
2. Use terrain covering world XZ `[0,128) × [0,128)`, or set both world bounds on the PCG volume. The referenced natural assets must be present in the project's or shared Engine asset index.
3. Create the PCG volume, then Preview and Generate. Set the partition grid to the same 64 m size, or supply that partition when generating.
4. Bake cells and build HLOD to test near and far representations. Spruce, birch and rock opt into HLOD; mesh grass rules should normally set `includeHlod = false`.

The empty biome labels work without biome volumes. Set a rule's `biome` and add matching WorldBiomeVolume components to limit it by authored biome. Water clearance uses authored shorelines, and manual roots are protected within the rule's `protectedDistance`. Editing a generated owner requires Detach before its next regeneration.

## Coastal biomes

`CoastalBiomes.pcg` uses the same generation controls with a 128 m grid. Set the volume bounds over a coast with still water near world Y=0. Preview reports water, slope and cell-budget rejections before you apply the result.

| Rule | World elevation | Slope | Distance from shore |
|---|---:|---:|---:|
| Shore rocks | 0.15–12 m | 0–42° | 0.6–18 m |
| Sheltered birch | 2–48 m | 0–26° | 8–70 m |
| Upland spruce | 7–180 m | 0–34° | ≥14 m |
| Exposed slope rocks | 3–400 m | 30–68° | ≥2 m |

The 130-placement budget applies to the combined rules in each 128 m vegetation cell. Use a separate profile when ground cover needs its own budget.
