/// @file    ManagedSettingsSnapshot.cs
/// @brief   保存形式から独立した設定 snapshot と path 別の実体参照 identity を返す。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Fbzz;

namespace Fbzz.ManagedHost;

internal static class ManagedSettingsSnapshot
{
    /// @note Snapshot はユーザー callback と property getter を評価せず、参照 GUID を再解決しない。
    internal static string Read(ManagedScriptRuntime.ScriptInstance instance, ulong generation, ulong instanceId)
    {
        Dictionary<string, JsonElement> settings = new(StringComparer.Ordinal), observations = new(StringComparer.Ordinal);
        Dictionary<string, object> references = new(StringComparer.Ordinal);
        Dictionary<string, object> settingReferences = new(StringComparer.Ordinal);
        Dictionary<string, string> diagnostics = new(StringComparer.Ordinal);
        foreach (ScriptFieldDescriptor field in instance.Type.Fields) {
            if (!field.IsInspectable) continue;
            object? value = field.Read(instance.Instance);
            try {
                using JsonDocument json = JsonDocument.Parse(StructuredFieldCodec.EncodeValue(value, field.ValueType));
                (field.IsSetting ? settings : observations).Add(field.Key, json.RootElement.Clone());
                foreach ((string path, SettingsReferenceSnapshot.Identity identity) in SettingsReferenceSnapshot.Capture(value, field.ValueType)) {
                    string fullPath = StructuredFieldCodec.ChildPath("", field.Key) + path;
                    references.Add(fullPath, identity);
                    if (field.IsSetting) settingReferences.Add(fullPath, identity);
                }
            } catch (Exception exception) {
                if (field.IsSetting) throw;
                diagnostics.Add(field.Key, exception.GetBaseException().Message);
            }
        }
        string revision = SettingsDocument.Hash(instance.Type.Fields.Where(field => field.IsSetting).ToDictionary(field => field, field => field.Read(instance.Instance)));
        return JsonSerializer.Serialize(new {
            schemaVersion = 1, generation = generation.ToString(CultureInfo.InvariantCulture),
            instanceId = instanceId.ToString(CultureInfo.InvariantCulture), savedScriptId = instance.Binding.SavedScriptId, ownerGuid = instance.Binding.GameObject.Guid,
            revision, settings, observations, references, diagnostics
        });
    }
}
