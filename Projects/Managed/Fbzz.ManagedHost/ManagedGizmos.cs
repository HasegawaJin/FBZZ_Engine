/// @file    ManagedGizmos.cs
/// @brief   設定だけを複製した非公開個体から読み取り専用の描画要求を取得する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    sealed class DrawLookup : IScriptComponentLookup
    {
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type)
            => throw new InvalidOperationException("Draw callbacks cannot acquire managed Script instances or register jobs/events.");
    }
    /// @note caller は native 描画 scope を所有する。候補の field 変更・constructor・private cache を live 個体へ戻さない。
    public ManagedRuntimeResult DrawGizmos(ulong generation, ulong instanceId, ulong ownerId, ulong callId,
        ScriptCallback callback, int commandCapacity, out byte[] commands)
    {
        commands = [];
        if (!IsPreparationThread() || callId == 0) return new(BridgeStatus.INVALID_SCOPE);
        if (callback is not (ScriptCallback.DRAW_GIZMOS or ScriptCallback.DRAW_GIZMOS_SELECTED) || commandCapacity < 1)
            return new(BridgeStatus.INVALID_ARGUMENT);
        if (!TryInstance(generation, instanceId, out ScriptInstance? record) || record.OwnerId != ownerId || !record.Binding.Active)
            return new(BridgeStatus.STALE_GENERATION);
        if (record.Binding.GameObject.ObjectId == 0) return new(BridgeStatus.INVALID_SCOPE);
        ScriptCallbackDescriptor? draw = record.Type.Callbacks.FirstOrDefault(value => value.Callback == callback);
        if (draw is null) { commands = new byte[4]; return new(BridgeStatus.OK); }
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try {
            Script candidate = (Script)SettingsDocument.Clone(record.Type, record.Instance);
            candidate.Binding = record.Binding;
            ScriptExecutionScope scope = new(m_bridge, new DrawLookup(), generation, instanceId, ownerId, callId,
                record.Binding.GameObject.SceneLifetime, callback) { Gizmos = new(commandCapacity) };
            ScriptApiRuntime.Current = scope;
            draw.CreateInvocation(candidate)();
            using MemoryStream stream = new(checked(4 + scope.Gizmos.Commands.Count * 52));
            using BinaryWriter writer = new(stream);
            writer.Write((uint)scope.Gizmos.Commands.Count);
            foreach (GizmoDrawCommand value in scope.Gizmos.Commands) {
                writer.Write((uint)value.Kind); writer.Write(value.A.X); writer.Write(value.A.Y); writer.Write(value.A.Z);
                writer.Write(value.B.X); writer.Write(value.B.Y); writer.Write(value.B.Z); writer.Write(value.Radius);
                writer.Write(value.Color.X); writer.Write(value.Color.Y); writer.Write(value.Color.Z); writer.Write(value.Color.W);
                writer.Write(value.DepthTest ? 1U : 0U);
            }
            commands = stream.ToArray();
            return new(BridgeStatus.OK);
        } catch (Exception exception) { return new(BridgeStatus.CALLBACK_FAILED, Describe(exception)); }
        finally { ScriptApiRuntime.Current = previous; }
    }
}
