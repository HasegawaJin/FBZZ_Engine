<!-- @file    ai-pose-batch-contract.md -->
<!-- @brief   AI pose commit wire contract, scope guards and integration evidence. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->

# AI world pose batch

`Fbzz.AI.AiPoseBatch.TryCommitWorldPoses(objects, positions, rotations, statuses)` returns the existing `BatchResult`. All spans are caller-owned and can be reused. At most 4096 rows are accepted per call. A 1000-agent update uses one dispatch and a 44004-byte input packet plus 4000-byte output.

The operation updates position and rotation together. Authored local scale is preserved. Native normalizes finite, nonzero quaternion inputs. Null slots produce `UNSET`; expired objects from the current Scene produce `EXPIRED`. Invalid pose data in skipped slots is ignored. Foreign Scene lifetimes, live enabled physics bodies, invalid pose math, and ancestor/descendant targets in one batch fail the whole call. Duplicate target rows count individually, and the final row determines the resulting pose. Additional status span elements are untouched. Zero-scale parents retain the Engine's finite local-axis fallback; a requested world position on a collapsed axis cannot be realized.

The native service validates the entire packet and computes affected ancestors/descendants in reusable shadow Transform storage. No Scene object is written on failure. Once validation succeeds, every affected ancestor, target and descendant receives its final staged Transform exactly once; world propagation has already completed in shadow storage. Scratch uses entity-slot epoch vectors, reused entries and traversal vectors rather than per-call hash containers. Storage can grow when Scene capacity or batch size grows; after warmup it is retained. Calls are synchronous on the callback thread; native never retains borrowed wire buffers. Host rejects reentry and validates all reply metadata and every status before publishing the first caller status. It does not retry mutations.

## Exact world-math reuse within AI pose transfer (2026-10-18)

Successfully validated AI hierarchy calculations are retained as pure values in a thread-local cache bounded to 65536 logical slots. Keys compare the complete bit patterns of local position/rotation/scale, parent presence and parent world position/rotation/scale. No Scene or object references are stored. Cache proofs are reset on SSE rounding, denormal or exception-mask changes at each synchronous AI packet boundary; floating exception status bits are not a cache input. A slot from another Scene can only hit when every arithmetic input matches exactly. Signed zeros are distinct inputs. Larger indices simply use ordinary computation.

AI pose staging can reuse those exact outputs. Only the AI service records successful finite/normalizable calculations. General TransformSystem passes retain their original calculations: extending the cache to these passes did not improve whole-frame measurements and was excluded. Edited bones, parents, physics interpolation and scale changes invalidate the arithmetic key immediately. Every Scene identity, hierarchy membership and physics-authority check remains outside this cache. Partial staging from a rejected packet cannot publish a pose through this mechanism because the complete inputs must match the live values again.

The pre-move traversal validates and retains a complete parent-before-child queue. After the root pose is staged, that queue performs the post-move calculation without rediscovering descendants: there are no callbacks or hierarchy edits in between. Child world values still become current before the call returns. Packet failure remains atomic. No update frequency, steering rule, random sequence, candidate order, animation timing or population limit changes.

## Runtime hook

The shared ABI definition adds `FBZZ_MANAGED_ENUM_VALUE(AI_CROWD_STEP, AI_CROWD_STEP, 340)`. Index 0 is the numeric crowd step; index 1 is this Scene pose commit. This feature changes no existing operation numbers, packet layouts or `IScriptNativeBridge` interface. The new Host partial directly implements the independent `IScriptAiPoseNativeBridge` interface.

`ManagedScriptRuntime.cpp` needs `#include "ManagedAiPoseTransfer.hpp"` and an `Impl` member `managed_detail::ManagedAiPoseTransfer aiPoseTransfer;`. Route index 1 after the existing callback identity, generation, main-thread and lifecycle policy checks, while requiring `Script::IsInPlayMode()`. It must run before the generic `request.integer` target-resolution path because integer carries the row count here. Keep it out of `IsConnectionOperation` and `IsDrawOperation` so Connect and readonly drawing callbacks cannot mutate poses.

```cpp
if (request->op == ManagedBridgeOp::AI_CROWD_STEP && request->index == 1) {
    if (!Script::IsInPlayMode()) return finish(ManagedBridgeStatus::INVALID_SCOPE);
    struct ResolutionContext { Impl& impl; Scene& scene; } context{impl, scene};
    const auto resolve = [](void* state, uint64_t token, GameObject*& object) {
        auto& context = *static_cast<ResolutionContext*>(state);
        const auto found = context.impl.objects.Find(token);
        if (found && found->scene != &context.scene) return ManagedBridgeStatus::SCENE_MISMATCH;
        object = context.impl.ResolveObject(context.scene, token);
        return ManagedBridgeStatus::OK;
    };
    return finish(impl.aiPoseTransfer.Run(scene, *request, *reply, resolve, &context));
}
```

The Scene pose operation requires `flags=0`, `index=1`, `integer=count`, and an exact input length. Input is little endian uint32 count, then `count` rows of uint64 ObjectId, uint64 SceneLifetime, float32 position XYZ, float32 quaternion XYZW (44 bytes each, no struct padding). Output is one uint32 `ObjectStatus` per row. Reply `index=applied`, `flags=skipped`, `outputBytes=count*4`. Failures return applied/skipped zero and write no output; `BUFFER_TOO_SMALL` reports required output bytes. Count is bounded to 4096, keeping all packets below 1 MiB. Scene lifetime is checked against `Scene::GetRenderSceneGeneration`; the shared resolver checks registry Scene and current entity identity.

Register `Engine/Auto/ManagedAiPoseTransferTests.cpp` in the existing Tests source list when integrating. Engine Managed sources already share their existing directory; no new Engine module is needed. Shared Runtime, ABI and CMake edits belong to the coordinating integration owner.

## Consumer

```csharp
// @note Keep these arrays for the lifetime of the population; populate from the crowd output and pose LOD.
BatchResult committed = AiPoseBatch.TryCommitWorldPoses(fishObjects, worldPositions, worldRotations, objectStatuses);
if (!committed.Succeeded) HandleCommitFailure(committed.Status);
```

Use the World rotation computed from steering or custom movement. Do not read each object's current rotation solely to write it back. A caller may choose a subset by pose LOD and submit its retained compact arrays. Sibling or independent root objects are supported; submit a target parent and target descendant in separate calls if that relationship is intentional. Geometry/habitat and custom gameplay decisions remain separate from pose publication.

## Reference and validation

Epic's [MassGameplay overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine) describes data-oriented MassEntity calculations, grouping for Simulation LOD, variable update frequency, and separate movement fragments/processors. This operation adopts contiguous data transfer and separation of simulation from representation publication. It retains FBZZ's existing GameObject and hierarchical Transform system rather than adding an ECS representation framework or copying UE implementation code. LOD timing and steering remain the crowd kernel/consumer's responsibilities.

The isolated CPU mock project compiles the production facade and Host marshaller with actual Engine Vector3, Quaternion, Mathf and API attribute sources. Eight groups cover row layout, null/expired slots, duplicate ordering, empty batch, expected errors, malformed reply metadata/statuses, scope/thread/current-callback preflight, reentry, bounds and retained scratch. 1000 rows over 100 warmed calls produced 100 dispatches and 0 managed allocated bytes on the test thread. This measures bridge overhead only, not native Scene cost or GPU performance.

The Scene TestKit file separately covers 1000 independent objects, final-write duplicates, rotated/signed-scale parent math, inactive descendants, ancestor/descendant atomic rejection, late invalid quaternions, foreign lifetime/Scene, enabled physics-body protection, overflow without live-cache repair, and malformed/capacity preflight. Native compilation and execution require the coordinator's shared build slot and are not claimed by the mock result.

## Packet topology and validated quaternion factors (2026-10-20)

After all object resolvers have run in their original row order, the AI transfer builds a fresh packet-local topology. Each distinct, target-free ancestor is validated once and appended parent before child. Each distinct target retains one breadth-first descendant range, including inactive descendants. Completed ancestor proofs stop repeated walks; an in-progress path detects a cycle. Ancestor/descendant target rejection finishes before world math begins. No topology proof survives the next packet epoch or crosses a later resolver call. Local Transform fields and hierarchy edits made by a resolver are therefore observed when the topology is built.

For R input rows, A distinct ancestors, and U objects in the disjoint target subtrees, topology discovery is O(R + A + U), bounded by Scene slot capacity. Position/rotation staging still follows every original row, including interleaved duplicates. Both the pre-move and post-move arithmetic run for each row's retained subtree, so a duplicate's intermediate invalid pose or overflow still rejects the complete packet. The arithmetic cost remains proportional to the sum of those per-row subtree sizes; this change does not discard intermediate writes or defer descendant synchronization. All live publication and caller statuses remain atomic.

Quaternion checks expose the length or squared length already computed for validation. Within a callback-free arithmetic phase, normalization, inversion, rotated offsets and local quaternion products reuse those exact factors. Reciprocal-then-component multiplication, Hamilton product order, inverse negation order, SetWorldPose's absolute parent-scale threshold of 1e-6, and finite validation of temporary projected scale remain unchanged. Parsed-row lengths are not reused across later resolver calls because floating-point control may have changed. Packet staging recalculates its factors under the final control. The shared bitwise world-math cache and its invalidation rules are unchanged.

Native regressions compare complete Transform bytes against the existing SetWorldPose/UpdateWorldTransform path for deep shared ancestry, inactive descendants, interleaved duplicate rows, nonunit quaternions, signed zeros and the parent-scale threshold. Separate cases retain intermediate-overflow rejection, observe late resolver edits and change SSE rounding during resolution. These contracts are correctness evidence; no performance improvement is claimed until the coordinator's matched Release measurements complete.
