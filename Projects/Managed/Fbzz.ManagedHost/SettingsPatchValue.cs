/// @file    SettingsPatchValue.cs
/// @brief   Script と DataAsset に共通の leaf・collection・Unicode text 候補変換。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace Fbzz.ManagedHost;

internal static class SettingsPatchValue
{
    /// @note Text ranges use original observed indices; descending application preserves every distinct page edit even when replacement lengths differ.
    internal static void ApplyBatch(ManagedTypeDescriptor descriptor, object candidate, JsonElement patches,
        Func<JsonElement, Type, string, object?> decode, Action<string> unique, ISet<string> structural)
    {
        if (patches.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Settings patches require an array.");
        Dictionary<string, List<JsonElement>> texts = new(StringComparer.Ordinal);
        foreach (JsonElement patch in patches.EnumerateArray()) {
            string action = patch.TryGetProperty("action", out JsonElement name) ? name.GetString() ?? "" : "set";
            if (action != "text_replace") { Apply(descriptor, candidate, patch, decode, unique, structural); continue; }
            string path = patch.GetProperty("path").GetString() ?? "";
            SettingsLeafChanges.Location location = SettingsLeafChanges.Locate(descriptor, candidate, path);
            if (!texts.TryGetValue(location.Path, out List<JsonElement>? ranges)) texts.Add(location.Path, ranges = new());
            ranges.Add(patch);
        }
        foreach ((string path, List<JsonElement> ranges) in texts) {
            unique(path);
            JsonElement[] ordered = ranges.OrderByDescending(value => value.GetProperty("index").GetInt32()).ToArray();
            long previousStart = long.MaxValue;
            foreach (JsonElement patch in ordered) {
                int start = patch.GetProperty("index").GetInt32(), count = patch.GetProperty("count").GetInt32();
                if (start < 0 || count < 0 || (long)start + count > previousStart || start == previousStart)
                    throw new InvalidDataException("Text replacement ranges overlap or repeat an insertion point.");
                Apply(descriptor, candidate, patch, decode, _ => { }, structural); previousStart = start;
            }
        }
    }
    internal static void Apply(ManagedTypeDescriptor descriptor, object candidate, JsonElement patch,
        Func<JsonElement, Type, string, object?> decode, Action<string> unique, ISet<string> structural)
    {
        Keys(patch, "path", "action", "value", "index", "count", "from", "to");
        string path = patch.GetProperty("path").GetString() ?? "";
        string action = patch.TryGetProperty("action", out JsonElement operation) ? operation.GetString() ?? "" : "set";
        SettingsLeafChanges.Location location = SettingsLeafChanges.Locate(descriptor, candidate, path, create: action == "create");
        unique(location.Path);
        object? value;
        if (action == "create") {
            if (patch.TryGetProperty("value", out _)) throw new InvalidDataException("Create-value patches cannot supply a value.");
            try { value = SettingsLeafChanges.CreateValue(location.ValueType, location.Range); }
            catch (Exception exception) { throw new InvalidDataException($"CS_CREATE_SETTING_VALUE {descriptor.TypeName}{location.Path}: {exception.GetBaseException().Message}", exception); }
        } else if (action is "remove" or "move") {
            if (location.Append || ManagedFieldSchema.Element(location.ValueType) is not Type element) throw new InvalidDataException("Structural settings edits require a collection path.");
            object? current = SettingsLeafChanges.Get(location.Field.Read(candidate), location.Field.ValueType, location.Segments);
            if (current is not IList list) throw new InvalidDataException("Structural settings edit collection is null.");
            List<object?> next = list.Cast<object?>().ToList();
            if (action == "remove") {
                int index = patch.GetProperty("index").GetInt32(), count = patch.TryGetProperty("count", out JsonElement length) ? length.GetInt32() : 1;
                if (index < 0 || count < 0 || index > next.Count || count > next.Count - index) throw new InvalidDataException("Settings removal range is out of bounds.");
                next.RemoveRange(index, count);
            } else {
                int from = patch.GetProperty("from").GetInt32(), to = patch.GetProperty("to").GetInt32();
                if (from < 0 || to < 0 || from >= next.Count || to >= next.Count) throw new InvalidDataException("Settings move indices are out of bounds.");
                object? moved = next[from]; next.RemoveAt(from); next.Insert(to, moved);
            }
            if (location.ValueType.IsArray) { Array array = Array.CreateInstance(element, next.Count); for (int index = 0; index < next.Count; index++) array.SetValue(next[index], index); value = array; }
            else { IList items = (IList)Activator.CreateInstance(location.ValueType)!; foreach (object? item in next) items.Add(item); value = items; }
            structural.Add(location.Path);
        } else if (action == "text_replace") {
            if (location.ValueType != typeof(string)) throw new InvalidDataException("Text replacement requires a string path.");
            string current = (string)SettingsLeafChanges.Get(location.Field.Read(candidate), location.Field.ValueType, location.Segments)!;
            int index = patch.GetProperty("index").GetInt32(), count = patch.GetProperty("count").GetInt32();
            if (index < 0 || count < 0 || index > current.Length || count > current.Length - index
                || (index > 0 && index < current.Length && char.IsLowSurrogate(current[index]) && char.IsHighSurrogate(current[index - 1]))
                || (index + count > 0 && index + count < current.Length && char.IsHighSurrogate(current[index + count - 1]) && char.IsLowSurrogate(current[index + count])))
                throw new InvalidDataException("Text replacement bounds must retain complete surrogate pairs.");
            string inserted = patch.GetProperty("value").GetString() ?? throw new InvalidDataException("Text replacement requires a string value.");
            _ = new UTF8Encoding(false, true).GetByteCount(inserted); value = current.Remove(index, count).Insert(index, inserted);
        } else if (action == "set") value = decode(patch.GetProperty("value"), location.ValueType, location.Path);
        else throw new InvalidDataException("Settings patch action must be set, create, remove, move or text_replace.");
        location.Field.AssignPrepared(candidate, SettingsLeafChanges.Set(location.Field.Read(candidate), location.Field.ValueType, location.Segments, value, append: location.Append));
    }
    internal static void Keys(JsonElement document, params string[] allowed)
    {
        if (document.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Expected a settings object.");
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (JsonProperty property in document.EnumerateObject())
            if (!keys.Add(property.Name) || !allowed.Contains(property.Name, StringComparer.Ordinal)) throw new InvalidDataException("Unknown or duplicate settings key.");
    }
}
