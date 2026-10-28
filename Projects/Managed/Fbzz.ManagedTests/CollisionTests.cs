/// @file    CollisionTests.cs
/// @brief   typed 物理 lifecycle の検出・同期 scope・保持参照と障害隔離を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class CollisionTests
{
    internal static (string Name, Action Test)[] Cases =>
    [
        ("PhysicsLifecycleSignatures", Signatures),
        ("PhysicsCallbackSnapshotsAndScope", SnapshotsAndScope),
        ("PhysicsCallbackFaultAndReferenceLifetime", FaultAndReferenceLifetime)
    ];

    static void Signatures()
    {
        using World world = new();
        ScriptTypeDescriptor type = world.Type;
        Program.Check(type.Callbacks.Count((ScriptCallbackDescriptor value) => ScriptCatalogBuilder.IsCollisionCallback(value.Callback)
            || ScriptCatalogBuilder.IsTriggerCallback(value.Callback)) == 6, "All six private typed callbacks must be discovered without registration or override.");
        ModuleBuilder module = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("InvalidPhysicsLifecycle"), AssemblyBuilderAccess.RunAndCollect).DefineDynamicModule("main");
        Invalid(module, "MissingArgument", "OnCollisionEnter", Type.EmptyTypes);
        Invalid(module, "WrongTriggerArgument", "OnTriggerEnter", [typeof(Collision)]);
        Invalid(module, "ByReferenceArgument", "OnCollisionStay", [typeof(Collision).MakeByRefType()]);
        Invalid(module, "StaticCallback", "OnTriggerStay", [typeof(GameObject)], MethodAttributes.Private | MethodAttributes.Static);
        Invalid(module, "AsyncCallback", "OnCollisionExit", [typeof(Collision)], async: true);
        Invalid(module, "OptionalArgument", "OnTriggerExit", [typeof(GameObject)], optional: true);
        CatalogBuildResult invalid = ScriptCatalogBuilder.Build(module.Assembly);
        Program.Check(!invalid.Success && invalid.Diagnostics.Count((ScriptDiagnostic value) => value.Code == "CS_LIFECYCLE_SIGNATURE") == 6,
            "Missing, wrong, ref, static, async and optional arguments must reject the candidate before delivery.");
    }

    static void Invalid(ModuleBuilder module, string name, string callback, Type[] parameters,
        MethodAttributes attributes = MethodAttributes.Private, bool async = false, bool optional = false)
    {
        TypeBuilder type = module.DefineType(name, TypeAttributes.Public, typeof(Script));
        type.DefineDefaultConstructor(MethodAttributes.Public);
        MethodBuilder method = type.DefineMethod(callback, attributes, typeof(void), parameters);
        if (async) method.SetCustomAttribute(new CustomAttributeBuilder(typeof(AsyncStateMachineAttribute).GetConstructor([typeof(Type)])!, [typeof(object)]));
        if (optional) method.DefineParameter(1, ParameterAttributes.Optional, "other");
        method.GetILGenerator().Emit(OpCodes.Ret);
        _ = type.CreateType();
    }

    static void SnapshotsAndScope()
    {
        using World world = new();
        ulong id = world.Create();
        Program.Check(world.Runtime.ConfigureFields(1, id, "{\"touchTransform\":true,\"readTime\":true}", false).Success, "Callback options must configure normally.");
        Collision collision = Snapshot(new GameObject(202, 1, "target-guid"));
        ScriptCallback[] callbacks = [ScriptCallback.ON_COLLISION_ENTER, ScriptCallback.ON_COLLISION_STAY, ScriptCallback.ON_COLLISION_EXIT,
            ScriptCallback.ON_TRIGGER_ENTER, ScriptCallback.ON_TRIGGER_STAY, ScriptCallback.ON_TRIGGER_EXIT];
        foreach (ScriptCallback callback in callbacks) world.Invoke(id, callback, collision);
        Script instance = world.Instance(id);
        foreach (string counter in new[] { "collisionEnterCount", "collisionStayCount", "collisionExitCount", "triggerEnterCount", "triggerStayCount", "triggerExitCount" })
            Program.Check(Equals(Program.Field(world.Type, counter).Read(instance), 1), "Each native event must invoke its corresponding private method once.");
        Program.Check(((int[])Program.Field(world.Type, "eventOrder").Read(instance)!).SequenceEqual([1, 2, 3, 4, 5, 6]), "Event delivery must retain the native caller order.");
        Program.Check(Equals(Program.Field(world.Type, "point").Read(instance), collision.Point)
            && Equals(Program.Field(world.Type, "normal").Read(instance), collision.Normal)
            && Equals(Program.Field(world.Type, "depth").Read(instance), collision.Depth)
            && Equals(Program.Field(world.Type, "relativeVelocity").Read(instance), collision.RelativeVelocity)
            && Equals(Program.Field(world.Type, "approachSpeed").Read(instance), collision.ApproachSpeed)
            && Equals(Program.Field(world.Type, "impulse").Read(instance), collision.Impulse), "The managed callback must receive the entire immutable native snapshot.");
        Program.Check(world.Bridge.Position.Equals(new Vector3(6f, 0f, 0f)) && world.Bridge.LastOwner == 101,
            "Engine operations must retain the receiving Script authority across all physics callbacks.");
        Program.Check(Equals(Program.Field(world.Type, "deltaSeconds").Read(instance), 0.25f) && ScriptApiRuntime.Current is null,
            "Physics time must use the live callback service and end its scope after synchronous delivery.");
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 90, ScriptCallback.ON_COLLISION_ENTER).Status == BridgeStatus.INVALID_ARGUMENT,
            "Parameterless dispatch must not accidentally invoke a typed callback.");
        Program.Check(world.Runtime.TryInvokeCollision(1, id, 101, 91, ScriptCallback.UPDATE, collision).Status == BridgeStatus.INVALID_ARGUMENT,
            "Collision payloads must not be accepted by ordinary lifecycle methods.");
        Collision foreign = Snapshot(new GameObject(202, 2, "target-guid"));
        Program.Check(world.Runtime.TryInvokeCollision(1, id, 101, 92, ScriptCallback.ON_COLLISION_ENTER, foreign).Status == BridgeStatus.INVALID_ARGUMENT,
            "Foreign scene payloads must be rejected without entering the callback.");
    }

    static void FaultAndReferenceLifetime()
    {
        using World world = new();
        ulong id = world.Create();
        world.Invoke(id, ScriptCallback.ON_COLLISION_ENTER, Snapshot(new GameObject(202, 1, "target-guid")));
        GameObject retained = (GameObject)Program.Field(world.Type, "other").Read(world.Instance(id))!;
        world.Bridge.Alive.Remove(202);
        world.Bridge.Alive.Add(404);
        world.Bridge.Guids["target-guid"] = 404;
        Program.Check(world.Runtime.TryInvoke(1, id, 101, 20, ScriptCallback.UPDATE).Success
            && Equals(Program.Field(world.Type, "cachedOtherAlive").Read(world.Instance(id)), false)
            && retained.ObjectId == 202 && world.Bridge.ResolveCount == 0, "A cached event target must expire without reviving its GUID or resolving again.");
        Program.Check(world.Runtime.ConfigureFields(1, id, "{\"attemptCachedMutation\":true}", false).Success, "The fault path must configure through ordinary settings.");
        ManagedRuntimeResult fault = world.Runtime.TryInvoke(1, id, 101, 21, ScriptCallback.UPDATE);
        Program.Check(fault.Status == BridgeStatus.CALLBACK_FAILED && fault.Message.Contains("expired", StringComparison.Ordinal)
            && ScriptApiRuntime.Current is null, "Expired target operations must stop the receiving instance and always clear its scope.");
        Program.Check(world.Runtime.TryInvokeCollision(1, id, 101, 22, ScriptCallback.ON_TRIGGER_ENTER, Snapshot(null)).Status == BridgeStatus.CALLBACK_FAILED,
            "A faulted instance must not receive further automatic physics notifications.");
        ulong healthy = world.Create();
        world.Invoke(healthy, ScriptCallback.ON_TRIGGER_ENTER, Snapshot(null));
        Program.Check(Equals(Program.Field(world.Type, "triggerEnterCount").Read(world.Instance(healthy)), 1)
            && Program.Field(world.Type, "other").Read(world.Instance(healthy)) is null,
            "A missing event target must remain null and a sibling instance must remain runnable.");
        Program.Check(world.Runtime.ConfigureFields(1, healthy, "{\"throwOnEnter\":true}", false).Success, "An intentional callback failure must be configurable.");
        ManagedRuntimeResult thrown = world.Runtime.TryInvokeCollision(1, healthy, 101, 23, ScriptCallback.ON_COLLISION_ENTER, Snapshot(null));
        Program.Check(thrown.Status == BridgeStatus.CALLBACK_FAILED && thrown.Message.Contains("Intentional collision callback failure.", StringComparison.Ordinal)
            && ScriptApiRuntime.Current is null, "Managed exceptions must cross the Host boundary as one callback failure.");
    }

    static Collision Snapshot(GameObject? other) => new(other, new Vector3(1f, 2f, 3f), new Vector3(-1f, 0f, 0f), 0.5f,
        new Vector3(4f, 5f, 6f), 3f, 2f);

    sealed class World : IDisposable
    {
        internal World()
        {
            Runtime = new(Bridge);
            ManagedRuntimeResult loaded = Runtime.LoadCandidate(Program.FixturePath("V1"), 1, internalMode: true);
            Program.Check(loaded.Success, loaded.Message);
        }
        internal Program.RecordingBridge Bridge { get; } = new();
        internal ManagedScriptRuntime Runtime { get; }
        internal ScriptTypeDescriptor Type => Runtime.GetCatalog(1)!.Find("ManagedFixtures.CollisionComponent")!;
        internal ulong Create()
        {
            Program.Check(Runtime.CreateInstance(1, Type.TypeName, 101, out ulong id).Success, "Physics fixture must construct.");
            Program.BindFixture(Runtime, 1, id);
            return id;
        }
        internal Script Instance(ulong id)
        { Program.Check(Runtime.TryInstance(1, id, out ManagedScriptRuntime.ScriptInstance? record), "Fixture instance must remain present."); return record!.Instance; }
        internal void Invoke(ulong id, ScriptCallback callback, Collision collision)
        { ManagedRuntimeResult invoked = Runtime.TryInvokeCollision(1, id, 101, 10, callback, collision); Program.Check(invoked.Success, invoked.Message); }
        public void Dispose() { Runtime.Dispose(); }
    }
}
