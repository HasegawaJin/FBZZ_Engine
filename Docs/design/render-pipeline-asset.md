<!-- @file    render-pipeline-asset.md -->
<!-- @brief   Ownership, serialization and runtime copies of pipeline quality assets. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-02 -->
# Render Pipeline Asset

## Ownership

The project chooses a built-in `RenderPipelineAsset` DataAsset (`.fzdata`) by
GUID. The asset owns rendering techniques and quality, not a scene's look or
an editor viewport's state. Graphics accepts a plain resolved `RenderSettings`;
it does not load Engine assets.

| Owner | Settings |
| --- | --- |
| RenderPipelineAsset | Requested Raster/Hybrid/Path mode, raster pipeline, shadow quality, Hybrid workload limits and budget targets, clustered lighting, instancing, asynchronous compute, particle budget, froxel grid dimensions, render graph scheduling and pass policy |
| PostProcessProfile / Volume | SSR, TAA, bloom, exposure, fog and other location-dependent effects |
| Scene components | Environment light / IBL sources and lighting |
| Player options | Brightness, render scale and bloom multiplier |
| Editor viewport | View mode, selection, gizmos and diagnostic overlays |
| Renderer runtime | Resources, dispatch receipts, resolved capabilities and histories |

This follows the distinction between a project pipeline asset and scene look
settings, not Unity's implementation or runtime types.
Reference: [Unity: Change or detect the active render pipeline](https://docs.unity3d.com/6000.1/Documentation/Manual/srp-setting-render-pipeline-asset.html).

## Persistence And Resolution

`ProjectSettings.renderPipelineAssetPath` stores the selected reference. TOML
writes a GUID reference when the asset database can resolve it. Inline `[render]`
settings remain the compatibility fallback and are never implicitly deleted or
overwritten by asset resolution in the editor.

1. Load the reference without loading an asset: project settings are loaded
   before asset manager initialization.
2. After initialization, copy the inline settings and apply only asset-owned
   values. Preserve selection, transient requests, player options and diagnostics.
3. Volume and scene component resolution continues as before.
4. A missing, malformed, wrong-type or unsupported-version asset leaves the
   inline fallback intact. Never apply partially parsed quality data.

The codec has a versioned schema and typed values. Inspector edits, file reloads,
load, save and Undo use the same codec. Saving must not silently upgrade an
unknown schema version. Backend capability results are never serialized.
An invalid reload preserves a cached last-good instance without overwriting
the invalid file on save. A cold invalid load instead uses the inline fallback.
Unknown schema-1 fields survive valid round trips. Pass-list edits preserve
unknown fields by the original entry identity, including duplicate pass names;
new or renamed entries do not inherit another entry's unknown data.
Inspector list operations participate in save and Undo. Temporarily invalid
edits do not create an empty redo snapshot or replace the last valid file.
Pending saves are tracked per absolute asset path, so selection changes or a
hidden Inspector do not lose valid edits. Current active input defers automatic
saving until release. Save failures remain dirty for Save All and are not
retried every frame; another edit or explicit save retries them. Save callbacks
own paths, not a frame's EditorContext. Explicit discard and shutdown do not
restart old pending writes.

## Authoring And Migration

Create pipeline assets through existing DataAsset creation. The Graphics panel
selects an asset or exports the current pipeline configuration without replacing
another file. The Inspector edits the shared asset. Detaching a valid asset
copies its owned values into the inline fallback to preserve the current
configuration. Clearing an invalid reference leaves the fallback unchanged.

Existing projects render identically until an asset is selected. SSR remains
part of a Volume profile; a pipeline asset does not silently enable it.

While stopped, open Project Settings > Graphics > Render Pipeline Asset and
use Save as Pipeline Asset to export into an unused path under Assets. Creation
registers its GUID, assigns the default and selects the asset Inspector.
Inspect Asset opens the shared configuration. Detach Asset keeps its effective
owned values inline and clears the reference without deleting the file.

## Runtime Settings

Editing views resolve the current asset each frame, so Inspector edits and
valid file reloads appear immediately. Starting Play or a standalone game
creates a mutable runtime copy from the resolved asset. Existing graphics
script setters/getters operate on that copy, not on the asset or the project's
authoring fallback. Do not reapply the asset every frame over player choices.
Stopping Play discards the runtime copy. The next session starts with the
latest asset. Asset edits during Play affect the next session.
UI operators, external play.control commands and playtest actions use the same
EditorApp start/stop lifecycle; none directly bypasses runtime-copy ownership.

## Hybrid Quality

Schema 1 adds an optional `[hybrid]` table to the asset and `[render.hybrid]`
to the inline ProjectSettings fallback. Missing fields retain explicit defaults;
malformed, out-of-range or non-finite values reject the whole incoming quality
configuration before applying anything. Both paths use the same typed codec.
Unknown asset fields survive valid round trips.

The Inspector provides Custom / Low / Balanced / High workload presets. It saves
the actual fields, not a GPU model or an effective backend decision. Selecting a
preset does not enable RT, change SSR's Volume settings, change player render
scale, or write through a running session's authoring asset.

The active controls are reflection samples (1..64), history limit (1..64),
spatial radius (0..2), glass boundary limit (1..16), opaque reflection distance
(0 = unbounded scene-lighting queries), reconstruction working-set MiB and
shared probe capture attempts per resource-manager frame (0..16). A finite
distance no-hit is unresolved and falls back through the current SSR/IBL resolver;
it is not treated as an exact black/environment miss. Shadow rays and glass
media paths do not inherit that cutoff.

History storage exceeding its per-view cap is retired through the resource
manager and current-frame RAW rendering remains available. The cap is logical
live reconstruction storage, not total VRAM: raw tracing, AS, textures, driver
heap alignment and fence-retired allocations are separate costs.

Hybrid probe capture uses a deterministic round-robin owner cursor, including
failed attempts, with a shared frame/epoch counter across views and scenes.
Each slot includes an atomic six-face capture and convolution. Existing valid
lighting remains usable while updates wait; uncaptured probes use global IBL.
Raster/Path retain their prior update scheduling. This explicit latency budget
does not freeze or substitute stale geometry in the Ray Scene.

Frame, AS update, trace, reconstruction and probe time budgets are persistent
**targets**, not measured timings or an automatic controller. Full preparation,
probe capture and GPU critical-path instrumentation is still required before
certifying the 1080p/60fps target. No speedup is inferred from a workload preset.

## Scope And Verification

The first implementation supports a project default. Quality-level selection
and scene/camera asset references are future consumers of the same resolver,
not additional field-by-field overrides in this schema. Requested and effective
render modes and fallback reasons remain distinct.

Tests cover typed round trips, owned-field completeness, missing/invalid assets,
GUID persistence, reload, Undo, transactional parsing, transient preservation
and legacy compatibility. Editor warmup, both viewports and both standalone
paths use the same default resolver.

On 2026-10-02 the Release editor and three test binaries built with zero errors
and warnings. All 522 selected regressions passed, including 20 asset codec
tests and six Inspector pending-save tests. The real editor's isolated Cornell
runtime scenario passed 78 steps and six captures: operator and external-bus
Play/Stop paths, runtime shadow changes, pause and restart. Original source
hashes (nine files) and the clone's project settings / pipeline asset (two
files) stayed unchanged. Initial, playing, stopped and restarted diffuse
lighting passed fixed-region image comparisons after isolating probe capture
from previous camera shadows.

The final Hybrid glass scenario passed 27 steps and six captures, including
Scene View round trips. These are correctness checks at Game 1548 x 871 and
Scene 1263 x 435, not a 1080p/60fps performance certification. No CodSpeed
comparison was available on this Windows/DX12 setup without a Linux/CI host.
Inspector queue tests exercise its real persistence entry points, not a
mouse-driven end-to-end export-dialog or Undo interaction test.

## Additional Hybrid Verification

The later 2026-10-02 implementation adds shared diffuse response, bounded rough
solid / initial-medium transport, proven short camera-motion history, Hybrid
workload presets and a shared probe capture limit. The Release editor and three
test binaries built with zero errors and warnings. The final selected suite
passed 483 tests, followed by six Inspector persistence tests (489 distinct
tests). The five earlier failures were repaired without loosening analytic
transport expectations: a synthetic Raster input had contradicted its TLAS
two-sided flag, a legacy finite-distance sample test needed a scene-lighting
query, and the old resolver assertion predated inside-medium result kind 2.

Native isolated scenarios passed 197 steps and produced 22 captures, including
the smooth-glass Scene View round trip, Play/Stop/pause/restart, rough glass and
a camera inside rough glass. All nine protected original files stayed unchanged.
The legacy clone serialized the new default `[render.hybrid]` table on exit;
after that normalization, a repeated 78-step runtime scenario left both clone
ProjectSettings and the pipeline asset byte-identical. These runs do not certify
full motion denoising or 1080p/60fps. Rough transmission still shows visible
grain, and per-effect downscaling / measured automatic budgets remain future work.
