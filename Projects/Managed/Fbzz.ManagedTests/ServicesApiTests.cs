/// @file    ServicesApiTests.cs
/// @brief   公開 Component facade と Host packet の caller/target 分離・拒否・再送禁止を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;
internal static unsafe class ServicesApiTests
{
    static BridgeRequest s_request;
    static byte[] s_input = [];
    static byte[] s_response = [];
    static BridgeStatus s_status;
    static long s_integer;
    static ulong s_scene;
    static int s_calls;
    static ulong s_componentLifetime = 91;
    static bool s_captureInput = true;
    static bool s_failLaterServicePage;
    static bool s_storeTransfer;
    static readonly MemoryStream s_storeBytes = new();
    static int s_storeChunks, s_storeCommits, s_storeAborts, s_storeFailChunk;
    static bool s_materialTransfer, s_materialCommitSuccess = true;
    static double[] s_materialValues = [];
    static readonly List<double> s_materialUploaded = [];
    static int s_materialPages, s_materialCloses, s_materialWriteChunks, s_materialCommits, s_materialFailPage;
    static byte[] s_displaySnapshot = [];
    static int s_displayPages, s_displayCloses;
    internal static void Run()
    {
        using ApiFixture fixture = new();
        Reset(); Reply([2f]);
        Animator animator = new(fixture.Target);
        Program.Check(animator.GetSpeed() == 2f && s_request.Op == BridgeOp.SERVICE_ANIMATOR && s_request.Index == 12
            && s_request.Flags == 1 && s_request.Integer == 202 && s_request.OwnerId == 101
            && s_request.InstanceId == 9 && s_request.Generation == 7 && s_request.CallId == 13,
            "A retained Component must keep target identity separate from current caller authority.");
        Reset(); Reply();
        animator.SetFloat("移動速度", 3f);
        Program.Check(s_calls == 1 && s_request.Op == BridgeOp.SERVICE_ANIMATOR && s_request.Flags == 0
            && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)) == 1
            && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(20)) == 1
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(24)) == 3f
            && Encoding.UTF8.GetString(s_input.AsSpan(32)) == "移動速度", "Float and UTF-8 strings must share one explicit value packet.");
        Reset(); Reply();
        animator.SetUpdateLod(true, 10f, 50f, 0.05f, 0.1f);
        Program.Check(s_calls == 1 && s_request.Op == BridgeOp.SERVICE_ANIMATOR && s_request.Index == 50
            && BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == 1
            && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)) == 4
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(24)) == 10f
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(36)) == 0.1f,
            "Pose LOD must retain caller/target identity and transfer four finite distance/interval values in one mutation.");
        int lodCalls = s_calls;
        Program.Throws<ArgumentOutOfRangeException>(() => animator.SetUpdateLod(true, float.NaN));
        Program.Check(s_calls == lodCalls, "Nonfinite pose LOD values must reject before dispatch.");
        Reset(); Reply();
        NavMeshAgent agent = new(fixture.Target); agent.SetTarget(fixture.Other, 0.7f);
        Program.Check(BinaryPrimitives.ReadUInt64LittleEndian(s_input) == 303 && s_request.Integer == 202,
            "Related object identity must be distinct from target and caller.");
        int calls = s_calls;
        Program.Throws<InvalidOperationException>(() => agent.SetTarget(new GameObject(404, 99, "foreign")));
        Program.Throws<ArgumentOutOfRangeException>(() => animator.SetSpeed(float.PositiveInfinity));
        Program.Check(s_calls == calls, "Foreign references and nonfinite numbers must reject before dispatch.");
        ScriptApiRuntime.Current = null;
        Program.Throws<InvalidOperationException>(() => animator.GetSpeed());
        ScriptApiRuntime.Current = fixture.Scope;
        Program.Check(s_calls == calls, "A facade must not carry callback authority after the callback.");

        Reset(); Reply([], [new string('あ', 700)]);
        UI ui = new(fixture.Target);
        Program.Check(ui.GetText().Length == 700 && s_calls == 2, "A readonly text query may retry capacity and retain UTF-8.");
        foreach (BridgeStatus status in new[] { BridgeStatus.BUFFER_TOO_SMALL, BridgeStatus.NOT_FOUND, BridgeStatus.INVALID_ARGUMENT })
        {
            Reset(status); Reply();
            Program.Throws<InvalidOperationException>(() => ui.SetText("new"));
            Program.Check(s_calls == 1, "A mutating service must never resend after any failure.");
        }
        Reset(); Reply([float.NaN]);
        Program.Throws<InvalidDataException>(() => animator.GetSpeed());
        Reset(); Reply([1]); s_scene = 99;
        Program.Throws<InvalidDataException>(() => animator.GetSpeed());
        Reset(); s_response = new byte[19];
        Program.Throws<InvalidDataException>(() => animator.GetSpeed());

        Reset(); Reply([0.1f]);
        SphereCollider sphere = new(fixture.Target);
        ScriptApiRuntime.Current = fixture.Scope;
        Program.Throws<InvalidOperationException>(() => sphere.Radius = 1f);
        Program.Check(s_calls == 0, "Collider mutation requires FixedUpdate before marshalling.");
        ScriptApiRuntime.Current = fixture.MakeScope(ScriptCallback.FIXED_UPDATE);
        Reply(); sphere.Radius = 1f;
        Program.Check(s_request.Op == BridgeOp.COLLIDER_WRITE && s_request.Index == 3
            && BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == 3, "A concrete shape must retain its native kind.");
        RigidBody body = new(fixture.Target); Reset(); Reply();
        body.Rotation = Quaternion.IDENTITY;
        Program.Check(s_request.Op == BridgeOp.PHYSICS_BODY_WRITE_ROTATION && s_input.Length == 40
            && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)) == 4, "Body rotation must use the common four-float packet.");
        int before = s_calls;
        Program.Throws<ArgumentOutOfRangeException>(() => body.Rotation = default);
        Program.Check(s_calls == before, "Degenerate body rotation cannot reach native assertion paths.");
        CharacterController character = new(fixture.Target); Reset(); Reply();
        ScriptApiRuntime.Current = fixture.Scope;
        Program.Throws<InvalidOperationException>(() => character.JumpAtVelocity(5f));
        Program.Check(s_calls == 0, "Character body mutation requires FixedUpdate.");
        ScriptApiRuntime.Current = fixture.MakeScope(ScriptCallback.FIXED_UPDATE);
        character.JumpAtVelocity(5f);
        Program.Check(s_request.Op == BridgeOp.SERVICE_CHARACTER && s_request.Index == 2, "The typed jump command must dispatch once.");

        ScriptApiRuntime.Current = fixture.Scope;
        Reset(); s_response = new byte[4];
        PhysicsQueryOptions options = new(4, QueryTriggerMode.ONLY, fixture.Other);
        Program.Check(!fixture.Script.Physics.Raycast(Vector3.ZERO, Vector3.FORWARD, 20f, options, out _)
            && s_request.Op == BridgeOp.PHYSICS_RAYCAST_FILTERED && s_input.Length == 28
            && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(12)) == 4
            && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)) == 2
            && BinaryPrimitives.ReadUInt64LittleEndian(s_input.AsSpan(20)) == 303,
            "Filtered queries must preserve Layer mask, trigger policy and ignored lifetime token.");
        Reset(); s_response = new byte[4];
        fixture.Script.Physics.OverlapSphere(Vector3.ZERO, 2f, default(PhysicsQueryOptions));
        Program.Check(s_input.Length == 16 && BinaryPrimitives.ReadUInt32LittleEndian(s_input) == uint.MaxValue,
            "Default options must include all Layers, while explicit zero is an empty mask.");
        Reset(); s_response = new byte[4];
        fixture.Script.Physics.SphereCast(Vector3.ZERO, 2f, Vector3.RIGHT, 3f, new PhysicsQueryOptions(0), out _);
        Program.Check(s_input.Length == 32 && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)) == 0,
            "Sphere cast must retain radius before its filter suffix.");

        Reset(); Reply(new SynthSpec().Values, integer: new SynthSpec { Seed = 0xfedcba98u, SampleRate = 48000 }.Packed);
        ScriptAudio audio = fixture.Target.Audio;
        SynthSpec spec = audio.MakeSpec(SynthPreset.HIT, 0x12345678u);
        Program.Check(spec.Seed == 0xfedcba98u && spec.SampleRate == 48000 && s_request.Index == 29
            && unchecked((ulong)BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8))) == ((ulong)SynthPreset.HIT << 32 | 0x12345678u),
            "Synth seed/sample-rate must remain exact integer bit patterns.");
        Reset(); Reply(integer: 1, text: ["17"]);
        AudioLoop loop = new(); Program.Check(audio.UpdateLoop(loop, "loop.wav", 0.5f) && loop.Handle == 17,
            "Loop updates return an opaque token without exposing native audio IDs.");
        Reset(); Reply(integer: 1);
        Program.Check(loop.IsValid && s_request.Index == 45, "Loop validity is checked in current caller scope.");
        Program.Check(NativeComponentFactory.GetKind(typeof(TerrainCollider)) == 19
            && NativeComponentFactory.GetKind(typeof(TextField)) == 23 && NativeComponentFactory.GetKind(typeof(Draggable)) == 25,
            "Facade factories must share native registration identifiers.");
        RetainedFacadeIdentityNeverRevives(fixture);
        SmallServicePacketsDoNotAllocate(fixture);
        ServiceCollectionsHaveNoPacketSizedTotalLimit(fixture);
        MalformedBlendWeightRepliesPreserveCaller(fixture);
        TerrainDirectionalContracts(fixture);
        RuntimeServiceContracts(fixture);
        RenderDisplayJointContracts(fixture);
        DisplayResolutionSnapshotsPreserveCallerBuffers(fixture);
    }

    static void RenderDisplayJointContracts(ApiFixture fixture)
    {
        Reset(); Reply(integer: 1); fixture.Script.Render.ShadowCascades = int.MaxValue;
        Program.Check(s_request.Op == BridgeOp.SERVICE_RENDER && s_request.Index == 16
            && BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == int.MaxValue, "Integral render controls must cross without float rounding.");
        Reset(); Reply(integer: 1); fixture.Script.Render.ShadowCascades = int.MinValue;
        Program.Check(BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == int.MinValue, "Negative cascade input must preserve the native integer clamp contract.");
        Reset(); Reply(integer: 0);
        Program.Check(!fixture.Script.Display.TrySetResolution(1280, 720) && s_request.Op == BridgeOp.SERVICE_DISPLAY
            && Encoding.UTF8.GetString(s_input).Contains("720", StringComparison.Ordinal), "Unavailable Editor/host window mutation must return false with exact dimensions.");
        Reset(); Program.Throws<ArgumentOutOfRangeException>(() => fixture.Script.Render.Brightness = float.NaN);
        Program.Throws<ArgumentOutOfRangeException>(() => fixture.Script.Display.TrySetResolution(0, 720));
        Program.Check(s_calls == 0, "Invalid numeric/display input rejects before dispatch.");
        Joint joint = new(fixture.Target); Reset(); Reply(integer: 1);
        Program.Throws<InvalidOperationException>(() => joint.TryConnectFixed(fixture.Other)); Program.Check(s_calls == 0, "Joint mutations reject Update before native access.");
        ScriptApiRuntime.Current = fixture.MakeScope(ScriptCallback.FIXED_UPDATE);
        Program.Check(joint.TryConnectHinge(fixture.Other, Vector3.UP, new Vector3(1, 2, 3), new Vector3(4, 5, 6))
            && s_request.Op == BridgeOp.SERVICE_JOINT && s_request.Index == 11, "FixedUpdate joint calls must preserve related owner and local-anchor packet shape.");
        int before = s_calls; Program.Throws<ArgumentOutOfRangeException>(() => joint.TryConnectSlider(fixture.Other, Vector3.ZERO));
        Program.Check(s_calls == before && NativeComponentFactory.GetKind(typeof(Joint)) == 34, "Zero axes reject before dispatch and Joint maps to its real row kind.");
        ScriptApiRuntime.Current = fixture.Scope;
    }

    static void DisplayResolutionSnapshotsPreserveCallerBuffers(ApiFixture fixture)
    {
        static byte[] Snapshot(uint firstWidth, uint firstHeight)
        {
            byte[] bytes = new byte[20];
            BinaryPrimitives.WriteUInt32LittleEndian(bytes, 2);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), firstWidth);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), firstHeight);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), 2560);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), 1440);
            return bytes;
        }
        DisplayResolution sentinel = new(-9, -7);
        DisplayResolution[] shortBuffer = [sentinel];
        Reset(); s_displaySnapshot = Snapshot(1920, 1080);
        CollectionReadResult shortResult = fixture.Script.Display.GetResolutions(shortBuffer);
        Program.Check(shortResult.Required == 2 && shortResult.Written == 0 && shortBuffer[0] == sentinel,
            "A short display mode buffer must report Required without publishing a prefix.");

        DisplayResolution[] output = [sentinel, sentinel, sentinel];
        Reset(); s_displaySnapshot = Snapshot(1920, 1080);
        CollectionReadResult full = fixture.Script.Display.GetResolutions(output);
        Program.Check(full.Required == 2 && full.Written == 2 && output[0] == new DisplayResolution(1920, 1080)
            && output[1] == new DisplayResolution(2560, 1440) && output[2] == sentinel,
            "A complete display snapshot must preserve its order and caller tail.");

        DisplayResolution[] unchanged = [sentinel, sentinel];
        Reset(); s_displaySnapshot = Snapshot(0, 1080);
        Program.Throws<InvalidDataException>(() => fixture.Script.Display.GetResolutions(unchanged));
        Program.Check(unchanged[0] == sentinel && unchanged[1] == sentinel,
            "Malformed display modes must fail before changing caller storage.");

        static byte[] Modes(uint count, uint invalidIndex = uint.MaxValue)
        {
            byte[] bytes = new byte[checked(4 + (int)count * 8)];
            BinaryPrimitives.WriteUInt32LittleEndian(bytes, count);
            for (uint index = 0; index < count; index++) {
                BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4 + (int)index * 8), index == invalidIndex ? 0U : 1920U);
                BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8 + (int)index * 8), 1080U);
            }
            return bytes;
        }
        Reset(); s_displaySnapshot = Modes(1000, 999);
        DisplayResolution[] lateMalformed = [sentinel];
        Program.Throws<InvalidDataException>(() => fixture.Script.Display.GetResolutions(lateMalformed));
        Program.Check(lateMalformed[0] == sentinel && s_displayPages == 1 && s_displayCloses == 1,
            "A short buffer must validate malformed entries already in the fetched page without publishing a prefix.");

        Reset(); s_displaySnapshot = Modes(10000);
        DisplayResolution[] largeShort = [sentinel];
        CollectionReadResult largeCount = fixture.Script.Display.GetResolutions(largeShort);
        Program.Check(largeCount.Required == 10000 && largeCount.Written == 0 && largeShort[0] == sentinel
            && s_displayPages == 1 && s_displayCloses == 1, "A count-only short read must not fetch a later page solely to validate unused modes.");
    }

    internal static void AuthoringServices()
    {
        using ApiFixture fixture = new();
        Material material = new(fixture.Target);
        MaterialCollections(material);
        Program.Check(NativeComponentFactory.GetKind(typeof(Material)) == 29, "Material must be the real component kind29.");
        Reset(); Reply(integer: 1);
        Program.Check(material.TrySetInt(2, "stage", int.MinValue) && s_request.Op == BridgeOp.SERVICE_MATERIAL
            && s_request.Index == 9 && BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == 2
            && Encoding.UTF8.GetString(s_input).Contains("-2147483648", StringComparison.Ordinal),
            "Explicit slot and full signed integer must cross independently without float rounding.");
        Reset(); Reply([1], integer: int.MinValue);
        Program.Check(material.TryGetInt(2, "stage", out int signed) && signed == int.MinValue, "Typed integer reads must preserve all bits.");
        Reset(); Reply([1], integer: uint.MaxValue);
        Program.Check(material.TryGetUInt(2, "mask", out uint unsigned) && unsigned == uint.MaxValue, "Typed unsigned reads must preserve all bits.");
        Reset(); Reply(integer: 0);
        Program.Check(!material.TryGetFloat(2, "missing", out float missing) && missing == 0
            && !material.TryGetColor(2, "missing", out Vector4 color) && color.X == 0 && color.Y == 0 && color.Z == 0 && color.W == 0,
            "Missing material properties must use documented out defaults.");
        int calls = s_calls;
        Program.Throws<ArgumentOutOfRangeException>(() => material.TrySetFloat(-1, "value", 1));
        Program.Throws<ArgumentException>(() => material.TrySetTexture(0, "baseTexture", default));
        Program.Check(calls == s_calls, "Invalid slots and empty texture references reject before native dispatch.");
        Reset(); Reply(integer: 0);
        Program.Check(!material.TrySetSharedMaterial(0, new MaterialAssetRef("11111111111111111111111111111111"))
            && s_request.Index == 24 && s_calls == 1, "Rejected shared assets must not replay a mutation.");
        Reset(); Reply(integer: 1);
        Program.Check(material.TrySetSharedMaterial(0, default) && s_calls == 1, "An empty typed reference must explicitly clear the selected shared material.");
        Reset(); Reply(integer: 1);
        Program.Check(fixture.Script.Save.Load() && s_request.Op == BridgeOp.SERVICE_SAVE && s_request.Index == 7 && s_calls == 1,
            "Load must target the real save store with one mutating dispatch.");
        Reset(); Reply(integer: int.MinValue);
        Program.Check(fixture.Script.Config.GetInt("limit", int.MaxValue) == int.MinValue
            && s_request.Op == BridgeOp.SERVICE_CONFIG && s_request.Index == 10
            && BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == int.MaxValue,
            "Config must have a distinct service and full-precision default value.");
        Reset(); Reply(integer: 0);
        Program.Check(!fixture.Script.Save.Load() && s_calls == 1, "A failed load must return false without retry.");
        Reset(); Reply(text: ["fallback"]);
        Program.Check(fixture.Script.Save.GetString("absent", "fallback") == "fallback", "Owned fallback strings must retain their actual result.");
        string fallback = new('f', 2 * 1024 * 1024);
        Reset(); Reply(integer: 0);
        Program.Check(ReferenceEquals(fixture.Script.Save.GetString("absent", fallback), fallback) && s_input.Length < 100,
            "Arbitrary caller fallback must stay local and retain its exact reference.");
        Reset(); Reply(text: [""], integer: 1);
        Program.Check(fixture.Script.Save.GetString("empty", fallback) == "", "An existing empty string must not select the fallback.");
        Reset();
        Program.Throws<EncoderFallbackException>(() => fixture.Script.Save.SetString("small", "bad\uD800"));
        Program.Throws<EncoderFallbackException>(() => fixture.Script.Save.SetString("large", new string('x', 2 * 1024 * 1024) + "\uD800"));
        Program.Check(s_calls == 0, "Small and large malformed Unicode must reject before native mutation.");
        string uploaded = new string('x', 2 * 1024 * 1024) + "あ\0tail";
        Reset(); Reply(); s_storeTransfer = true; s_storeBytes.SetLength(0);
        s_storeChunks = 0; s_storeCommits = 0; s_storeAborts = 0; s_storeFailChunk = 0;
        fixture.Script.Save.SetString("large", uploaded);
        Program.Check(s_storeChunks > 1 && s_storeCommits == 1 && s_storeAborts == 0
            && new UTF8Encoding(false, true).GetString(s_storeBytes.ToArray()) == uploaded,
            "A large immutable string must send bounded raw UTF8 packets and commit once, preserving NUL and Unicode.");
        Reset(); Reply(); s_storeTransfer = true; s_storeBytes.SetLength(0);
        s_storeChunks = 0; s_storeCommits = 0; s_storeAborts = 0; s_storeFailChunk = 2;
        Program.Throws<InvalidOperationException>(() => fixture.Script.Config.SetString("large", uploaded));
        Program.Check(s_storeCommits == 0 && s_storeAborts == 1, "Failed upload must abort unpublished reception without replaying commit.");
        s_storeTransfer = false;
        s_componentLifetime++;
        Program.Check(!material.IsAlive && new Material(fixture.Target) != material, "An explicit relookup must use a new material birth; old facade cannot revive.");
    }

    static void MaterialCollections(Material material)
    {
        s_materialValues = new double[9004];
        for (int index = 0; index < s_materialValues.Length; index++) s_materialValues[index] = index;
        s_materialValues[0] = int.MinValue; s_materialValues[^1] = uint.MaxValue;
        double[] shortBuffer = Enumerable.Repeat(-77d, 9003).ToArray();
        Reset(); s_materialTransfer = true;
        Program.Check(!material.TryGetValues(1, "array", shortBuffer, out CollectionReadResult shortCapacity)
            && shortCapacity.Required == 9004 && shortCapacity.Written == 0 && shortBuffer.All(value => value == -77)
            && s_materialPages == 0 && s_materialCloses == 1,
            "Short caller storage must remain unchanged and close count-only metadata without requesting value pages.");
        double[] full = Enumerable.Repeat(-77d, 9007).ToArray();
        Reset(); s_materialTransfer = true;
        Program.Check(material.TryGetValues(1, "array", full, out CollectionReadResult capacity)
            && capacity.Required == 9004 && capacity.Written == 9004 && full.AsSpan(0, 9004).SequenceEqual(s_materialValues)
            && full[9004] == -77 && full[9006] == -77 && s_materialPages == 2,
            "Internal packets must preserve all double values and caller tail without a packet-sized result cap.");
        Array.Fill(full, -77); Reset(); s_materialTransfer = true; s_materialFailPage = 2;
        Program.Throws<InvalidOperationException>(() => material.TryGetValues(1, "array", full, out _));
        Program.Check(full.All(value => value == -77) && s_materialCloses == 1, "A later page failure must not publish a prefix into caller storage.");
        Reset(); s_materialTransfer = true;
        Program.Check(material.TrySetValues(1, "array", s_materialValues) && s_materialCommits == 1 && s_materialWriteChunks == 2
            && s_materialUploaded.SequenceEqual(s_materialValues), "Write packets must feed one commit without float rounding.");
        Reset(); s_materialTransfer = true; s_materialCommitSuccess = false;
        Program.Check(!material.TrySetValues(1, "array", s_materialValues) && s_materialCommits == 1, "Native rejection must remain false without commit replay.");
        Reset(); s_materialTransfer = true;
        Program.Throws<ArgumentOutOfRangeException>(() => material.TrySetValues(1, "array", new double[] { 1, double.NaN }));
        Program.Check(s_calls == 0, "Nonfinite input must reject before opening a candidate.");
        s_materialTransfer = false;
        float[] rowMajor = Enumerable.Range(0, 16).Select(index => (float)index).ToArray(); Matrix4 matrix = new(rowMajor);
        float[] shortMatrix = Enumerable.Repeat(-77f, 15).ToArray(); CollectionReadResult matrixCapacity = matrix.CopyTo(shortMatrix);
        Program.Check(matrixCapacity.Required == 16 && matrixCapacity.Written == 0 && shortMatrix.All(value => value == -77), "Matrix short CopyTo preserves output.");
        float[] copied = Enumerable.Repeat(-77f, 19).ToArray(); matrixCapacity = matrix.CopyTo(copied);
        Program.Check(matrixCapacity.Written == 16 && copied.AsSpan(0, 16).SequenceEqual(rowMajor) && matrix[2, 3] == 11 && copied[16] == -77,
            "Matrix packing must use row-major values and preserve extra capacity.");
    }

    static void RetainedFacadeIdentityNeverRevives(ApiFixture fixture)
    {
        Reset(); Reply();
        Animator held = fixture.Target.GetComponent<Animator>()!;
        Program.Check(ReferenceEquals(held, fixture.Target.GetComponent<Animator>()), "Repeated native lookup must reuse the same live facade.");
        Animator duplicate = (Animator)NativeComponentFactory.Create(typeof(Animator), new GameObject(202, 42, "target"), held.LifetimeToken)!;
        ScriptApiRuntime.Current = null;
        Program.Check(held == duplicate && held.Equals(duplicate) && held.GetHashCode() == duplicate.GetHashCode(),
            "Native identity equality must be pure and survive reconstructed owner wrappers.");
        ScriptApiRuntime.Current = fixture.Scope;
        s_componentLifetime = 92;
        Program.Check(!held.IsAlive, "Removal/re-addition must expire a retained Component.");
        Program.Throws<InvalidOperationException>(() => held.SetSpeed(3f));
        Animator current = fixture.Target.GetComponent<Animator>()!;
        Program.Check(current != held && current.IsAlive, "Explicit lookup must return the replacement identity.");
        Program.Check(held == duplicate, "Expiration must not change immutable Component equality.");
        Program.Check(!typeof(Component).IsAssignableFrom(typeof(UI)) && NativeComponentFactory.GetType(7) is null,
            "Owner UI services must not advertise a synthetic Component row.");
        Reset();
    }
    static void SmallServicePacketsDoNotAllocate(ApiFixture fixture)
    {
        Reset(); Reply([0.5f]);
        AudioSource source = new(fixture.Target, 91);
        s_captureInput = false;
        try
        {
            for (int index = 0; index < 100; index++) _ = source.GetVolume();
            long before = GC.GetAllocatedBytesForCurrentThread();
            float total = 0f;
            for (int index = 0; index < 1000; index++) total += source.GetVolume();
            long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Program.Check(total == 500f && allocated == 0, "Warm scalar service reads must not allocate packet or value arrays.");
            Reply([1f, 2f, 3f, 4f]);
            for (int index = 0; index < 100; index++) _ = fixture.Target.UI.GetImageColor();
            before = GC.GetAllocatedBytesForCurrentThread();
            for (int index = 0; index < 1000; index++) _ = fixture.Target.UI.GetImageColor();
            allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Program.Check(allocated == 0, "Warm owner-service vector reads must use owned inline values without packet allocation.");
        }
        finally { s_captureInput = true; }
    }

    static void ServiceCollectionsHaveNoPacketSizedTotalLimit(ApiFixture fixture)
    {
        const int total = 32771;
        float[] values = new float[total]; string[] names = new string[total];
        for (int index = 0; index < total; index++) { values[index] = index; names[index] = "歩行" + index.ToString(System.Globalization.CultureInfo.InvariantCulture); }
        names[0] = new string('動', 3000);
        Reset(); Reply(values, names);
        Animator animator = new(fixture.Target, 91);
        BlendWeight sentinel = new("sentinel", -1);
        BlendWeight[] insufficient = new BlendWeight[total - 1]; Array.Fill(insufficient, sentinel);
        CollectionReadResult count = animator.GetCurrentBlendWeights(Span<BlendWeight>.Empty);
        Program.Check(count.Required == total && count.Written == 0 && s_calls == 1, "Count-only service lookup must read only its first bounded packet beyond32768.");
        CollectionReadResult shortResult = animator.GetCurrentBlendWeights(insufficient);
        Program.Check(shortResult.Required == total && shortResult.Written == 0, "Insufficient service capacity must report the whole collection.");
        foreach (BlendWeight value in insufficient) Program.Check(value.Name == "sentinel" && value.Weight == -1, "Insufficient service capacity must leave output untouched.");
        BlendWeight[] output = new BlendWeight[total + 1]; output[total] = sentinel;
        CollectionReadResult copied = animator.GetCurrentBlendWeights(output);
        Program.Check(copied.Complete && copied.Required == total && copied.Written == total && output[total].Name == "sentinel", "Caller output must receive all entries with its unused tail unchanged.");
        for (int index = 0; index < total; index++) Program.Check(output[index].Name == names[index] && output[index].Weight == values[index], "Paged service decoding must preserve weights and UTF8 strings across packet boundaries.");
        BlendWeight[] owned = animator.GetCurrentBlendWeights();
        Program.Check(owned.Length == total && owned[total - 1].Name == names[total - 1], "Ordinary array result must include the full collection.");
        Reset(); Reply(text: [new string('界', 6 * 1024 * 1024)]);
        string large = fixture.Target.UI.GetText();
        Program.Check(large.Length == 6 * 1024 * 1024 && large[0] == '界' && large[^1] == '界', "A UTF8 service result larger than16MiB must stream without a total byte cap.");
        Reset(); Reply([1, 2, 3, 4], ["", "", "", ""]);
        BlendWeight[] reusable = new BlendWeight[4];
        s_captureInput = false;
        try {
            for (int index = 0; index < 100; index++) animator.GetCurrentBlendWeights(reusable);
            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int index = 0; index < 1000; index++) animator.GetCurrentBlendWeights(reusable);
            long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Program.Check(allocated == 0, "Warm caller-buffer service reads with empty names must allocate no transfer or collection arrays.");
        }
        finally { s_captureInput = true; }
    }

    static void MalformedBlendWeightRepliesPreserveCaller(ApiFixture fixture)
    {
        Animator animator = new(fixture.Target, 91);
        BlendWeight sentinel = new("sentinel", -1);
        BlendWeight[] output = new BlendWeight[3];
        void CheckUntouched() => Program.Check(output.All(value => value.Name == sentinel.Name && value.Weight == sentinel.Weight),
            "Every failure must preserve both the active caller range and unused tail.");

        Reset(); Reply([1, float.NaN], ["first", "second"]); Array.Fill(output, sentinel);
        Program.Throws<InvalidDataException>(() => animator.GetCurrentBlendWeights(output)); CheckUntouched();

        Reset(); Reply([1, 2], ["first", "second"]); Array.Fill(output, sentinel);
        s_response[^13] = 0xff;
        Program.Throws<DecoderFallbackException>(() => animator.GetCurrentBlendWeights(output)); CheckUntouched();

        Reset(); Reply([1, 2], ["first", "second"]); Array.Fill(output, sentinel);
        Array.Resize(ref s_response, s_response.Length + 1);
        Program.Throws<InvalidDataException>(() => animator.GetCurrentBlendWeights(output)); CheckUntouched();

        Reset(); Reply([1, 2], ["first", new string('界', 3000)]); Array.Fill(output, sentinel);
        s_failLaterServicePage = true;
        Program.Throws<InvalidDataException>(() => animator.GetCurrentBlendWeights(output));
        Program.Check(s_calls >= 2, "The malformed response must be encountered after a valid first page."); CheckUntouched();

        Reset(); Reply([3, 4], ["after", "failure"]);
        Program.Check(animator.GetCurrentBlendWeights(output).Written == 2 && output[0].Weight == 3 && output[1].Name == "failure"
            && output[2].Name == sentinel.Name, "A failed staged read must not poison the next read or caller tail.");
    }

    static void TerrainDirectionalContracts(ApiFixture fixture)
    {
        Vector3 reference = new(6, -4, 7);
        Reset(); Reply([6,-6,7,0,1,0], integer: 1);
        Program.Check(fixture.Script.Scene.TryGetTerrainFloorAt(reference, 10, out Vector3 floor, out Vector3 normal)
            && floor.Y == -6 && normal.Y == 1 && s_request.Op == BridgeOp.SERVICE_SCENE && s_request.Index == 2
            && s_request.Flags == 1 && BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)) == 4
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(28)) == -4
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(36)) == 10,
            "Scene floor query must transfer reference Y and distance and decode the nearest floor.");
        Reset(); Reply([6,-2,7,0,-1,0], integer: 1);
        Program.Check(fixture.Script.Scene.TryGetTerrainCeilingAt(reference, 10, out Vector3 ceiling, out normal)
            && ceiling.Y == -2 && normal.Y == -1 && s_request.Index == 3, "Scene ceiling uses a separate directional selector.");
        Terrain terrain = new(fixture.Target, 91);
        Reset(); Reply([6,-6,7,0,1,0], integer: 1);
        Program.Check(terrain.TrySampleFloorWorld(reference, 10, out TerrainSurfaceSample sample)
            && sample.Position.Y == -6 && sample.Normal.Y == 1 && s_request.Op == BridgeOp.SERVICE_TERRAIN
            && s_request.Index == 16 && s_request.Integer == 202, "Terrain floor must retain its target identity.");
        Reset(); Reply([6,-2,7,0,-1,0], integer: 1);
        Program.Check(terrain.TrySampleCeilingWorld(reference, 10, out sample) && sample.Position.Y == -2
            && sample.Normal.Y == -1 && s_request.Index == 17, "Terrain ceiling must preserve the downward normal.");
        Reset(); Reply();
        Program.Check(!terrain.TrySampleFloorWorld(reference, 1, out sample) && sample.Position.Y == 0 && sample.Normal.Y == 0,
            "Missing floor must return a default sample without inventing height.");
        Reset(); Reply();
        Program.Check(!fixture.Script.Scene.TryGetTerrainCeilingAt(reference, 1, out ceiling, out normal)
            && ceiling.Y == 0 && normal.Y == 0, "Missing ceiling must return default position and normal.");
        Reset();
        Program.Check(!terrain.TrySampleFloorWorld(reference, -1, out sample)
            && !terrain.TrySampleCeilingWorld(new(6, float.NaN, 7), 10, out sample)
            && !fixture.Script.Scene.TryGetTerrainFloorAt(reference, float.PositiveInfinity, out floor, out normal)
            && s_calls == 0, "Invalid directional numeric inputs must return false before dispatch.");
        ScriptApiRuntime.Current = null;
        Program.Throws<InvalidOperationException>(() => terrain.TrySampleFloorWorld(reference, -1, out _));
        Program.Throws<InvalidOperationException>(() => fixture.Script.Scene.TryGetTerrainFloorAt(reference, -1, out _, out _));
        ScriptApiRuntime.Current = fixture.Scope;
        Reset(); Reply([1], integer: 1);
        Program.Throws<InvalidOperationException>(() => terrain.TrySampleCeilingWorld(reference, 10, out _));
    }

    static void RuntimeServiceContracts(ApiFixture fixture)
    {
        Program.Check(NativeComponentFactory.GetKind(typeof(Light)) == (uint)NativeComponentKind.LIGHT
            && NativeComponentFactory.GetKind(typeof(ParticleSystem)) == (uint)NativeComponentKind.PARTICLE_SYSTEM,
            "Runtime component mapping must match canonical enum IDs within the pending mask.");
        Reset(); Reply([2]); Light light = new(fixture.Target, 91);
        Program.Check(light.Intensity == 2 && s_request.Op == BridgeOp.SERVICE_LIGHT, "Light reads must reach the real component service ID.");
        Reset(); Reply(); light.SetSpotAngles(15, 40);
        Program.Check(s_request.Index == 11 && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(24)) == 15
            && BinaryPrimitives.ReadSingleLittleEndian(s_input.AsSpan(28)) == 40, "Spot angle pairs must transfer atomically.");
        int calls = s_calls; Program.Throws<ArgumentOutOfRangeException>(() => light.SetSpotAngles(50, 30));
        Program.Check(s_calls == calls, "Invalid cone pairs must be rejected before native dispatch.");
        ParticleSystem particle = new(fixture.Target, 91); Reset(); Reply(); particle.Play();
        Program.Check(s_request.Op == BridgeOp.SERVICE_PARTICLE && s_request.Index == 6 && BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == 1,
            "Particle playback defaults to native restart semantics.");
        calls = s_calls; Program.Throws<ArgumentOutOfRangeException>(() => particle.Burst(-1)); Program.Check(s_calls == calls, "Negative bursts must not cross the bridge.");
        Reset(); Reply(integer: 800); Program.Check(fixture.Script.App.WindowWidth == 800 && s_request.Op == BridgeOp.SERVICE_APPLICATION, "App dimensions must use the actual window service.");
        Reset(); Reply(); fixture.Script.App.Quit(); Program.Check(s_request.Index == 4 && s_request.Flags == 0, "Quit must be a single non-replayed intent.");
        Reset(); Reply(text: ["Registered"]); Program.Check(fixture.Script.Scene.RegisteredName == "Registered", "Scene names must come from registered-manager service.");
        Reset(); Reply(integer: 1); Program.Check(fixture.Script.Scene.TryLoadRegistered("Next") && s_request.Op == BridgeOp.SERVICE_SCENE && s_request.Index == 1,
            "Scene transition must report queue acceptance.");
        Reset(); Reply(integer: 1); Program.Check(fixture.Script.Cursor.RequestedVisible && s_request.Index == 4, "Requested visibility must keep the legacy visibility method.");
        Reset(); Reply(integer: 1); Program.Check(fixture.Script.Cursor.Visible && s_request.Index == 4, "Legacy Visible must keep requested-value semantics.");
        Reset(); Reply(integer: 2); Program.Check(fixture.Script.Cursor.RequestedLockMode == CursorLockMode.LOCKED && s_request.Index == 5, "Requested lock mode must keep its stable method.");
        Reset(); Reply(integer: 2); Program.Check(fixture.Script.Cursor.LockMode == CursorLockMode.LOCKED && s_request.Index == 5, "Legacy LockMode must keep requested-value semantics.");
        Reset(); Reply(integer: 0); Program.Check(!fixture.Script.Cursor.EffectiveVisible && s_request.Index == 7 && s_request.Flags == 1, "Effective visibility must use the read-only applied-state method.");
        Reset(); Reply(integer: 0); Program.Check(fixture.Script.Cursor.EffectiveLockMode == CursorLockMode.NONE && s_request.Index == 8, "Host release must be distinguishable from requested Locked state.");
        Reset(); Reply(integer: 0); Program.Check(fixture.Script.Cursor.InputOwner == CursorInputOwner.HOST && s_request.Index == 9, "Input ownership must have an independent query.");
        Reset(); Reply(integer: 3); Program.Check(fixture.Script.Cursor.SuspendedReason == CursorSuspendReason.WINDOW_INACTIVE && s_request.Index == 10, "Focus suspension must have an explicit reason.");
        Reset(); Reply(integer: 0); Program.Check(!fixture.Script.Cursor.WindowActive && s_request.Index == 11, "Window focus must be independent of requested cursor settings.");
        Reset(); Reply(integer: 47); CursorRequest cursor = fixture.Script.Cursor.Push(CursorLockMode.CONFINED, true, (int)CursorPriority.UI);
        Program.Check(s_request.Op == BridgeOp.SERVICE_CURSOR && s_request.Index == 0
            && BinaryPrimitives.ReadInt64LittleEndian(s_input.AsSpan(8)) == 101, "Cursor requests must identify their actual Script owner.");
        Reset(); Reply(integer: 1); Program.Check(cursor.Set(CursorLockMode.LOCKED, false) && s_request.Index == 1, "Cursor policy update must preserve its request.");
        Reset(); Reply(integer: 1); Program.Check(cursor.IsActive && s_request.Index == 3, "Cursor active state must be checked by owned token.");
        Reset(); Reply(integer: 1); Program.Check(cursor.Release() && !cursor.IsActive && s_calls == 1, "Release must retire the managed token once.");
        using JsonDocument catalog = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement particleType = catalog.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == "Fbzz.ParticleSystem");
        JsonElement enabled = particleType.GetProperty("members").EnumerateArray().Single(member => member.GetProperty("name").GetString() == "Enabled");
        Program.Check(enabled.GetProperty("contract").GetProperty("runModeAccess").GetProperty("read").GetArrayLength() == 2
            && enabled.GetProperty("contract").GetProperty("runModeAccess").GetProperty("write").GetArrayLength() == 1,
            "Catalog must distinguish Edit-readable component state from Play-only writes.");
    }

    static void Reset(BridgeStatus status = BridgeStatus.OK)
    {
        s_status = status; s_scene = 42; s_calls = 0; s_input = []; s_response = []; s_integer = 0; s_componentLifetime = 91; s_storeTransfer = false;
        s_failLaterServicePage = false;
        s_materialTransfer = false; s_materialCommitSuccess = true; s_materialFailPage = 0;
        s_materialPages = s_materialCloses = s_materialWriteChunks = s_materialCommits = 0; s_materialUploaded.Clear();
        s_displaySnapshot = [];
        s_displayPages = s_displayCloses = 0;
    }
    static void Reply(float[]? values = null, string[]? text = null, long integer = 0)
    {
        float[] numbers = values ?? []; string[] strings = text ?? [];
        int length = 20 + numbers.Length * 4;
        foreach (string value in strings) length += 4 + Encoding.UTF8.GetByteCount(value);
        s_response = new byte[length]; s_integer = integer;
        BinaryPrimitives.WriteUInt32LittleEndian(s_response, (uint)numbers.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(s_response.AsSpan(4), (uint)strings.Length);
        int offset = 8;
        foreach (float value in numbers) { BinaryPrimitives.WriteSingleLittleEndian(s_response.AsSpan(offset), value); offset += 4; }
        foreach (string value in strings)
        {
            int count = Encoding.UTF8.GetByteCount(value);
            BinaryPrimitives.WriteUInt32LittleEndian(s_response.AsSpan(offset), (uint)count); offset += 4;
            offset += Encoding.UTF8.GetBytes(value, s_response.AsSpan(offset, count));
        }
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static int Dispatch(BridgeRequest* request, BridgeReply* reply)
    {
        if (request->Op == BridgeOp.COMPONENT_READ && request->Flags == 1)
        { *reply = new BridgeReply { Status = BridgeStatus.OK, Integer = unchecked((long)s_componentLifetime) }; return 0; }
        if (request->Op == BridgeOp.OBJECT_IS_ALIVE)
        { *reply = new BridgeReply { Status = BridgeStatus.OK, Integer = 1 }; return 0; }
        s_calls++; s_request = *request;
        if (s_captureInput) s_input = new ReadOnlySpan<byte>(request->Input, checked((int)request->InputBytes)).ToArray();
        if (request->Op == BridgeOp.SERVICE_DISPLAY
            && ((request->Index == 9 && request->Flags == 1) || request->Flags == (512U | 1U) || request->Flags == (768U | 1U))) {
            uint count = BinaryPrimitives.ReadUInt32LittleEndian(s_displaySnapshot);
            if (request->Flags == 1) {
                if (request->OutputCapacity < s_displaySnapshot.Length) {
                    *reply = new BridgeReply { Status = BridgeStatus.BUFFER_TOO_SMALL, CallId = s_scene, Index = count, Integer = 1, Generation = (ulong)s_displaySnapshot.Length };
                    return (int)BridgeStatus.BUFFER_TOO_SMALL;
                }
                *reply = new BridgeReply { Status = BridgeStatus.OK, CallId = s_scene, Index = count, OutputBytes = (uint)s_displaySnapshot.Length };
                s_displaySnapshot.CopyTo(new Span<byte>(request->Output, s_displaySnapshot.Length)); return 0;
            }
            if (request->Flags == (512U | 1U)) {
                ulong offset = ((ulong)request->Reserved1 << 32) | request->Reserved0;
                s_displayPages++;
                int available = s_displaySnapshot.Length - checked((int)offset);
                int length = Math.Min(checked((int)request->OutputCapacity), available);
                s_displaySnapshot.AsSpan(checked((int)offset), length).CopyTo(new Span<byte>(request->Output, length));
                *reply = new BridgeReply { Status = BridgeStatus.OK, CallId = s_scene, Index = count, Generation = (ulong)s_displaySnapshot.Length,
                    Integer = checked((long)(offset + (uint)length)), OutputBytes = (uint)length, Flags = offset + (uint)length == (ulong)s_displaySnapshot.Length ? 1U : 0U };
                return 0;
            }
            if (request->Flags == (768U | 1U)) { s_displayCloses++; *reply = new BridgeReply { Status = BridgeStatus.OK }; return 0; }
        }
        if (s_materialTransfer && request->Op == BridgeOp.SERVICE_MATERIAL) {
            if (request->Index == 30) Reply(text: [s_materialValues.Length.ToString(System.Globalization.CultureInfo.InvariantCulture)], integer: 88);
            else if (request->Index == 32) Reply(integer: 88);
            else if (request->Index == 31) {
                s_materialPages++;
                if (s_materialPages == s_materialFailPage) { *reply = new BridgeReply { Status = BridgeStatus.INVALID_ARGUMENT }; return (int)BridgeStatus.INVALID_ARGUMENT; }
                int offset = checked((int)BinaryPrimitives.ReadUInt64LittleEndian(s_input.AsSpan(8)));
                int count = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)));
                Span<byte> output = new(request->Output, checked((int)request->OutputCapacity));
                for (int index = 0; index < count; index++) BinaryPrimitives.WriteDoubleLittleEndian(output[(index * 8)..], s_materialValues[offset + index]);
                *reply = new BridgeReply { Status = BridgeStatus.OK, CallId = s_scene, Integer = count, OutputBytes = (uint)(count * 8) }; return 0;
            } else if (request->Index == 33) {
                s_materialWriteChunks++;
                int count = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)));
                for (int index = 0; index < count; index++) s_materialUploaded.Add(BinaryPrimitives.ReadDoubleLittleEndian(s_input.AsSpan(24 + index * 8)));
                *reply = new BridgeReply { Status = BridgeStatus.OK, Integer = s_materialUploaded.Count }; return 0;
            } else if (request->Index == 34) {
                s_materialCommits++; *reply = new BridgeReply { Status = BridgeStatus.OK, Integer = s_materialCommitSuccess ? 1 : 0 }; return 0;
            } else if (request->Index == 35) {
                s_materialCloses++; *reply = new BridgeReply { Status = BridgeStatus.OK }; return 0;
            }
        }
        if (s_storeTransfer && request->Op is BridgeOp.SERVICE_SAVE or BridgeOp.SERVICE_CONFIG) {
            long integer = 0;
            if (request->Index == 22) integer = 77;
            else if (request->Index == 23) {
                s_storeChunks++;
                if (s_storeChunks == s_storeFailChunk) { *reply = new BridgeReply { Status = BridgeStatus.INVALID_ARGUMENT }; return (int)BridgeStatus.INVALID_ARGUMENT; }
                int count = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(s_input.AsSpan(16)));
                if (count > 65536 - 24 || s_input.Length != 24 + count) { *reply = new BridgeReply { Status = BridgeStatus.INVALID_ARGUMENT }; return (int)BridgeStatus.INVALID_ARGUMENT; }
                s_storeBytes.Write(s_input.AsSpan(24)); integer = s_storeBytes.Length;
            } else if (request->Index == 24) { s_storeCommits++; integer = 1; }
            else if (request->Index == 25) s_storeAborts++;
            *reply = new BridgeReply { Status = BridgeStatus.OK, CallId = s_scene, Integer = integer, OutputBytes = (uint)s_response.Length };
            s_response.CopyTo(new Span<byte>(request->Output, s_response.Length)); return 0;
        }
        if (request->Flags == 257) {
            ulong offset = ((ulong)request->Reserved1 << 32) | request->Reserved0;
            if (offset > (ulong)s_response.Length) { *reply = new BridgeReply { Status = BridgeStatus.INVALID_ARGUMENT }; return (int)BridgeStatus.INVALID_ARGUMENT; }
            int count = Math.Min((int)request->OutputCapacity, s_response.Length - (int)offset);
            *reply = new BridgeReply { Status = s_status, CallId = s_scene, Integer = s_integer, Generation = (ulong)s_response.Length,
                OutputBytes = (uint)count, Flags = offset + (ulong)count == (ulong)s_response.Length ? 1u : 0u };
            if (s_failLaterServicePage && offset != 0) reply->CallId = s_scene + 1;
            s_response.AsSpan((int)offset, count).CopyTo(new Span<byte>(request->Output, count));
            return (int)s_status;
        }
        *reply = new BridgeReply { Status = s_status, CallId = s_scene, Integer = s_integer, OutputBytes = (uint)s_response.Length };
        if (s_status != BridgeStatus.OK) return (int)s_status;
        if (request->OutputCapacity < s_response.Length) { reply->Status = BridgeStatus.BUFFER_TOO_SMALL; return (int)BridgeStatus.BUFFER_TOO_SMALL; }
        s_response.CopyTo(new Span<byte>(request->Output, s_response.Length)); return 0;
    }
    sealed class OwnerScript : Script { }
    sealed class ApiFixture : IDisposable, IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous = ScriptApiRuntime.Current;
        internal ApiFixture()
        {
            Type type = typeof(EntryPoint).GetNestedType("NativeBridge", BindingFlags.NonPublic)!;
            ConstructorInfo constructor = type.GetConstructor(BindingFlags.NonPublic | BindingFlags.Instance, null, [typeof(nint)], null)!;
            nint address = (nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&Dispatch;
            Bridge = (IScriptNativeBridge)constructor.Invoke([address]);
            ScriptBinding binding = new(7, 9, 101) { GameObject = new GameObject(101, 42, "owner") };
            Script = new OwnerScript { Binding = binding }; Scope = MakeScope(ScriptCallback.UPDATE);
            ScriptApiRuntime.Current = Scope;
        }
        internal IScriptNativeBridge Bridge { get; }
        internal Script Script { get; }
        internal ScriptExecutionScope Scope { get; }
        internal GameObject Target { get; } = new(202, 42, "target");
        internal GameObject Other { get; } = new(303, 42, "other");
        internal ScriptExecutionScope MakeScope(ScriptCallback callback) => new(Bridge, this, 7, 9, 101, 13, 42, callback);
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
        public void Dispose() { ScriptApiRuntime.Current = m_previous; }
    }
}
