<!-- @file    pix-profiling.md -->
<!-- @brief   Opt-in PIX connection, private event SDK and capture evidence contracts. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-02 -->
# PIX Profiling

## Scope

Windows / DX12 performance investigation uses PIX GPU Captures and Timing Captures.
CodSpeed is not part of this workflow. Capture/replay diagnostics, live timing,
CPU recording time and GPU execution time remain separate observations.
The initial target is RTX 4070, 1920 x 1080, 60 fps in one standalone Game view.

## Editor Connection

- GPU attachment requires the matching `WinPixGpuCapturer.dll` before the first
  D3D12 API call. `--pix-capture` is an explicit diagnostic launch option.
- An explicit PIX directory may be selected; otherwise use a validated installed
  stable release. Load by an absolute path with restricted DLL search behavior.
- Ordinary launches do not load the capturer. DX11, absent/invalid PIX and loading
  failure do not silently report GPU-capture readiness.
- Never unload a capturer while D3D12 objects or queues exist. Never inject into
  unrelated processes, restart an editor without consent, or discard edits.
- An Editor operation opens PIX. It does not claim that a pre-existing normal
  Editor can become GPU-capture-ready after device initialization.

The default directory is `%ProgramFiles%/Microsoft PIX/2603.25`. Override it with
`--pix-path <absolute directory>` only together with `--pix-capture`. Invalid
options or a failed capturer load stop startup before renderer creation. A
matching preloaded module is retained; a different loaded version is rejected.
The diagnostic launch does not take the normal job-breakaway or automatic
source-rebuild/relaunch path, so the capture PID remains the launched process.

`tools.pix_open` is an Action projected into native/ImGui Tools menus and the
palette/AI bus. `tools.pix_status` is a Query; `captureReady` reflects verified
startup state, while `installed` reflects UI executable availability. Opening
`WinPix.exe` does not attach to the Editor. Save work and explicitly start the
next Editor session with `--pix-capture` when the current session is not ready.

## Events

- Use Microsoft's WinPixEventRuntime for supported CPU/GPU event encoding;
  do not encode PIX marker packets manually.
- Keep the SDK private to the Windows/DX12 implementation. Existing upper layers
  continue using `IRenderer`; no backend downcasts or DX headers become public.
- RenderGraph pass names identify GPU scopes. AS and probe/bake preparation must
  be distinguished from graph execution, including explicit view identity where
  available. Event scopes may not span command lists or mismatched submission.
- Marker failures or disabled capture must not change rendering behavior.
- Vendor required SDK files under `ThirdParty/`, with `LICENSE`, `VERSION` and a
  `THIRD-PARTY-NOTICES.md` entry. No FetchContent or implicit package downloads.

WinPixEventRuntime 1.0.240308001 is a PRIVATE Graphics dependency with `USE_PIX`
enabled in Release. Its GPU overloads emit implicit CPU scopes on the calling
thread. Dispatch/IBL scopes therefore finish synchronously before another list
is reset/closed; arbitrary cross-list CPU scope interleaving is not supported.
Graphics scopes close inside-out at a split and reopen outside-in only after a
successful Reset; failure retires tokens without emitting on a closed list.
CPU durations are recording work, not GPU execution. PIX scopes are independent
of the existing timestamp query capacity and preserve view/frame/output extent.

The event DLL follows Graphics into Editor, Sandbox, test executables and SDK
runtime directories even when capturer loading is off. The SDK includes its
LICENSE/VERSION/package notices, stages them as `EngineLicenses`, and preserves
them in game output. DX12 game packaging rejects a missing DLL or notice file.

## Capture Protocol

1. Use an isolated copy of the existing scene/assets and matching completed
   Release binaries/shaders. Preserve original scene and settings hashes.
2. Use standalone for the one-Game-view baseline. Editor batch scenarios render
   both Scene and Game and are labeled separately rather than treated as InGame.
3. Request 1920 x 1080 and verify actual client/internal target dimensions;
   windowed mode can be clamped to the monitor work area.
4. Warm shaders, initial probes and reconstruction. Record whether periodic
   probe updates, AS work, camera/object motion or history resets are present.
5. Save the GPU `.wpix`, screenshot, named event list and separate live Timing
   Capture. Keep capture instrumentation overhead and startup outside steady
   observations. A single replay frame is not a sustained-FPS certificate.
6. Close only the process created for this capture, through normal shutdown.
   Do not stop another editor/build or delete any build tree.

## Evidence

Report GPU/driver, build/capture versions, workload, actual dimensions, settings,
sample count, frame count and available symbols. Distinguish unavailable values
from zero. Report average/tail frame intervals only when live capture data
supports them; never derive FPS from a lockstep test's fixed `dt`, CPU callback
duration, partial graph pass sum or one instrumented replay.

## References

- [PIX capture/attachment](https://devblogs.microsoft.com/pix/taking-a-capture/)
- [PIX command-line capture](https://devblogs.microsoft.com/pix/pixtool/)
- [PIX GPU captures](https://devblogs.microsoft.com/pix/gpu-captures/)
- [GPU timing counter semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/gpu-captures/pix-gpu-captures)
- [PIX Timing Captures](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/pix-timing-captures)
- [Windows Developer Mode](https://learn.microsoft.com/en-us/windows/advanced-settings/developer-mode)
- [WinPixEventRuntime](https://devblogs.microsoft.com/pix/winpixeventruntime/)

## Status

PIX 2603.25 x64 is installed and its installer signature/hash and CLI help have
been verified. Release compilation completed without warnings; 520 regression
tests and the SharedSDKTemplates contract test passed. Isolated ordinary and
PIX-ready startup scenarios passed, and an invalid PIX directory failed before
renderer creation as expected.

On 2026-10-02, PIX attached for GPU Capture to the isolated Release standalone
process and saved one Present-to-Present frame as `Artifacts/PIX/GPU 1.wpix`.
The capture contains the frame marker and its image is 1904 x 993, not the
requested 1920 x 1080. Its SHA256 is
`34207D863E56F12F3DD9CDCA913EF951CE244282322036D9A2ED57F762CD08BB`.
The capture image was exported to
`Scratch/PixCapture/Release20261002/CornellHybridGpu.png`.
The owned Game process closed normally and all nine original asset/settings
hashes remained unchanged.

GPU replay analysis and event-list export were initially rejected with
`E_PIX_FEATURE_REQUIRES_DEVELOPER_MODE`; the user confirmed Developer Mode was
not enabled, then enabled it manually. Replay analysis and CSV/PNG export now
succeed. No Windows security/developer setting was changed automatically.

## Replay Analysis 2026-10-02

The same captured frame (`physical=587`, `View=1`, `extent=1904x993`) was replayed
three times. CSVs and the structured summary are under
`Scratch/PixCapture/Release20261002/`:

- `CornellHybridEventsTimed.csv`
- `CornellHybridEventsTimedRepeat1.csv`
- `CornellHybridEventsTimedRepeat2.csv`
- `Analysis.json`

The first replay's direct sibling pass EOP durations are:

| Work | EOP duration |
|------|-------------:|
| RayReflection | 275.751936 ms |
| RayReflectionTemporal | 1.208320 ms |
| RayReflectionSpatial | 1.181696 ms |
| SSR | 0.101376 ms |
| ReflectionSourceLighting | 0.631808 ms |
| DeferredLighting | 0.671744 ms |
| View marker, including other passes | 280.226816 ms |
| Frame marker | 280.246272 ms |

Every replay's view EOP equals the sum of its direct sibling pass EOP values.
Nested Compute/Dispatch events are not added again. The RayReflection pass has
one Dispatch whose EOP equals the pass EOP, so the measured hotspot is shader
execution rather than CPU AS preparation. The exported `Queue ID` is an
event-local identifier used by `Parent`, not a count of D3D12 queues.

Frame-marker EOP ranges from 280.246272 to 367.857664 ms across these three
replays; RayReflection accounts for 98.00-98.40% of view EOP. Absolute durations
vary appreciably, but the dominant pass is consistent. These are repeated
measurements of one saved frame, not three live gameplay frames. No controlled
live Timing Capture, sustained FPS or 1080p / 60 fps certification is available.
Present duration is absent, and the Frame marker is not a complete
Present-to-Present latency measurement. AS/Probe update markers are absent in
this frame; their cost is not established as zero.

EOP and TOP are different observations. Temporal's TOP duration is about 277 ms
in the first replay, while its EOP is 1.208320 ms. TOP includes overlap/contention
with prior work and must not be interpreted as its independent incremental cost.
EOP attribution identifies this replay's bottleneck, not isolated shader latency.

The isolated project configures four reflection samples and a 16-boundary glass
limit. Those settings have not been independently verified in the capture's
constant buffer. Source inspection of the frozen shader copies identifies these
next comparison candidates, not proven causes:

1. `RayReflection.cs.hlsl` initializes the primary glass medium before the SSR
   early-out; `RayHybridGlass.hlsli` scans scene instances and performs owner
   queries for valid opaque pixels as well as glass receivers.
2. A scene containing glass selects non-opaque candidate processing globally;
   candidate material work and glass-aware lighting/shadow queries can therefore
   affect pixels whose final reflection is not glass.
3. Smooth glass maintains a 17-state pending array and branches into reflection
   and transmission, bounded by up to 512 work items. Register pressure, spills
   and divergence remain hypotheses until additional counters or comparisons.

First inspect the Dispatch constants, then compare glass inactive/active at the
same camera and extent. Separately vary sample count and boundary budget, and
compare IOR 1 against 1.5 while keeping glass active. Preserve history warmup and
initial/probe work labels; do not claim an improvement from unrelated workloads.

## Independent Hybrid Optimization 2026-10-02

No new rendering library was added. Primary medium initialization now has a
CPU-validated camera-outside-solid fast path, and the glass traversal keeps its
current path local while storing only pending siblings. Sampling, optics, RNG
order, boundary/work limits and fail-closed behavior are unchanged. Unknown
geometry, camera-inside, orthographic and secondary-ray initialization retain
the previous path. Scenes without glass skip the new CPU proof.

Release builds completed without errors or warnings, and all 559 related
regressions passed, including 39 new CPU/GPU cases. These are correctness
checks, not performance benchmarks. Release's disabled debug layer is not
certified by this result.

PIX UI automation was stopped by the user's physical Escape key. No new GPU
capture or replay comparison was completed for this implementation. The timings
above belong to the old baseline only; the optimized speedup and sustained
1080p / 60 fps remain unverified. Reuse the baseline conditions and independently
verify the effective Dispatch constants before reporting a speed comparison.
