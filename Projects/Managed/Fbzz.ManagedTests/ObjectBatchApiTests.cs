/// @file    ObjectBatchApiTests.cs
/// @brief   一括 Script API の呼び出し権限・参照スロット・再利用バッファ契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using Fbzz;

namespace Fbzz.ManagedTests;

internal static class ObjectBatchApiTests
{
    internal static void Run()
    {
        BufferedRequestsUseOneBridgeCall();
        InvalidRequestsDoNotDispatch();
        NativeFailuresRetainOutputs();
        ServicesRequireCurrentScope();
        EmptyAndLargeRequestsStillDispatch();
    }

    static void BufferedRequestsUseOneBridgeCall()
    {
        using ApiFixture fixture = new();
        ScriptObjects objects = fixture.Script.Objects;
        Program.Check(ReferenceEquals(objects, fixture.Script.Objects), "A Script must reuse one bound object service.");
        GameObject[] targets = fixture.Targets();
        Vector3 sentinel = new(71f, 72f, 73f);
        Vector3[] positions = [sentinel, sentinel, sentinel, sentinel, sentinel, sentinel];
        ObjectStatus[] statuses = [(ObjectStatus)77, (ObjectStatus)77, (ObjectStatus)77, (ObjectStatus)77, (ObjectStatus)77, (ObjectStatus)77];
        Program.Check(objects.TryReadWorldPositions(targets, positions, statuses), "A normal request may contain null, missing and expired slots.");
        Program.Check(fixture.Bridge.Calls == 1 && ScriptApiRuntime.IsSameScope(fixture.Bridge.LastScope, fixture.Scope)
            && fixture.Bridge.LastScope.OwnerId == 303 && fixture.Bridge.LastPositionsLength == targets.Length
            && fixture.Bridge.LastStatusesLength == targets.Length, "The batch must retain caller authority and pass only the active output prefix once.");
        Program.Check(positions[0].Equals(new Vector3(2f, 3f, 4f)) && positions[4].Equals(positions[0])
            && positions[1].Equals(sentinel) && positions[2].Equals(sentinel) && positions[3].Equals(sentinel)
            && positions[5].Equals(sentinel), "Missing slots and output tails must retain their earlier positions.");
        Program.Check(statuses[0] == ObjectStatus.VALID && statuses[1] == ObjectStatus.UNSET
            && statuses[2] == ObjectStatus.EXPIRED && statuses[3] == ObjectStatus.EXPIRED
            && statuses[4] == ObjectStatus.VALID && statuses[5] == (ObjectStatus)77,
            "Every input slot must retain its order, duplicates and distinct unset/expired status.");
        Program.Check(fixture.Bridge.LastObjects[1] is null && fixture.Bridge.LastObjects[2].ObjectId == 0
            && fixture.Bridge.LastObjects[2].Guid == "missing" && ReferenceEquals(fixture.Bridge.LastObjects[0], fixture.Bridge.LastObjects[4]),
            "The API must not resolve missing GUIDs, drop null slots or deduplicate retained references.");

        Vector3 invalidUnused = new(float.NaN, float.PositiveInfinity, 0f);
        Vector3[] desired = [new Vector3(10f, 0f, 0f), invalidUnused, invalidUnused, invalidUnused, new Vector3(20f, 0f, 0f), invalidUnused];
        Program.Check(objects.TrySetWorldPositions(targets, desired, statuses) && fixture.Bridge.Calls == 2
            && fixture.Bridge.LastPositionsLength == targets.Length && fixture.Bridge.Positions[202].Equals(desired[4]),
            "Only valid slots use position values; duplicate requests must arrive in input order without scalar writes.");
        Program.Check(float.IsNaN(fixture.Bridge.LastPositions[1].X) && statuses[5] == (ObjectStatus)77,
            "Unused non-finite inputs must be forwarded without touching the output tail.");

        BatchResult moved = objects.TranslateWorld(targets, Vector3.FORWARD, activeOnly: true);
        Program.Check(moved.Succeeded && moved.Status == BatchStatus.OK && moved.InputCount == 5
            && moved.AppliedCount == 2 && moved.SkippedCount == 3 && fixture.Bridge.Calls == 3
            && fixture.Bridge.LastActiveOnly && fixture.Bridge.LastDisplacement.Equals(Vector3.FORWARD),
            "Translate must forward activeOnly and expose native batch counts through one call.");
    }

    static void InvalidRequestsDoNotDispatch()
    {
        using ApiFixture fixture = new();
        ScriptObjects objects = fixture.Script.Objects;
        GameObject[] targets = fixture.Targets();
        Vector3 sentinel = new(81f, 82f, 83f);
        Vector3[] positions = [sentinel, sentinel, sentinel, sentinel, sentinel];
        ObjectStatus[] statuses = [(ObjectStatus)88, (ObjectStatus)88, (ObjectStatus)88, (ObjectStatus)88, (ObjectStatus)88];
        Program.Check(!objects.TryReadWorldPositions(targets, positions.AsSpan(0, 4), statuses)
            && !objects.TryReadWorldPositions(targets, positions, statuses.AsSpan(0, 4))
            && !objects.TrySetWorldPositions(targets, positions.AsSpan(0, 4), statuses)
            && !objects.TrySetWorldPositions(targets, positions, statuses.AsSpan(0, 4)), "Insufficient input or output buffers must reject the whole request.");
        GameObject[] wrongScene = [new GameObject(0, 99, "missing-other-scene")];
        Program.Check(!objects.TryReadWorldPositions(wrongScene, positions, statuses)
            && !objects.TrySetWorldPositions(wrongScene, positions, statuses), "Even missing non-null wrappers from another Scene must be rejected.");
        BatchResult sceneFailure = objects.TranslateWorld(wrongScene, Vector3.ZERO);
        BatchResult finiteFailure = objects.TranslateWorld(targets, new Vector3(0f, float.NaN, 0f));
        Program.Check(sceneFailure.Status == BatchStatus.SCENE_MISMATCH && sceneFailure.InputCount == 1
            && finiteFailure.Status == BatchStatus.INVALID_ARGUMENT && finiteFailure.InputCount == targets.Length
            && !sceneFailure.Succeeded && sceneFailure.AppliedCount == 0 && sceneFailure.SkippedCount == 0
            && finiteFailure.AppliedCount == 0 && finiteFailure.SkippedCount == 0,
            "Expected request failures must preserve input counts and report no partial application.");
        Program.Check(fixture.Bridge.Calls == 0, "Shape, Scene and shared displacement errors must not dispatch a native operation.");
        for (int index = 0; index < positions.Length; index++)
            Program.Check(positions[index].Equals(sentinel) && statuses[index] == (ObjectStatus)88, "Preflight failures must retain every output slot.");
    }

    static void NativeFailuresRetainOutputs()
    {
        using ApiFixture fixture = new();
        GameObject[] targets = [new GameObject(202, 55, "target"), new GameObject(202, 55, "target")];
        Vector3 sentinel = new(91f, 92f, 93f);
        Vector3[] positions = [sentinel, sentinel, sentinel];
        ObjectStatus[] statuses = [(ObjectStatus)99, (ObjectStatus)99, (ObjectStatus)99];
        fixture.Bridge.NextStatus = BatchStatus.INVALID_SCOPE;
        Program.Check(!fixture.Script.Objects.TryReadWorldPositions(targets, positions, statuses), "A native phase rejection must return false.");
        fixture.Bridge.NextStatus = BatchStatus.PHYSICS_BODY;
        Program.Check(!fixture.Script.Objects.TrySetWorldPositions(targets, positions, statuses), "A native physics-body rejection must return false.");
        BatchResult rejected = fixture.Script.Objects.TranslateWorld(targets, Vector3.RIGHT);
        Program.Check(!rejected.Succeeded && rejected.Status == BatchStatus.PHYSICS_BODY && rejected.InputCount == targets.Length
            && rejected.AppliedCount == 0 && rejected.SkippedCount == 0, "Failed native replies must never expose successful slot counts.");
        fixture.Bridge.NextStatus = BatchStatus.OK;
        Vector3[] invalid = [new Vector3(5f, 0f, 0f), new Vector3(float.PositiveInfinity, 0f, 0f)];
        Program.Check(!fixture.Script.Objects.TrySetWorldPositions(targets, invalid, statuses)
            && fixture.Bridge.Positions[202].Equals(new Vector3(2f, 3f, 4f)), "Per-slot finite validation belongs to the batch bridge and must not leave an earlier scalar write applied.");
        Program.Check(fixture.Bridge.Calls == 4, "Each well-shaped request must dispatch exactly once, including native rejection.");
        for (int index = 0; index < positions.Length; index++)
            Program.Check(positions[index].Equals(sentinel) && statuses[index] == (ObjectStatus)99, "Native whole-request failures must retain outputs and tails.");
    }

    static void ServicesRequireCurrentScope()
    {
        using ApiFixture fixture = new();
        ScriptObjects objects = fixture.Script.Objects;
        GameObject[] targets = [];
        Vector3[] positions = [];
        ObjectStatus[] statuses = [];
        ScriptApiRuntime.Current = null;
        Program.Throws<InvalidOperationException>(() => objects.TranslateWorld(targets, Vector3.ZERO));
        Program.Throws<InvalidOperationException>(() => objects.TryReadWorldPositions(targets, positions, statuses));
        Program.Throws<InvalidOperationException>(() => objects.TrySetWorldPositions(targets, positions, statuses));
        ScriptApiRuntime.Current = fixture.Scope;
        fixture.Binding.Active = false;
        Program.Throws<InvalidOperationException>(() => objects.TranslateWorld(targets, Vector3.ZERO));
        fixture.Binding.Active = true;
        ScriptApiRuntime.Current = fixture.MakeScope(8, 55, 1);
        Program.Throws<InvalidOperationException>(() => objects.TryReadWorldPositions(targets, positions, statuses));
        ScriptApiRuntime.Current = fixture.MakeScope(7, 56, 1);
        Program.Throws<InvalidOperationException>(() => objects.TrySetWorldPositions(targets, positions, statuses));
        ScriptApiRuntime.Current = fixture.MakeScope(7, 55, 0);
        Program.Throws<InvalidOperationException>(() => objects.TranslateWorld(targets, Vector3.ZERO));
        Program.Check(fixture.Bridge.Calls == 0, "A retained service must never dispatch without a live matching receiver and callback scope, even for empty requests.");
    }

    static void EmptyAndLargeRequestsStillDispatch()
    {
        using ApiFixture fixture = new();
        ScriptObjects objects = fixture.Script.Objects;
        Program.Check(objects.TranslateWorld([], Vector3.ZERO).Succeeded
            && objects.TryReadWorldPositions([], Span<Vector3>.Empty, Span<ObjectStatus>.Empty)
            && objects.TrySetWorldPositions([], ReadOnlySpan<Vector3>.Empty, Span<ObjectStatus>.Empty)
            && fixture.Bridge.Calls == 3, "Empty requests must still reach native receiver and phase validation once each.");
        BatchResult invalidEmpty = objects.TranslateWorld([], new Vector3(float.NaN, 0f, 0f));
        Program.Check(!invalidEmpty.Succeeded && invalidEmpty.InputCount == 0 && fixture.Bridge.Calls == 3,
            "A non-finite shared displacement must be invalid even for an empty input.");
        GameObject[] duplicates = new GameObject[4097];
        Array.Fill(duplicates, new GameObject(202, 55, "target"));
        BatchResult large = objects.TranslateWorld(duplicates, Vector3.RIGHT);
        Program.Check(large.Succeeded && large.InputCount == 4097 && large.AppliedCount == 4097
            && fixture.Bridge.Calls == 4 && fixture.Bridge.LastObjects.Length == 4097,
            "Input slots must not be capped to Entity capacity or processed through per-object bridge calls.");
    }

    sealed class BatchOwnerScript : Script { }

    sealed class ApiFixture : IDisposable, IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous = ScriptApiRuntime.Current;
        internal ApiFixture()
        {
            Binding = new ScriptBinding(7, 11, 101) { GameObject = new GameObject(101, 55, "owner") };
            Script = new BatchOwnerScript { Binding = Binding };
            Scope = MakeScope(7, 55, 1);
            ScriptApiRuntime.Current = Scope;
        }
        internal BatchBridge Bridge { get; } = new();
        internal ScriptBinding Binding { get; }
        internal Script Script { get; }
        internal ScriptExecutionScope Scope { get; }
        internal ScriptExecutionScope MakeScope(ulong generation, ulong scene, ulong call)
            => new(Bridge, this, generation, 22, 303, call, scene, ScriptCallback.UPDATE);
        internal GameObject[] Targets()
        {
            GameObject live = new(202, 55, "target");
            return [live, null!, new GameObject(0, 55, "missing"), new GameObject(999, 55, "expired"), live];
        }
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => throw new NotSupportedException();
        public void Dispose() { ScriptApiRuntime.Current = m_previous; }
    }

    /// @note Scalar 呼び出しは全て拒否し、batch の送信先・スロット・出力範囲だけを固定値で観測する。
    sealed class BatchBridge : IScriptNativeBridge
    {
        internal readonly Dictionary<ulong, Vector3> Positions = new() { [202] = new Vector3(2f, 3f, 4f) };
        internal int Calls;
        internal ScriptExecutionScope LastScope;
        internal GameObject[] LastObjects = [];
        internal Vector3[] LastPositions = [];
        internal int LastPositionsLength;
        internal int LastStatusesLength;
        internal Vector3 LastDisplacement;
        internal bool LastActiveOnly;
        internal BatchStatus NextStatus = BatchStatus.OK;
        void Record(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects)
        { Calls++; LastScope = scope; LastObjects = objects.ToArray(); }
        static ObjectStatus StatusOf(GameObject? target)
            => target is null ? ObjectStatus.UNSET : target.ObjectId == 202 ? ObjectStatus.VALID : ObjectStatus.EXPIRED;
        BatchResult Result(ReadOnlySpan<GameObject> objects)
        {
            int applied = 0;
            foreach (GameObject target in objects) if (StatusOf(target) == ObjectStatus.VALID) applied++;
            return new BatchResult(NextStatus, objects.Length, applied, objects.Length - applied);
        }
        public BatchResult TranslateWorld(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Vector3 displacement, bool activeOnly)
        { Record(scope, objects); LastDisplacement = displacement; LastActiveOnly = activeOnly; return Result(objects); }
        public BatchResult ReadWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, Span<Vector3> positions, Span<ObjectStatus> statuses)
        {
            Record(scope, objects); LastPositionsLength = positions.Length; LastStatusesLength = statuses.Length;
            BatchResult result = Result(objects);
            if (!result.Succeeded) return result;
            for (int index = 0; index < objects.Length; index++)
            {
                statuses[index] = StatusOf(objects[index]);
                if (statuses[index] == ObjectStatus.VALID) positions[index] = Positions[objects[index].ObjectId];
            }
            return result;
        }
        public BatchResult SetWorldPositions(ScriptExecutionScope scope, ReadOnlySpan<GameObject> objects, ReadOnlySpan<Vector3> positions, Span<ObjectStatus> statuses)
        {
            Record(scope, objects); LastPositions = positions.ToArray(); LastPositionsLength = positions.Length; LastStatusesLength = statuses.Length;
            BatchResult result = Result(objects);
            if (!result.Succeeded) return result;
            for (int index = 0; index < objects.Length; index++)
                if (StatusOf(objects[index]) == ObjectStatus.VALID && !positions[index].IsFinite)
                    return new BatchResult(BatchStatus.INVALID_ARGUMENT, objects.Length, 0, 0);
            for (int index = 0; index < objects.Length; index++)
            {
                statuses[index] = StatusOf(objects[index]);
                if (statuses[index] == ObjectStatus.VALID) Positions[objects[index].ObjectId] = positions[index];
            }
            return result;
        }
        public bool IsAlive(ScriptExecutionScope scope, ulong objectId) => throw new NotSupportedException();
        public GameObject? CreateObject(ScriptExecutionScope scope, string name, Vector3 position, Quaternion rotation, bool usePose) => throw new NotSupportedException();
        public GameObject? InstantiateObject(ScriptExecutionScope scope, string prefab, Vector3 position, Quaternion rotation, bool usePose) => throw new NotSupportedException();
        public bool CanCreate(ScriptExecutionScope scope, int count) => throw new NotSupportedException();
        public bool DestroyObject(ScriptExecutionScope scope, ulong objectId, float delay) => throw new NotSupportedException();
        public BatchResult DestroyObjects(ScriptExecutionScope scope, ReadOnlySpan<GameObject?> objects, float delay) => throw new NotSupportedException();
        public Vector3 ReadPhysicsVector(ScriptExecutionScope scope, ulong objectId, uint field) => throw new NotSupportedException();
        public float ReadPhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint field) => throw new NotSupportedException();
        public bool ReadPhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint field) => throw new NotSupportedException();
        public void WritePhysicsVector(ScriptExecutionScope scope, ulong objectId, uint field, Vector3 value) => throw new NotSupportedException();
        public void WritePhysicsScalar(ScriptExecutionScope scope, ulong objectId, uint field, float value) => throw new NotSupportedException();
        public void WritePhysicsFlag(ScriptExecutionScope scope, ulong objectId, uint field, bool value) => throw new NotSupportedException();
        public void PhysicsBodyCommand(ScriptExecutionScope scope, ulong objectId, uint command, Vector3 value, Vector3 point, uint axes) => throw new NotSupportedException();
        public Vector3 ReadWorldGravity(ScriptExecutionScope scope) => throw new NotSupportedException();
        public bool LayersCollide(ScriptExecutionScope scope, int a, int b) => throw new NotSupportedException();
        public bool Raycast(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit) => throw new NotSupportedException();
        public bool SphereCast(ScriptExecutionScope scope, Vector3 center, float radius, Vector3 direction, float maxDistance, out RaycastHit hit) => throw new NotSupportedException();
        public RaycastHit[] RaycastAll(ScriptExecutionScope scope, Vector3 origin, Vector3 direction, float maxDistance) => throw new NotSupportedException();
        public GameObject[] OverlapSphere(ScriptExecutionScope scope, Vector3 center, float radius) => throw new NotSupportedException();
        public Quaternion ReadRotation(ScriptExecutionScope scope, ulong objectId, bool world) => throw new NotSupportedException();
        public void WriteLocalRotation(ScriptExecutionScope scope, ulong objectId, Quaternion value) => throw new NotSupportedException();
        public Vector3 ReadLocalScale(ScriptExecutionScope scope, ulong objectId) => throw new NotSupportedException();
        public void WriteLocalScale(ScriptExecutionScope scope, ulong objectId, Vector3 value) => throw new NotSupportedException();
        public Vector3 ReadDirection(ScriptExecutionScope scope, ulong objectId, uint direction) => throw new NotSupportedException();
        public void Rotate(ScriptExecutionScope scope, ulong objectId, Vector3 axis, float degrees) => throw new NotSupportedException();
        public void LookAt(ScriptExecutionScope scope, ulong objectId, Vector3 worldTarget) => throw new NotSupportedException();
        public Vector3 ReadPosition(ScriptExecutionScope scope, ulong objectId, bool world) => throw new NotSupportedException();
        public void WritePosition(ScriptExecutionScope scope, ulong objectId, Vector3 value, bool world) => throw new NotSupportedException();
        public void TranslateLocal(ScriptExecutionScope scope, ulong objectId, Vector3 delta) => throw new NotSupportedException();
        public bool ReadKey(ScriptExecutionScope scope, KeyCode key, uint state) => throw new NotSupportedException();
        public float ReadAction(ScriptExecutionScope scope, string action, uint state) => throw new NotSupportedException();
        public float DeltaSeconds(ScriptExecutionScope scope) => throw new NotSupportedException();
        public ulong ResolveGameObject(ulong generation, ulong instanceId, ulong ownerId, string guid) => throw new NotSupportedException();
        public ulong[] GetManagedComponents(ScriptExecutionScope scope, ulong objectId) => throw new NotSupportedException();
        public string GetObjectName(ScriptExecutionScope scope, ulong objectId) => throw new NotSupportedException();
        public GameObject? FindByName(ScriptExecutionScope scope, string name, bool includeInactive) => throw new NotSupportedException();
        public GameObject? FindChild(ScriptExecutionScope scope, ulong objectId, ReadOnlySpan<string> names, bool includeInactive) => throw new NotSupportedException();
    }
}
