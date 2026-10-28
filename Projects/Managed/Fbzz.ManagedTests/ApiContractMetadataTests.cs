/// @file    ApiContractMetadataTests.cs
/// @brief   SDK 宣言契約を実際の読み取り・失敗出力・lifecycle 署名と照合する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class ApiContractMetadataTests
{
    internal static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(CatalogLifecycleAndGenericConstraintsMatchCompiledSdk), CatalogLifecycleAndGenericConstraintsMatchCompiledSdk),
        (nameof(ReadAndMutationScopesDescribeConnectSeparately), ReadAndMutationScopesDescribeConnectSeparately),
        (nameof(BoundsReadWorksInUpdateWhileShapeMutationRequiresFixedUpdate), BoundsReadWorksInUpdateWhileShapeMutationRequiresFixedUpdate),
        (nameof(TryConversionsExposeTheirActualFailureOutputs), TryConversionsExposeTheirActualFailureOutputs),
        (nameof(ImportantNumericalContractsRemainOnTheirDeclarations), ImportantNumericalContractsRemainOnTheirDeclarations)
        , (nameof(TypedAssetValuesArePureAndAvailableWithoutCallbacks), TypedAssetValuesArePureAndAvailableWithoutCallbacks)
        , (nameof(WorldFamilyQueriesExposeReadonlyDrawAndFixedGeometryBoundaries), WorldFamilyQueriesExposeReadonlyDrawAndFixedGeometryBoundaries)
        , (nameof(NativeLookupDrawAuthorityIsExplicitAndConditional), NativeLookupDrawAuthorityIsExplicitAndConditional)
        , (nameof(RenderDisplayAndJointQueriesMatchNativeAvailability), RenderDisplayAndJointQueriesMatchNativeAvailability)
    ];

    static JsonElement Member(JsonElement root, string type, string name) => root.GetProperty("types").EnumerateArray()
        .Single(value => value.GetProperty("name").GetString() == type).GetProperty("members").EnumerateArray()
        .First(value => value.GetProperty("name").GetString() == name);
    static void TypedAssetValuesArePureAndAvailableWithoutCallbacks()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        foreach (string type in new[] { "Fbzz.MaterialAssetRef", "Fbzz.TextureAssetRef" })
            foreach (string name in new[] { ".ctor", "Reference", "IsEmpty" }) {
                JsonElement contract = Member(document.RootElement, type, name).GetProperty("contract");
                Program.Check(contract.GetProperty("pure").GetBoolean() && contract.GetProperty("thread").GetString() == "any"
                    && contract.GetProperty("knowledge").GetString() == "declared", "GUID values must expose actual managed-only catalog contracts.");
                Program.Check(contract.GetProperty("allowedCallbacks").GetProperty(name == ".ctor" ? "call" : "read")[0].GetString() == "any",
                    "GUID value operations are available without a callback or Scene.");
            }
    }
    static bool Allows(JsonElement member, string access, string callback) => member.GetProperty("contract")
        .GetProperty("allowedCallbacks").GetProperty(access).EnumerateArray().Any(value => value.GetString() == callback);

    static void NativeLookupDrawAuthorityIsExplicitAndConditional()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement root = document.RootElement;
        foreach (string type in new[] { "Fbzz.Component", "Fbzz.GameObject" })
            foreach (string name in new[] { "GetComponent", "GetComponents" }) {
                JsonElement[] methods = root.GetProperty("types").EnumerateArray().Single(value => value.GetProperty("name").GetString() == type)
                    .GetProperty("members").EnumerateArray().Where(value => value.GetProperty("name").GetString() == name).ToArray();
                Program.Check(methods.Length == (name == "GetComponent" ? 1 : 2), "Every lookup overload must publish its conditional policy.");
                foreach (JsonElement method in methods) {
                    JsonElement extra = method.GetProperty("contract").GetProperty("conditionalAllowedCallbacks");
                    Program.Check(!Allows(method, "call", "DrawGizmos") && Allows(method, "call", "Connect")
                        && extra.GetProperty("condition").GetString() == "native_component_generic_argument"
                        && extra.GetProperty("call").EnumerateArray().Any(value => value.GetString() == "DrawGizmos"),
                        "Native generic lookups may query draw scope without granting managed Script retrieval that authority.");
                }
            }
        foreach (string name in new[] { "TryGet", "IsAlive", "IsMissing" }) {
            JsonElement method = Member(root, "Fbzz.ComponentRef`1", name);
            string access = name == "TryGet" ? "call" : "read";
            JsonElement extra = method.GetProperty("contract").GetProperty("conditionalAllowedCallbacks");
            Program.Check(!Allows(method, access, "DrawGizmos") && extra.GetProperty("condition").GetString() == "native_reference_identity"
                && extra.GetProperty(access).EnumerateArray().Any(value => value.GetString() == "DrawGizmosSelected"),
                "Held reference metadata must restrict draw permission to exact native reference identities.");
        }
        Program.Check(Member(root, "Fbzz.ComponentRef`1", "Resolve").GetProperty("contract")
            .GetProperty("conditionalAllowedCallbacks").GetProperty("call").GetArrayLength() == 0,
            "Explicit persistent re-resolution must not inherit native held-reference draw permission.");
    }

    static void RenderDisplayAndJointQueriesMatchNativeAvailability()
    {
        Program.Check(NativeComponentFactory.GetKind(typeof(Joint)) == (uint)NativeComponentKind.JOINT
            && ScriptAuthoringMetadata.NativeName(typeof(Joint)) == "JointComponent"
            && !ScriptAuthoringMetadata.Validate(typeof(JointRequirementFixture), false).Any(),
            "Joint must remain in the native Component allowlist and resolve its native authoring name.");
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement root = document.RootElement;
        foreach (string name in new[] { "Brightness", "BloomScale", "RenderScale", "RenderWidth", "RenderHeight", "Quality", "ShadowsEnabled", "ShadowResolution", "ShadowCascades", "SSR", "GTAO", "SSAO", "TAA", "FXAA", "Bloom", "MotionBlur", "VolumetricLight", "ContactShadow" }) {
            JsonElement contract = Member(root, "Fbzz.ScriptRender", name).GetProperty("contract");
            string[] callbacks = contract.GetProperty("allowedCallbacks").GetProperty("read").EnumerateArray().Select(value => value.GetString()!).ToArray();
            string[] modes = contract.GetProperty("runModeAccess").GetProperty("read").EnumerateArray().Select(value => value.GetString()!).ToArray();
            Program.Check(callbacks.Contains("DrawGizmos") && callbacks.Contains("DrawGizmosSelected") && modes.SequenceEqual(["Play"]),
                "Render getters must expose readonly query access only while the native Play settings copy exists.");
        }
        foreach (string name in new[] { "CanChangeWindow", "ClientWidth", "ClientHeight", "MonitorSize", "IsFullscreen", "VSync" }) {
            JsonElement contract = Member(root, "Fbzz.ScriptDisplay", name).GetProperty("contract");
            string[] callbacks = contract.GetProperty("allowedCallbacks").GetProperty("read").EnumerateArray().Select(value => value.GetString()!).ToArray();
            string[] modes = contract.GetProperty("runModeAccess").GetProperty("read").EnumerateArray().Select(value => value.GetString()!).ToArray();
            Program.Check(callbacks.Contains("DrawGizmos") && callbacks.Contains("DrawGizmosSelected")
                && modes.SequenceEqual(["Edit", "Play"]), "Display getters must describe actual host queries in Edit and Play.");
        }
        foreach (string name in new[] { "IsConnected", "Enabled", "Type", "ConnectedObject", "Distance", "CurrentDistance" }) {
            JsonElement contract = Member(root, "Fbzz.Joint", name).GetProperty("contract");
            string[] callbacks = contract.GetProperty("allowedCallbacks").GetProperty("read").EnumerateArray().Select(value => value.GetString()!).ToArray();
            Program.Check(callbacks.Contains("DrawGizmos") && callbacks.Contains("DrawGizmosSelected")
                && contract.GetProperty("runModeAccess").GetProperty("read").GetArrayLength() == 1,
                "Joint state queries must expose readonly drawing while remaining Play-only.");
        }
        foreach (string name in new[] { "TryConnectFixed", "TryConnectDistance", "TryConnectRope", "TryConnectSpring", "TryConnectHinge", "TryConnectSlider", "TryConnectChain", "Break", "SetLimits", "ClearLimits", "SetMotor", "ClearMotor", "SetDistance" }) {
            JsonElement contract = Member(root, "Fbzz.Joint", name).GetProperty("contract");
            Program.Check(contract.GetProperty("runModeAccess").GetProperty("call").EnumerateArray().Any(value => value.GetString() == "Play")
                && contract.GetProperty("allowedCallbacks").GetProperty("call").EnumerateArray().Any(value => value.GetString() == "FixedUpdate"),
                "Joint mutations must remain FixedUpdate-only in Play.");
        }
    }

    static void WorldFamilyQueriesExposeReadonlyDrawAndFixedGeometryBoundaries()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement root = document.RootElement;
        foreach (var target in new[] { ("Fbzz.Terrain", "TrySampleWorld", "call"), ("Fbzz.Water", "GetSurfaceHeightWorld", "call"),
            ("Fbzz.VFX", "Duration", "read"), ("Fbzz.SequencePlayer", "Time", "read") }) {
            JsonElement member = Member(root, target.Item1, target.Item2);
            Program.Check(Allows(member, target.Item3, "Connect") && Allows(member, target.Item3, "DrawGizmos")
                && Allows(member, target.Item3, "Update"), "Explicit world queries expose their real readonly callback policy.");
        }
        Program.Check(Allows(Member(root, "Fbzz.Terrain", "SetHeightAtGrid"), "call", "FixedUpdate")
            && !Allows(Member(root, "Fbzz.Terrain", "SetHeightAtGrid"), "call", "Update"), "Physical terrain geometry changes require FixedUpdate.");
        foreach (string type in new[] { "Fbzz.VFX", "Fbzz.SequencePlayer" })
            Program.Check(!Allows(Member(root, type, "Play"), "call", "Connect")
                && !Allows(Member(root, type, "Play"), "call", "DrawGizmos"), "Playback commands must not inherit readonly query authority.");
        Program.Check(!Allows(Member(root, "Fbzz.Animator", "GetSpeed"), "call", "DrawGizmos"),
            "Adding query alias must preserve existing read catalog boundaries.");
    }

    static void CatalogLifecycleAndGenericConstraintsMatchCompiledSdk()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement root = document.RootElement;
        JsonElement[] callbacks = root.GetProperty("lifecycleCallbacks").EnumerateArray().ToArray();
        MethodInfo[] compiled = typeof(Script).GetMethods(BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly)
            .Where(method => method.IsVirtual).ToArray();
        Program.Check(callbacks.Length == compiled.Length, "Every optional protected callback must be discoverable through the SDK catalog.");
        foreach (JsonElement callback in callbacks)
        {
            string name = callback.GetProperty("name").GetString()!;
            MethodInfo method = compiled.Single(value => value.Name == name);
            Program.Check(method.IsFamily && method.ReturnType == typeof(void), "Lifecycle authoring must use protected void overrides.");
            string? parameter = callback.GetProperty("parameterType").GetString();
            Program.Check(method.GetParameters().Select(value => value.ParameterType.FullName)
                .SequenceEqual(parameter is null ? [] : new[] { parameter }), "Catalog callback parameters must match the compiled virtual slot.");
            Program.Check(callback.GetProperty("executionPolicy").GetString() == ScriptLifecycleContract.GetExecutionPolicy(
                Enum.Parse<ScriptCallback>(callback.GetProperty("callback").GetString()!)), "Catalog policy must use the same definition as runtime dispatch.");
        }
        JsonElement assetReference = root.GetProperty("types").EnumerateArray().Single(value => value.GetProperty("name").GetString() == "Fbzz.DataAssetRef`1");
        Program.Check(assetReference.GetProperty("genericParameters")[0].GetProperty("constraints")[0].GetString() == "Fbzz.DataAsset",
            "A catalog-only author must know the generic type constraint, not only method constraints.");
    }

    static void ReadAndMutationScopesDescribeConnectSeparately()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement root = document.RootElement;
        JsonElement position = Member(root, "Fbzz.Transform", "WorldPosition");
        Program.Check(Allows(position, "read", "Connect") && !Allows(position, "write", "Connect"), "Connect may inspect position but cannot move the world.");
        Program.Check(Allows(Member(root, "Fbzz.Animator", "GetSpeed"), "call", "Connect")
            && !Allows(Member(root, "Fbzz.Animator", "SetSpeed"), "call", "Connect"), "Readonly native services and mutations must not share one method policy.");
        Program.Check(Allows(Member(root, "Fbzz.Collider", "TryGetWorldBounds"), "call", "Update")
            && Allows(Member(root, "Fbzz.Collider", "TryGetWorldBounds"), "call", "Connect"), "Readonly bounds must not inherit Collider mutation restrictions.");
        Program.Check(Allows(Member(root, "Fbzz.ScriptJobs", "Wait"), "call", "Connect")
            && Allows(Member(root, "Fbzz.ScriptEvents", "Subscribe"), "call", "Connect")
            && !Allows(Member(root, "Fbzz.ScriptEvents", "Publish"), "call", "Connect"), "Connect registers owned work without publishing events.");
        Program.Check(!Allows(Member(root, "Fbzz.ScriptTime", "DeltaSeconds"), "read", "Connect"), "Connect does not acquire an update delta.");
        JsonElement wait = Member(root, "Fbzz.ScriptJobs", "Wait");
        Program.Check(wait.GetProperty("parameters").EnumerateArray().Single(parameter => parameter.GetProperty("name").GetString() == "action")
            .GetProperty("synchronousCallback").GetBoolean(), "Catalog-only authors must see the synchronous delegate requirement.");
        Program.Check(!wait.GetProperty("parameters").EnumerateArray().Single(parameter => parameter.GetProperty("name").GetString() == "seconds")
            .GetProperty("synchronousCallback").GetBoolean(), "Synchronous delegate constraints must describe the actual annotated parameter.");
    }

    static void BoundsReadWorksInUpdateWhileShapeMutationRequiresFixedUpdate()
    {
        using ScopeFixture fixture = new();
        SphereCollider collider = new(fixture.Target);
        Program.Check(collider.TryGetWorldBounds(out Vector3 min, out Vector3 max)
            && min.Equals(new Vector3(-1, -2, -3)) && max.Equals(new Vector3(1, 2, 3)), "Update must be able to query primitive bounds.");
        Program.Check(fixture.Bridge.LastReadOnly && fixture.Bridge.Calls == 1, "Bounds dispatch is a readonly service call.");
        Program.Throws<InvalidOperationException>(() => collider.Radius = 2f);
        Program.Check(fixture.Bridge.Calls == 1, "Update shape mutation must be rejected before dispatch.");
        ScriptApiRuntime.Current = fixture.Scope(ScriptCallback.FIXED_UPDATE);
        collider.Radius = 2f;
        Program.Check(!fixture.Bridge.LastReadOnly && fixture.Bridge.Calls == 2, "The same facade mutates only in FixedUpdate.");
    }

    static void TryConversionsExposeTheirActualFailureOutputs()
    {
        using ScopeFixture fixture = new(); fixture.Bridge.ConversionSucceeded = false;
        Camera camera = new(fixture.Target);
        Program.Check(!camera.TryWorldToViewportPoint(new Vector3(1, 2, 3), out Vector3 viewport) && viewport.Equals(Vector3.ZERO),
            "Projection failure must overwrite the out value with zero.");
        Program.Check(!camera.TryViewportToWorldPoint(new Vector2(0.5f, 0.5f), 2f, out Vector3 world) && world.Equals(Vector3.ZERO),
            "Unprojection failure must overwrite the out value with zero.");
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        foreach (string name in new[] { "TryWorldToViewportPoint", "TryViewportToWorldPoint" })
            Program.Check(Member(document.RootElement, "Fbzz.Camera", name).GetProperty("contract").GetProperty("failureBehavior")
                .GetProperty("contract").GetString()!.Contains("to_zero", StringComparison.Ordinal), "Failure output behavior must survive metadata export.");
    }

    static void ImportantNumericalContractsRemainOnTheirDeclarations()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement move = Member(document.RootElement, "Fbzz.CharacterController", "Move");
        JsonElement units = move.GetProperty("contract").GetProperty("units").GetProperty("parameters");
        Program.Check(units.GetProperty("desiredVelocity").GetString() == "m/s" && units.GetProperty("deltaSeconds").GetString() == "s"
            && units.GetProperty("acceleration").GetString() == "m/s^2", "Movement callers need velocity, time and acceleration units.");
        Program.Check(move.GetProperty("contract").GetProperty("provenance").GetString() == "SDK_member_attribute",
            "Numerical meaning must come from the declaration, not a duplicate host dictionary.");
        Program.Check(Member(document.RootElement, "Fbzz.Collision", "Point").GetProperty("contract").GetProperty("units").GetProperty("result").GetString() == "m",
            "Collision snapshots must carry known units even though they do not need a live native callback.");
    }

    sealed class ScopeFixture : IDisposable, IScriptComponentLookup
    {
        readonly ScriptExecutionScope? m_previous = ScriptApiRuntime.Current;
        internal ContractBridge Bridge { get; } = new();
        internal GameObject Target { get; } = new(202, 42, "target");
        internal ScopeFixture() { ScriptApiRuntime.Current = Scope(ScriptCallback.UPDATE); }
        internal ScriptExecutionScope Scope(ScriptCallback callback) => new(Bridge, this, 1, 9, 101, 13, 42, callback);
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
        public void Dispose() { ScriptApiRuntime.Current = m_previous; }
    }

    sealed class ContractBridge : Program.RecordingBridge, IScriptNativeBridge
    {
        internal int Calls;
        internal bool LastReadOnly;
        internal bool ConversionSucceeded = true;
        ulong IScriptNativeBridge.ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind)
            => objectId == 202 && kind is 3 or 8 ? 91UL : 0UL;
        NativeServiceResult IScriptNativeBridge.InvokeService(ScriptExecutionScope scope, ulong objectId, uint service, uint method, long integer,
            ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related, bool readOnly)
        {
            Calls++; LastReadOnly = readOnly;
            if (service == 150 && method == 10) return new([-1, -2, -3, 1, 2, 3], [], 1, null);
            if (service == 183 && method is 19 or 20) return new(ConversionSucceeded ? [1, 2, 3] : [], [], ConversionSucceeded ? 1 : 0, null);
            return new([], [], 0, null);
        }
    }

    [RequireComponent(typeof(Joint))]
    sealed class JointRequirementFixture : Script { }
}
