/// @file    TransformPoseApiTests.cs
/// @brief   回転・拡縮 facade と生成 Host marshaller の対象寿命・拒否・単発配送を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static unsafe class TransformPoseApiTests
{
    static BridgeRequest s_last;
    static int s_calls;
    static Quaternion s_local;
    static Quaternion s_world;
    static Vector3 s_localPosition;
    static Vector3 s_worldPosition;
    static Vector3 s_scale;
    static BridgeStatus s_rejection;
    static bool s_alive;

    /// @brief 回転・拡縮 facade と生成 Host marshaller の対象寿命・拒否・単発配送を検証する。
    internal static void Run()
    {
        foreach (var (name, valueType) in new (string, Type)[]
        {
            ("Position", typeof(Vector3)), ("Rotation", typeof(Quaternion)), ("WorldRotation", typeof(Quaternion)),
        })
        {
            var property = typeof(Transform).GetProperty(name, BindingFlags.Public | BindingFlags.Instance);
            Program.Check(property != null && property.PropertyType == valueType
                && property.GetGetMethod() != null && property.GetSetMethod() != null,
                $"Transform.{name} must be a publicly readable and writable {valueType.Name}.");
        }
        IScriptNativeBridge bridge = CreateBridge();
        ScriptExecutionScope scope = new(bridge, new NoComponents(), 7, 9, 101, 13, 42, ScriptCallback.UPDATE);
        GameObject target = new(202, 42, "target-guid");
        Transform pose = target.Transform;
        s_calls = 0; s_alive = true; s_rejection = BridgeStatus.OK;
        s_local = new Quaternion(0f, 0f, 0f, 2f);
        s_world = Quaternion.FromAxisAngle(Vector3.UP, Mathf.HALF_PI);
        s_localPosition = new Vector3(1f, 2f, 3f);
        s_worldPosition = new Vector3(4f, 5f, 6f);
        s_scale = new Vector3(2f, 3f, 4f);
        ScriptApiRuntime.Current = scope;
        try
        {
            Program.Check(pose.LocalRotation.Equals(s_local) && s_last.Index == 0
                && pose.WorldRotation.Equals(s_world) && s_last.Index == 1, "Rotation reads must distinguish local and world without changing stored components.");
            Program.Check(pose.LocalScale.Equals(s_scale), "Scale must read the local value.");
            Near(pose.Forward, s_world * Vector3.FORWARD);
            Program.Check(s_last.Index == 0, "Forward must use direction selector zero.");
            Near(pose.Right, s_world * Vector3.RIGHT);
            Program.Check(s_last.Index == 1, "Right must use direction selector one.");
            Near(pose.Up, s_world * Vector3.UP);
            Program.Check(s_last.Index == 2, "Up must use direction selector two.");
            Program.Check(s_last.Integer == 202 && s_last.Generation == 7 && s_last.InstanceId == 9
                && s_last.OwnerId == 101 && s_last.CallId == 13, "A retained target must use current caller authority and a separate object identity.");

            CheckWorldPoseAliases(pose);

            Quaternion written = new(1f, 2f, 3f, 4f);
            pose.LocalRotation = written;
            Program.Check(s_local.Equals(written) && s_last.Op == BridgeOp.TRANSFORM_WRITE_LOCAL_ROTATION
                && s_last.W == 4f, "The quaternion setter must preserve all four local components.");
            pose.LocalScale = new Vector3(0f, -2f, 3f);
            Program.Check(s_scale.Equals(new Vector3(0f, -2f, 3f)) && s_last.Op == BridgeOp.TRANSFORM_WRITE_LOCAL_SCALE,
                "Zero and negative finite scale must reach native validation.");
            int calls = s_calls;
            pose.Rotate(Vector3.ZERO, 90f);
            Program.Check(s_calls == calls + 1 && s_local.Equals(written), "A zero axis is a native no-op and still checks scope and receiver once.");
            pose.Rotate(Vector3.UP, 90f);
            Program.Check(s_last.Op == BridgeOp.TRANSFORM_ROTATE && s_last.Y == 1f && s_last.W == 90f,
                "Rotate must pack the axis and degree angle without applying a managed rotation first.");
            pose.LookAt(new Vector3(4f, 5f, 6f));
            Program.Check(s_last.Op == BridgeOp.TRANSFORM_LOOK_AT && s_last.X == 4f && s_last.Y == 5f && s_last.Z == 6f,
                "LookAt must send the world-space target unchanged.");

            calls = s_calls;
            Program.Throws<ArgumentOutOfRangeException>(() => pose.LocalRotation = default);
            Program.Throws<ArgumentOutOfRangeException>(() => pose.LocalRotation = new Quaternion(Mathf.EPSILON * 0.5f, 0f, 0f, 0f));
            Program.Throws<ArgumentOutOfRangeException>(() => pose.LocalRotation = new Quaternion(float.MaxValue, 0f, 0f, 1f));
            Program.Throws<ArgumentOutOfRangeException>(() => pose.LocalRotation = new Quaternion(float.NaN, 0f, 0f, 1f));
            Program.Throws<ArgumentOutOfRangeException>(() => pose.LocalScale = new Vector3(1f, float.PositiveInfinity, 1f));
            Program.Throws<ArgumentOutOfRangeException>(() => pose.Rotate(Vector3.UP, float.NaN));
            Program.Throws<ArgumentOutOfRangeException>(() => pose.Rotate(new Vector3(float.NaN, 0f, 0f), 0f));
            Program.Throws<ArgumentOutOfRangeException>(() => pose.LookAt(new Vector3(float.NaN, 0f, 0f)));
            Program.Check(s_calls == calls, "Invalid managed values must not dispatch or partially update the target.");
            pose.LocalRotation = new Quaternion(Mathf.EPSILON, 0f, 0f, 0f);
            Program.Check(s_local.X == Mathf.EPSILON, "The exact normalization threshold must remain accepted.");

            Quaternion beforeRotation = s_local;
            Quaternion beforeWorldRotation = s_world;
            Vector3 beforeWorldPosition = s_worldPosition;
            Vector3 beforeScale = s_scale;
            foreach (BridgeStatus rejection in new[] { BridgeStatus.INVALID_ARGUMENT, BridgeStatus.PHYSICS_BODY, BridgeStatus.BUFFER_TOO_SMALL })
            {
                s_rejection = rejection; calls = s_calls;
                Program.Throws<InvalidOperationException>(() => pose.LocalRotation = Quaternion.IDENTITY);
                Program.Check(s_calls == calls + 1 && s_local.Equals(beforeRotation), "Rejected mutation must dispatch once and preserve the old rotation.");
                calls = s_calls;
                Program.Throws<InvalidOperationException>(() => pose.LocalScale = Vector3.ONE);
                Program.Check(s_calls == calls + 1 && s_scale.Equals(beforeScale), "Rejected mutation must not replay or change scale.");
                calls = s_calls;
                Program.Throws<InvalidOperationException>(() => pose.Position = Vector3.ZERO);
                Program.Check(s_calls == calls + 1 && s_worldPosition.Equals(beforeWorldPosition),
                    "A rejected Position update must not replay or change world position.");
                calls = s_calls;
                Program.Throws<InvalidOperationException>(() => pose.Rotation = Quaternion.IDENTITY);
                Program.Check(s_calls == calls + 1 && s_world.Equals(beforeWorldRotation),
                    "A rejected Rotation update must not replay or change world rotation.");
                calls = s_calls;
                Program.Throws<InvalidOperationException>(() => pose.WorldRotation = Quaternion.IDENTITY);
                Program.Check(s_calls == calls + 1 && s_world.Equals(beforeWorldRotation),
                    "A rejected WorldRotation update must not replay or change world rotation.");
            }
            s_rejection = BridgeStatus.OK;
            s_alive = false;
            Program.Throws<InvalidOperationException>(() => { _ = pose.WorldRotation; });
            Program.Throws<InvalidOperationException>(() => pose.Rotation = Quaternion.IDENTITY);
            Program.Throws<InvalidOperationException>(() => pose.Position = Vector3.ZERO);
            Program.Throws<InvalidOperationException>(() => pose.Rotate(Vector3.UP, 10f));
            Program.Check(s_local.Equals(beforeRotation), "A retained expired Transform must not modify a replacement object.");
            s_alive = true;
            Transform foreign = new GameObject(303, 99, "foreign-guid").Transform;
            Program.Throws<InvalidOperationException>(() => { _ = foreign.Forward; });
            Program.Throws<InvalidOperationException>(() => new GameObject(0, 42, "missing-guid").Transform.LookAt(Vector3.ZERO));

            bool rejectedThread = false;
            calls = s_calls;
            Thread thread = new(() =>
            {
                ScriptApiRuntime.Current = scope;
                try { pose.Rotate(Vector3.UP, 10f); }
                catch (InvalidOperationException) { rejectedThread = true; }
                finally { ScriptApiRuntime.Current = null; }
            });
            thread.Start(); thread.Join();
            Program.Check(rejectedThread && s_calls == calls, "Copying a scope to another thread must not authorize a pose operation.");
        }
        finally { ScriptApiRuntime.Current = null; s_rejection = BridgeStatus.OK; }
        int completedCalls = s_calls;
        Program.Throws<InvalidOperationException>(() => { _ = pose.LocalRotation; });
        Program.Throws<InvalidOperationException>(() => pose.LocalScale = Vector3.ONE);
        Program.Throws<InvalidOperationException>(() => pose.LookAt(Vector3.ZERO));
        Program.Throws<InvalidOperationException>(() => { _ = pose.Position; });
        Program.Throws<InvalidOperationException>(() => { _ = pose.Rotation; });
        Program.Throws<InvalidOperationException>(() => pose.Position = Vector3.ONE);
        Program.Throws<InvalidOperationException>(() => pose.Rotation = Quaternion.IDENTITY);
        Program.Throws<InvalidOperationException>(() => pose.WorldRotation = Quaternion.IDENTITY);
        Program.Check(s_calls == completedCalls, "A retained facade must reject access after the callback before native dispatch.");
    }

    static void CheckWorldPoseAliases(Transform pose)
    {
        int calls = s_calls;
        Program.Check(pose.Position.Equals(s_worldPosition) && s_calls == calls + 1
            && s_last.Op == BridgeOp.TRANSFORM_READ && s_last.Index == 1,
            "Position must read the world position with one request.");
        calls = s_calls;
        Program.Check(pose.Rotation.Equals(s_world) && s_calls == calls + 1
            && s_last.Op == BridgeOp.TRANSFORM_READ_ROTATION && s_last.Index == 1,
            "Rotation must read the world rotation with one request.");

        Vector3 localPosition = s_localPosition;
        Quaternion localRotation = s_local;
        Vector3 position = new(6f, -2f, 8f);
        calls = s_calls;
        pose.Position = position;
        Program.Check(s_calls == calls + 1 && s_worldPosition.Equals(position) && s_localPosition.Equals(localPosition)
            && s_last.Op == BridgeOp.TRANSFORM_WRITE_WORLD && s_last.X == 6f && s_last.Y == -2f && s_last.Z == 8f,
            "Position must submit one unchanged world-space position through WorldPosition's operation.");
        Program.Check(s_last.Integer == 202 && s_last.Generation == 7 && s_last.InstanceId == 9
            && s_last.OwnerId == 101 && s_last.CallId == 13, "Position must retain the target and the current caller authority.");
        Quaternion rotation = new(2f, -3f, 4f, 5f);
        calls = s_calls;
        pose.Rotation = rotation;
        Program.Check(s_calls == calls + 1 && s_world.Equals(rotation) && s_local.Equals(localRotation)
            && s_last.Op == BridgeOp.TRANSFORM_WRITE_WORLD_ROTATION && s_last.X == 2f && s_last.Y == -3f
            && s_last.Z == 4f && s_last.W == 5f, "Rotation must submit an unchanged world-space quaternion exactly once.");
        Program.Check(s_last.Integer == 202 && s_last.Generation == 7 && s_last.InstanceId == 9
            && s_last.OwnerId == 101 && s_last.CallId == 13, "World rotation must retain the target and the current caller authority.");
        calls = s_calls;
        Quaternion boundaryRotation = new(Mathf.EPSILON, 0f, 0f, 0f);
        pose.WorldRotation = boundaryRotation;
        Program.Check(s_calls == calls + 1 && s_world.Equals(boundaryRotation)
            && s_last.Op == BridgeOp.TRANSFORM_WRITE_WORLD_ROTATION,
            "WorldRotation must share the world operation and accept the exact normalization threshold.");

        calls = s_calls;
        Program.Throws<ArgumentOutOfRangeException>(() => pose.Position = new Vector3(0f, float.PositiveInfinity, 0f));
        Program.Throws<ArgumentOutOfRangeException>(() => pose.Rotation = default);
        Program.Throws<ArgumentOutOfRangeException>(() => pose.Rotation = new Quaternion(float.NaN, 0f, 0f, 1f));
        Program.Throws<ArgumentOutOfRangeException>(() => pose.WorldRotation = new Quaternion(Mathf.EPSILON * 0.5f, 0f, 0f, 0f));
        Program.Throws<ArgumentOutOfRangeException>(() => pose.WorldRotation = new Quaternion(float.MaxValue, 0f, 0f, 1f));
        Program.Check(s_calls == calls && s_worldPosition.Equals(position) && s_world.Equals(boundaryRotation),
            "Invalid world pose values must be rejected before dispatch.");
    }

    static IScriptNativeBridge CreateBridge()
    {
        Type type = typeof(EntryPoint).GetNestedType("NativeBridge", BindingFlags.NonPublic)!;
        ConstructorInfo constructor = type.GetConstructor(BindingFlags.NonPublic | BindingFlags.Instance, null, [typeof(nint)], null)!;
        nint address = (nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&Dispatch;
        return (IScriptNativeBridge)constructor.Invoke([address]);
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        s_last = *request; s_calls++;
        BridgeStatus status = s_rejection;
        if (status == BridgeStatus.OK && (request->Integer == 0 || !s_alive)) status = BridgeStatus.NOT_FOUND;
        if (status == BridgeStatus.OK && request->Integer == 303) status = BridgeStatus.SCENE_MISMATCH;
        *reply = new BridgeReply { Status = status };
        if (status != BridgeStatus.OK) return (int)status;
        switch (request->Op)
        {
            case BridgeOp.TRANSFORM_READ:
                Vector3 position = request->Index == 0 ? s_localPosition : s_worldPosition;
                reply->X = position.X; reply->Y = position.Y; reply->Z = position.Z;
                break;
            case BridgeOp.TRANSFORM_WRITE_WORLD:
                s_worldPosition = new Vector3(request->X, request->Y, request->Z);
                break;
            case BridgeOp.TRANSFORM_READ_ROTATION:
                Quaternion rotation = request->Index == 0 ? s_local : s_world;
                reply->X = rotation.X; reply->Y = rotation.Y; reply->Z = rotation.Z; reply->W = rotation.W;
                break;
            case BridgeOp.TRANSFORM_WRITE_LOCAL_ROTATION: s_local = new Quaternion(request->X, request->Y, request->Z, request->W); break;
            case BridgeOp.TRANSFORM_WRITE_WORLD_ROTATION: s_world = new Quaternion(request->X, request->Y, request->Z, request->W); break;
            case BridgeOp.TRANSFORM_READ_SCALE: reply->X = s_scale.X; reply->Y = s_scale.Y; reply->Z = s_scale.Z; break;
            case BridgeOp.TRANSFORM_WRITE_LOCAL_SCALE: s_scale = new Vector3(request->X, request->Y, request->Z); break;
            case BridgeOp.TRANSFORM_READ_DIRECTION:
                Vector3 axis = request->Index == 0 ? Vector3.FORWARD : request->Index == 1 ? Vector3.RIGHT : Vector3.UP;
                Vector3 direction = s_world * axis;
                reply->X = direction.X; reply->Y = direction.Y; reply->Z = direction.Z;
                break;
        }
        return 0;
    }

    static void Near(Vector3 actual, Vector3 expected)
        => Program.Check((actual - expected).Length < 0.00002f, "Direction must use the world rotation and native basis axis.");
    sealed class NoComponents : IScriptComponentLookup
    { public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => []; }
}
