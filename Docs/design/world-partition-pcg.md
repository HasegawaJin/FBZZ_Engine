<!-- @file    world-partition-pcg.md -->
<!-- @brief   Shared cells, editor generation, streamed publication and cell HLOD ownership. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-07 -->
# World cells, PCG and HLOD

## Implementation

`Engine/Scene/WorldCell.hpp` is the coordinate contract used by generation, persistence, streaming and editor operations. A controller's configured grid is shared by PCG preview, generation and cell baking. The operation catalog exposes the same services to AI clients.

The pipeline takes an authored region through deterministic generation, persisted cell records, asynchronous loading and an independently baked distant representation. It uses the existing Scene, asset, instancing, scheduler and editor-operation infrastructure.

The Engine implementation is split across `WorldCell`, `WorldPcg`, `WorldPartitionManifest`, `WorldPartition`, `WorldHlod` and `SceneForest`. Editor uses the services in `Tools/WorldAuthoring.hpp`, the registrations in `Op/WorldPartitionOperators.cpp`, and the `WorldAuthoringPanel`.

## Using the World / PCG panel

Open an authored Scene and choose **View → Editors → World / PCG**. The panel is hidden by default. Its buttons invoke the same registered operations as the command palette and AI clients.

1. In **World cells → Grid and loading distances**, choose a manifest such as `Assets/World/Forest.worldcells`, grid origin, cell size and loading distances. Choose **Apply grid settings**. Leave the partition target empty to create or resolve the unique controller, or select a controller and choose **Use selected partition**.
2. In **PCG → Project placement profiles**, choose **New profile** or load an existing `.pcg` from `Assets`. Add or duplicate rules, assign a placement asset or mesh and material parts, and edit the placement conditions. Choose **Save profile**, then **Use saved profile for volume** to populate the volume's profile field.
3. In **Volume settings**, set the profile path, optional bounds and seed, then choose **Apply / create volume**. Existing Terrain supplies the ground samples. Select a saved volume and choose **Use selected volume** to edit it again.
4. Choose **Preview** and inspect accepted/rejected counts and the bounded position samples. Choose **Generate** to apply that current preview through Undo. Editing the profile, terrain, bounds, seed or output invalidates the fingerprint and requires another preview. **Update only these bounds** limits regeneration; **Detach selected generated object** preserves an output as manually owned content.
5. In **World cells**, choose **Bake cells**, normally with **Build HLOD on bake** enabled. By default this persists generated static output. **Include scene content** considers eligible authored static objects, and **Stream terrain tiles** bakes static heightfields. The result reports sources, cells, terrain tiles, instances, HLOD groups and objects kept persistent. Save the authored Scene and dirty assets with the normal editor save workflow.
6. Enter Play to stream the isolated Play Scene from its active main camera, or set the controller's source camera/position. **Streaming** displays the desired/current representation and failure diagnostics. Select a cell in the grid or enter its signed X/Z coordinates, then use **Load near**, **Load HLOD**, **Unload** or **Clear override**. **Retry** retries failed work; **Reset streaming** removes runtime-owned residents. Ordinary Edit mode refuses manifests that require retiring authored source objects.

After saving changed generation, bake again to publish a new near snapshot. **Build HLOD** alone rebuilds from the existing persisted near cells, so unsaved live changes do not silently change the distant representation.

### Terrain edit refresh

After the first **Generate**, committing a Terrain edit updates the affected placements of enabled PCG volumes using their saved profiles. Brush strokes refresh on release. The edit and its placement changes share one Undo/Redo entry. Sites that still qualify keep their candidate IDs and follow the current surface. Sites can appear or disappear as the height, slope, holes, painted layers or shoreline change.

Local rules refresh the changed area and the surrounding samples they depend on. A 128 m placement quota refreshes complete quota cells. Rules with minimum spacing, competition, canopy or neighbor dependencies re-evaluate the affected volume because a placement change can affect other candidates. Hand-placed and detached content stays under its original owner. An output edited by hand is skipped until it is detached or restored.

Candidate sampling uses conservative terrain, water and biome bounds to avoid querying unrelated sources. When bounds overlap, samples retain the original evaluation order and tie handling. Profiles with many exclusion boxes use a bounding volume hierarchy and still include points on box boundaries.

The Terrain brush reports the grid region it changed. Local refresh reuses cached XZ bounds from the current placement snapshots to skip row checks in distant batches and reuses indexed asset references across strokes. Brush, ridge and height stamp Undo keep exact expected Terrain states in shared immutable blocks. Consecutive edits reuse byte-identical blocks and record alternate values for edited samples. Resizing or initializing arrays still captures every sample. The CPU Profiler exposes `WorldPcg::RefreshTerrain`, `WorldPcg::Prepare`, `WorldPcg::EvaluateRules`, and `WorldPcg::ValidateOutputHistory` for measuring commit latency.

Undo and Redo of a terrain edit with one generated placement command validate the owner and exact saved file bytes before applying the combined edit. The editor marks the scene dirty after both changes. The file validation parses the same bytes it inspected when the saved file has changed.

The first layer-paint stroke on a tile prepares the texture pair for rectangular updates on supported renderers. Subsequent dabs and sparse Undo/Redo upload the affected vertex rectangle. A full layer edit, a missed preview revision, or an unsupported renderer uses a complete upload. `Terrain::SplatUpload` measures that transfer, while `TerrainTool::CaptureStroke` and `TerrainTool::CommitStroke` measure editor work.

Sculpt previews on GPU terrain update the affected height-buffer rows, including samples shared by adjoining tiles. The affected patches retain their existing index buffers during height-only edits. The ridge, height stamp and river network tools request the same local updates on apply and Undo/Redo. The first local edit prepares the tile buffer for ordered copies. A hole stroke rebuilds the touched patches' topology and leaves the height buffer available. Patch buffer publication batches the renderer operations with bounded staging space. Grid resizing, a missed preview revision, and renderers without ordered copies use a full height upload. `Terrain::HeightUpload` measures the transfer path.

Terrain Fiber skips signature checks for patches outside a contiguous brush revision. Height checks include the normal sampling halo, and paint checks include vertices shared with adjoining patches. Full edits and patches that missed a revision still verify their signatures.

For local height and hole edits, compatible water depth and shoreline atlases resample the affected texels, using all overlapping terrains to choose the bottom. Once the GPU depth texture supports rectangular updates, the extractor uploads only the rectangle whose texels changed. Changes to the atlas domain or terrain layout, full edits, and missed revisions trigger a full bake.

Placement changes use the usual asset save workflow. A volume that cannot refresh keeps its previous output and reports the reason in the Editor Console. **Bake cells** publishes refreshed output to World Partition.

Custom rules live in the project's `.pcg` files. Multiple volumes can reference the same file and set their own bounds and seed. A rule's key identifies its generated outputs and neighbor references. The form assigns a fresh key on Add and Duplicate, and the rule name is editable. Removing a rule with dependents requires updating their neighbor or canopy settings before saving. The form covers common placement, water, terrain material, clustering, and mesh settings. **Edit TOML** exposes the full native schema and **Edit rules** validates the text before opening the form again.

The profile form keeps a draft until **Save profile** validates and writes it through `pcg.profile.save`. Overwriting the loaded file requires its inspected fingerprint. Changing **Profile file** to a new path saves a separate project definition. **Discard draft** releases unsaved edits so another profile can be loaded.

### Scene visualization

Select a biome volume to see its world-space box, biome, density and priority. **PCG → Scene visualization** can show up to 128 additional biome volumes. PCG uses an XZ footprint drawn at the volume's origin height. **Use selected volume** or **Refresh volume bounds** reads the effective profile bounds and its exclusion boxes. Draft bounds and partial update regions are yellow. The viewport does not read profile files while drawing.

**Preview** shows sampled accepted sites as circles and rejected ground sites as crosses. Filter by rule or rejection reason and hover over a site to see its world position. The rule totals and rejection counts cover the entire preview. The markers are bounded samples. Missing ground contributes to the count only. Scene changes and Undo history invalidate the preview markers. **Generate** checks the preview fingerprint before saving placements.

**Live preview** refreshes the current volume after 250 ms without another edit. Volume bounds, seed, profile rules and the partial update region participate, as do scene edits recorded in Undo. An unsaved profile affects the preview when it matches the effective volume profile. Unsaved volume settings and profile rules produce a draft preview. Save or apply the settings to get a preview that can be generated. Partial update bounds have their own fields.

Automatic previews use the same placement calculation with bounded candidate, contour, habitat and existing-placement work. A region over budget pauses automatic refresh until another edit. Use a smaller update region or **Preview** to run the full calculation explicitly. Invalid input clears the stale markers and displays its validation message.

`pcg.volume.inspect` returns effective bounds and exclusion boxes without running generation. `pcg.preview` accepts `maxRejectedPoints` from 0 to 1024, with zero as the default. The optional `rejectedPoints` samples come from the actual rejection branches, including candidates evicted by the cell quota. Diagnostic sampling preserves placement IDs, transforms and fingerprints.

`pcg.preview` can also take `live=true` or the optional `draftProfileText`, `draftProfilePath`, `draftSeed`, `draftBoundsMin` / `draftBoundsMax`, `draftUseProfileBounds`, `draftProtectManual` and `draftEnabled` parameters. Draft results carry `draft=true` and omit `expected`. A live result on saved inputs retains the normal fingerprint.

AI clients discover category `World` through `editor_op_list`, read with `editor_op_query`, and invoke mutations/actions with `editor_op_invoke`. The corresponding raw bus names are `editor.op.list`, `editor.op.query` and `editor.op.invoke`. Query/invoke payloads carry an operation `id` and its `args` object. For example, preview uses:

```json
{"id":"pcg.preview","args":{"partition":"<partition-guid>","volume":"<volume-guid>","maxPoints":256}}
```

Pass that result's exact `expected` string back to `pcg.generate` with the same partition, volume and optional partial bounds. Discover the profile schema through `pcg.profile.inspect` instead of inventing a profile format. Inspect `worldcell.stream_status` after control operations; requesting a load queues work and is not proof that the cell has finished publishing. Generic `dryRun` validates availability and arguments; `pcg.preview` computes the real plan.

## Shared cell coordinates

`WorldCellGrid` contains a finite world-space origin and a cell size in metres. The default size is 256 m; the core accepts sizes from 1 m through 65,536 m. Higher-level generation and streaming services must also enforce their own work and memory budgets.

`WorldCellId` uses signed 32-bit X/Z coordinates. Ownership is the half-open interval `[origin + id * size, origin + (id + 1) * size)` on each axis. Negative coordinates use mathematical floor. Finite Y positions do not change ownership. Invalid grids, nonfinite positions and unrepresentable cell indices fail without changing the caller's output cell.

`WorldCellFootprint` is a two-dimensional XZ footprint at the grid origin's Y; it is not a content culling volume. Baking must separately retain conservative content bounds, including geometry protruding beyond its owning footprint. `WorldCellDistanceXZ` measures distance to the closest footprint point, rather than to the cell centre, so loading decisions do not depend on the source's altitude.

Manifests sort cells by Z and then X. `WorldCellName` preserves signed coordinates as `x_z`. Generation uses a fixed integer hash of the authored seed, signed cell identity and an independently separated generation stream. Standard-library random distributions and `std::hash` are not persistence contracts.

## Ownership and persistence

| Data | Authoritative owner | Required boundary |
|---|---|---|
| Hand-authored objects and overrides | Existing Scene and assets | Generation cannot erase unrelated rows or reconstruct the entire Scene |
| Generation rules and seed | Authored PCG configuration | Preview and apply share the same validated inputs |
| Generated content | Dedicated generation owner and cell | Stable ownership identifies what may be replaced during regeneration |
| Cell manifest | Versioned persisted asset | Records grid, content bounds, near/far references and source revision |
| Cell placement data | Existing `.meshinstances` assets | Keep existing placement validation, GUID references and atomic file writes |
| Runtime residents | Streamer-owned cell roots and leases | Release only content owned by that cell |
| Distant proxy | Separate baked cell asset | Cannot masquerade as the near data's individual LOD selection |

Generated records and manually authored records remain separate. A changed terrain region invalidates the affected generation cells; regeneration preserves unrelated cells, manual content and protected regions. A bake validates all inputs before publication and reports persisted outputs explicitly. Editor Undo and disk persistence are distinct operations: undoing a Scene change must not be described as restoring overwritten files.

Cells containing hierarchies must form closed forests. A parent from another cell cannot be silently adopted: recursive unload could otherwise destroy another cell's content. Canonical GUID/object identities remain durable; resident EntityIDs are temporary caches and do not replace unresolved cross-cell references.

`ValidateSceneForestDocument` performs worker-safe structural preflight without a Scene, Script factory, asset store or GPU access. It rejects missing/duplicate saved identities, external or cyclic parents, ambiguous legacy parent names, unsupported component keys or value shapes, reserved runtime object names, nested partition controllers, nonfinite values and malformed transform arrays. TerrainGrid dimensions must be positive and contain at most 1,048,576 slots. Pure preflight does not resolve assets or run the complete component codecs.

`AppendSceneForest` is main-thread-only. It additionally rejects saved-identity collisions with the destination and insufficient entity capacity, then passively prepares the full custom Scene component data before publishing any objects. It preserves GUIDs, local transforms, prefab metadata, custom Terrain geometry and TerrainGrid references. All owners and references exist, with flushed world transforms, before each new Script is constructed and receives its normal passive-copy `OnAfterDeserialize` callback. Preparation does not construct scripts or invoke callbacks. Existing passive-copy lifecycle rules do not add an `OnValidate` callback.

Passive Terrain data is strict: missing referenced assets or invalid inline geometry fail instead of publishing flat fallback geometry. When inline Terrain data is present it is an authoritative snapshot, even when the external terrain path is retained as metadata. This lets workers embed the exact Terrain file snapshot they read and avoids reopening it during publication. Data-only publication with null renderer resources leaves model/mesh references unresolved so the runtime can bind its retained asset leases.

## Editor generation

The initial PCG path is a reusable rule-based generator over a defined region. It can sample terrain height, slope and occupancy, apply biome/density/spacing rules, filter water or protected areas, and emit existing instanced placements. A visual node editor can use the same services later; it must not become a second implementation of generation.

Use `TrySampleTerrainWorld` or the floor/ceiling surface queries for terrain occupancy. `TerrainComponent::GetHeightAt` clamps outside the terrain and ignores holes, so it cannot decide whether a candidate belongs to a surface. Water-surface queries have mutable caches and must not be evaluated concurrently against the same live component. Copy immutable numeric inputs before worker processing.

Preview is read-only and bounded. It reports the full candidate/result counts separately from any truncated sample list. Apply recomputes or validates the same generation plan, including Scene generation, authored inputs, terrain revisions, current output ownership and seed. A stale preview must fail before changing content. One checked editor command owns each apply so Scene/draft changes participate in Undo/Redo.

`maxPlacementsPerCell` caps the combined output of a profile's rules in each 128 m vegetation cell, anchored to the partition origin. Zero preserves the uncapped profile. The generator applies biome, terrain, water and exclusion rules first, then keeps the candidates with the lowest seed-derived ranks in each cell. The same candidate keeps its ID and transform across cap changes. Streaming still groups retained output on its own configured grid. Preview reports `rejected.cellBudget` separately from density and protected-area rejections. Independent profiles can give trees and ground cover separate placement quotas. Existing authored objects remain a separate owner and can be reserved when setting those quotas.

### Vegetation placement rules

The following optional `.pcg` fields work with the existing profile editor, `pcg.preview` and `pcg.generate`. They apply to mesh placements such as trees, shrubs and mesh grass. Terrain Fiber uses its saved terrain layer mask. Existing profiles retain their placement and fingerprints when the optional fields are omitted.

### Habitat and shore bands

| Optional fields | Rule |
|---|---|
| `useWaterLevel`, `minWaterOffset`, `maxWaterOffset` | Accept the signed difference `groundY - waterY` in metres. Negative values are within the authored water column. On dry ground outside the water footprint, use the nearest shoreline within 128 m. Existing land rules keep their dry-ground check. |
| `minNearRelief`, `maxNearRelief`, `minFarRelief`, `maxFarRelief` | Filter the signed height relative to the mean of the surrounding ground at 16 m and 64 m. The authoring sampler uses eight evenly spaced directions per radius and requires at least four observations at each requested radius. Negative values favor valleys and positive values favor ridges. See [Esri Topographic Position Index](https://doc.esri.com/en/arcgis-pro/latest/tool-reference/spatial-analyst/how-topographic-position-index-works.html). |
| `maxCliffDistance`, `cliffFarScale` | Place below a nearby steep source and taper density and scale with distance. Authoring probes eight directions at increasing radii, accepts an uphill-facing source slope of at least 50°, and reports the nearest sampled distance. The configured distance is in world metres (0 disables, max 256); the default far scale multiplier is 0.45. |
| Profile `pathMaterial`, rule `minPathDistance`, `maxPathDistance`, `pathFade` | Use the stored 0.35 weight contour of the designated path material, reject sites on it, and keep grass or shrubs in different bands with a smooth density transition. Distances are world XZ metres. |

These are authoring filters over loaded terrain and water. Missing samples reject an enabled habitat test. Cliffs are sampled approximations. The optional neighborhood probes share an editor work budget. The preview reports separate water-level, relief, cliff and path rejection totals.

| Fields | Placement rule |
|---|---|
| `minSpacing` | Minimum horizontal distance in metres between candidates of this rule. Zero disables it. A deterministic lattice traversal reserves accepted sites before the cell cap. Cropping a region can change which site wins a spacing conflict. |
| `patchGroup`, `minPatch`, `maxPatch` | Keep candidates inside a smooth world-anchored noise band. Use the same nonempty group and `noiseScale` to share a mask across species. The lower bound is inclusive and the upper bound is exclusive except at 1. A forest can use `minPatch = 0.52` and a meadow `maxPatch = 0.52`. |
| `neighborRule`, `minNeighborDistance`, `maxNeighborDistance` | Filter by the nearest emitted placement of another rule in the same profile. Distances are horizontal world metres. The lower bound is inclusive, the upper bound is inclusive, and a zero upper bound means unlimited. Use this for understory inside a radius, a shrub ring along the forest edge, or open grass outside tree clusters. A missing source satisfies an exclusion radius and fails a finite inclusion radius. |
| `waterFade` | Smooth density over this many world metres from the configured shoreline distance limits. Zero preserves a sharp boundary. An unbounded upper distance has no fade at that end. |
| `heightFade`, `slopeFade` | Taper placement density inside the configured height range (world metres) and slope range (degrees). Zero keeps a sharp boundary. The slope taper skips the physical limits at 0° and 90°. |
| `aspectHeading`, `aspectHalfAngle`, `aspectMinSlope`, `aspectFade` | Prefer slopes facing a world XZ sector. +Z is 0°, +X is 90°, and the half-angle of 180° disables the rule. The slope direction is downhill. Slopes below `aspectMinSlope` (default 3°) are unaffected. `aspectFade` feathers density inside the sector boundary in degrees. |
| `includeMaterial`, `minMaterialWeight` | Place only where the highest authored terrain surface has enough weight painted for this material. The default threshold is 0.25 and the matched weight also scales density. |
| `excludeMaterial`, `excludeMaterialWeight` | Reject where the named material reaches the threshold (default 0.35). Use a saved path or rock layer to keep it clear of grass and trees. |
| `competitionGroup`, `competitionRadius`, `competitionPriority` | Within a named group, reject candidates whose horizontal exclusion discs overlap. Radii add and use world metres. Higher priority wins within each dependency stage. Equal priority uses rule key, then lattice traversal order. |
| `canopySourceGroup`, `canopyGroup`, `canopyRadius`, `minCanopyCount`, `maxCanopyCount` | Tag multiple source rules with `canopySourceGroup` and count their emitted roots within the consumer rule's horizontal radius. Inclusive counts select clearings, forest edges or dense understory. The maximum defaults to −1 for unlimited; `maxCanopyCount = 0` selects open ground. |
| `scaleCompetitionRadius` | Multiply the competition radius by each candidate's chosen placement scale. The resulting radius must fit within the 1000 m maximum. Default `false` preserves fixed world-metre radii. |

Neighbor links must be acyclic and resolve to rule keys. Rules in the same dependency stage compete by the existing placement rank. Earlier stages consume their cell quota first, so a dependent placement can refer only to roots that survive the cap. Leave budget for the later vegetation layer. The distance rule uses authored ground sites before visual embedding. It approximates canopy coverage through the specified radius. It does not read light or shadows.

Positive spacing and neighbor distances are 0.25–1000 m. Water fade is 0–1000 m. Spacing reserves at most 1,048,576 sites per rule. Proximity only considers roots emitted in the current generation bounds, so preview the whole interacting woodland region when setting its margins.

Material filters use the bilinearly interpolated saved splat weights with asset identity resolved per terrain. The layer index may differ across tiles. Duplicate layers for the same material contribute their combined weight. Missing layers have weight zero. Competition runs after terrain, material and same-rule spacing checks, before the cell cap. A provisional winner can reserve space even if the cap later removes it. Dependencies still use the surviving emitted roots. Competition radii are 0.25–1000 m when a group is present. Generation fails before publication if the competition budget of 1,048,576 reserved sites or 16 million distance tests is exhausted. Preview reports `rejected.material` and `rejected.competition`. See [Epic Open World Tools, Landscape Layers / Collision Radius / Overlap Priority](https://dev.epicgames.com/documentation/unreal-engine/open-world-tools-property-reference-in-unreal-engine).

Regenerate the full region for competing rules. The Editor rejects a partial replacement when competing placements would be retained outside the generation bounds.

Canopy dependencies resolve every source in the named group and reject missing groups or dependency cycles. Counts only include roots emitted after the cell cap in the current region. Use `canopySourceGroup = "Canopy"` for spruce and birch, then use `canopyGroup = "Canopy"` with `canopyRadius = 12.0` for understory. A minimum of 3 selects denser forest, a range of 1–2 selects a forest edge, and a maximum of 0 selects openings. Positive radii range from 0.25 to 1000 m and integer counts from 0 to 1024. Preview reports `rejected.canopy`. Canopy distance checks have a 16 million test budget and fail without publishing partial results. See [Epic PCG Node Reference, Point Neighborhood / Apply Scale to Bounds / Self Pruning](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-framework-node-reference-in-unreal-engine).

Canopy sources and their consumers must be regenerated together. The Editor rejects a partial replacement if this would leave previously generated participants outside the generation bounds.

For a patch of exposed highland grass, set `minHeight = 80.0`, `maxHeight = 240.0`, `heightFade = 20.0`, `maxSlope = 32.0`, and `slopeFade = 6.0`. For vegetation that grows on the +Z-facing side of a hill, add `aspectHeading = 0.0`, `aspectHalfAngle = 65.0`, and `aspectFade = 15.0`. Change the heading to 180° for the opposite face. These density factors multiply the existing biome, material, clustering and water factors. Preview reports excluded directions as `rejected.aspect` and taper losses as `rejected.density`. See [Epic PCG Node Reference, Normal To Density / Curve Remap Density](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-framework-node-reference-in-unreal-engine).

Preview exposes `rejected.patch`, `rejected.neighbor`, and `rejected.spacing`. For example, set a spruce rule to `minSpacing = 6.0`, then add a mesh grass rule with `neighborRule = "spruce"`, `minNeighborDistance = 1.0`, and `maxNeighborDistance = 8.0`. A separate meadow rule can use the same `patchGroup` as the spruce with the complementary band. Use the same `noiseScale` for both sides of that band.

## Asynchronous streaming

| Phase | Allowed work | Forbidden work |
|---|---|---|
| Main-thread request | Validate manifest/configuration, choose cells and request dependencies | Blocking waits for worker completion |
| Worker read/prepare | Read resolved files and produce immutable CPU results | Scene mutation, AssetStore mutation, GPU resources or live editor queries |
| Main-thread publication | Validate attempt/epoch, attach content, update resident state and retain leases | Publishing stale results from another project or generation |
| Main-thread retirement | Remove owned roots, retire bindings and release leases | Destroying unrelated or manually edited content |

Each request carries a cell generation/attempt identity. Cancellation, project changes, reloads and replaced manifests invalidate old results. Frame publication has a bounded commit budget. Distance hysteresis separates loading from unloading to avoid repeated work around a boundary. A failed replacement keeps the previously published representation available and exposes diagnostics for inspection and retry.

Asset I/O, model/texture residency and Scene-object residency are separate responsibilities. Reuse `AssetStreamer`, leases and the existing frame pump where their contracts apply. Do not claim that reading a cell document asynchronously makes every referenced asset load asynchronous; baked and source-model paths have different existing decode capabilities.

## Terrain tile baking

Terrain baking copies active standalone heightfields from the authored Scene into content-addressed `.terrain` assets. It preserves the authored grid and its `.terrain` assets for editing. Tile cuts follow the source chunk lattice and copy both sides of each shared vertex edge. Height samples, hole cells, splat layers, terrain material settings, terrain colliders and unpainted Blade Fiber settings are carried into the runtime representation. A source with added cave triangles, custom Fiber paint, extra components, hierarchy or references from other objects stays persistent. The edit grid's own slot links identify authored originals and do not pin them. The initial tile planner accepts positive axis-aligned transforms.

The near and far cell payloads refer to the same heightfield tiles. The renderer selects terrain LOD for the current view, and tiles unload beyond the configured residency distance. With **Render-only HLOD terrain** (`omitTerrainCollisionInHlod`), the distant payload omits `TerrainColliderComponent`. Enable it for partitions where every physics actor stays in near cells. The near payload records the option so **Build HLOD** can reproduce it. With HLOD baking disabled, the near distance controls residency. Baked tiles are loaded in the isolated Play Scene. Startup retirement and initial cell publication remain separate steps. Edit the originals and rebake to publish changed height or paint.

## Cell HLOD

An authored cluster mesh can represent a forest stand, cliff group or building block. Put it on a standalone `MeshRenderer` with an explicit compatible material, enable `WorldHlodSource.includeHlod` and `farOnlyMesh`, and set `includeHlod=false` on the near owners it replaces. The near payload retains the source record with rendering disabled. The HLOD build and rebuild publish the mesh at the authored position. Place each proxy within its owner cell.

**Stream near light probes** (`includeLightProbes`) includes unreferenced, standalone Light Probe Volumes in the near cell containing their center. The near payload owns their runtime resources. The far payload drops those volumes and uses the remaining lighting environment. Choose a near range that covers the probe box from all relevant views.

The first HLOD strategy is an aggregated instancing proxy. A cell's multiple source owners are rebased to the cell origin and grouped by the appropriate mesh/material/render configuration. The lowest-detail available authored mesh LOD supplies the distant geometry. This does not assume a mesh simplifier or material-atlas baker.

The proxy is a separately persisted cell representation with source-revision provenance. Bake must validate transform composition, reject unsupported shear, and avoid colliding placement IDs when merging owners. Proxy draw distance is controlled by the cell representation policy rather than inherited accidentally from a near grass or tree component.

A resident cell publishes exactly one representation: near or HLOD. While a replacement loads, keep the old published representation. Switch the complete representation at a frame boundary so colour, shadow, velocity and selection extraction cannot submit both versions. Bounds include the actual source geometry; a cell-footprint box alone is insufficient for overhanging canopies and rocks.

## Editor and AI operations

All authoring operations enter the existing `OperatorRegistry`. Menus, the command palette and the AI bus project that registry; the domain must not add a separate protocol or bypass normal permissions. Read inspection and generation preview are Queries, checked Scene/draft changes are Mutations with Undo, and disk-only bake/export is an Action with explicit persisted-output reporting.

The registered catalog must expose configuration, affected bounds/cells, seed, budgets, source/output revisions, preview fingerprint, counts, streaming states and diagnostics. Semantic preview is a real Query: the generic bus `dryRun` checks arguments and availability but does not execute generation planning. Authoring during Play is rejected; runtime inspection can remain available through its own read boundary.

Exact operation IDs and structured argument schemas are finalized by their production registrations. AI clients should discover them through `editor_op_list` and use `editor_op_query` / `editor_op_invoke` rather than infer signatures from this document.

The initial registrations define this workflow:

| Operation | Kind | Purpose |
|---|---|---|
| `worldcell.inspect` | Query | Inspect partition settings and authored cells |
| `worldcell.configure` | Mutation | Create/configure the shared grid and streaming settings |
| `pcg.profile.inspect` | Query | Read a saved profile or obtain its schema/example |
| `pcg.profile.save` | Action | Validate and atomically persist a `.pcg` profile |
| `pcg.volume.configure` | Mutation | Attach a profile, bounds, seed and manual-edit policy |
| `pcg.preview` | Query | Compute counts, bounded samples and an `expected` fingerprint |
| `pcg.generate` | Mutation | Apply the same current plan through one checked Undo command |
| `pcg.detach` | Mutation | Preserve generated output as manually owned content |
| `worldcell.bake` | Action | Persist cell payloads and the manifest; optionally build HLOD |
| `hlod.build` | Action | Rebuild the separately persisted distant representation |
| `worldcell.stream_status` | Query | Inspect queued/resident/failed cells without advancing work |
| `worldcell.stream_control` | Action | Advance streaming or control a cell override/retry/reset |

A typical authoring pass configures a partition, saves a profile, configures a PCG volume, previews, passes the returned `expected` fingerprint to generation, and bakes the cells. Profile replacement uses its previous fingerprint for compare-and-swap. Bake/export file actions are not Undoable. During Play, the isolated Scene clone can retire the manifest's authored source identities before publishing streamed residents; ordinary Edit inspection does not retire authoring content. Per-cell loading overrides last until cleared, reconfiguration or reset.

## Current implementation boundaries

- PCG uses a native `.pcg` rule form, a TOML editor and a bounded preview. Editor ground sampling chooses the highest valid authored Terrain surface at each candidate XZ. Water clearance is a bounded shoreline calculation, with requested authoring clearance limited to 10,000 m.
- The baker partitions eligible static leaf objects, instanced placements and opt-in heightfields. Unsupported component combinations, complex per-object material overrides, hierarchies and objects referenced by persistent content stay in the authored Scene. Water, cameras, characters and scripts remain persistent. Use the bake result's `terrainTiles` and `pinned` counts to assess coverage. Streamed tile normals are evaluated from each tile's own height samples. Authoring-grid seam correction and streaming-aware navmesh regeneration are separate work.
- HLOD reuses authored low-detail mesh parts and material groups. It does not create simplified meshes, material atlases or new art assets. Sources excluded by `includeHlod` or their finite draw-distance policy stay excluded when rebuilding from persisted near data. Positive scales and finite, affine TRS without shear are required.
- Bake is bounded to 4,096 cells and 1,048,576 placements per action; HLOD is bounded by its group/placement settings. Runtime separately limits concurrent jobs, publications per frame, resident cells and CPU payload/placement bytes. Renderer and asset streaming budgets remain separate; these limits do not promise a fixed frame time.
- Cell payloads and placement files use immutable content addresses. The manifest is the publication point; rebakes retain previous files. A failed file action can leave unused immutable payloads, and automatic garbage collection of old bake generations is not part of this change. File publication is not reversed by Scene Undo.
- Source retirement requires an isolated Play Scene and a successful all-near structural preflight. Editor streaming can inspect/control source-free manifests; it refuses to retire the working authoring Scene. A loaded controller grid must match the saved manifest, otherwise rebake before streaming. The active runtime supports one enabled partition controller per Scene.

## Verification

Focused verification covers negative and shifted cell ownership; invalid/overflow coordinates without partial output; deterministic generation and stream-domain separation; reproducible regeneration with protected manual content; stale-preview rejection and Undo/Redo; persistence round trips; asynchronous cancellation/project-switch/failed-replacement behaviour; near/proxy exclusivity; and conservative HLOD bounds.

`WorldCellTests.cpp` freezes the integer-hash compatibility vectors and tests signed/half-open coordinate boundaries. `SceneForestTests.cpp` covers structural preflight, all-or-nothing rejection before mutation, passive Script lifecycle, Terrain snapshots and relative assets, TerrainGrid remapping, legacy flow preservation and unload of only the appended roots. These source files are test coverage definitions; execution results must be reported by the build/test run.

`WorldCellBakingTests.cpp` exercises checked partition configuration, the profile/volume/preview/generate/bake path, authoring-scene preservation, persisted source identities, real placement-file streaming through deterministic injected jobs, exclusive near/HLOD switching, owned unload, persisted-near HLOD rebuild, stale fingerprints, budget failures and manifest preservation after invalid metadata/payloads. Its material channel supplies a CPU fixture; cell and placement codecs use their production implementations.

The 2026-11-07 Release build produced the Engine and Editor test binaries and the Editor launcher. An isolated native Editor smoke run passed all 29 steps: AI operations generated 26 spruce, birch and rock placements in one cell, saved the near payload, rebuilt three HLOD groups from disk, then switched near → HLOD → unloaded in Play and restored the edit scene on Stop. Authored, near, HLOD and unloaded renders were captured; the reviewed frames show the same placements with lower-detail crowns in HLOD, and the persistent Terrain after unload. This smoke uses explicit cell overrides; distance selection and cancellation are covered by runtime tests.

The final Release test run passed all 168 selected tests (121 Engine, 46 Editor, 1 Graphics), including existing Terrain serialization, Scene and instancing regression coverage.

## References

- [Epic: World Partition](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine)
- [Epic: PCG generation modes](https://dev.epicgames.com/documentation/unreal-engine/using-pcg-generation-modes-in-unreal-engine)
- [Epic: PCG with World Partition](https://dev.epicgames.com/documentation/unreal-engine/using-pcg-with-world-partition-in-unreal-engine)
- [Asset streaming](asset-streaming.md)
- [Instanced meshes](instanced-mesh.md)
- [AI authoring tools](ai-authoring-tools.md)
