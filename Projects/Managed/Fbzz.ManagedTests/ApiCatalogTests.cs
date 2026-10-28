/// @file    ApiCatalogTests.cs
/// @brief   公開 SDK の overload・値・nullability を実 catalog で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Fbzz.ManagedHost;
namespace Fbzz.ManagedTests;
public static class ApiCatalogTests
{
    public static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(PublicSdkMetadataContainsExactSignaturesWithoutInternalServices), PublicSdkMetadataContainsExactSignaturesWithoutInternalServices),
        (nameof(StructuredContractsDistinguishPureMathSpaceAndCallbackRestrictions), StructuredContractsDistinguishPureMathSpaceAndCallbackRestrictions)];
    static void Require(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); }
    static JsonElement Type(JsonElement root, string name) => root.GetProperty("types").EnumerateArray().Single((JsonElement value) => value.GetProperty("name").GetString() == name);
    static JsonElement Member(JsonElement type, string name) => type.GetProperty("members").EnumerateArray().First((JsonElement value) => value.GetProperty("name").GetString() == name);
    static void PublicSdkMetadataContainsExactSignaturesWithoutInternalServices()
    {
        string first = ApiCatalog.Json;
        Require(first == ApiCatalog.Json, "Catalog serialization must be stable.");
        using JsonDocument document = JsonDocument.Parse(first);
        JsonElement root = document.RootElement;
        Require(root.GetProperty("schemaVersion").GetInt32() == 2, "Unknown catalog schema.");
        Require(!root.GetProperty("types").EnumerateArray().Any((JsonElement value) => value.GetProperty("name").GetString() == "Fbzz.IScriptNativeBridge"), "Internal bridge must not be published.");
        JsonElement gameObject = Type(root, "Fbzz.GameObject");
        JsonElement get = Member(gameObject, "GetComponent");
        Require(get.GetProperty("mayBeNull").GetBoolean(), "GetComponent missing result must be nullable.");
        Require(get.GetProperty("genericParameters")[0].GetProperty("constraints")[0].GetString() == "Fbzz.Component", "Generic component constraint is lost.");
        JsonElement transform = Type(root, "Fbzz.Transform");
        Require(Member(transform, "LocalPosition").GetProperty("writable").GetBoolean(), "LocalPosition setter is public.");
        JsonElement scene = Type(root, "Fbzz.ScriptScene");
        Require(scene.GetProperty("members").EnumerateArray().Count((JsonElement value) => value.GetProperty("name").GetString() == "Create") == 2, "Create overloads must be distinct.");
        JsonElement destroy = Member(gameObject, "Destroy");
        Require(destroy.GetProperty("parameters")[0].GetProperty("optional").GetBoolean(), "Destroy delay default is missing.");
        Require(destroy.GetProperty("parameters")[0].GetProperty("defaultValue").GetSingle() == 0f, "Destroy default delay must be zero.");
        Require(Member(Type(root, "Fbzz.Vector3"), "op_Addition").GetProperty("kind").GetString() == "operator", "Public math operators must be discoverable.");
        foreach (string owner in new[] { "Fbzz.GameObject", "Fbzz.Component", "Fbzz.Animator" }) {
            string name = owner == "Fbzz.Animator" ? "GetCurrentBlendWeights" : "GetComponents";
            JsonElement caller = Type(root, owner).GetProperty("members").EnumerateArray().Single(value
                => value.GetProperty("name").GetString() == name && value.GetProperty("signature").GetString()!.Contains("System.Span<", StringComparison.Ordinal));
            Require(caller.GetProperty("returnType").GetString() == "Fbzz.CollectionReadResult"
                && caller.GetProperty("contract").GetProperty("failureBehavior").GetProperty("contract").GetString()!.Contains("written_zero_without_output_changes", StringComparison.Ordinal),
                "Caller-buffer collection contracts must reach API metadata.");
        }
    }

    static void StructuredContractsDistinguishPureMathSpaceAndCallbackRestrictions()
    {
        using JsonDocument document = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement root = document.RootElement;
        JsonElement Contract(string type, string member) => Member(Type(root, type), member).GetProperty("contract");
        foreach (string type in new[] { "Fbzz.Vector3", "Fbzz.Quaternion", "Fbzz.Mathf" })
            foreach (JsonElement member in Type(root, type).GetProperty("members").EnumerateArray())
                Require(member.GetProperty("contract").GetProperty("pure").GetBoolean(), "Math must not require a native callback.");
        JsonElement local = Contract("Fbzz.Transform", "LocalPosition");
        JsonElement world = Contract("Fbzz.Transform", "WorldPosition");
        Require(local.GetProperty("coordinate").GetProperty("result").GetString() == "local_parent_space", "Local space was lost.");
        Require(world.GetProperty("coordinate").GetProperty("result").GetString() == "world", "World space was lost.");
        Require(local.GetProperty("units").GetProperty("result").GetString() == "m", "Position meters were lost.");
        Require(!local.GetProperty("pure").GetBoolean(), "Transform operations need native authority.");
        JsonElement body = Contract("Fbzz.RigidBody", "Velocity");
        Require(body.GetProperty("allowedCallbacks").GetProperty("write").EnumerateArray().Select((JsonElement item) => item.GetString()).SequenceEqual(new[] { "FixedUpdate" }), "Body mutation requires FixedUpdate.");
        Require(body.GetProperty("allowedCallbacks").GetProperty("read").EnumerateArray().Any((JsonElement item) => item.GetString() == "Update"), "Body reads are not restricted to FixedUpdate.");
        JsonElement time = Contract("Fbzz.ScriptTime", "DeltaSeconds");
        Require(time.GetProperty("allowedCallbacks").GetProperty("read").EnumerateArray().Any((JsonElement item) => item.GetString() == "OnCollisionEnter"), "Physics notification delta is supported.");
        Require(!time.GetProperty("allowedCallbacks").GetProperty("read").EnumerateArray().Any((JsonElement item) => item.GetString() == "Awake"), "Awake must not gain a time delta.");
        JsonElement radians = Contract("Fbzz.Mathf", "ToRadians");
        Require(radians.GetProperty("units").GetProperty("parameters").GetProperty("degrees").GetString() == "deg", "Degree input is explicit.");
        Require(radians.GetProperty("units").GetProperty("result").GetString() == "rad", "Radians output is explicit.");
        JsonElement[] creates = Type(root, "Fbzz.ScriptScene").GetProperty("members").EnumerateArray().Where((JsonElement item) => item.GetProperty("name").GetString() == "Create").ToArray();
        Require(creates.Select((JsonElement item) => item.GetProperty("contract").GetProperty("id").GetString()).Distinct().Count() == 2, "Overload contracts must have full signature identity.");
        foreach (JsonElement create in creates)
        {
            JsonElement parameters = create.GetProperty("contract").GetProperty("units").GetProperty("parameters");
            bool hasPose = create.GetProperty("parameters").GetArrayLength() == 3;
            Require(parameters.TryGetProperty("position", out _) == hasPose, "An overload must not acquire a nonexistent parameter contract.");
        }
        Require(Contract("Fbzz.SettingAttribute", "Label").GetProperty("pure").ValueKind == JsonValueKind.Null, "Unreviewed contracts remain unknown.");
        Require(!Type(root, "Fbzz.NativeServiceResult").GetProperty("authoringRecommended").GetBoolean(), "Bridge packets are not standard authoring entry points.");
        foreach (JsonElement comparison in Type(root, "Fbzz.GameObject").GetProperty("members").EnumerateArray()
            .Where((JsonElement item) => item.GetProperty("name").GetString() is "Equals" or "GetHashCode" or "op_Equality" or "op_Inequality"))
        {
            JsonElement identity = comparison.GetProperty("contract");
            Require(identity.GetProperty("pure").GetBoolean() && identity.GetProperty("thread").GetString() == "any", "Identity comparison is pure managed code.");
            Require(identity.GetProperty("allowedCallbacks").GetProperty("call")[0].GetString() == "any", "Identity comparison must not require live authority.");
        }
        JsonElement ground = Contract("Fbzz.CharacterController", "RegisterGroundContact");
        string?[] allowedGround = ground.GetProperty("allowedCallbacks").GetProperty("call").EnumerateArray().Select((JsonElement item) => item.GetString()).ToArray();
        Require(allowedGround.SequenceEqual(new[] { "FixedUpdate", "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit" }), "Ground contact has the physics notification exception.");
        Require(!local.GetProperty("allowedCallbacks").GetProperty("read").EnumerateArray().Any((JsonElement item) => item.GetString() == "ReloadRetired"), "Retired generation cannot use world API.");
    }
}
