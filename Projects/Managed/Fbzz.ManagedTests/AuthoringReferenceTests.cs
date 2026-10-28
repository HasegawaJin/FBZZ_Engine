/// @file    AuthoringReferenceTests.cs
/// @brief   編集途中の必須参照と実行前検証を分離し、入れ子参照 identity と診断を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class AuthoringReferenceTests
{
    internal static (string Name, Action Test)[] Cases =>
    [
        ("AuthoringRequiredReferencesInStages", ReferencesInStages),
        ("AuthoringRequiredValuesRemainAtomic", ValuesRemainAtomic),
        ("AuthoringNestedRequiredLeafDiagnostics", LeafDiagnostics),
        ("AuthoringNestedReferenceProjectionIdentity", ProjectionIdentity),
        ("AuthoringSiblingEditsRetainReferenceTokens", SiblingEditsRetainTokens),
        ("AuthoringReferenceRebindIsExplicitAndAtomic", RebindIsExplicitAndAtomic),
        ("AuthoringMovedCollectionTokensRemainStable", MovedCollectionTokens),
        ("AuthoringAmbiguousReferenceMovesRejectAtomically", AmbiguousMovesReject)
    ];
    const string TYPE = "ManagedFixtures.RequiredReferenceComponent";
    const string SETTINGS = """
        {"settings":{"target":"target-guid","targets":["target-guid"],"list":["target-guid"],"speed":2}}
        """;

    static void ReferencesInStages()
    {
        using World world = new();
        world.Edit("{\"settings\":{\"target\":\"target-guid\",\"targets\":[],\"list\":[],\"speed\":2}}");
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success,
            "One connected required reference must remain editable while runtime validation rejects the other missing references.");
        world.Edit(SETTINGS);
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success,
            "A different unconnected root setting must not block editing a completed nested group.");
        world.Edit("{\"requiredList\":[\"target-guid\"]}");
        Program.Check(world.Runtime.ValidateReferences(1, world.Id).Success,
            "Runtime validation must accept the completed authoring state.");
        world.Edit("{\"requiredList\":[]}");
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success,
            "Authoring may clear a required collection without accidentally passing runtime validation.");
        Program.Check(world.Runtime.ConfigureFields(1, world.Id, "{\"settings\":{\"target\":null,\"targets\":[],\"list\":[]}}",
            false, SettingsValidationPurpose.SNAPSHOT).Success, "Undo must restore an incomplete setting state.");
    }

    static void ValuesRemainAtomic()
    {
        using World world = new();
        string before = world.Value("settings");
        string list = world.Value("requiredList");
        foreach (string bad in new[] { "11", "1e100", "\"fast\"" }) {
            ManagedRuntimeResult result = world.Runtime.ConfigureFields(1, world.Id,
                "{\"requiredList\":[\"target-guid\"],\"settings\":{\"speed\":" + bad + "}}", false, SettingsValidationPurpose.AUTHORING);
            Program.Check(!result.Success && world.Value("settings") == before && world.Value("requiredList") == list,
                "Authoring must retain finite, type, range and all-or-nothing write validation: " + bad);
        }
        Program.Check(!world.Runtime.ConfigureFields(1, world.Id, "{}", false, (SettingsValidationPurpose)99).Success,
            "An unknown validation purpose must be rejected.");
    }

    static void LeafDiagnostics()
    {
        using World world = new();
        world.Edit("{\"requiredList\":[\"target-guid\"],\"settings\":{\"target\":null,\"targets\":[\"target-guid\"],\"list\":[\"target-guid\"]}}");
        Program.Check(world.Runtime.ValidateReferences(1, world.Id).Message.Contains(".settings.target", StringComparison.Ordinal),
            "A nested required single-reference failure must name the leaf.");
        world.Edit("{\"settings\":{\"target\":\"target-guid\",\"targets\":[\"target-guid\"],\"list\":[null]}}");
        Program.Check(world.Runtime.ValidateReferences(1, world.Id).Message.Contains(".settings.list[0]", StringComparison.Ordinal),
            "A missing required collection slot must include its index.");
        world.Edit(SETTINGS);
        world.Edit("{\"requiredList\":[\"target-guid\"],\"branches\":[{\"leaves\":[{\"target\":null}]}]}");
        Program.Check(world.Runtime.ValidateReferences(1, world.Id).Message.Contains(".branches[0].leaves[0].target", StringComparison.Ordinal),
            "Nested class/struct collection diagnostics must retain the complete authoring path.");
        using JsonDocument schema = JsonDocument.Parse(JsonSerializer.Serialize(StructuredFieldCodec.Schema(world.Field("settings").ValueType)));
        JsonElement target = schema.RootElement.GetProperty("fields").EnumerateArray().First((JsonElement field) => field.GetProperty("name").GetString() == "target");
        Program.Check(target.GetProperty("requiredReference").GetBoolean() && target.GetProperty("requiredAt").GetString() == "Runtime",
            "Nested schema must expose the required condition and validation stage to Inspector and AI.");
    }

    static void ProjectionIdentity()
    {
        using World world = new();
        world.Edit(SETTINGS);
        object settings = world.Field("settings").Read(world.Instance)!;
        Type type = world.Field("settings").ValueType;
        ulong[] initial = StructuredFieldCodec.ReferenceIdentities(settings, type, 1);
        Program.Check(initial.SequenceEqual(new ulong[] { 202, 202, 202 }),
            "Reference projections must follow JSON token order, preserving duplicated references.");
        world.Bridge.Alive.Remove(202); world.Bridge.Alive.Add(303); world.Bridge.Guids["target-guid"] = 303;
        int resolves = world.Bridge.ResolveCount;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, type, 1).SequenceEqual(initial)
            && world.Bridge.ResolveCount == resolves, "Display projection must retain the expired token instead of resolving a recreated GUID.");
        settings.GetType().GetField("target")!.SetValue(settings, new GameObject(303, 2, "target-guid"));
        ulong[] mixed = StructuredFieldCodec.ReferenceIdentities(settings, type, 1);
        Program.Check(mixed.SequenceEqual(new ulong[] { 202, 0, 202 }),
            "A token from another Scene must remain unresolved in the display projection.");
    }

    static void SiblingEditsRetainTokens()
    {
        using World world = new();
        world.Edit(SETTINGS);
        world.Edit("{\"requiredList\":[\"target-guid\"]}");
        world.Bridge.Alive.Remove(202); world.Bridge.Alive.Add(303); world.Bridge.Guids["target-guid"] = 303;
        int resolves = world.Bridge.ResolveCount;
        world.Edit(SETTINGS.Replace("\"speed\":2", "\"speed\":3", StringComparison.Ordinal));
        object settings = world.Field("settings").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, settings.GetType(), 1).SequenceEqual(new ulong[] { 202, 202, 202 })
            && world.Bridge.ResolveCount == resolves, "A sibling numeric edit must preserve every retained reference token, without resolving its GUID.");
        Program.Check((float)settings.GetType().GetField("speed")!.GetValue(settings)! == 3f
            && !world.Runtime.ValidateReferences(1, world.Id).Success, "Authoring must apply the numeric edit while missing references still block execution.");
        world.Bridge.Guids["other-guid"] = 203;
        world.Edit(SETTINGS.Replace("\"target\":\"target-guid\"", "\"target\":\"other-guid\"", StringComparison.Ordinal));
        settings = world.Field("settings").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, settings.GetType(), 1).SequenceEqual(new ulong[] { 202, 203, 202 }),
            "Changing a reference GUID must bind that changed leaf, retaining its unchanged siblings.");
    }

    static void RebindIsExplicitAndAtomic()
    {
        using World world = new(); world.Edit(SETTINGS);
        world.Bridge.Alive.Remove(202); world.Bridge.Alive.Add(303); world.Bridge.Guids["target-guid"] = 303;
        string before = world.Value("settings");
        foreach (string[] paths in new[] {
            new[] { "/settings/missing" }, new[] { "/settings/speed" },
            new[] { "/settings/target", "/settings/target" }, new[] { "/settings/target/$go" }
        }) {
            ManagedRuntimeResult invalid = world.Runtime.ConfigureFields(1, world.Id,
                SETTINGS.Replace("\"speed\":2", "\"speed\":4", StringComparison.Ordinal), false, SettingsValidationPurpose.AUTHORING, paths);
            Program.Check(!invalid.Success && world.Value("settings") == before,
                "Unknown, non-reference, token-member or duplicated paths must reject the complete transaction.");
            object retained = world.Field("settings").Read(world.Instance)!;
            Program.Check(StructuredFieldCodec.ReferenceIdentities(retained, retained.GetType(), 1).All(static (ulong id) => id == 202),
                "Rejected intents must not replace any existing reference identity.");
        }
        ManagedRuntimeResult preview = world.Runtime.ConfigureFields(1, world.Id, SETTINGS, true,
            SettingsValidationPurpose.AUTHORING, new[] { "/settings/target" });
        Program.Check(preview.Success && world.Value("settings") == before, "Explicit rebind preview must not modify fields.");
        Program.Check(world.Runtime.ConfigureFields(1, world.Id, SETTINGS, false,
            SettingsValidationPurpose.AUTHORING, new[] { "/settings/target" }).Success, "An explicit same-GUID leaf rebind must succeed.");
        object settings = world.Field("settings").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, settings.GetType(), 1).SequenceEqual(new ulong[] { 202, 303, 202 }),
            "Only the explicitly rebound leaf may receive the replacement identity.");
        Program.Check(world.Runtime.ConfigureFields(1, world.Id, SETTINGS, false, SettingsValidationPurpose.SNAPSHOT).Success,
            "Snapshot restore must keep its existing GUID restoration contract.");
        settings = world.Field("settings").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, settings.GetType(), 1).All(static (ulong id) => id == 303),
            "Internal snapshot restore must resolve its saved GUID values.");
    }

    static void MovedCollectionTokens()
    {
        using World world = new();
        world.Edit("{\"settings\":{\"target\":\"target-guid\",\"targets\":[\"target-guid\",\"child-guid\"],\"list\":[\"target-guid\",\"child-guid\"]},\"requiredList\":[\"target-guid\",\"child-guid\"]}");
        world.Bridge.Alive.Remove(202); world.Bridge.Alive.Add(303); world.Bridge.Guids["target-guid"] = 303;
        int resolves = world.Bridge.ResolveCount;
        world.Edit("{\"settings\":{\"target\":\"target-guid\",\"targets\":[\"child-guid\",\"target-guid\"],\"list\":[\"child-guid\",\"target-guid\"],\"speed\":3},\"requiredList\":[\"child-guid\",\"target-guid\"]}");
        object settings = world.Field("settings").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, settings.GetType(), 1).SequenceEqual(new ulong[] { 203, 202, 202, 203, 202 })
            && world.Bridge.ResolveCount == resolves, "Array/List reorder must retain unique live and expired tokens when their canonical leaf paths move.");
        object list = world.Field("requiredList").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(list, list.GetType(), 1).SequenceEqual(new ulong[] { 203, 202 }),
            "A root List reorder must preserve expired tokens too.");
        world.Edit("{\"requiredList\":[\"target-guid\"]}");
        list = world.Field("requiredList").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(list, list.GetType(), 1).SequenceEqual(new ulong[] { 202 })
            && world.Bridge.ResolveCount == resolves, "Removing an earlier array/List element must preserve the shifted expired token.");
    }

    static void AmbiguousMovesReject()
    {
        using World world = new(); world.Edit(SETTINGS);
        object settings = world.Field("settings").Read(world.Instance)!;
        world.Bridge.Alive.Remove(202); world.Bridge.Alive.Add(303); world.Bridge.Guids["target-guid"] = 303;
        settings.GetType().GetField("targets")!.SetValue(settings, new[] { new GameObject(303, 1, "target-guid") });
        string before = world.Value("settings");
        string moved = "{\"settings\":{\"target\":\"target-guid\",\"targets\":[\"target-guid\"],\"list\":[null,\"target-guid\"],\"speed\":4}}";
        int resolves = world.Bridge.ResolveCount;
        Program.Check(!world.Runtime.ConfigureFields(1, world.Id, moved, false, SettingsValidationPurpose.AUTHORING).Success
            && world.Value("settings") == before && world.Bridge.ResolveCount == resolves,
            "A moved GUID with different retained token identities must reject the complete edit instead of resolving a replacement.");
        world.Edit(SETTINGS.Replace("\"speed\":2", "\"speed\":3", StringComparison.Ordinal));
        settings = world.Field("settings").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, settings.GetType(), 1).SequenceEqual(new ulong[] { 202, 202, 303 }),
            "Numeric siblings may still change while same-GUID distinct tokens remain attached to their existing paths.");
        string reduced = "{\"settings\":{\"target\":\"target-guid\",\"targets\":[],\"list\":[\"target-guid\"],\"speed\":4}}";
        string numericBefore = world.Value("settings");
        Program.Check(!world.Runtime.ConfigureFields(1, world.Id, reduced, false, SettingsValidationPurpose.AUTHORING).Success
            && world.Value("settings") == numericBefore, "Removing an ambiguous duplicate must not silently select a different retained token by its new position.");
        Program.Check(world.Runtime.ConfigureFields(1, world.Id, moved, false, SettingsValidationPurpose.AUTHORING,
            new[] { "/settings/list/1", "/settings/target", "/settings/targets/0" }).Success,
            "Explicit intents for every ambiguous incoming leaf must resolve the selected current Scene identity.");
        settings = world.Field("settings").Read(world.Instance)!;
        Program.Check(StructuredFieldCodec.ReferenceIdentities(settings, settings.GetType(), 1).All(static (ulong id) => id == 303),
            "An explicitly rebound ambiguous group may contain only its selected replacement tokens.");
    }

    sealed class World : IDisposable
    {
        internal World()
        {
            Runtime = new ManagedScriptRuntime(Bridge);
            ManagedRuntimeResult loaded = Runtime.LoadCandidate(Program.FixturePath("V1"), 1, internalMode: true);
            Program.Check(loaded.Success, loaded.Message);
            ManagedRuntimeResult created = Runtime.CreateInstance(1, TYPE, 101, out ulong id);
            Program.Check(created.Success, created.Message); Id = id;
            Program.BindFixture(Runtime, 1, Id);
        }
        internal Program.RecordingBridge Bridge { get; } = new();
        internal ManagedScriptRuntime Runtime { get; }
        internal ulong Id { get; }
        internal Script Instance { get { Program.Check(Runtime.TryInstance(1, Id, out ManagedScriptRuntime.ScriptInstance? record), "The authoring fixture must remain registered."); return record!.Instance; } }
        internal ScriptFieldDescriptor Field(string name) => Program.Field(Runtime.GetCatalog(1)!.Find(TYPE)!, name);
        internal string Value(string name) { ScriptFieldDescriptor field = Field(name); return StructuredFieldCodec.Encode(field.Read(Instance), field.ValueType); }
        internal void Edit(string json) { ManagedRuntimeResult result = Runtime.ConfigureFields(1, Id, json, false, SettingsValidationPurpose.AUTHORING); Program.Check(result.Success, result.Message); }
        public void Dispose() => Runtime.Dispose();
    }
}
