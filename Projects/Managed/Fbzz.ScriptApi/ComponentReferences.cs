/// @file    ComponentReferences.cs
/// @brief   保存アドレスと実行時の正確な寿命を区別する型付き Component/Script 参照。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Diagnostics.CodeAnalysis;

namespace Fbzz;

internal sealed record ComponentReferenceIdentity(string Kind, string OwnerGuid, uint ComponentKind, string SavedScriptId, string TypeKey,
    ulong ObjectId = 0, ulong SceneLifetime = 0, ulong ComponentLifetime = 0, ulong ScriptInstanceId = 0, ulong Generation = 0, string ScriptBirth = "");
internal interface IComponentReference { ComponentReferenceIdentity? Identity { get; } }
internal interface IComponentReferenceLookup
{
    Component? GetHeldReference(ScriptExecutionScope scope, ComponentReferenceIdentity identity, Type declaredType);
}

/// @note default は未設定、保存アドレスだけなら欠損。TryGet は失効参照を GUID で再検索しない。
public readonly struct ComponentRef<T> : IComponentReference where T : Component
{
    readonly ComponentReferenceIdentity? m_identity;
    internal ComponentRef(ComponentReferenceIdentity? identity) { m_identity = identity; }
    ComponentReferenceIdentity? IComponentReference.Identity => m_identity;
    /// @note 現在の同期 read scope で渡された Component の個体を保持する。所有権と user Script の CLR 強参照は保持しない。
    [ApiContract(Call = "read", Failure = "unbound_expired_or_foreign_component_throws", Purity = ApiPurity.ENGINE)]
    public ComponentRef(T component)
    {
        ArgumentNullException.ThrowIfNull(component);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (component is Script) {
            ScriptBinding binding = component.Binding ?? throw new InvalidOperationException("Script reference is unbound.");
            if (!binding.Active || binding.Generation != scope.Generation || binding.GameObject.SceneLifetime != scope.SceneLifetime || binding.SavedScriptId.Length == 0 || binding.ScriptBirth.Length == 0)
                throw new InvalidOperationException("Script reference belongs to an expired generation or unresolved owner.");
            m_identity = new("script", binding.GameObject.Guid, uint.MaxValue, binding.SavedScriptId, binding.TypeKey,
                binding.GameObject.ObjectId, binding.GameObject.SceneLifetime, ScriptInstanceId: binding.InstanceId, Generation: binding.Generation, ScriptBirth: binding.ScriptBirth);
        } else {
            uint kind = NativeComponentFactory.GetKind(component.GetType());
            if (kind == uint.MaxValue) throw new NotSupportedException("Component facade has no saved native type identity.");
            GameObject owner = component.GameObject;
            if (owner.SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("Component reference belongs to another Scene.");
            ulong lifetime = component switch { NativeComponent native => native.LifetimeToken, RigidBody body => body.LifetimeToken,
                Transform => owner.ObjectId, _ => 0 };
            m_identity = new("native", owner.Guid, kind, "", "", owner.ObjectId, owner.SceneLifetime, lifetime);
        }
        if (!TryGet(out _)) throw new InvalidOperationException("Component reference has expired or is pending removal.");
    }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public bool IsEmpty => m_identity is null;
    [ApiContract(Read = "read", AccessCondition = "native_reference_identity", ConditionalRead = "query", Failure = "false_for_empty_or_expired_identity; invalid_callback_scope_throws")]
    public bool IsAlive => TryGet(out _);
    [ApiContract(Read = "read", AccessCondition = "native_reference_identity", ConditionalRead = "query", Failure = "empty_is_not_missing; invalid_callback_scope_throws")]
    public bool IsMissing => !IsEmpty && !IsAlive;
    [ApiContract(Call = "read", AccessCondition = "native_reference_identity", ConditionalCall = "query", Failure = "false_and_null_for_empty_or_expired_identity; never_resolves_GUID")]
    public bool TryGet([NotNullWhen(true)] out T? component)
    {
        component = null;
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (m_identity is null || m_identity.ObjectId == 0 || m_identity.SceneLifetime != scope.SceneLifetime) return false;
        if (m_identity.Kind == "native") {
            Type? facade = NativeComponentFactory.GetType(m_identity.ComponentKind);
            if (facade is null || !typeof(T).IsAssignableFrom(facade) || m_identity.ComponentLifetime == 0
                || !scope.Bridge.IsAlive(scope, m_identity.ObjectId)
                || scope.Bridge.ReadComponentLifetime(scope, m_identity.ObjectId, m_identity.ComponentKind) != m_identity.ComponentLifetime) return false;
            component = (T?)NativeComponentFactory.Create(facade, new GameObject(m_identity.ObjectId, m_identity.SceneLifetime, m_identity.OwnerGuid), m_identity.ComponentLifetime);
        } else if (scope.Callback is ScriptCallback.DRAW_GIZMOS or ScriptCallback.DRAW_GIZMOS_SELECTED)
            throw new InvalidOperationException("Gizmo previews cannot access managed Script instances.");
        else if (scope.Components is IComponentReferenceLookup lookup) component = (T?)lookup.GetHeldReference(scope, m_identity, typeof(T));
        else throw new InvalidOperationException("This scope cannot access managed Script references.");
        return component is not null;
    }
    /// @return 明示的に現在の Scene で保存アドレスを解決した新しい参照。欠損ならアドレスを保持し、元の参照は変更しない。
    [ApiContract(Call = "read", Failure = "returns_missing_reference_if_address_unavailable; wrong_declared_type_throws")]
    public ComponentRef<T> Resolve()
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (m_identity is null) return default;
        ComponentReferenceIdentity next = scope.Bridge.ResolveComponentReference(scope, m_identity);
        ComponentRef<T> resolved = new(next);
        if (next.ObjectId != 0 && !resolved.TryGet(out _)) throw new InvalidOperationException("Saved Component reference is incompatible with its declared type or current identity.");
        return resolved;
    }
}
