/// @file    ManagedDataAssets.cs
/// @brief   世代付き DataAsset schema と既存 TOML アセットの独立した値コピーを接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Buffers;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal interface IScriptDataAssetNativeBridge
{
    string? ReadDataAsset(ScriptExecutionScope scope, string reference);
    Stream? OpenDataAssetStream(ScriptExecutionScope scope, string reference)
    { string? json = ReadDataAsset(scope, reference); return json is null ? null : new MemoryStream(Encoding.UTF8.GetBytes(json), writable: false); }
}

public sealed partial class ManagedScriptRuntime : IScriptDataAssetRuntime
{
    DataAsset? IScriptDataAssetRuntime.LoadDataAsset(ScriptExecutionScope scope, Type requestedType, string reference)
    {
        if (!ScriptApiRuntime.IsSameScope(ScriptApiRuntime.Current, scope) || !m_generations.TryGetValue(scope.Generation, out ScriptGeneration? generation))
            throw new InvalidOperationException("DataAsset requires the current generation scope.");
        DataAssetTypeDescriptor descriptor = generation.Catalog.DataAssets.FirstOrDefault((DataAssetTypeDescriptor value) => value.Type == requestedType)
            ?? throw new InvalidOperationException("DataAsset type belongs to an unavailable code generation.");
        if (m_bridge is not IScriptDataAssetNativeBridge bridge) throw new NotSupportedException("DataAsset native services are unavailable.");
        using Stream? source = bridge.OpenDataAssetStream(scope, reference);
        if (source is null) return null;
        ScriptExecutionScope? previous = ScriptApiRuntime.Current;
        try
        {
            ScriptApiRuntime.Current = null;
            using JsonDocument document = JsonDocument.Parse(source, new JsonDocumentOptions { MaxDepth = StructuredFieldCodec.MAX_DEPTH + 4 });
            JsonElement root = document.RootElement;
            HashSet<string> keys = new(StringComparer.Ordinal);
            foreach (JsonProperty property in root.EnumerateObject())
                if (!keys.Add(property.Name)) throw new InvalidDataException("DataAsset has duplicate fields.");
            string declaredType = root.GetProperty("type").GetString()!;
            if (declaredType != descriptor.PersistentId && declaredType != descriptor.TypeName
                && !(requestedType.Name == declaredType && generation.Catalog.DataAssets.Count((DataAssetTypeDescriptor value) => value.Type.Name == declaredType) == 1))
                throw new InvalidDataException("DataAsset stored type does not match the requested type.");
            PortableDataAssetDocument.ValidateSchema(descriptor);
            if (root.TryGetProperty("formatVersion", out _)) return (DataAsset)PortableDataAssetDocument.Read(descriptor, root);
            /// @note Passive reads use explicit schema defaults; initializer defaults run only during an explicit create action.
            DataAsset result = (DataAsset)SettingsDocument.InertValue(descriptor);
            Dictionary<ScriptFieldDescriptor, object?> fields = new();
            foreach (ScriptFieldDescriptor field in descriptor.Fields.Where((ScriptFieldDescriptor field) => field.IsSetting))
            {
                JsonElement value;
                if ((root.TryGetProperty(field.Key, out _) ? 1 : 0) + field.PreviousKeys.Count((string key) => root.TryGetProperty(key, out _)) > 1)
                    throw new InvalidDataException($"DataAsset field has ambiguous migration keys: {field.Key}.");
                bool found = root.TryGetProperty(field.Key, out value);
                if (!found)
                    foreach (string key in field.PreviousKeys) if (root.TryGetProperty(key, out value)) { found = true; break; }
                if (!found) continue;
                object? decoded = field.Kind == ScriptFieldKind.STRUCTURED && value.ValueKind == JsonValueKind.Object && value.TryGetProperty("json", out JsonElement envelope)
                    ? StructuredFieldCodec.Decode(envelope.GetString()!, field.ValueType, static (string _) => throw new InvalidDataException("DataAsset cannot contain scene references."))
                    : ScriptFieldCodec.DecodeJson(value, field, static (string _) => throw new InvalidDataException("DataAsset cannot contain scene references."));
                if (!field.Validate(decoded) || (field.Kind == ScriptFieldKind.STRING && decoded is null)) throw new InvalidDataException($"Invalid DataAsset field: {field.Key}.");
                fields.Add(field, decoded);
            }
            foreach (ScriptFieldDescriptor field in descriptor.Fields.Where((ScriptFieldDescriptor field) => field.IsSetting))
            {
                object? value = fields.TryGetValue(field, out object? restored) ? restored : field.Read(result);
                if (!field.Validate(value) || (field.Kind == ScriptFieldKind.STRING && value is null))
                    throw new InvalidDataException($"Invalid DataAsset default: {field.Key}.");
            }
            foreach (KeyValuePair<ScriptFieldDescriptor, object?> field in fields) field.Key.AssignPrepared(result, field.Value);
            return result;
        }
        finally { ScriptApiRuntime.Current = previous; }
    }
}

public static unsafe partial class EntryPoint
{
    sealed partial class NativeBridge : IScriptDataAssetNativeBridge
    {
        public Stream? OpenDataAssetStream(ScriptExecutionScope scope, string reference)
        {
            byte[] input = s_utf8.GetBytes(reference); BridgeReply opened = default;
            fixed (byte* bytes = input) {
                BridgeRequest request = Request(scope, 0, BridgeOp.READ_ASSET); request.Flags = 1; request.Input = bytes; request.InputBytes = (uint)input.Length;
                int status = m_dispatch(&request, &opened);
                if (status == (int)BridgeStatus.NOT_FOUND && opened.Status == BridgeStatus.NOT_FOUND) return null;
                RequireSuccess(status, opened.Status);
            }
            ulong token = unchecked((ulong)opened.Integer);
            if (token == 0 || opened.Generation > long.MaxValue) throw new InvalidDataException("Native asset snapshot identity/size is invalid.");
            ImmutableValueSnapshot.SnapshotStream? result = new(new SnapshotStorageBudget());
            byte[] packet = ArrayPool<byte>.Shared.Rent(65536);
            try {
                long offset = 0, total = (long)opened.Generation;
                while (offset < total) {
                    fixed (byte* output = packet) {
                        BridgeRequest request = Request(scope, token, BridgeOp.READ_ASSET); request.Flags = 2; request.Index = 65536;
                        request.Reserved0 = (uint)offset; request.Reserved1 = (uint)((ulong)offset >> 32); request.Output = output; request.OutputCapacity = 65536;
                        BridgeReply reply = default; int status = m_dispatch(&request, &reply); RequireSuccess(status, reply.Status);
                        if (reply.Generation != opened.Generation || reply.OutputBytes == 0 || reply.OutputBytes > 65536
                            || reply.Integer != offset + reply.OutputBytes || reply.Flags != (reply.Integer == total ? 1U : 0U))
                            throw new InvalidDataException("Native asset snapshot changed its bounded continuation contract.");
                        result.Write(packet.AsSpan(0, (int)reply.OutputBytes)); offset = reply.Integer;
                    }
                }
                result.Position = 0; Stream ready = result; result = null; return ready;
            } finally {
                result?.Dispose(); ArrayPool<byte>.Shared.Return(packet, clearArray: true);
                BridgeRequest close = Request(scope, token, BridgeOp.READ_ASSET); close.Flags = 3; BridgeReply reply = default; _ = m_dispatch(&close, &reply);
            }
        }
        public string? ReadDataAsset(ScriptExecutionScope scope, string reference)
        {
            using Stream? source = OpenDataAssetStream(scope, reference);
            if (source is null) return null;
            using StreamReader reader = new(source, s_utf8, detectEncodingFromByteOrderMarks: false); return reader.ReadToEnd();
        }
    }
}
