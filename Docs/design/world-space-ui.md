# World-space UI depth and mounted displays

`UICanvas.renderMode = WorldSpace` places the Canvas in the scene. Its default `depthMode = Scene Depth` hides pixels behind opaque scene geometry. `Always On Top` retains the Canvas position, perspective and size while bypassing scene depth. Screen Space Overlay always remains an overlay.

The Inspector's World & Camera Space group contains these settings:

| Setting | Meaning |
| --- | --- |
| `canvasWidth`, `canvasHeight` | Canvas layout dimensions in pixels. |
| `worldScale` | Metres per Canvas pixel for World Space and Screen Space Camera. |
| `planeDistance` | Screen Space Camera's distance from the camera, in metres. |
| `faceCamera` | Orient a World Space Canvas toward the camera. |
| `depthMode` | `0`: Scene Depth; `1`: Always On Top. The legacy/default value is Scene Depth. |
| `surfaceOffset` | Signed metres toward the Canvas front, local `-Z`. Default `0` preserves the authored plane. |

For a display mounted on a mesh, place the Canvas on the display face and use a small positive `surfaceOffset`, such as `0.001` to `0.003`, if needed to avoid a coincident surface. Keep Scene Depth enabled so bezels, machinery and held objects can cover it. Canvas layout pixels and `worldScale` define its physical size; the existing Canvas mapping does not inherit Transform scale.

The public C# `Canvas` facade exposes `GetDepthMode`, `SetDepthMode(CanvasDepthMode)`, `GetSurfaceOffset` and `SetSurfaceOffset(float)`. The offset accepts finite values in `[-1, 1]` metres. The same operations are available on the owner UI service with Canvas-prefixed names. Existing Canvas sizing and sorting APIs retain their behavior.

UI is composited after postprocessing. A depth-tested pipeline alone is insufficient because the final output's depth does not otherwise contain the scene geometry. The UI pass now declares an HDR dependency and transfers the camera HDR depth into the final output depth before drawing. `UIDepthCopy.hlsl` uses normalized UV and point sampling so the output and internal rendering resolution may differ. The source is Reversed-Z; the copy and UI clip matrix adapt to the output depth convention. The color buffer is preserved.

UI tests depth but never writes it. This applies to default sprites, text and custom UI materials. Opaque meshes that write depth can occlude it; transparent surfaces without depth writes do not become occluders. A final target without a depth attachment cannot render Scene Depth canvases; those canvases are suppressed there and do not participate in pointer/navigation events. Their active control interactions are canceled. Overlays and Always On Top remain available.

Canvas depth mode and surface offset use the normal reflected component serialization. Scenes saved before these fields existed load with Scene Depth and zero offset. The serialization and legacy-load regressions are in `CanvasGeometryGuardTests.cpp`; rendered occlusion also requires a native Editor or GPU scenario because configuration round trips do not establish the depth-buffer binding.

For side-by-side Editor versions, the new process may opt into `FBZZ_MANAGED_CACHE_SCOPE=world-ui-preview` at launch. Its evaluated inputs, candidates, current receipt, ownership ledger and cleanup then use `Library/ManagedScripts/<configuration>/world-ui-preview`. An unset or empty variable preserves the existing cache root. A scope accepts 1–64 ASCII letters, digits, hyphens and underscores; path separators, device names and generation-directory names are rejected. Linked cache directories are rejected. This keeps each host API version's managed manifest separate while an older Editor remains open. The scope is process-local and does not change project assets or system settings.
