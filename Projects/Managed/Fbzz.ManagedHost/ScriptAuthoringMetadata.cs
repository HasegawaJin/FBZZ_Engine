/// @file    ScriptAuthoringMetadata.cs
/// @brief   型付きの必須 Component と既定実行順を constructor 無しで取得する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed partial class ScriptTypeDescriptor
{
    public IReadOnlyList<Type> RequiredComponentTypes { get; }
    public int ExecutionOrder { get; }
    internal IReadOnlyList<string> NativeRequiredComponentNames { get; }
}

internal static class ScriptAuthoringMetadata
{
    internal static Type[] Requirements(Type type) => type.GetCustomAttributes<RequireComponentAttribute>(true)
        .SelectMany(static attribute => attribute.Types ?? []).Where(static value => value is not null).Distinct().ToArray();
    internal static IEnumerable<string> Validate(Type type, bool asset)
    {
        RequireComponentAttribute[] attributes = type.GetCustomAttributes<RequireComponentAttribute>(true).ToArray();
        if (asset && (attributes.Length != 0 || type.IsDefined(typeof(ExecutionOrderAttribute), true)))
            yield return "RequireComponent and ExecutionOrder are only supported on Script classes.";
        foreach (RequireComponentAttribute attribute in attributes) {
            if (attribute.Types is null || attribute.Types.Length == 0) { yield return "RequireComponent requires at least one Component type."; continue; }
            foreach (Type? required in attribute.Types)
                if (required is null || required.ContainsGenericParameters || required == typeof(Script)
                    || (NativeComponentFactory.GetKind(required) == uint.MaxValue
                        && (!typeof(Script).IsAssignableFrom(required) || required.Assembly != type.Assembly)))
                    yield return $"Unsupported required Component type: {required?.FullName ?? "null"}.";
        }
    }
    internal static string? NativeName(Type type) => NativeComponentFactory.GetKind(type) switch
    {
        1 => "RigidBodyComponent", 2 => "BoxColliderComponent", 3 => "SphereColliderComponent", 4 => "CapsuleColliderComponent",
        5 => "AnimatorComponent", 6 => "AudioSourceComponent", 8 => "CameraComponent", 9 => "NavMeshAgentComponent",
        10 => "CharacterControllerComponent", 11 => "UIText", 12 => "UIImage", 13 => "UIButton", 14 => "UICanvas",
        15 => "CylinderColliderComponent", 16 => "AabbColliderComponent", 17 => "MeshColliderComponent",
        18 => "ConvexHullColliderComponent", 19 => "TerrainColliderComponent", 20 => "UISlider", 21 => "UIToggle",
            22 => "UIScrollView", 23 => "UIInputField", 24 => "UICanvasGroup", 25 => "UIDragSource", 26 => "UIDropTarget",
            (uint)NativeComponentKind.LIGHT => "LightComponent", (uint)NativeComponentKind.PARTICLE_SYSTEM => "ParticleEmitter",
            (uint)NativeComponentKind.MATERIAL => "MaterialComponent",
            (uint)NativeComponentKind.VFX => "VFXComponent", (uint)NativeComponentKind.SEQUENCE => "SequencePlayerComponent",
            (uint)NativeComponentKind.TERRAIN => "TerrainComponent", (uint)NativeComponentKind.WATER => "WaterComponent", (uint)NativeComponentKind.JOINT => "JointComponent",
            (uint)NativeComponentKind.LINE_RENDERER => "LineRendererComponent",
            (uint)NativeComponentKind.SPRITE_RENDERER => "SpriteRendererComponent",
            (uint)NativeComponentKind.SORTING_GROUP => "SortingGroupComponent",
            (uint)NativeComponentKind.TRAIL => "TrailComponent",
            (uint)NativeComponentKind.MESH_TRAIL => "MeshTrailComponent",
            (uint)NativeComponentKind.MESH_RENDERER => "MeshRenderer",
            (uint)NativeComponentKind.SKINNED_MESH_RENDERER => "SkinnedMeshRenderer",
            (uint)NativeComponentKind.MOTION_WARP => "MotionWarpComponent",
            (uint)NativeComponentKind.IK_SOLVER => "IKSolverComponent",
            (uint)NativeComponentKind.RAGDOLL => "RagdollComponent",
            (uint)NativeComponentKind.SPRING_BONE => "SpringBoneComponent",
            (uint)NativeComponentKind.UI_ANIMATOR => "UIAnimator",
            (uint)NativeComponentKind.NAV_MESH_PATROL => "NavMeshPatrolComponent",
            (uint)NativeComponentKind.SPLINE_FOLLOWER => "SplineFollowerComponent",
            (uint)NativeComponentKind.CAMERA_SHAKE => "CameraShakeComponent",
            (uint)NativeComponentKind.VIRTUAL_CAMERA => "VirtualCameraComponent",
            (uint)NativeComponentKind.AUDIO_MIXER_SEND => "AudioMixerSendComponent",
            (uint)NativeComponentKind.FLOW_FIELD => "FlowField",
            (uint)NativeComponentKind.INSTANCED_MESH => "InstancedMeshComponent",
            _ => null
    };
}

public sealed partial class ManagedScriptRuntime
{
    readonly Dictionary<(ulong Generation, ulong Object, ulong Lifetime), Dictionary<string, ScriptInstance>> m_boundScripts = new();
    Dictionary<string, ScriptInstance>? BoundScripts(ulong generation, ulong objectId, ulong lifetime)
        => m_boundScripts.TryGetValue((generation, objectId, lifetime), out Dictionary<string, ScriptInstance>? values) ? values : null;
    void RemoveBoundScript(ScriptInstance record)
    {
        GameObject owner = record.Binding.GameObject;
        var key = (record.Generation, owner.ObjectId, owner.SceneLifetime);
        if (!m_boundScripts.TryGetValue(key, out Dictionary<string, ScriptInstance>? values)) return;
        if (values.TryGetValue(record.Binding.SavedScriptId, out ScriptInstance? stored) && stored == record)
            values.Remove(record.Binding.SavedScriptId);
        if (values.Count == 0) m_boundScripts.Remove(key);
    }
    void AddBoundScript(ScriptInstance record)
    {
        GameObject owner = record.Binding.GameObject;
        if (owner.ObjectId == 0) return;
        var key = (record.Generation, owner.ObjectId, owner.SceneLifetime);
        if (!m_boundScripts.TryGetValue(key, out Dictionary<string, ScriptInstance>? values)) {
            values = new(StringComparer.Ordinal); m_boundScripts.Add(key, values);
        }
        values.Add(record.Binding.SavedScriptId, record);
    }
    ManagedRuntimeResult ValidateComponentRequirements(ScriptInstance record, ScriptExecutionScope? currentScope = null)
    {
        if (record.Type.RequiredComponentTypes.Count == 0) return new(BridgeStatus.OK);
        GameObject owner = record.Binding.GameObject;
        if (owner.ObjectId == 0 || owner.SceneLifetime == 0) return new(BridgeStatus.INVALID_ARGUMENT, "Required Component owner is unresolved.");
        ScriptExecutionScope scope = currentScope ?? new(m_bridge, this, record.Generation, record.Binding.InstanceId, record.OwnerId, 0, owner.SceneLifetime, ScriptCallback.ON_VALIDATE);
        foreach (Type type in record.Type.RequiredComponentTypes) {
            uint kind = NativeComponentFactory.GetKind(type);
            Dictionary<string, ScriptInstance>? attached = BoundScripts(record.Generation, owner.ObjectId, owner.SceneLifetime);
            bool present = kind != uint.MaxValue ? m_bridge.ReadComponent(scope, owner.ObjectId, kind)
                : attached is not null && attached.Values.Any(value => value.Binding.Active
                    && type.IsInstanceOfType(value.Instance) && m_bridge.HasScriptEntry(scope, owner.ObjectId, value.Binding.SavedScriptId));
            if (!present) return new(BridgeStatus.INVALID_ARGUMENT,
                $"CS_REQUIRED_COMPONENT {record.Type.TypeName}: owner '{owner.Guid}' requires {type.FullName}; attach it explicitly.");
        }
        return new(BridgeStatus.OK);
    }
}
