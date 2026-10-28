<!-- @file    ai-crowd-bridge.md -->
<!-- @brief   Crowd snapshot packet, integration hooks and verification boundaries. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->

# Crowd snapshot bridge

`Fbzz.AI.AiCrowd` owns two fixed-size managed agent snapshots. `Step(deltaSeconds)` sends settings, profiles, agents, threats and obstacles in one native call and publishes a validated result. It retains no native handle, scene reference or callback. Slot seeds, decision timers, fear memory and the round-robin cursor belong to the caller's state, so separate crowds can share the runtime's retained scratch workspace safely.

The shared operation is `AI_CROWD_STEP = 340`. Request `index = 0` selects numeric crowd simulation. Op 220 belongs to `CAPTURE_RELOAD` and must not be changed. Pose commit uses its separate index-1 contract described in [AI pose batch](ai-pose-batch-contract.md); this page specifies only index 0.

## Packet contract

All packet values are little-endian float32, uint32 or int32, with 4-byte packing. No pointers or managed object references occur inside the payload. The input begins with twelve uint32 values, totaling 48 bytes:

| Header slot | Value |
| --- | --- |
| 0 | Packet version 1 |
| 1 | Settings size: 96 bytes |
| 2 | Profile size: 64 bytes |
| 3 | Agent size: 164 bytes |
| 4 | Threat size: 28 bytes |
| 5 | Obstacle size: 16 bytes |
| 6 | Stats size: 44 bytes |
| 7 | Agent count, 0 to 4096 |
| 8 | Profile count, 1 to 64 |
| 9 | Threat count, 0 to 64 |
| 10 | Obstacle count, 0 to 256 |
| 11 | Reserved: zero |

After the header come one settings struct, profiles, agents, threats, then obstacles. Output contains one stats struct followed by agents in the original slot order. Agent fields include `SenseGroup`, `SensedIndex` and `SensedDistance`; the 164-byte layout is authoritative.

At all maximum counts, input is 681,872 bytes and output is 671,788 bytes. Each is below the 1 MiB packet limit. Request flags, integer, x/y/z/w and reserved0/reserved1 must be zero. Success returns the agent count in reply index, the exact payload length in outputBytes, and zero reply flags/reserved. Invalid input publishes no output. Buffer-too-small reports the required output length before simulation, with no output writes. Overlapping input/output ranges are rejected.

## Atomicity and lifetime

The native transfer validates header version, every declared struct size, bounded counts, exact input length, request fields and reserved values. The kernel validates numeric values and movement policies before writing its separate next snapshot. Native transfer copies output only after successful computation. Host validation checks return-code/status agreement, output length, counts, stats and every returned agent before copying to the caller's next buffer.

`AiCrowd.Step` swaps snapshots, stores stats and advances the settings cursor only after success. Invalid numeric or policy input returns false and preserves the published snapshot, stats and settings. Rejected callback scope or malformed native output throws through the existing managed callback exception boundary. Reentry guards clear in finally/RAII paths.

Step requires the current callback thread, active generation/instance/call scope, and Play mode. Existing runtime callback-policy checks reject callbacks that cannot access or mutate engine state. Managed configuration, agent/profile edits, cached snapshot queries and position copying do not require native access. The managed object owns all state; no native identity needs release when a script generation retires.

Host packet byte arrays grow only when the largest batch grows. Native vectors and the spatial workspace retain capacity between calls. Initial growth may allocate. Warmed same-size steps reuse these buffers. No per-agent script callback, native-to-managed callback or all-pairs fallback occurs in this bridge.

## Threat and obstacle broadphase

The agent-neighbor stamped grid and decision budgets are unchanged. Threats and sphere obstacles use separate equal-count AABB hierarchies rebuilt once per valid Step, with retained workspace storage and the existing 64/256 input limits. Reserved binary-node capacity is at most 127/511 nodes. Eight or fewer spheres keep the original linear scan. The hierarchy design follows the primitive-subdivision and bounded-storage principle in [PBRT, Bounding Volume Hierarchies](https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies); no rendering or scene dependency is introduced into the kernel.

Queries return conservative candidates in original input-index order. The original float squared-distance comparisons remain authoritative: threat boundaries are inclusive, obstacle avoidance/projection boundaries are strict, threat group filters and equal-strength ties are unchanged. Earlier weaker threats may extend remembered duration before a stronger threat replaces the danger position. Bounds are computed in double with operand-scaled float slack to retain those rounded comparisons at large-coordinate cancellation boundaries.

Avoidance queries use the existing lookahead point and body/separation padding. Projection queries use the newly integrated `next` point and body padding. After a projection moves `next`, the kernel queries again and resumes strictly after the projected obstacle's original index; previously passed obstacles are not revisited. This preserves the existing one-pass sequential projection even when it leaves a previous sphere penetrated. After four requeries, or when all spheres are already candidates, the remaining original-order scan covers subsequent motion without further spatial searches.

Each spatial query visits at most 64 tree nodes with a 16-entry traversal stack. A traversal-budget/stack limit, non-finite or out-of-domain query point, or query padding outside the accepted numeric domain falls back to all sphere indices in original order. Thus huge lookahead/padding and overlapping giant spheres cannot create unbounded cell expansion or truncate safety candidates. Invalid input spheres still reject the whole Step before output/stats writes. Packet layouts, public stats, decision fairness, movement clocks and callback rules do not change.

Additional kernel regression cases cover input-order threat memory/ties, group exclusion, exact and zero-radius boundaries, large-coordinate rounding, projection into newly relevant later obstacles, long projection cascades, maximum dense inputs, large-query fallback, retained-workspace shrinkage and invalid-input atomicity. These are public `StepCrowd` result checks; performance results require a separate matched measurement and are not inferred from candidate culling.

## Integration and script extension

The common ABI enum declaration is `FBZZ_MANAGED_ENUM_VALUE(AI_CROWD_STEP, AI_CROWD_STEP, 340)`. Its generated Host enum and schema hash must ship with the matching rebuilt ScriptApi and Host. The native runtime includes `ManagedAiCrowdTransfer.hpp`, owns a `managed_detail::ManagedAiCrowdTransfer` in its Impl, and dispatches index 0 after existing scope checks and before generic target-object service resolution. Play validation precedes the transfer. The new source belongs to the existing Scene/Managed module; the crowd kernel belongs to the existing AI module.

Script may change editable profiles and per-agent intent, target, target velocity, sense group, external velocity and flags through the facade. `REQUEST_DECISION` requests a budgeted decision. Threat inputs affect continuous steering independently of decision cadence. `EXTERNAL` or `OVERRIDE_STEERING` supplies custom desired velocity while retaining acceleration, turn and habitat constraints. Script can clear `ACTIVE` to own that slot's entire movement and publish its authored position through existing batch APIs.

`Snapshot` exposes cached position, velocity, mode, fear and transient event bits. Reacquire it after a successful Step. Events describe the current Step and invoke no callbacks; consume or latch transitions before the next Step. `CopyPositions` uses caller storage. Animation, audio, VFX, floor/ceiling sampling and scene-object ownership remain with the script. Sensing reports a nearest slot among bounded tested candidates, not a globally closest entity; `DecisionAge` exposes its freshness.

## References and validation scope

The [official Epic Mass Gameplay overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine), read on 2026-10-11, describes variable-frequency simulation LOD, data configured for processors, and velocity/force adjustments. FBZZ adopts these principles for batched data and independently updated decisions/movement, without copying UE source or adopting its full ownership model. [Microsoft MemoryMarshal.AsBytes](https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.memorymarshal.asbytes) documents unmanaged value constraints and raw-layout exposure; explicit packing, size assertions and the versioned header protect this numeric boundary.

The isolated facade runner compiles the production facade and Host marshaller with callback/ABI stubs and a Cdecl native mock. It passed 44 checks covering layout, extension fields, one-call dispatch, scope/reentry, malformed later-slot atomicity, exception recovery and maximum packets. Its 1000-agent, 300-Step warmed run used exactly 300 dispatches and allocated 0 managed bytes. Recorded transport-only elapsed times varied across runs, approximately 27 to 41 ms total; these are not native AI simulation timings.

The parent's final isolated native CPU suite passed 25 tests against the actual kernel and transfer boundary. Native benchmarks measure kernel/CPU work separately from managed transport. Neither those CPU tests nor mock durations establish GPU cost, scene pose cost or the appearance of the 1024-fish scene. Common engine/Host compilation, SDK synchronization and scene integration are separate validation steps owned by the integration coordinator.
