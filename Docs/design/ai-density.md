<!-- @file    ai-density.md -->
<!-- @brief   Authoritative coarse plankton density, bounded feeding and deterministic saved state. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->

# AI density field

`Fbzz.AI.AiDensityField` owns a coarse world-space 3D energy reservoir. Plankton rendering reads the field; particle entities do not define food availability and are never individually consumed. A giant filter feeder can remain a primary consumer with a plankton diet. Body size does not assign its trophic tier.

## Public API and units

```csharp
var plankton = new AiDensityField(
    minimum: new Vector3(-64, -32, -64), maximum: new Vector3(64, 0, 64),
    cellsX: 32, cellsY: 16, cellsZ: 32, capacity: 10f, initialDensity: 3f, seed: 71);

bool stepped = plankton.Step(0.5f, growthPerSecond: 0.03f,
    recoveryPerSecond: 0.01f, flowVelocity: new Vector3(0.4f, 0, 0.1f));
bool hasFood = plankton.FindFeedingGoal(worldPosition, radius: 30f,
    maxCells: 64, out Vector3 goal, minimumDensity: 2f);
Vector3 mouth = worldPosition + worldRotation * localMouthOffset;
bool consumed = plankton.TryConsumeSphere(mouth, radius: 2f,
    requestedEnergy: 1.5, out double actualEnergy, maxCells: 64);
float visualDensity = plankton.ReadDensity(worldPosition);
```

Bounds, positions, radii, mouth offsets and cell widths use meters. Flow uses meters/second. Growth and recovery rates use inverse seconds. Density and capacity use energy per cubic meter. Requested and returned feeding energy use energy units; the application defines the meaning of one energy unit and uses the same unit for ecology energy.

Pass the actual consumed amount to `AiEcology.ApplyNutrition(handle, (float)actualEnergy)`. Constrain requested energy to the ecology API's finite float range before this conversion: arbitrary double requests can represent a much larger reservoir.

The field has immutable finite ordered bounds, grid and capacity. Per-axis limits are 32 x 16 x 32, for at most 16384 cells. Invalid constructor arguments throw before creating an instance. Initial density is uniform and must satisfy `0 <= initialDensity <= capacity`, with finite positive capacity. `TrySetDensity(position, density)` lets a script shape food patches and custom volumes while enforcing the same cell bounds.

Seed is stored provenance supplied by the creator. Step, query and consumption consume no random generator; ordering and inputs determine their results.

`ReadDensity` samples piecewise constant cell density and returns zero outside the bounds or for a nonfinite position. Both finite AABB endpoints are supported. `TotalEnergy` is a compensated double integral over all cells and costs O(CellCount), so use it for diagnostics or infrequent accounting.

## Growth, recovery and stable flow

Call `Step` once per field at the selected low-frequency ecology update, such as every 0.5 seconds. Do not call it once per fish. Accepted delta time is 0 through 0.5 seconds. Negative, larger or nonfinite time steps, rates and velocities fail without changing density or elapsed time. Growth and recovery rates must be finite and nonnegative.

Each cell uses the bounded update

`d' = min(capacity, d + dt * (growth * d * (1 - d/capacity) + recovery * (capacity - d)))`.

Growth is logistic. Recovery replenishes available capacity and can recover an empty reservoir. Both saturate at capacity, including very large finite rates. Zero rates create no energy. Internal density, cell volumes and integrated energy use double precision; visual density is returned as float.

Flow uses the per-step donor densities after growth. Its three explicit fractions are `abs(vAxis) * dt / cellWidthAxis`. If their sum exceeds one, the fractions are scaled so their combined CFL fraction is at most one. The velocity sign selects the adjacent receiving cell. Each proposed flux is limited by the donor's remaining density and the receiver's available capacity. Exterior faces retain material rather than discarding it.

Outgoing proposals use the original donor snapshot. A receiver cannot donate newly received material during the same step. A full receiver can cause material to remain in its donor; this is bounded gameplay redistribution rather than a resolved fluid simulation.

With zero growth and recovery, accepted donor-to-receiver transfers conserve the integrated reservoir within double floating-point rounding. Cells remain finite, nonnegative and at most capacity. Growth, recovery and consumption are the explicit sources and sinks. VFX visibility and particle count are not sources or sinks.

## Bounded nearest queries and actual consumption

`FindFeedingGoal(position, radius, maxCells, out goal, minimumDensity=0)` searches nearest cells whose boxes intersect the sphere. A reused min-heap orders cells by distance to their AABB, with flat cell index as a deterministic tie-breaker. It returns the first visited cell whose density is strictly above the threshold.

The goal is the selected cell center. That center can lie up to half a cell diagonal beyond the query radius. If the budget is exhausted before qualifying food is found, the method returns false; this does not imply the whole field is empty.

Search and consumption both accept 1 through 1024 visited cells. Small budgets such as 32 or 64 suit individual eaters. `LastQueryCellCount` reports visited cells. Each visit can enqueue six adjacent cells, so at most `1 + 6*maxCells` new cell distances are evaluated. Heap storage is bounded by the total field cell count. A small query budget never triggers a hidden whole-field scan. A sphere outside the field with no intersection visits zero cells. Searching changes only reusable query scratch and its diagnostic counter.

`TryConsumeSphere(center, radius, double requestedEnergy, out double actualEnergy, maxCells=256)` reports whether the arguments were valid. A valid empty or insufficient reservoir succeeds with less or zero energy. Invalid center, radius, request or budget returns false and actualEnergy zero without changing the field. Zero radius or zero energy is a successful no-op.

The sphere selects intersecting coarse cells. Consumption then removes up to the requested energy from their whole-cell reservoirs in nearest order. It does not solve the exact sphere/cell overlap volume. Choose grid resolution and mouth radius suitable for the ecology scale. A small mouth can access a larger coarse cell, while the requested feeding rate and energy bound the actual amount removed.

All selected density changes are staged. Returned energy is the actual before/after density reduction multiplied by cell volume. If one representable density step would exceed the remaining request, the proposed new density is rounded upward toward retaining mass, reducing the reported consumption. Requests below a reservoir's density ULP consume zero and leave it unchanged. This prevents granting measured nutrition while failing to deplete the reservoir.

One script or controller owns the field and calls it sequentially on one thread. The next eater immediately sees previous depletion, so two sequential callers cannot each receive the same available food. The API is not thread-safe, has no global static field and must not be shared with worker threads. The habitat/controller lifetime owns it independently of transient visual particles.

## Save and restore

Allocate retained storage with `new double[field.CellCount]`. `TrySave(storage, out AiDensitySnapshot header)` copies density cells and preserves the caller tail. Insufficient capacity returns false without writing storage. Store header and density cells together.

Snapshot schema version 1 contains world AABB, grid dimensions, capacity, elapsed time and seed. Preserve the cell doubles without conversion to float to replay exactly. Create a field with matching geometry and capacity, then call `TryRestore(in header, cells)`.

Restore validates schema, topology, bounds, capacity, finite nonnegative time and every finite cell value in `[0, capacity]` before changing any state. Data is copied without retaining caller array references. Time and seed are restored. A later identical sequence of steps and meals reproduces identical cell bits in the tested .NET runtime and architecture. Cross-runtime or cross-architecture floating-point bit identity is not promised.

## Reference choices and limits

Epic's [MassGameplay overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine) describes data-oriented simulation, variable simulation update frequency and separate world representation. The adopted principle is authoritative low-frequency density independent of particle representation. A small bounded managed field at ecology frequency does not require another native bridge. This implementation does not add MassEntity or copy UE source.

Craig Reynolds' own [Steering Behaviors for Autonomous Characters](https://www.red3d.com/cwr/steer/) separates goal selection, steering and locomotion and describes combining movement behaviors. This supports the surrounding consumer contract: density chooses a feeding goal, while crowd steering and caller-defined movement reach it. Reynolds' work is not a plankton density, growth or nutrient-conservation model; those policies are defined here.

Clawpack's [Advection chapter](https://www.clawpack.org/riemann_book/html/Advection.html) derives conservation from boundary flux and defines density-times-velocity transport. The implementation uses that flux principle for coarse cells, with its own explicit CFL and capacity bounds. Logistic growth, recovery and feeding selection are gameplay policies rather than claims of biological fidelity or a full fluid solver.

## CPU validation

```powershell
dotnet run --project Stage/Scratch/Tests/AiDensity/AiDensityTests.csproj --configuration Release
```

The runner clears package sources and compiles the production density file with the actual SDK Vector3, Mathf and API attribute files. Ten groups passed 1717 checks, covering invalid construction and read boundaries, growth/recovery/saturation, 1D and 3D closed-boundary flow, high-speed CFL clamp and conservation, bounded nearest goals, actual consumption and sequential depletion, NaN/invalid atomic rejection, sub-ULP and extreme finite geometry, exact saved-state replay, late invalid restoration and caller tails.

At the maximum 16384 cells, 100 iterations combining Step, a 64-cell-limited feeding goal and consumption query, save and restore took 51.24 ms in the local Release runner. The measured thread allocated 0 managed bytes after warmup. This is an isolated managed CPU measurement, not game-frame or GPU performance. Construction allocates bounded arrays; production methods reuse them without per-call allocation.
