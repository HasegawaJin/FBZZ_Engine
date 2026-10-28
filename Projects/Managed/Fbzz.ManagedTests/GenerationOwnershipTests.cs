/// @file    GenerationOwnershipTests.cs
/// @brief   parked Edit・Play・candidate の共存と lease を越える破壊の拒否を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class GenerationOwnershipTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("OwnedEditGenerationSurvivesPlayReloadCandidates", RetainedEdit),
        ("GenerationLeaseCountsAndResourceSlotsRemainExplicit", LeaseCounts)
    ];
    static void RetainedEdit()
    {
        using ManagedScriptRuntime runtime = new(new Program.RecordingBridge());
        Success(runtime.LoadCandidate(Program.FixturePath("V1"), 1, true));
        Success(runtime.CreateInstance(1, "ManagedFixtures.HealthComponent", 101, out ulong original));
        Program.BindFixture(runtime, 1, original);
        Success(runtime.RetainGeneration(1));
        Success(runtime.LoadCandidate(Program.FixturePath("V2"), 2, true));
        Success(runtime.CreateInstance(2, "ManagedFixtures.HealthComponent", 202, out ulong play));
        Program.BindFixture(runtime, 2, play, 202, 202, "target-guid");
        Success(runtime.LoadCandidate(Program.FixturePath("V2"), 3, true));
        Program.Check(runtime.GetCatalog(1) is not null && runtime.GetCatalog(2) is not null && runtime.GetCatalog(3) is not null,
            "An owned Edit context and both unowned Play/candidate contexts must coexist without replacing original CLR instances.");
        Program.Check(runtime.RetireGeneration(1).Status == BridgeStatus.PENDING_LIMIT && runtime.TryInstance(1, original, out _),
            "Retiring an owned generation must reject without clearing its original instances or catalog.");
        Success(runtime.RetireGeneration(2));
        Program.Check(!runtime.TryInstance(2, play, out _) && runtime.TryInstance(1, original, out _),
            "Play retirement must not clear parked Edit records.");
        Success(runtime.LoadCandidate(Program.FixturePath("V2"), 4, true));
        Success(runtime.RetireGeneration(3)); Success(runtime.RetireGeneration(4));
        Success(runtime.ReleaseGeneration(1));
        Program.Check(runtime.TryInstance(1, original, out _), "Lease release itself must not recreate or retire the original instance.");
        Success(runtime.RetireGeneration(1));
        Program.Check(runtime.GetCatalog(1) is null && !runtime.TryInstance(1, original, out _), "Explicit retirement is valid only after ownership is released.");
    }
    static void LeaseCounts()
    {
        using ManagedScriptRuntime runtime = new();
        Success(runtime.LoadCandidate(Program.FixturePath("V1"), 1, true));
        Success(runtime.RetainGeneration(1)); Success(runtime.RetainGeneration(1));
        Success(runtime.ReleaseGeneration(1));
        Program.Check(runtime.RetireGeneration(1).Status == BridgeStatus.PENDING_LIMIT, "Every retained lease must be balanced before retirement.");
        Success(runtime.ReleaseGeneration(1));
        Program.Check(runtime.ReleaseGeneration(1).Status == BridgeStatus.INVALID_ARGUMENT, "An unbalanced release must not underflow ownership.");
        Success(runtime.LoadCandidate(Program.FixturePath("V2"), 2, true));
        Program.Check(runtime.LoadCandidate(Program.FixturePath("V2"), 3, true).Status == BridgeStatus.PENDING_LIMIT,
            "Without retained ownership, the configured current/candidate resource budget remains enforced.");
    }
    static void Success(ManagedRuntimeResult result) => Program.Check(result.Success, result.Message);
}
