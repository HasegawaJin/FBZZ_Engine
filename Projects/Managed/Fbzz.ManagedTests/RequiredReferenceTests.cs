/// @file    RequiredReferenceTests.cs
/// @brief   Real Host configuration and restore checks for recursive required references.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class RequiredReferenceTests
{
    internal static (string Name, Action Test)[] Cases =>
    [
        ("RequiredNestedDefaultsAndBinding", DefaultsAndBinding),
        ("RequiredNestedConfigureIsAtomic", ConfigureIsAtomic),
        ("RequiredNestedTokensDoNotResurrect", TokensDoNotResurrect),
        ("RequiredNestedSnapshotPreparation", SnapshotPreparation),
        ("RequiredNestedSchemaDiagnostics", SchemaDiagnostics)
    ];
    const string TYPE = "ManagedFixtures.RequiredReferenceComponent";
    const string VALID = """
        {"settings":{"target":"target-guid","targets":["target-guid"],"list":["target-guid"],"speed":2},
         "branches":[{"leaves":[{"target":"target-guid"}]}],"requiredList":["target-guid"],
         "optional":null,"optionalList":[null,"missing-guid"]}
        """;

    static void DefaultsAndBinding()
    {
        using World world = new(bind: false);
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success,
            "Unconnected nested defaults must load and construct, but cannot pass reference validation.");
        string defaults = world.Settings;
        Program.Check(world.Runtime.CaptureReload(1, world.Id, out _).Success,
            "Missing required references must remain representable in save and Undo snapshots.");
        Program.BindFixture(world.Runtime, 1, world.Id);
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success,
            "Binding an owner must not make unset nested required references valid.");
        Program.Check(world.Runtime.ConfigureFields(1, world.Id, VALID, validateOnly: true).Success
            && world.Settings == defaults, "A valid nested Inspector preview must not change the instance.");
        world.Configure(VALID);
        Program.Check(world.Runtime.ValidateReferences(1, world.Id).Success,
            "Bound live nested references must validate; optional null, missing and empty fields remain optional.");
        world.Configure("{\"branches\":[],\"optionalList\":[]}");
    }

    static void ConfigureIsAtomic()
    {
        using World world = new();
        world.Configure(VALID);
        string before = world.Settings;
        string branches = world.FieldValue("branches");
        string list = world.FieldValue("requiredList");
        (string Field, string Value)[] rejected =
        [
            ("target", "null"), ("target", "\"missing-guid\""),
            ("targets", "[]"), ("targets", "null"), ("targets", "[null]"),
            ("targets", "[\"target-guid\",\"missing-guid\"]"),
            ("list", "[]"), ("list", "null"), ("list", "[null]"),
            ("list", "[\"target-guid\",\"missing-guid\"]")
        ];
        foreach ((string field, string value) in rejected)
        {
            using JsonDocument parsed = JsonDocument.Parse("{\"target\":\"target-guid\",\"targets\":[\"target-guid\"],\"list\":[\"target-guid\"],\"speed\":9}");
            Dictionary<string, JsonElement> settings = new(StringComparer.Ordinal);
            foreach (JsonProperty property in parsed.RootElement.EnumerateObject()) settings[property.Name] = property.Value;
            settings[field] = JsonSerializer.Deserialize<JsonElement>(value);
            string delta = JsonSerializer.Serialize(new { settings });
            Program.Check(!world.Runtime.ConfigureFields(1, world.Id, delta, validateOnly: true).Success
                && !world.Runtime.ConfigureFields(1, world.Id, delta, validateOnly: false).Success,
                "Nested required configuration must reject " + field + "=" + value);
            Program.Check(world.Settings == before, "Rejected nested writes must retain every prior field.");
        }
        foreach (string delta in new[]
        {
            "{\"branches\":[{\"leaves\":[{\"target\":null}]}]}",
            "{\"branches\":[{\"leaves\":[{\"target\":\"missing-guid\"}]}]}",
            "{\"requiredList\":[]}", "{\"requiredList\":[null]}",
            "{\"requiredList\":[\"target-guid\",\"missing-guid\"]}"
        })
        {
            Program.Check(!world.Runtime.ConfigureFields(1, world.Id, delta, false).Success,
                "Required references must be checked through class arrays, List<struct> and direct Lists.");
            Program.Check(world.Settings == before && world.FieldValue("branches") == branches && world.FieldValue("requiredList") == list,
                "Rejected deep collection writes must preserve the complete setting state.");
        }
    }

    static void TokensDoNotResurrect()
    {
        using World world = new();
        world.Configure(VALID);
        object settings = world.Field("settings").Read(world.Instance)!;
        FieldInfo target = settings.GetType().GetField("target")!;
        GameObject retained = (GameObject)target.GetValue(settings)!;
        world.Bridge.Alive.Remove(202);
        world.Bridge.Alive.Add(303);
        world.Bridge.Guids["target-guid"] = 303;
        int resolves = world.Bridge.ResolveCount;
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success && world.Bridge.ResolveCount == resolves,
            "Recursive liveness validation must inspect retained tokens without resolving a recreated GUID.");
        Program.Check(ReferenceEquals(retained, target.GetValue(settings)) && retained.ObjectId == 202,
            "A missing required wrapper must not mutate into its same-GUID replacement.");
        world.Configure(VALID);
        settings = world.Field("settings").Read(world.Instance)!;
        target.SetValue(settings, new GameObject(303, 2, "target-guid"));
        resolves = world.Bridge.ResolveCount;
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success && world.Bridge.ResolveCount == resolves,
            "An otherwise live nested token from another Scene must be rejected without rebinding it.");
        target.SetValue(settings, new GameObject(0, 1, "target-guid"));
        Program.Check(!world.Runtime.ValidateReferences(1, world.Id).Success,
            "An unresolved nested token must fail even if its GUID now names a live entity.");
        world.Configure(VALID);
    }

    static void SnapshotPreparation()
    {
        using World world = new();
        Program.Check(world.Runtime.CaptureReload(1, world.Id, out string missing).Success,
            "Reload capture must retain the representable missing-reference state.");
        world.Configure(VALID);
        Program.Check(world.Runtime.ConfigureFields(1, world.Id,
            "{\"settings\":{\"target\":null,\"targets\":[],\"list\":[]}}", false, purpose: SettingsValidationPurpose.SNAPSHOT).Success
            && !world.Runtime.ValidateReferences(1, world.Id).Success,
            "Undo restores nested missing references, while publication validation still rejects them.");
        Program.Check(!world.Runtime.ConfigureFields(1, world.Id,
            "{\"settings\":{\"speed\":1e100}}", false, purpose: SettingsValidationPurpose.SNAPSHOT).Success,
            "Deferring required liveness must retain finite-value validation.");
        Program.Check(world.Runtime.LoadCandidate(Program.FixturePath("V2"), 2, internalMode: true).Success,
            "A reload candidate with unconnected nested defaults must prepare.");
        Program.Check(world.Runtime.CreateInstance(2, TYPE, 101, out ulong next).Success,
            "The new generation must construct before reference binding.");
        Program.BindFixture(world.Runtime, 2, next);
        Program.Check(world.Runtime.RestoreReload(2, next, missing).Success
            && !world.Runtime.ValidateReferences(2, next).Success,
            "Reload restores state first and rejects missing required references before native commit.");
        Program.Check(world.Runtime.ConfigureFields(2, next, VALID, false).Success
            && world.Runtime.ValidateReferences(2, next).Success,
            "The same candidate must pass publication checks after explicit live reference connection.");
    }

    static void SchemaDiagnostics()
    {
        ModuleBuilder module = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("RequiredReferenceSchemaFixture"),
            AssemblyBuilderAccess.RunAndCollect).DefineDynamicModule("main");
        TypeBuilder options = module.DefineType("InvalidRequiredOptions", TypeAttributes.Public);
        options.SetCustomAttribute(new CustomAttributeBuilder(typeof(SerializableAttribute).GetConstructor(Type.EmptyTypes)!, []));
        options.DefineDefaultConstructor(MethodAttributes.Public);
        FieldBuilder wrong = options.DefineField("count", typeof(int), FieldAttributes.Public);
        wrong.SetCustomAttribute(new CustomAttributeBuilder(typeof(RequiredReferenceAttribute).GetConstructor(Type.EmptyTypes)!, []));
        Type invalid = options.CreateType()!;
        TypeBuilder actor = module.DefineType("InvalidRequiredComponent", TypeAttributes.Public, typeof(Script));
        actor.DefineDefaultConstructor(MethodAttributes.Public);
        actor.DefineField("settings", invalid, FieldAttributes.Public);
        CatalogBuildResult result = ScriptCatalogBuilder.Build(actor.CreateType()!.Assembly);
        Program.Check(!result.Success && result.Diagnostics.Any(static (ScriptDiagnostic diagnostic)
            => diagnostic.Code == "CS_STRUCTURED_SCHEMA" && diagnostic.Message.Contains("RequiredReference", StringComparison.Ordinal)),
            "Invalid nested RequiredReference types must reject discovery with a schema diagnostic.");
    }

    sealed class World : IDisposable
    {
        internal World(bool bind = true)
        {
            Runtime = new ManagedScriptRuntime(Bridge);
            ManagedRuntimeResult load = Runtime.LoadCandidate(Program.FixturePath("V1"), 1, internalMode: true);
            Program.Check(load.Success, load.Message);
            ManagedRuntimeResult create = Runtime.CreateInstance(1, TYPE, 101, out ulong id);
            Program.Check(create.Success, create.Message);
            Id = id;
            if (bind) Program.BindFixture(Runtime, 1, Id);
        }
        internal Program.RecordingBridge Bridge { get; } = new();
        internal ManagedScriptRuntime Runtime { get; }
        internal ulong Id { get; }
        internal Script Instance
        {
            get { Program.Check(Runtime.TryInstance(1, Id, out ManagedScriptRuntime.ScriptInstance? record), "The fixture must stay registered."); return record!.Instance; }
        }
        internal ScriptFieldDescriptor Field(string name) => Program.Field(Runtime.GetCatalog(1)!.Find(TYPE)!, name);
        internal string FieldValue(string name) { ScriptFieldDescriptor field = Field(name); return StructuredFieldCodec.Encode(field.Read(Instance), field.ValueType); }
        internal string Settings => FieldValue("settings");
        internal void Configure(string json) { ManagedRuntimeResult result = Runtime.ConfigureFields(1, Id, json, false); Program.Check(result.Success, result.Message); }
        public void Dispose() => Runtime.Dispose();
    }
}
