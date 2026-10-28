/// @file    ScriptCatalog.cs
/// @brief   通常 C# の非公開 lifecycle と設定 field を世代単位で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using Fbzz;

[assembly: InternalsVisibleTo("Fbzz.ManagedTests")]

namespace Fbzz.ManagedHost;

public enum DiagnosticSeverity { WARNING, ERROR }
public sealed record ScriptSourceInfo(string Path, int Line, string ContentHash);

public sealed record ScriptDiagnostic(string Code, string TypeName, string MemberName, string Message, DiagnosticSeverity Severity);

public sealed class CatalogBuildResult
{
    internal CatalogBuildResult(ScriptCatalog? catalog, IReadOnlyList<ScriptDiagnostic> diagnostics)
    { Catalog = catalog; Diagnostics = diagnostics; }
    public ScriptCatalog? Catalog { get; }
    public IReadOnlyList<ScriptDiagnostic> Diagnostics { get; }
    public bool Success => Catalog is not null;
}

/// @note Type / FieldInfo / delegate は所有するゲーム世代の終了時に解放する。
public sealed class ScriptCatalog
{
    internal ScriptCatalog(IReadOnlyList<ScriptTypeDescriptor> types, IReadOnlyList<DataAssetTypeDescriptor>? assets = null)
    { Types = types; DataAssets = assets ?? []; }
    public IReadOnlyList<ScriptTypeDescriptor> Types { get; }
    public IReadOnlyList<DataAssetTypeDescriptor> DataAssets { get; }
    internal IEnumerable<ManagedTypeDescriptor> AllTypes => Types.Cast<ManagedTypeDescriptor>().Concat(DataAssets);
    public ScriptTypeDescriptor? Find(string name) => Types.FirstOrDefault((ScriptTypeDescriptor type) => type.TypeName == name || type.PersistentId == name);
}

public abstract class ManagedTypeDescriptor
{
    protected ManagedTypeDescriptor(Type type, ConstructorInfo constructor, IReadOnlyList<ScriptFieldDescriptor> fields)
    { Type = type; m_constructor = constructor; Fields = fields; }
    readonly ConstructorInfo m_constructor;
    public Type Type { get; }
    public string TypeName => Type.FullName ?? Type.Name;
    public string PersistentId { get; internal set; } = "";
    public IReadOnlyList<ScriptSourceInfo> Sources { get; internal set; } = [];
    public IReadOnlyList<ScriptFieldDescriptor> Fields { get; }
    internal object CreateValue() => m_constructor.Invoke(null);
}

public sealed class DataAssetTypeDescriptor : ManagedTypeDescriptor
{
    internal DataAssetTypeDescriptor(Type type, ConstructorInfo constructor, IReadOnlyList<ScriptFieldDescriptor> fields)
        : base(type, constructor, fields) { }
    public DataAsset CreateInstance() => (DataAsset)CreateValue();
    internal IReadOnlyList<ScriptCallbackDescriptor> AuthoringCallbacks { get; set; } = [];
}

public sealed partial class ScriptTypeDescriptor : ManagedTypeDescriptor
{
    internal ScriptTypeDescriptor(Type type, ConstructorInfo constructor, IReadOnlyList<ScriptFieldDescriptor> fields, IReadOnlyList<ScriptCallbackDescriptor> callbacks)
        : base(type, constructor, fields)
    {
        Callbacks = callbacks;
        RequiredComponentTypes = ScriptAuthoringMetadata.Requirements(type);
        ExecutionOrder = type.GetCustomAttribute<ExecutionOrderAttribute>(true)?.Order ?? 0;
        NativeRequiredComponentNames = RequiredComponentTypes.Select(ScriptAuthoringMetadata.NativeName)
            .Where(static name => name is not null).Select(static name => name!).ToArray();
    }
    public IReadOnlyList<ScriptCallbackDescriptor> Callbacks { get; }
    public bool ExecuteAlways => Type.IsDefined(typeof(ExecuteAlwaysAttribute), true);
    public Script CreateInstance() => (Script)CreateValue();
}

public sealed class ScriptCallbackDescriptor
{
    internal ScriptCallbackDescriptor(ScriptCallback callback, MethodInfo method) { Callback = callback; m_method = method; }
    readonly MethodInfo m_method;
    public ScriptCallback Callback { get; }
    public string Name => m_method.Name;
    internal void Invoke(object instance) => m_method.Invoke(instance, null);
    /// @note 個体ごとに一度解決し、通常フレームで反射検索を行わない。
    public Action CreateInvocation(Script instance) => m_method.CreateDelegate<Action>(instance);
    internal Delegate CreateTypedInvocation(Script instance)
        => ScriptCatalogBuilder.IsCollisionCallback(Callback) ? m_method.CreateDelegate<Action<Collision>>(instance)
            : ScriptCatalogBuilder.IsTriggerCallback(Callback) ? m_method.CreateDelegate<Action<GameObject?>>(instance)
            : Callback == ScriptCallback.ON_ANIMATION_EVENT ? m_method.CreateDelegate<Action<AnimationEvent>>(instance)
            : Callback == ScriptCallback.ON_ANIMATOR_MOVE ? m_method.CreateDelegate<Action<RootMotionInfo>>(instance)
            : Callback == ScriptCallback.ON_SEQUENCE_EVENT ? m_method.CreateDelegate<Action<SequenceEvent>>(instance)
            : Callback == ScriptCallback.ON_SEQUENCE_FINISHED ? m_method.CreateDelegate<Action<string>>(instance)
            : CreateInvocation(instance);
}

public sealed class ScriptFieldDescriptor
{
    internal ScriptFieldDescriptor(FieldInfo field, string key, ScriptFieldKind kind, bool setting, bool reload, string[] previousKeys)
    { m_field = field; Key = key; Kind = kind; IsSetting = setting; IsReloadState = reload; PreviousKeys = previousKeys; }
    readonly FieldInfo m_field;
    internal FieldInfo ReflectionField => m_field;
    public string Name => m_field.Name;
    public string Key { get; }
    public Type ValueType => m_field.FieldType;
    public ScriptFieldKind Kind { get; }
    public bool IsArray => Kind is >= ScriptFieldKind.FLOAT_ARRAY and <= ScriptFieldKind.GAME_OBJECT_ARRAY;
    public ScriptFieldKind ElementKind => IsArray ? (ScriptFieldKind)((uint)Kind - 6) : Kind;
    public Type ElementType => IsArray ? ValueType.GetElementType()! : ValueType;
    public bool IsSetting { get; }
    public bool IsReloadState { get; }
    public bool IsInspectable => Kind != ScriptFieldKind.NONE;
    public bool IsPublic => m_field.IsPublic;
    public bool RequiredReference => m_field.IsDefined(typeof(RequiredReferenceAttribute), false);
    public bool MayBeNull => ManagedFieldSchema.MayBeNull(ValueType);
    public IReadOnlyList<string> PreviousKeys { get; }
    public SettingAttribute? Metadata => m_field.GetCustomAttribute<SettingAttribute>(false);
    public ValueRangeAttribute? Range => m_field.GetCustomAttribute<ValueRangeAttribute>(false);
    public string Label => Metadata?.Label ?? (Name.StartsWith("m_", StringComparison.Ordinal) ? Name[2..] : Name);
    public bool Visible => Metadata?.Visible ?? true;
    public object? Read(object instance) => m_field.GetValue(instance);
    public bool Validate(object? value)
    {
        if (Range is ValueRangeAttribute range && !ManagedFieldSchema.WithinRange(value, range)) return false;
        if (Kind == ScriptFieldKind.ASSET_REFERENCE) return value is IAssetReference asset && !((asset.Reference ?? "").Contains('\0'));
        if (Kind == ScriptFieldKind.STRUCTURED) return StructuredFieldCodec.Validate(value, ValueType);
        if (IsArray)
        {
            if (value is not Array array || value.GetType() != ValueType) return false;
            foreach (object? element in array)
                if (!ValidateElement(element, ElementType, ElementKind, false)) return false;
            return true;
        }
        return ValidateElement(value, ValueType, Kind, true);
    }
    bool ValidateElement(object? value, Type expected, ScriptFieldKind kind, bool scalar)
    {
        if (kind == ScriptFieldKind.GAME_OBJECT) return value is null || value is GameObject;
        if (kind == ScriptFieldKind.STRING) return value is string || (scalar && value is null);
        if (value is null || value.GetType() != expected) return false;
        if (value is float number && !float.IsFinite(number)) return false;
        if (value is Vector3 vector) return vector.IsFinite;
        if (value is Quaternion rotation) return rotation.IsFinite;
        if (expected.IsEnum) return ScriptFieldCodec.ValidEnum(expected, value);
        return kind != ScriptFieldKind.NONE;
    }
    internal bool ValidateRequired(object? value, Func<GameObject, bool> isAlive)
        => StructuredFieldCodec.ValidateRequired(value, ValueType, RequiredReference, isAlive);
    internal bool ValidateRequired(object? value, Func<GameObject, bool> isAlive, out string path)
        => StructuredFieldCodec.ValidateRequired(value, ValueType, RequiredReference, isAlive, out path);
    internal bool ValidateRequired(object? value, Func<GameObject, bool> isAlive, Func<ComponentReferenceIdentity, bool> componentAlive, out string path)
        => StructuredFieldCodec.ValidateRequired(value, ValueType, RequiredReference, isAlive, out path, componentAlive);
    /// @note 検証に失敗した値は書き込まない。property getter を呼ばない。
    public bool TryWrite(Script instance, object? value)
    {
        if (!IsSetting || !Validate(value)) return false;
        m_field.SetValue(instance, value);
        return true;
    }
    internal void AssignPrepared(object instance, object? value) => m_field.SetValue(instance, value);
}

public static class ScriptCatalogBuilder
{
    static readonly IReadOnlyDictionary<string, ScriptLifecycleDefinition> s_callbackNames
        = ScriptLifecycleContract.Callbacks.ToDictionary(static value => value.Name, StringComparer.Ordinal);
    static readonly IReadOnlyDictionary<ScriptCallback, ScriptLifecycleDefinition> s_callbacks
        = ScriptLifecycleContract.Callbacks.ToDictionary(static value => value.Callback);
    static readonly IReadOnlyDictionary<ScriptCallback, Type?> s_arguments = s_callbacks.ToDictionary(static item => item.Key,
        static item => item.Value.ParameterTypeName is string name ? typeof(Script).Assembly.GetType(name) ?? Type.GetType(name, throwOnError: true) : null);

    internal static bool IsCollisionCallback(ScriptCallback callback)
        => s_callbacks.TryGetValue(callback, out ScriptLifecycleDefinition value) && value.ParameterTypeName == typeof(Collision).FullName;
    internal static bool IsTriggerCallback(ScriptCallback callback)
        => s_callbacks.TryGetValue(callback, out ScriptLifecycleDefinition value) && value.ParameterTypeName == typeof(GameObject).FullName;
    internal static Type? CallbackArgument(ScriptCallback callback)
        => s_arguments.TryGetValue(callback, out Type? type) ? type : null;
    internal static bool IsEngineNotification(ScriptCallback callback)
        => callback is ScriptCallback.ON_ANIMATION_EVENT or ScriptCallback.ON_ANIMATOR_MOVE or ScriptCallback.ON_SEQUENCE_EVENT
            or ScriptCallback.ON_SEQUENCE_FINISHED or ScriptCallback.ON_NAV_MESH_DESTINATION_REACHED or ScriptCallback.ON_NAV_MESH_PATH_FAILED
            or ScriptCallback.ON_NAV_MESH_TARGET_SPOTTED or ScriptCallback.ON_NAV_MESH_TARGET_LOST;

    public static CatalogBuildResult Build(Assembly assembly)
    {
        List<ScriptDiagnostic> diagnostics = new();
        List<ScriptTypeDescriptor> descriptors = new();
        List<DataAssetTypeDescriptor> assets = new();
        Type[] types;
        try { types = assembly.GetTypes(); }
        catch (ReflectionTypeLoadException exception)
        {
            diagnostics.Add(new("CS_TYPE_LOAD", assembly.GetName().Name ?? "", "", exception.Message, DiagnosticSeverity.ERROR));
            return new(null, diagnostics);
        }
        foreach (Type type in types.OrderBy((Type value) => value.FullName, StringComparer.Ordinal))
        {
            bool asset = typeof(DataAsset).IsAssignableFrom(type) && type != typeof(DataAsset);
            if ((!typeof(Script).IsAssignableFrom(type) && !asset) || type == typeof(Script) || type.IsAbstract) continue;
            if (type.IsNested || type.ContainsGenericParameters)
            {
                Add(diagnostics, type, "", "CS_SCRIPT_TYPE", "A concrete Script must be unnested and non-generic.");
                continue;
            }
            int before = diagnostics.Count;
            ConstructorInfo? constructor = type.GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, Type.EmptyTypes, null);
            if (constructor is null)
                Add(diagnostics, type, "", "CS_CONSTRUCTOR", "A concrete Script requires a parameterless constructor.");
            List<ScriptFieldDescriptor> fields = BuildFields(type, diagnostics);
            List<ScriptCallbackDescriptor> callbacks = asset ? [] : BuildCallbacks(type, diagnostics);
            foreach (string error in ScriptAuthoringMetadata.Validate(type, asset))
                Add(diagnostics, type, "", "CS_COMPONENT_REQUIREMENT", error);
            IReadOnlyList<EditorActionDescriptor> actions = [];
            try { actions = ManagedEditorActions.Discover(type); }
            catch (Exception exception) { Add(diagnostics, type, "", "CS_EDITOR_ACTION", exception.Message); }
            if (asset && fields.Any((ScriptFieldDescriptor field) => field.IsSetting && ContainsSceneReference(field.ValueType)))
                Add(diagnostics, type, "", "CS_ASSET_SCENE_REFERENCE", "DataAsset persists portable values and asset GUIDs; keep GameObject/Component/Script references in private runtime caches, not saved settings.");
            bool failed = diagnostics.Skip(before).Any((ScriptDiagnostic value) => value.Severity == DiagnosticSeverity.ERROR);
            if (!failed && constructor is not null)
            {
                if (asset) assets.Add(new(type, constructor, fields) { AuthoringCallbacks = BuildAssetCallbacks(type, diagnostics) });
                else descriptors.Add(new(type, constructor, fields, callbacks) { Actions = actions });
            }
        }
        bool errors = diagnostics.Any((ScriptDiagnostic value) => value.Severity == DiagnosticSeverity.ERROR);
        return new(errors ? null : new ScriptCatalog(descriptors, assets), diagnostics);
    }

    static List<ScriptCallbackDescriptor> BuildAssetCallbacks(Type type, List<ScriptDiagnostic> diagnostics)
    {
        Dictionary<ScriptCallback, ScriptCallbackDescriptor> callbacks = new();
        for (Type? current = type; current is not null && current != typeof(DataAsset); current = current.BaseType)
            foreach (MethodInfo method in current.GetMethods(BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)) {
                ScriptCallback callback = method.Name == "Reset" ? ScriptCallback.RESET : ScriptCallback.ON_VALIDATE;
                if (method.Name is not ("Reset" or "OnValidate")) continue;
                bool valid = !method.IsStatic && !method.IsGenericMethod && method.ReturnType == typeof(void) && method.GetParameters().Length == 0
                    && method.IsFamily && method.IsVirtual && method.GetBaseDefinition().DeclaringType == typeof(DataAsset)
                    && !method.IsDefined(typeof(AsyncStateMachineAttribute), false);
                if (!valid) Add(diagnostics, type, method.Name, "CS_AUTHORING_SIGNATURE", "DataAsset authoring requires a protected override synchronous void method without parameters.");
                else callbacks.TryAdd(callback, new(callback, method));
            }
        return callbacks.Values.ToList();
    }
    static List<ScriptCallbackDescriptor> BuildCallbacks(Type type, List<ScriptDiagnostic> diagnostics)
    {
        Dictionary<ScriptCallback, ScriptCallbackDescriptor> callbacks = new();
        foreach (Type declaring in Hierarchy(type))
        {
            MethodInfo[] methods = declaring.GetMethods(BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            foreach (MethodInfo method in methods.OrderBy((MethodInfo value) => value.MetadataToken))
            {
                if (!s_callbackNames.TryGetValue(method.Name, out ScriptLifecycleDefinition definition)) continue;
                ScriptCallback callback = definition.Callback;
                Type? argument = CallbackArgument(callback);
                ParameterInfo[] parameters = method.GetParameters();
                bool signature = argument is null ? parameters.Length == 0
                    : parameters.Length == 1 && parameters[0].ParameterType == argument && !parameters[0].IsOptional;
                bool valid = !method.IsStatic && !method.IsGenericMethod && method.ReturnType == typeof(void)
                    && method.IsFamily && method.IsVirtual && method.GetBaseDefinition().DeclaringType == typeof(Script)
                    && signature && !method.IsDefined(typeof(AsyncStateMachineAttribute), false);
                if (!valid)
                {
                    string expected = argument is null ? "no parameters" : $"one {argument.Name} parameter passed by value";
                    Add(diagnostics, type, method.Name, "CS_LIFECYCLE_SIGNATURE", $"Reserved lifecycle requires a protected override synchronous void method with {expected}.");
                    continue;
                }
                if (!callbacks.ContainsKey(callback)) callbacks.Add(callback, new(callback, method));
            }
        }
        return callbacks.Values.OrderBy((ScriptCallbackDescriptor value) => (uint)value.Callback).ToList();
    }

    static List<ScriptFieldDescriptor> BuildFields(Type type, List<ScriptDiagnostic> diagnostics)
    {
        List<ScriptFieldDescriptor> fields = new();
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (Type declaring in Hierarchy(type))
        {
            FieldInfo[] declared = declaring.GetFields(BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            foreach (FieldInfo field in declared.OrderBy((FieldInfo value) => value.MetadataToken))
            {
                if (field.IsDefined(typeof(CompilerGeneratedAttribute), false)) continue;
                SettingAttribute? setting = field.GetCustomAttribute<SettingAttribute>(false);
                bool reload = field.IsDefined(typeof(KeepOnReloadAttribute), false);
                bool nonSerialized = field.IsDefined(typeof(NonSerializedAttribute), false);
                bool mutable = !field.IsStatic && !field.IsLiteral && !field.IsInitOnly;
                ScriptFieldKind kind;
                try { kind = GetKind(field.FieldType); }
                catch (Exception exception)
                {
                    Add(diagnostics, type, field.Name, "CS_STRUCTURED_SCHEMA", exception.Message);
                    kind = ScriptFieldKind.NONE;
                }
                bool isSetting = mutable && !nonSerialized && (field.IsPublic || setting is not null) && kind != ScriptFieldKind.NONE;
                ValueRangeAttribute? range = field.GetCustomAttribute<ValueRangeAttribute>(false);
                if (range is not null) {
                    try { ManagedFieldSchema.ValidateRangeContract(field.FieldType, range); }
                    catch (Exception exception) { Add(diagnostics, type, field.Name, "CS_FIELD_RANGE", exception.Message); }
                }
                if (field.IsDefined(typeof(RequiredReferenceAttribute), false)
                    && (!isSetting || !StructuredFieldCodec.IsReferenceType(field.FieldType)))
                    Add(diagnostics, type, field.Name, "CS_REQUIRED_REFERENCE", "RequiredReference requires a saved GameObject or ComponentRef<T>, or a collection of these references.");
                if ((setting is not null || reload) && (!mutable || kind == ScriptFieldKind.NONE))
                    Add(diagnostics, type, field.Name, "CS_FIELD_UNSUPPORTED", typeof(Component).IsAssignableFrom(field.FieldType)
                        ? "Use ComponentRef<T> for saved Component/Script references; direct Component values are private runtime caches."
                        : "An explicit setting or reload field requires a supported mutable instance field.");
                if (setting is not null && nonSerialized)
                    Add(diagnostics, type, field.Name, "CS_FIELD_CONFLICT", "Setting and NonSerialized cannot be combined.");
                if (field.IsPublic && mutable && !nonSerialized && kind == ScriptFieldKind.NONE)
                    Add(diagnostics, type, field.Name, "CS_FIELD_NOT_SAVED", "This public field type is not included in the supported save codec.", DiagnosticSeverity.WARNING);
                if (field.IsStatic) continue;
                FieldKeyAttribute? keyMetadata = field.GetCustomAttribute<FieldKeyAttribute>(false);
                string key = keyMetadata is null ? field.Name : keyMetadata.Key;
                string[] previousKeys = field.GetCustomAttributes<PreviousFieldKeyAttribute>(false).Select((PreviousFieldKeyAttribute value) => value.Key).ToArray();
                /// @note Debug 専用 key は継承で隠れた同名 field を保存 field と混同しない。
                if (!isSetting && !reload) key = $"{declaring.FullName}.{field.Name}";
                if (isSetting || reload)
                {
                    if (string.IsNullOrWhiteSpace(key) || key.Contains('\0') || !keys.Add(key))
                        Add(diagnostics, type, field.Name, "CS_FIELD_KEY", "Saved field keys must be nonempty, contain no NUL, and be unique across the inheritance hierarchy.");
                    foreach (string previous in previousKeys)
                        if (string.IsNullOrWhiteSpace(previous) || previous.Contains('\0') || !keys.Add(previous))
                            Add(diagnostics, type, field.Name, "CS_FIELD_KEY", "Previous field keys must be nonempty, contain no NUL, and have an unambiguous destination.");
                }
                fields.Add(new(field, key, kind, isSetting, reload, previousKeys));
            }
        }
        return fields.OrderBy((ScriptFieldDescriptor value) => value.Key, StringComparer.Ordinal).ToList();
    }

    static IEnumerable<Type> Hierarchy(Type type)
    {
        for (Type? declaring = type; declaring is not null && declaring != typeof(Script) && declaring != typeof(Component) && declaring != typeof(DataAsset); declaring = declaring.BaseType)
            yield return declaring;
    }

    internal static ScriptFieldKind GetKind(Type type)
    {
        if (type.IsArray)
        {
            if (!type.IsSZArray) return ScriptFieldKind.NONE;
            ScriptFieldKind element = GetKind(type.GetElementType()!);
            return element is >= ScriptFieldKind.FLOAT and <= ScriptFieldKind.GAME_OBJECT ? (ScriptFieldKind)((uint)element + 6)
                : StructuredFieldCodec.IsSupported(type) ? ScriptFieldKind.STRUCTURED : ScriptFieldKind.NONE;
        }
        if (type == typeof(float)) return ScriptFieldKind.FLOAT;
        if (type == typeof(int) || (type.IsEnum && Enum.GetUnderlyingType(type) == typeof(int))) return ScriptFieldKind.INT;
        if (type == typeof(bool)) return ScriptFieldKind.BOOL;
        if (type == typeof(string)) return ScriptFieldKind.STRING;
        if (type == typeof(Vector3)) return ScriptFieldKind.VECTOR3;
        if (type == typeof(Quaternion)) return ScriptFieldKind.QUATERNION;
        if (type == typeof(GameObject)) return ScriptFieldKind.GAME_OBJECT;
        if (typeof(IAssetReference).IsAssignableFrom(type)) return ScriptFieldKind.ASSET_REFERENCE;
        return StructuredFieldCodec.IsSupported(type) ? ScriptFieldKind.STRUCTURED : ScriptFieldKind.NONE;
    }

    static bool ContainsSceneReference(Type type, HashSet<Type>? visited = null)
    {
        if (type == typeof(GameObject) || typeof(Component).IsAssignableFrom(type) || ComponentReferenceCodec.IsReference(type)) return true;
        if (type.IsArray) return ContainsSceneReference(type.GetElementType()!, visited);
        if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>)) return ContainsSceneReference(type.GetGenericArguments()[0], visited);
        if (type.IsPrimitive || type == typeof(string) || type.IsEnum || type == typeof(Vector3) || type == typeof(Quaternion)
            || (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(DataAssetRef<>))) return false;
        visited ??= new();
        if (!visited.Add(type)) return false;
        return type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .Any((FieldInfo field) => !field.IsStatic && (field.IsPublic || field.IsDefined(typeof(SettingAttribute), false))
                && !field.IsDefined(typeof(NonSerializedAttribute), false) && ContainsSceneReference(field.FieldType, visited));
    }

    static void Add(List<ScriptDiagnostic> diagnostics, Type type, string member, string code, string message, DiagnosticSeverity severity = DiagnosticSeverity.ERROR)
        => diagnostics.Add(new(code, type.FullName ?? type.Name, member, message, severity));
}
