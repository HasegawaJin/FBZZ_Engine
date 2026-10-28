/// @file    AssetLeaseTests.cs
/// @brief   SDK asset lease reply validation and exact Script ownership checks.
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Collections.Generic;
using Fbzz;

namespace Fbzz.ManagedTests;

internal static class AssetLeaseTests
{
    internal static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(AssetStatusRejectsMalformedTransportValues), AssetStatusRejectsMalformedTransportValues),
        (nameof(ForeignScriptAssetsCannotAcquireAnOrphanedLease), ForeignScriptAssetsCannotAcquireAnOrphanedLease),
        (nameof(ReleaseIsIdempotentAndRetirementNeedsNoCallback), ReleaseIsIdempotentAndRetirementNeedsNoCallback)
    ];

    static void AssetStatusRejectsMalformedTransportValues()
    {
        AssetLoadStatus status = AssetLease.ParseStatus(new([6, 0, 1, 0, 7], ["18446744073709551615", "ready"], 42, null));
        Program.Check(status.State == AssetLoadState.READY && status.PublishedRevision == 42
            && status.ResidentBytes == ulong.MaxValue && status.UpdateFailed, "A failed refresh may preserve the previous READY asset and exact byte count.");
        foreach (NativeServiceResult invalid in new[] {
            new NativeServiceResult([9, 0, 0, 0, 0], ["0", ""], 1, null),
            new NativeServiceResult([6.5f, 0, 0, 0, 0], ["0", ""], 1, null),
            new NativeServiceResult([6, 0, 2, 0, 0], ["0", ""], 1, null),
            new NativeServiceResult([6, 0, 0, 255.5f, 0], ["0", ""], 1, null),
            new NativeServiceResult([6, 0, 0, 0, 0], ["00", ""], 1, null),
            new NativeServiceResult([6, 0, 0, 0, 0], ["18446744073709551616", ""], 1, null),
            new NativeServiceResult([6, 0, 0, 0, 0], ["0", ""], (long)uint.MaxValue + 1, null),
            new NativeServiceResult([6, 0, 0, 0], ["0", ""], 1, null)
        }) Program.Throws<InvalidOperationException>(() => AssetLease.ParseStatus(invalid));
    }

    static void ForeignScriptAssetsCannotAcquireAnOrphanedLease()
    {
        IScriptComponentLookup components = new EmptyComponents();
        AssetBridge bridge = new();
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try
        {
            ScriptBinding owner = new(1, 9, 101); owner.GameObject = new GameObject(202, 42, "owner");
            ScriptBinding foreign = new(1, 10, 102); foreign.GameObject = new GameObject(202, 42, "owner");
            ScriptApiRuntime.Current = new(bridge, components, 1, 9, 101, 13, 42, ScriptCallback.UPDATE);
            Program.Throws<InvalidOperationException>(() => foreign.Assets.Request(new TextureAssetRef("asset-guid")));
            Program.Check(bridge.AssetRequests == 0, "A foreign Script facade is rejected before native acquisition.");
            AssetLease? lease = owner.Assets.Request(new TextureAssetRef("asset-guid"));
            if (lease is null) throw new InvalidOperationException("The exact calling Script should acquire a lease.");
            Program.Check(bridge.AssetRequests == 1, "The exact calling Script may acquire a lease.");
            lease.Dispose();
        }
        finally { ScriptApiRuntime.Current = previous; }
    }

    static void ReleaseIsIdempotentAndRetirementNeedsNoCallback()
    {
        IScriptComponentLookup components = new EmptyComponents(); AssetBridge bridge = new();
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try
        {
            ScriptBinding owner = new(1, 9, 101); owner.GameObject = new GameObject(202, 42, "owner");
            ScriptApiRuntime.Current = new(bridge, components, 1, 9, 101, 14, 42, ScriptCallback.UPDATE);
            AssetLease lease = owner.Assets.Request(new TextureAssetRef("asset-guid"))!;
            Program.Check(lease.Release() && !lease.Release() && bridge.ReleaseCalls == 1, "Repeated Release performs one native release.");
            AssetLease retired = owner.Assets.Request(new TextureAssetRef("asset-guid"))!;
            owner.Assets.InvalidateOwned();
            Program.Check(retired.IsReleased && !retired.Release(), "Automatic retirement invalidates retained handles.");
            ScriptApiRuntime.Current = null;
            retired.Dispose();
            Program.Check(bridge.ReleaseCalls == 1, "Dispose after retirement needs no callback or native authority.");
        }
        finally { ScriptApiRuntime.Current = previous; }
    }

    sealed class EmptyComponents : IScriptComponentLookup
    {
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
    }

    sealed class AssetBridge : Program.RecordingBridge, IScriptNativeBridge
    {
        internal int AssetRequests;
        internal int ReleaseCalls;
        NativeServiceResult IScriptNativeBridge.InvokeService(ScriptExecutionScope scope, ulong objectId, uint service, uint method,
            long integer, ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related, bool readOnly)
        {
            if (service != (uint)ScriptServiceKind.SERVICE_ASSETS) throw new InvalidOperationException("Unexpected service in asset test.");
            if (method == 0) { AssetRequests++; return new([], [], 77, null); }
            if (method == 4) { ReleaseCalls++; return new([], [], 1, null); }
            throw new InvalidOperationException("Unexpected asset service operation.");
        }
    }
}
