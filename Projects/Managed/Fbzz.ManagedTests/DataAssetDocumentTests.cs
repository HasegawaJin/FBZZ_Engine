/// @file    DataAssetDocumentTests.cs
/// @brief   DataAsset の constructor 分離、候補原子性、分割転送と collectible 世代の解放を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Text;
using System.Text.Json;
using Fbzz;
using Fbzz.ManagedHost;
namespace Fbzz.ManagedTests;
internal static class DataAssetDocumentTests
{
    internal static (string Name, Action Test)[] Cases => [
        ("DataAssetCreateOnlyRunsConstructorAndResetWhileEditNormalizes", CreateAndEdit),
        ("DataAssetStagedDocumentCrosses16MiBAndRetainsLastValues", LargeTransfer),
        ("DataAssetDocumentsCloseOnGenerationRetirement", Retirement),
        ("DataAssetNestedSceneReferencesHavePortableSchemaDiagnostics", PortableSchema)
    ];
    const string SMALL = "cs-internal:ManagedFixtures.PortableProfileDataAsset", LARGE = "cs-internal:ManagedFixtures.LargePortableProfileDataAsset";
    static ManagedScriptRuntime Runtime() { ManagedScriptRuntime runtime = new(); Success(runtime.LoadCandidate(Program.FixturePath("V1"), 1, true)); return runtime; }
    static ulong Open(ManagedScriptRuntime runtime, string type, bool create)
    { Success(runtime.OpenDataAssetDocument(1, JsonSerializer.Serialize(new { type, guid = "profile-guid", create }), out ulong id)); return id; }
    static string Revision(ManagedScriptRuntime runtime, ulong id)
    { Success(runtime.ReadDataAssetPage(1, id, "{\"path\":\"\",\"count\":1}", out string page)); using JsonDocument input = JsonDocument.Parse(page); return input.RootElement.GetProperty("revision").GetString()!; }
    static void Stage(ManagedScriptRuntime runtime, ulong id, byte[] data)
    { for (int offset = 0; offset < data.Length; offset += 65536) Success(runtime.StageDataAssetDocument(1, id, offset, data.AsSpan(offset, Math.Min(65536, data.Length - offset)))); }
    static string Prepare(ManagedScriptRuntime runtime, ulong id, string fields, string? patches = null)
    {
        string revision = Revision(runtime, id); Success(runtime.BeginDataAssetPreparation(1, id));
        Stage(runtime, id, Encoding.UTF8.GetBytes("{\"expectedRevision\":\"" + revision + "\",\"fields\":" + fields + (patches is null ? "" : ",\"patches\":" + patches) + "}"));
        Success(runtime.FinishDataAssetDocument(1, id, out string summary)); return summary;
    }
    static byte[] Output(ManagedScriptRuntime runtime, ulong id)
    {
        using MemoryStream bytes = new(); long start = 0, total;
        do { Success(runtime.ReadPreparedDataAssetBytes(1, id, start, 65536, out byte[] page, out total)); bytes.Write(page); start += page.Length; } while (start < total);
        return bytes.ToArray();
    }
    static void CreateAndEdit()
    {
        using ManagedScriptRuntime runtime = Runtime();
        Type type = runtime.GetCatalog(1)!.DataAssets.Single(value => value.PersistentId == SMALL).Type;
        FieldInfo calls = type.GetField("constructorCalls")!;
        ulong create = Open(runtime, SMALL, true); Program.Check((int)calls.GetValue(null)! == 0, "Passive schema/document defaults must not execute a constructor.");
        Prepare(runtime, create, "{\"amount\":900}");
        Success(runtime.FinishDataAssetDocument(1, create, out _));
        byte[] saved = Output(runtime, create); using JsonDocument created = JsonDocument.Parse(saved);
        JsonElement fields = created.RootElement.GetProperty("fields");
        Program.Check(fields.GetProperty("amount").GetInt32() == 100 && fields.GetProperty("resetCount").GetInt32() == 1
            && fields.GetProperty("validateCount").GetInt32() == 1 && (int)calls.GetValue(null)! == 1,
            "Explicit create must run constructor/Reset once and normalize; replayed finish cannot repeat callbacks.");
        ulong edit = Open(runtime, SMALL, false); Stage(runtime, edit, saved); Success(runtime.FinishDataAssetDocument(1, edit, out _));
        Prepare(runtime, edit, "{\"amount\":-20}"); using JsonDocument edited = JsonDocument.Parse(Output(runtime, edit));
        Program.Check(edited.RootElement.GetProperty("fields").GetProperty("amount").GetInt32() == 0
            && edited.RootElement.GetProperty("fields").GetProperty("resetCount").GetInt32() == 1 && (int)calls.GetValue(null)! == 1,
            "Passive reopen and detached edits must not rerun constructor/Reset.");
        string before = Revision(runtime, edit); Success(runtime.BeginDataAssetPreparation(1, edit));
        Stage(runtime, edit, Encoding.UTF8.GetBytes("{\"expectedRevision\":\"" + before + "\",\"fields\":{\"amount\":5,\"unknown\":4}}"));
        Program.Check(!runtime.FinishDataAssetDocument(1, edit, out _).Success && Revision(runtime, edit) == before,
            "A later invalid field rejects the whole candidate without changing the retained source.");
        Success(runtime.CloseDataAssetDocument(1, edit)); Success(runtime.CloseDataAssetDocument(1, create));
    }
    static void LargeTransfer()
    {
        using ManagedScriptRuntime runtime = Runtime(); ulong create = Open(runtime, LARGE, true); Prepare(runtime, create, "{}");
        byte[] saved = Output(runtime, create); Program.Check(saved.Length > 18 * 1024 * 1024, "The persisted document must cross the former generic16MiB boundary.");
        ulong reopened = Open(runtime, LARGE, false); Stage(runtime, reopened, saved); Success(runtime.FinishDataAssetDocument(1, reopened, out _));
        Success(runtime.ReadDataAssetPage(1, reopened, "{\"path\":\"/values\",\"start\":40000,\"count\":1}", out string values));
        using JsonDocument tail = JsonDocument.Parse(values); Program.Check(tail.RootElement.GetProperty("items")[0].GetInt32() == 40000, "The array tail survives complete staged input.");
        Prepare(runtime, reopened, "{}", "[{\"path\":\"/values/40000\",\"value\":77},{\"path\":\"/text\",\"action\":\"text_replace\",\"index\":0,\"count\":1,\"value\":\"先頭\"},{\"path\":\"/text\",\"action\":\"text_replace\",\"index\":18874368,\"count\":2,\"value\":\"末尾\"}]");
        using JsonDocument updated = JsonDocument.Parse(Output(runtime, reopened)); JsonElement fields = updated.RootElement.GetProperty("fields");
        Program.Check(fields.GetProperty("values")[40000].GetInt32() == 77 && fields.GetProperty("text").GetString()!.EndsWith("末尾", StringComparison.Ordinal)
            && fields.GetProperty("text").GetString()!.StartsWith("先頭", StringComparison.Ordinal),
            "Later collection/text edits preserve every value beyond old packet and element boundaries.");
        Success(runtime.ReadPreparedDataAssetReview(1, reopened, "{\"count\":1,\"wireBytes\":2048}", out string review));
        Program.Check(Encoding.UTF8.GetByteCount(review) <= 2048, "Candidate review must bound aggregate returned JSON without limiting retained values.");
    }
    static void Retirement()
    {
        using ManagedScriptRuntime runtime = Runtime(); ulong id = Open(runtime, SMALL, true); Prepare(runtime, id, "{}");
        Program.Check(runtime.AssetDocumentCount == 1, "An editable document owns its generation-local types and candidate snapshots.");
        Success(runtime.RetireGeneration(1)); Program.Check(runtime.AssetDocumentCount == 0 && runtime.GetCatalog(1) is null
            && runtime.ReadPreparedDataAssetBytes(1, id, 0, 64, out _, out _).Status == BridgeStatus.STALE_GENERATION,
            "Retirement closes owned documents/snapshots and invalidates old handles without pinning the collectible type graph.");
    }
    [Serializable] public sealed class SceneCarrier { public ComponentRef<Camera> camera = default; }
    static void PortableSchema()
    {
        AssemblyBuilder assembly = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("PortableAssetSchemaProbe"), AssemblyBuilderAccess.RunAndCollect);
        TypeBuilder builder = assembly.DefineDynamicModule("main").DefineType("SceneBoundAsset", TypeAttributes.Public, typeof(DataAsset));
        builder.DefineDefaultConstructor(MethodAttributes.Public); builder.DefineField("nested", typeof(SceneCarrier), FieldAttributes.Public); _ = builder.CreateType();
        CatalogBuildResult result = ScriptCatalogBuilder.Build(assembly);
        Program.Check(!result.Success && result.Diagnostics.Any(value => value.Code == "CS_ASSET_SCENE_REFERENCE" && value.Message.Contains("private runtime caches", StringComparison.Ordinal)),
            "Nested scene-bound refs require an actionable asset portability diagnostic, not unresolved-null persistence.");
    }
    static void Success(ManagedRuntimeResult result) => Program.Check(result.Success, result.Message);
}
