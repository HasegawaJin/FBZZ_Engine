/// @file    RuntimeTextureTests.cs
/// @brief   Validate texture byte rectangles, actual-caller ownership and material lifetime fences.
/// @author  Hasegawa Jin
/// @date    2026-10-26
using System;
using System.Collections.Generic;
using Fbzz;

namespace Fbzz.ManagedTests;

internal static class RuntimeTextureTests
{
    internal static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(TextureRectangleValidationPreservesPitchAndBytes), TextureRectangleValidationPreservesPitchAndBytes),
        (nameof(TextureOwnershipAndDisposalNeverRetainCallbackAuthority), TextureOwnershipAndDisposalNeverRetainCallbackAuthority),
        (nameof(RuntimeTextureMaterialBindingChecksReceiverLifetime), RuntimeTextureMaterialBindingChecksReceiverLifetime)
    ];

    static ScriptExecutionScope Scope(TextureBridge bridge, ulong generation = 1, ulong instance = 9, ulong owner = 101)
        => new(bridge, new EmptyComponents(), generation, instance, owner, 13, 42, ScriptCallback.UPDATE);

    static void TextureRectangleValidationPreservesPitchAndBytes()
    {
        TextureBridge bridge = new(); ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try
        {
            ScriptApiRuntime.Current = Scope(bridge);
            RuntimeTexture mask = RuntimeTexture.CreateR8(4, 3);
            byte[] padded = [0, 64, 19, 23, 128, 255];
            Program.Check(mask.UpdateRegion(1, 1, 2, 2, padded, 4), "A padded R8 rectangle should upload.");
            Program.Check(bridge.UploadCalls == 1 && bridge.Pitch == 4 && bridge.Bytes.AsSpan().SequenceEqual(padded),
                "The transport retains exact UNORM bytes and padding, without reading beyond the final row.");
            Program.Throws<ArgumentOutOfRangeException>(() => mask.UpdateRegion(3, 0, 2, 1, padded));
            Program.Throws<ArgumentOutOfRangeException>(() => mask.UpdateRegion(0, 0, 2, 2, padded, 1));
            Program.Throws<ArgumentException>(() => mask.UpdateRegion(0, 0, 2, 2, new byte[5], 4));
            Program.Check(bridge.UploadCalls == 1, "Invalid rectangles are rejected before any native write.");
            Program.Throws<ArgumentOutOfRangeException>(() => RuntimeTexture.CreateR8(0, 32));
            RuntimeTexture rgba = RuntimeTexture.CreateRGBA8(4, 3);
            Program.Check(rgba.Format == RuntimeTextureFormat.RGBA8 && rgba.UpdateRegion(0, 0, 2, 2, new byte[20], 12)
                && bridge.Bytes.Length == 20, "RGBA8 uses four bytes per pixel and accepts padded rows.");
            bridge.TextureAlive = false;
            Program.Check(!mask.IsValid, "A native resource reset invalidates the retained texture facade.");
        }
        finally { ScriptApiRuntime.Current = previous; }
    }

    static void TextureOwnershipAndDisposalNeverRetainCallbackAuthority()
    {
        TextureBridge bridge = new(); ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try
        {
            ScriptApiRuntime.Current = Scope(bridge);
            RuntimeTexture texture = RuntimeTexture.CreateR8(2, 2);
            ScriptApiRuntime.Current = Scope(bridge, instance: 10, owner: 102);
            Program.Throws<InvalidOperationException>(() => { _ = texture.IsValid; });
            Program.Throws<InvalidOperationException>(() => texture.UpdateRegion(0, 0, 1, 1, new byte[1]));
            Program.Throws<InvalidOperationException>(() => texture.Dispose());
            ScriptApiRuntime.Current = new(bridge, new EmptyComponents(), 1, 10, 102, 13, 42, ScriptCallback.ON_DESTROY);
            Program.Throws<InvalidOperationException>(() => texture.Dispose());
            ScriptApiRuntime.Current = Scope(bridge);
            Program.Check(texture.IsValid, "A foreign cleanup callback cannot mark its owner's texture disposed.");
            ScriptApiRuntime.Current = null;
            Program.Throws<InvalidOperationException>(() => texture.UpdateRegion(0, 0, 1, 1, new byte[1]));
            ScriptApiRuntime.Current = Scope(bridge);
            texture.Dispose(); texture.Dispose();
            Program.Check(bridge.ReleaseCalls == 1 && !texture.IsValid, "Deterministic disposal releases exactly once.");
            ScriptApiRuntime.Current = null; texture.Dispose();
            ScriptApiRuntime.Current = Scope(bridge);
            RuntimeTexture retired = RuntimeTexture.CreateR8(2, 2);
            ScriptApiRuntime.Current = Scope(bridge, generation: 2);
            Program.Check(!retired.IsValid, "A previous generation is never revived by a fresh callback.");
            retired.Dispose();
            Program.Check(bridge.ReleaseCalls == 1, "Retirement already releases native ownership; stale disposal does not call into it.");
        }
        finally { ScriptApiRuntime.Current = previous; }
    }

    static void RuntimeTextureMaterialBindingChecksReceiverLifetime()
    {
        TextureBridge bridge = new(); ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try
        {
            ScriptApiRuntime.Current = Scope(bridge);
            RuntimeTexture texture = RuntimeTexture.CreateR8(4, 4);
            Material material = new(new GameObject(303, 42, "receiver"), 77);
            Program.Check(material.TrySetRuntimeTexture(0, "tex5", texture) && bridge.BoundObject == 303,
                "The actual Script may bind its texture to a separate live material owner.");
            bridge.AcceptBinding = false;
            Program.Check(!material.TrySetRuntimeTexture(0, "tex5", texture), "A reflected-property rejection remains false.");
            bridge.ComponentLifetime = 78;
            Program.Throws<InvalidOperationException>(() => material.TrySetRuntimeTexture(0, "tex5", texture));
            Program.Check(bridge.BindingCalls == 2, "A replacement Material cannot be modified through an old facade.");
        }
        finally { ScriptApiRuntime.Current = previous; }
    }

    sealed class EmptyComponents : IScriptComponentLookup
    {
        public Component[] GetComponents(ScriptExecutionScope scope, GameObject target, Type type) => [];
    }

    sealed class TextureBridge : Program.RecordingBridge, IScriptNativeBridge
    {
        internal int UploadCalls, ReleaseCalls, BindingCalls, Pitch;
        internal byte[] Bytes = [];
        internal bool TextureAlive = true, AcceptBinding = true;
        internal ulong ComponentLifetime = 77, BoundObject;
        long IScriptNativeBridge.CreateRuntimeTexture(ScriptExecutionScope scope, int width, int height, RuntimeTextureFormat format, out ulong ownerObject)
        { ownerObject = 202; return 77; }
        bool IScriptNativeBridge.UpdateRuntimeTexture(ScriptExecutionScope scope, ulong ownerObject, long texture,
            int x, int y, int width, int height, int rowPitch, ReadOnlySpan<byte> pixels)
        { UploadCalls++; Pitch = rowPitch; Bytes = pixels.ToArray(); return true; }
        ulong IScriptNativeBridge.ReadComponentLifetime(ScriptExecutionScope scope, ulong objectId, uint kind) => ComponentLifetime;
        NativeServiceResult IScriptNativeBridge.InvokeService(ScriptExecutionScope scope, ulong objectId, uint service, uint method,
            long integer, ReadOnlySpan<float> values, ReadOnlySpan<string> text, GameObject? related, bool readOnly)
        {
            if (service == (uint)ScriptServiceKind.SERVICE_ASSETS && method == 15) return new([], [], TextureAlive ? 1 : 0, null);
            if (service == (uint)ScriptServiceKind.SERVICE_ASSETS && method == 16) { ReleaseCalls++; return new([], [], 1, null); }
            if (service == (uint)ScriptServiceKind.SERVICE_MATERIAL && method == 42) {
                BindingCalls++; BoundObject = objectId;
                Program.Check(integer == 0 && text.Length == 2 && text[0] == "tex5" && text[1] == "77", "Binding preserves exact slot, name and integer token.");
                return new([], [], AcceptBinding ? 1 : 0, null);
            }
            throw new InvalidOperationException("Unexpected runtime texture service call.");
        }
    }
}
