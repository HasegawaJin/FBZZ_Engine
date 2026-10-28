/// @file    GizmoMetadataAbiTests.cs
/// @brief   実 Host ABI の optional Gizmo metadata と継承・保持世代を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static unsafe class GizmoMetadataAbiTests
{
    internal static (string Name, Action Test)[] Cases =>
    [
        ("GizmoMetadataAbiDistinguishesAbsentBothAndInheritedCallbacks", DistinguishesCallbacks),
        ("GizmoMetadataAbiKeepsCallbackMasksInsideTheirCodeGeneration", KeepsGenerationMasks),
        ("CallbackAbiRejectsUndefinedValuesBeforeExecutingOrInspectingFaultState", RejectsUndefinedCallbacks),
        ("LifecycleMetadataAbiDistinguishesAbsentInheritedAndOverriddenUpdates", LifecyclePresence),
        ("LifecycleMetadataAbiKeepsPresenceInsideItsCodeGeneration", LifecycleGenerations)
    ];
    static int s_nativeCalls;
    readonly record struct Metadata(uint Index, uint Flags, long Mask);

    static void Initialize()
    {
        s_nativeCalls = 0;
        BridgeRequest request = new() {
            AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.INITIALIZE, Generation = BridgeAbi.SCHEMA_HASH,
            Index = (uint)sizeof(BridgeRequest), Flags = (uint)sizeof(BridgeReply),
            Integer = (long)(nint)(delegate* unmanaged[Cdecl]<BridgeRequest*, BridgeReply*, int>)&NativeDispatch
        };
        Program.Check(Dispatch(request).Status == BridgeStatus.OK, "The real metadata ABI must initialize.");
    }

    static void Load(ulong generation, string version)
    {
        BridgeRequest request = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.LOAD_CANDIDATE,
            Generation = generation, Flags = BridgeAbi.INTERNAL_MODE };
        Program.Check(Dispatch(request, Program.FixturePath(version)).Status == BridgeStatus.OK, "The metadata fixture generation must load.");
    }

    static Dictionary<string, Metadata> ReadTypes(ulong generation)
    {
        Dictionary<string, Metadata> types = new(StringComparer.Ordinal);
        byte[] output = new byte[2048];
        BridgeRequest request = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.GET_TYPE, Generation = generation, Flags = 1 };
        BridgeReply first = Dispatch(request, output: output);
        Program.Check(first.Status == BridgeStatus.OK && first.Index != 0, "Type metadata must retain its type-count reply.");
        uint count = first.Index;
        for (uint index = 0; index < count; ++index) {
            request.Index = index; request.Flags = 1;
            BridgeReply named = Dispatch(request, output: output);
            Program.Check(named.Status == BridgeStatus.OK && named.Index == count && (named.Flags & 2U) == 0,
                "The type-name selector must retain its existing Flags contract.");
            string name = Encoding.UTF8.GetString(output.AsSpan(0, checked((int)named.OutputBytes)));
            request.Flags = 0;
            BridgeReply metadata = Dispatch(request, output: output);
            Program.Check(metadata.Status == BridgeStatus.OK && metadata.Index == count && (metadata.Flags & 2U) != 0,
                "The persistent-ID selector must advertise optional Gizmo metadata explicitly.");
            Program.Check((metadata.Flags & 1U) == (named.Flags & 1U) && metadata.Integer >= 0 && metadata.Integer <= 3,
                "ExecuteAlways must retain bit zero and the Gizmo mask must contain only its two declared bits.");
            Program.Check(metadata.OutputBytes != 0, "Gizmo metadata must not replace the existing persistent-ID payload.");
            types.Add(name, new(index, metadata.Flags, metadata.Integer));
        }
        return types;
    }

    static void DistinguishesCallbacks()
    {
        Initialize();
        try {
            Load(700, "V1");
            Dictionary<string, Metadata> types = ReadTypes(700);
            Program.Check(types["ManagedFixtures.HealthComponent"].Mask == 0 && (types["ManagedFixtures.HealthComponent"].Flags & 3U) == 2,
                "An entry with no Gizmo override must explicitly advertise known absence without becoming ExecuteAlways.");
            Program.Check(types["ManagedFixtures.GizmoProbeComponent"].Mask == 3,
                "Regular and selected Gizmo overrides must advertise both bits.");
            Program.Check(types["ManagedFixtures.InheritedSelectedGizmoComponent"].Mask == 2
                && (types["ManagedFixtures.InheritedSelectedGizmoComponent"].Flags & 3U) == 3,
                "An inherited selected-only override must be discovered while ExecuteAlways remains true.");
            Metadata inherited = types["ManagedFixtures.InheritedSelectedGizmoComponent"];
            BridgeRequest request = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.GET_TYPE,
                Generation = 700, Index = inherited.Index, Flags = 0 };
            BridgeReply discovery = Dispatch(request);
            Program.Check(discovery.Status == BridgeStatus.BUFFER_TOO_SMALL && discovery.Integer == 2 && (discovery.Flags & 3U) == 3,
                "Readonly capacity discovery must carry the same optional metadata without a payload retry changing it.");
            request.Flags = 4;
            Program.Check(Dispatch(request, output: new byte[2048]).Integer == 0,
                "Execution-order metadata must not be overwritten by the Gizmo mask.");
            Program.Check(s_nativeCalls == 0, "Passive type metadata must not execute user draw callbacks or native Engine services.");
        } finally { Shutdown(); }
    }

    static void KeepsGenerationMasks()
    {
        Initialize();
        try {
            Load(710, "V1");
            Metadata original = ReadTypes(710)["ManagedFixtures.GizmoReloadMaskComponent"];
            Program.Check(original.Mask == 1, "The first generation must expose its regular-only override.");
            Load(711, "V2");
            Program.Check(ReadTypes(711)["ManagedFixtures.GizmoReloadMaskComponent"].Mask == 2,
                "The replacement generation must expose its selected-only override.");
            Program.Check(ReadTypes(710)["ManagedFixtures.GizmoReloadMaskComponent"].Mask == 1,
                "A retained old generation must keep its own callbacks instead of receiving the replacement mask.");
            BridgeRequest retire = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.RETIRE_GENERATION, Generation = 710 };
            Program.Check(Dispatch(retire).Status == BridgeStatus.OK, "The old metadata generation must retire.");
            BridgeRequest stale = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.GET_TYPE,
                Generation = 710, Index = original.Index, Flags = 0 };
            Program.Check(Dispatch(stale, output: new byte[2048]).Status == BridgeStatus.STALE_GENERATION,
                "Retired generation metadata must reject rather than revive a cached callback mask.");
            Program.Check(ReadTypes(711)["ManagedFixtures.GizmoReloadMaskComponent"].Mask == 2 && s_nativeCalls == 0,
                "Retiring another generation must not change the live metadata or enter native services.");
        } finally { Shutdown(); }
    }

    static void LifecyclePresence()
    {
        Initialize();
        try {
            Load(730, "V1");
            Dictionary<string, Metadata> types = ReadTypes(730);
            Program.Check((types["ManagedFixtures.HealthComponent"].Flags & 60U) == 4U,
                "Base lifecycle defaults must advertise known absence for all three updates.");
            foreach (string name in new[] { "ManagedFixtures.InheritedOnlyCallbackComponent", "ManagedFixtures.InheritedCallbackComponent" })
                Program.Check((types[name].Flags & 60U) == 12U,
                    "Inherited and overridden Update methods must both remain present, with missing Fixed/Late callbacks absent.");
            Metadata entry = types["ManagedFixtures.LifecyclePresenceComponent"];
            Program.Check((entry.Flags & 60U) == 12U && entry.Mask == 0,
                "Lifecycle presence must retain the separate two-bit Gizmo mask.");
            BridgeRequest request = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.GET_TYPE,
                Generation = 730, Index = entry.Index, Flags = 0 };
            BridgeReply discovery = Dispatch(request);
            Program.Check(discovery.Status == BridgeStatus.BUFFER_TOO_SMALL && discovery.Flags == entry.Flags,
                "Capacity discovery must expose the same lifecycle capability and presence bits.");
            foreach (uint selector in new uint[] { 1, 2, 3, 4 }) {
                request.Flags = selector;
                Program.Check((Dispatch(request, output: new byte[16384]).Flags & 60U) == 0,
                    "Optional presence bits must not change other GET_TYPE selectors.");
            }
            Program.Check(s_nativeCalls == 0, "Lifecycle discovery must not invoke author code or native services.");
        } finally { Shutdown(); }
    }

    static void LifecycleGenerations()
    {
        Initialize();
        try {
            Load(740, "V1");
            Load(741, "V2");
            Program.Check((ReadTypes(740)["ManagedFixtures.LifecyclePresenceComponent"].Flags & 60U) == 12U,
                "A retained first generation must keep its Update-only callback presence.");
            Program.Check((ReadTypes(741)["ManagedFixtures.LifecyclePresenceComponent"].Flags & 60U) == 52U,
                "The replacement generation must advertise Fixed/Late updates and no Update override.");
            BridgeRequest retire = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.RETIRE_GENERATION, Generation = 740 };
            Program.Check(Dispatch(retire).Status == BridgeStatus.OK, "The old lifecycle metadata must retire independently.");
            Program.Check((ReadTypes(741)["ManagedFixtures.LifecyclePresenceComponent"].Flags & 60U) == 52U && s_nativeCalls == 0,
                "Retiring old metadata must preserve replacement presence without entering native services.");
        } finally { Shutdown(); }
    }

    static void RejectsUndefinedCallbacks()
    {
        Initialize();
        try {
            Load(720, "V1");
            BridgeRequest create = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.CREATE, Generation = 720, OwnerId = 101 };
            BridgeReply created = Dispatch(create, "ManagedFixtures.ThrowingCallbackComponent\n11111111-1111-4111-8111-111111111111");
            Program.Check(created.Status == BridgeStatus.OK, "The callback boundary fixture must construct.");
            BridgeRequest bind = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.BIND_OWNER,
                Generation = 720, InstanceId = created.InstanceId, OwnerId = 101, Integer = 101, CallId = 1 };
            Program.Check(Dispatch(bind, "owner-101\n22222222-2222-4222-8222-222222222222").Status == BridgeStatus.OK,
                "The callback fixture must bind before invocation.");
            BridgeRequest invoke = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.INVOKE,
                Generation = 720, InstanceId = created.InstanceId, OwnerId = 101, CallId = 1 };
            invoke.Index = (uint)ScriptCallback.AWAKE;
            Program.Check(Dispatch(invoke).Status == BridgeStatus.OK, "The defined zero-valued Awake callback must remain valid.");
            foreach (uint index in new uint[] { 25, 99, uint.MaxValue }) {
                invoke.Index = index;
                Program.Check(Dispatch(invoke).Status == BridgeStatus.INVALID_ARGUMENT, "Undefined callback values must be rejected at the ABI boundary.");
            }
            BridgeRequest read = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.READ_FIELD,
                Generation = 720, InstanceId = created.InstanceId, OwnerId = 101 };
            Program.Check(Dispatch(read, "updateCount").Integer == 0, "Rejected callback values must not execute user code.");
            invoke.Index = (uint)ScriptCallback.UPDATE;
            Program.Check(Dispatch(invoke).Status == BridgeStatus.CALLBACK_FAILED && Dispatch(read, "updateCount").Integer == 1,
                "A defined callback must still execute exactly once and preserve its failure status.");
            invoke.Index = uint.MaxValue;
            Program.Check(Dispatch(invoke).Status == BridgeStatus.INVALID_ARGUMENT && Dispatch(read, "updateCount").Integer == 1,
                "Undefined callback rejection must precede the already-faulted instance check.");
        } finally { Shutdown(); }
    }

    static void Shutdown()
    {
        BridgeRequest request = new() { AbiVersion = BridgeAbi.VERSION, Op = BridgeOp.SHUTDOWN };
        Program.Check(Dispatch(request).Status == BridgeStatus.OK, "The metadata ABI session must release.");
    }

    static BridgeReply Dispatch(BridgeRequest request, string input = "", byte[]? output = null)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(input);
        fixed (byte* source = bytes) fixed (byte* destination = output) {
            request.Input = source; request.InputBytes = (uint)bytes.Length;
            request.Output = destination; request.OutputCapacity = (uint)(output?.Length ?? 0);
            BridgeReply reply = default;
            int status = EntryPoint.DispatchManaged(ref request, ref reply);
            Program.Check(status == (int)reply.Status, "The ABI return code must match the metadata reply status.");
            return reply;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new Type[] { typeof(CallConvCdecl) })]
    static int NativeDispatch(BridgeRequest* request, BridgeReply* reply)
    {
        ++s_nativeCalls;
        if (reply is not null) reply->Status = BridgeStatus.UNSUPPORTED;
        return (int)BridgeStatus.UNSUPPORTED;
    }
}
