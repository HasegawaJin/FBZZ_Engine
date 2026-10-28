/// @file    ParticleServicesApiTests.cs
/// @brief   粒子 facade の exact lifetime・型付き packet・enum/catalog 契約の回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;
namespace Fbzz.ManagedTests;
internal static class ParticleServicesApiTests
{
    internal static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(ParticleGroupsTransferExactEnumsAssetsAndMasks), ParticleGroupsTransferExactEnumsAssetsAndMasks),
        (nameof(ParticleDiagnosticsAndCatalogKeepRequestedAndActualPathsDistinct), ParticleDiagnosticsAndCatalogKeepRequestedAndActualPathsDistinct),
        (nameof(ParticleSnapshotPropertiesArePureStoredValues), ParticleSnapshotPropertiesArePureStoredValues)
    ];
    static void ParticleGroupsTransferExactEnumsAssetsAndMasks()
    {
        using Fixture fixture = new(); ParticleSystem particle = new(fixture.Target, 91);
        particle.FlowFieldChannels = 0x80000001;
        Program.Check(fixture.Bridge.Selector == 67 && fixture.Bridge.Integer == 0x80000001L && fixture.Bridge.Service == 189,
            "Particle masks must preserve the high uint32 bit in the exact service packet.");
        particle.SetCollision(ParticleCollisionMode.PLANE, ParticleCollisionResponse.STOP, 0.2f, 0.7f);
        Program.Check(fixture.Bridge.Selector == 50 && fixture.Bridge.Integer == 0x202 && fixture.Bridge.Values.SequenceEqual(new[] { 0.2f, 0.7f }),
            "Collision mode, response and scalars must change together through one packet.");
        fixture.Bridge.Reply = new([], [], 1, null);
        Program.Check(particle.SetMeshShape(new ModelAssetRef("model-guid"), -1, 2, true, -3)
            && fixture.Bridge.Selector == 38 && fixture.Bridge.Integer == -1 && fixture.Bridge.Text.SequenceEqual(new[] { "guid:model-guid" })
            && fixture.Bridge.Values.SequenceEqual(new[] { 2f, -3f, 1f }), "Model shape must use typed GUID and retain signed normal velocity.");
        Program.Check(particle.TrySetMaterial(new MaterialAssetRef("material-guid")) && fixture.Bridge.Selector == 73
            && fixture.Bridge.Text[0] == "guid:material-guid", "Material assignment must reach the particle service rather than MaterialComponent slots.");
        int calls = fixture.Bridge.Calls;
        Program.Throws<ArgumentOutOfRangeException>(() => particle.SimulationMode = (ParticleSimulationMode)9);
        Program.Throws<ArgumentOutOfRangeException>(() => particle.SetNoise(float.NaN));
        Program.Check(fixture.Bridge.Calls == calls, "Malformed enums and nonfinite groups must not dispatch.");
        fixture.Bridge.Birth = 92;
        Program.Throws<InvalidOperationException>(() => particle.SetSphereShape(1));
        Program.Check(fixture.Bridge.Calls == calls, "A retained facade must not mutate a replacement Particle row.");
    }
    static void ParticleDiagnosticsAndCatalogKeepRequestedAndActualPathsDistinct()
    {
        using Fixture fixture = new(); ParticleSystem particle = new(fixture.Target, 91);
        fixture.Bridge.Reply = new([], [], 1, null);
        Program.Check(particle.SimulationMode == ParticleSimulationMode.GPU && fixture.Bridge.Selector == 43 && fixture.Bridge.ReadOnly,
            "Requested simulation mode must be an explicit readonly query.");
        fixture.Bridge.Reply = new([], [], 9, null);
        Program.Check(particle.GpuFallbackReason == ParticleGpuFallbackReason.SELF_SHADOW && fixture.Bridge.Selector == 69,
            "The native material-dependent reason must remain independently observable.");
        fixture.Bridge.Reply = new([], [], 99, null);
        Program.Throws<InvalidOperationException>(() => { _ = particle.GpuFallbackReason; });
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement particleType = document.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == "Fbzz.ParticleSystem");
        foreach (string name in new[] { "SimulationMode", "SimulationSpace", "FlowFieldChannels", "Prewarm" }) {
            JsonElement contract = particleType.GetProperty("members").EnumerateArray().Single(member => member.GetProperty("name").GetString() == name).GetProperty("contract");
            Program.Check(contract.GetProperty("runModeAccess").GetProperty("read").GetArrayLength() == 2
                && contract.GetProperty("runModeAccess").GetProperty("write").GetArrayLength() == 1,
                "Catalog must expose Edit/Play reads separately from Play-only Particle writes.");
        }
        Program.Check((int)ParticleGpuFallbackReason.RESERVED_VECTOR_FIELD == 11 && (int)ParticleGpuFallbackReason.LIGHT == 12,
            "New GPU fallback reasons must preserve reserved ABI values.");
    }
    static void ParticleSnapshotPropertiesArePureStoredValues()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        foreach ((string type, string[] properties) in new[] {
            ("Fbzz.ParticlePlaybackSettings", new[] { "Loop", "ClearOnStop", "Duration" }),
            ("Fbzz.ParticleMeshShapeSettings", new[] { "Model", "MeshIndex", "Scale", "NormalVelocity", "FollowSkinnedAnimation" }),
            ("Fbzz.ParticleRenderSettings", new[] { "Mode", "StretchScale" }),
            ("Fbzz.ParticleCollisionSettings", new[] { "Mode", "Response", "Radius", "Bounciness" }),
            ("Fbzz.ParticleSubEmitterSettings", new[] { "Birth", "Death", "Collision", "BurstCount" }),
            ("Fbzz.ParticleNoiseSettings", new[] { "Strength", "Frequency", "Speed" })
        }) {
            JsonElement snapshot = document.RootElement.GetProperty("types").EnumerateArray().Single(value => value.GetProperty("name").GetString() == type);
            foreach (string property in properties) {
                JsonElement contract = snapshot.GetProperty("members").EnumerateArray().Single(value => value.GetProperty("name").GetString() == property).GetProperty("contract");
                Program.Check(contract.GetProperty("pure").GetBoolean() && contract.GetProperty("thread").GetString() == "any"
                    && contract.GetProperty("allowedCallbacks").GetProperty("read")[0].GetString() == "any",
                    "Owned Particle snapshot properties must remain pure stored reads, including after a readonly draw query.");
            }
        }
    }
    sealed class Fixture : IDisposable, IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous = ScriptApiRuntime.Current;
        internal Fixture() { ScriptApiRuntime.Current = new(Bridge, this, 7, 9, 101, 13, 42, ScriptCallback.UPDATE); }
        internal Bridge Bridge { get; } = new();
        internal GameObject Target { get; } = new(202, 42, "target");
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
        public void Dispose() { ScriptApiRuntime.Current = m_previous; }
    }
    sealed class Bridge : Program.RecordingBridge, IScriptNativeBridge
    {
        internal ulong Birth = 91;
        internal int Calls;
        internal uint Service, Selector;
        internal long Integer;
        internal float[] Values = [];
        internal string[] Text = [];
        internal bool ReadOnly;
        internal NativeServiceResult Reply = new([], [], 0, null);
        ulong IScriptNativeBridge.ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind)
            => objectId == 202 && kind == (uint)NativeComponentKind.PARTICLE_SYSTEM ? Birth : 0;
        NativeServiceResult IScriptNativeBridge.InvokeService(ScriptExecutionScope scope, ulong objectId, uint service, uint method, long integer,
            ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related, bool readOnly)
        {
            Calls++; Service = service; Selector = method; Integer = integer; Values = values.ToArray(); Text = text.ToArray(); ReadOnly = readOnly;
            return Reply;
        }
    }
}
