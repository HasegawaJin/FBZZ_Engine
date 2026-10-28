/// @file    ManagedEditorActions.cs
/// @brief   安定 action ID の発見と独立した設定候補への同期実行。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed class EditorActionDescriptor
{
    internal EditorActionDescriptor(string key, string label, MethodInfo method) { Key = key; Label = label; m_method = method; }
    readonly MethodInfo m_method;
    public string Key { get; }
    public string Label { get; }
    internal void Invoke(Script candidate) => m_method.CreateDelegate<Action>(candidate)();
}

public sealed partial class ScriptTypeDescriptor
{
    public IReadOnlyList<EditorActionDescriptor> Actions { get; internal set; } = [];
}

internal static class ManagedEditorActions
{
    internal static IReadOnlyList<EditorActionDescriptor> Discover(Type type)
    {
        List<EditorActionDescriptor> actions = new();
        HashSet<string> keys = new(StringComparer.Ordinal);
        HashSet<MethodInfo> overrides = new();
        for (Type? declaring = type; declaring is not null && declaring != typeof(Script); declaring = declaring.BaseType)
            foreach (MethodInfo method in declaring.GetMethods(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly)) {
                if (!overrides.Add(method.GetBaseDefinition())) continue;
                EditorActionAttribute? metadata = method.GetCustomAttribute<EditorActionAttribute>(true);
                if (metadata is null) continue;
                if (!typeof(Script).IsAssignableFrom(type) || string.IsNullOrWhiteSpace(metadata.Key) || metadata.Key.Contains('\0')
                    || !keys.Add(metadata.Key) || method.IsStatic || method.IsGenericMethod || method.ReturnType != typeof(void)
                    || method.GetParameters().Length != 0 || method.IsDefined(typeof(AsyncStateMachineAttribute), false)
                    || ScriptLifecycleContract.Callbacks.Any(callback => callback.Name == method.Name))
                    throw new InvalidDataException($"CS_EDITOR_ACTION {type.FullName}.{method.Name}: action requires a unique nonempty ID and synchronous zero-argument instance void method outside lifecycle names.");
                actions.Add(new(metadata.Key, metadata.Label ?? method.Name, method));
            }
        return actions.OrderBy(action => action.Key, StringComparer.Ordinal).ToArray();
    }
    internal static void Invoke(ScriptTypeDescriptor type, Script candidate, string key)
    {
        EditorActionDescriptor action = type.Actions.SingleOrDefault(value => value.Key == key)
            ?? throw new InvalidDataException($"CS_EDITOR_ACTION {type.TypeName}: action '{key}' is absent from the current catalog generation.");
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        ScriptApiRuntime.Current = null;
        try { action.Invoke(candidate); }
        catch (Exception exception) { throw new InvalidDataException($"CS_EDITOR_ACTION {type.TypeName}.{key}: {ManagedScriptRuntime.Describe(exception)}", exception); }
        finally { ScriptApiRuntime.Current = previous; }
    }
}
