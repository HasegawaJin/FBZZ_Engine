/// @file    ApiContracts.cs
/// @brief   SDK 宣言属性と共通 lifecycle 定義から公開契約を作る。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using Fbzz;

namespace Fbzz.ManagedHost;

/// @note 個別契約の正本は SDK member の ApiContractAttribute。XML 生成も同じ属性を読む。
/// @note 未指定の数値・失敗契約は unknown。共通の native facade 権限と数値の意味を混同しない。
internal static class ApiContracts
{
    static readonly string[] s_live = ScriptLifecycleContract.Callbacks
        .Where(value => value.ExecutionPolicy == "live").Select(value => value.Name)
        .Concat(new[] { "Job", "Event" }).Distinct(StringComparer.Ordinal).ToArray();
    static readonly string[] s_read = s_live.Concat(new[] { "Connect" }).ToArray();
    static readonly string[] s_time = ["Update", "FixedUpdate", "LateUpdate", "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit",
        "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit"];
    static readonly string[] s_fixed = ["FixedUpdate"];
    static readonly string[] s_draw = ScriptLifecycleContract.Callbacks.Where(value => value.ExecutionPolicy == "draw_readonly").Select(value => value.Name).ToArray();
    static readonly string[] s_query = s_read.Concat(s_draw).Distinct(StringComparer.Ordinal).ToArray();
    static readonly string[] s_ground = ["FixedUpdate", "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit"];
    static readonly string[] s_unknown = ["unknown"];

    internal static object Describe(MemberInfo member, string signature)
    {
        Type type = member.DeclaringType!;
        string id = type.FullName + "." + signature;
        ApiContractAttribute? declaration = member.GetCustomAttribute<ApiContractAttribute>(false);
        bool math = type == typeof(Vector3) || type == typeof(Quaternion) || type == typeof(Mathf)
            || type == typeof(Vector2) || type == typeof(Vector4) || type == typeof(CollectionReadResult) || type == typeof(RandomStream) || type == typeof(Matrix4)
            || type == typeof(DisplayResolution) || type == typeof(AssetLoadStatus);
        bool nativeService = typeof(NativeComponent).IsAssignableFrom(type) || type == typeof(RigidBody);
        bool coreService = type == typeof(Transform) || type == typeof(ScriptTime) || type == typeof(ScriptInput)
            || type == typeof(ScriptObjects) || type == typeof(ScriptPhysics) || type == typeof(ScriptScene)
            || type == typeof(GameObject) || type == typeof(Component) || type == typeof(Script)
            || type == typeof(ScriptJobs) || type == typeof(ScriptEvents) || type == typeof(ScriptDataAssets)
            || type == typeof(ScriptApplication) || type == typeof(ScriptCursor) || type == typeof(CursorRequest)
            || type == typeof(ScriptRender) || type == typeof(ScriptDisplay)
            || type == typeof(ScriptAssets) || type == typeof(AssetLease)
            || type == typeof(UI) || type == typeof(ScriptAudio) || typeof(ScriptStore).IsAssignableFrom(type)
            || type.FullName is "Fbzz.ButtonAction" or "Fbzz.AxisAction";
        bool property = member is PropertyInfo;
        bool field = member is FieldInfo;
        bool read = member is PropertyInfo p ? p.GetGetMethod() is not null : field;
        bool write = member is PropertyInfo q ? q.GetSetMethod() is not null : member is FieldInfo f ? !f.IsInitOnly && !f.IsLiteral : true;
        bool engine = declaration?.Purity == ApiPurity.ENGINE || nativeService || coreService;
        bool cached = CachedFacade(type, member.Name);
        bool identity = (type == typeof(GameObject) || type == typeof(Component) || type == typeof(NativeComponent)
            || type == typeof(RigidBody) || type == typeof(Transform))
            && member.Name is "Equals" or "GetHashCode" or "op_Equality" or "op_Inequality";
        bool pure = declaration?.Purity == ApiPurity.MANAGED || math || cached || identity;
        bool play = type == typeof(ScriptRender) || type == typeof(ScriptDisplay) || type == typeof(ScriptAssets) || type == typeof(AssetLease)
            || nativeService && !(typeof(NativeComponent).IsAssignableFrom(type) && member.Name == "IsAlive")
            || type == typeof(ScriptPhysics) || type == typeof(ScriptJobs) || type == typeof(ScriptEvents)
            || (type == typeof(ScriptScene) && member.Name is "Create" or "Instantiate" or "CanCreate" or "Destroy" or "DestroyMany"
                or "CreateMany" or "InstantiateMany" or "CreatePool")
            || (type == typeof(GameObject) && member.Name is "Destroy" or "AddComponent" or "RemoveComponent");
        string[] defaultRead = pure ? ["any"] : engine ? s_read : s_unknown;
        string[] defaultCall = pure ? ["any"] : engine ? s_live : s_unknown;
        string lifetime = declaration?.Lifetime ?? (pure ? "managed_value" : type == typeof(GameObject) || type == typeof(Transform)
            ? "non_owning_scene_entity_identity; GUID_recreation_does_not_revive_reference"
            : nativeService ? "non_owning_scene_entity_component_birth_identity; removal_is_dead; readdition_does_not_revive"
            : type == typeof(Component) || type == typeof(Script) ? "managed_script_generation_and_scene_entity"
            : coreService ? "owning_script_generation_and_scene; current_caller_authority" : "unknown");
        string failure = declaration?.Failure ?? (identity ? "immutable_identity_comparison; ordinary_CLR_null; destroyed_reference_is_not_null; recreated_GUID_has_different_identity"
            : math ? "managed_value_operation; documented_float32_and_degenerate_input_rules"
            : cached ? "no_native_call" : engine ? "exception_on_invalid_scope_thread_lifetime_or_native_rejection" : "unknown");
        HashSet<string> parameters = member is MethodBase method
            ? method.GetParameters().Select(value => value.Name ?? string.Empty).ToHashSet(StringComparer.Ordinal) : new(StringComparer.Ordinal);
        Dictionary<string, string> ParameterValues(string[]? source)
        {
            Dictionary<string, string> values = parameters.ToDictionary(name => name, _ => "unknown", StringComparer.Ordinal);
            foreach (string pair in source ?? [])
            {
                int separator = pair.IndexOf('=');
                if (separator <= 0 || !parameters.Contains(pair[..separator]))
                    throw new InvalidOperationException("Invalid SDK parameter contract: " + id + " " + pair);
                values[pair[..separator]] = pair[(separator + 1)..];
            }
            return values;
        }
        return new {
            id, knowledge = declaration is not null ? "declared" : math || engine || cached ? "scope_only" : "unknown",
            provenance = declaration is not null ? "SDK_member_attribute" : math || engine || cached ? "SDK_facade_scope" : "unknown",
            pure = pure ? (bool?)true : engine ? false : null,
            units = new { result = declaration?.Units ?? "unknown", parameters = ParameterValues(declaration?.ParameterUnits) },
            coordinate = new { result = declaration?.Coordinate ?? "unknown", parameters = ParameterValues(declaration?.ParameterCoordinates) },
            allowedCallbacks = new {
                read = property || field ? read ? ResolveCallbacks(declaration?.Read, defaultRead) : Array.Empty<string>() : Array.Empty<string>(),
                write = property || field ? write ? ResolveCallbacks(declaration?.Write, defaultCall) : Array.Empty<string>() : Array.Empty<string>(),
                call = property || field ? Array.Empty<string>() : ResolveCallbacks(declaration?.Call, defaultCall)
            },
            conditionalAllowedCallbacks = new {
                condition = declaration?.AccessCondition,
                read = property || field ? read && declaration?.ConditionalRead is not null
                    ? ResolveCallbacks(declaration.ConditionalRead, []) : Array.Empty<string>() : Array.Empty<string>(),
                call = !property && !field && declaration?.ConditionalCall is not null
                    ? ResolveCallbacks(declaration.ConditionalCall, []) : Array.Empty<string>()
            },
            runModes = declaration?.RunModes ?? (pure ? new[] { "any" } : engine ? play ? new[] { "Play" } : new[] { "Edit", "Play" } : s_unknown),
            runModeAccess = new {
                read = property || field ? read ? declaration?.ReadRunModes ?? declaration?.RunModes ?? (pure ? new[] { "any" } : engine ? play ? new[] { "Play" } : new[] { "Edit", "Play" } : s_unknown) : Array.Empty<string>() : Array.Empty<string>(),
                write = property || field ? write ? declaration?.WriteRunModes ?? declaration?.RunModes ?? (pure ? new[] { "any" } : engine ? play ? new[] { "Play" } : new[] { "Edit", "Play" } : s_unknown) : Array.Empty<string>() : Array.Empty<string>(),
                call = property || field ? Array.Empty<string>() : declaration?.CallRunModes ?? declaration?.RunModes ?? (pure ? new[] { "any" } : engine ? play ? new[] { "Play" } : new[] { "Edit", "Play" } : s_unknown)
            },
            thread = pure ? "any" : engine ? "current_main_thread_callback" : "unknown",
            failureBehavior = new { category = failure == "unknown" ? "unknown" : pure ? "managed_value" : "documented_result_or_exception", contract = failure }, lifetime
        };
    }

    internal static string[] ResolveCallbacks(string? policy, string[] fallback) => policy switch {
        null => fallback,
        "live" => s_live,
        "read" or "input" or "subscription" => s_read,
        "query" => s_query,
        "fixed" => s_fixed,
        "draw" => s_draw,
        "time" => s_time,
        "physics" => s_ground,
        "any" => ["any"],
        "none" => [],
        "unknown" => s_unknown,
        _ => throw new InvalidOperationException("Unknown SDK callback contract: " + policy)
    };

    static bool CachedFacade(Type type, string name) => type == typeof(GameObject) && name == "Transform"
        || (type == typeof(Transform) || typeof(NativeComponent).IsAssignableFrom(type) || type == typeof(RigidBody)) && name == "GameObject";
}
