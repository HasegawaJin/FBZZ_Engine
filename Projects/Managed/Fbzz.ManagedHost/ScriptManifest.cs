/// @file    ScriptManifest.cs
/// @brief   実行しない PE metadata 読取と永続型 ID の候補 manifest 検証。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text.Json;

namespace Fbzz.ManagedHost;

public sealed record ScriptSourceEntry(string ClrName, IReadOnlyList<ScriptSourceInfo> Sources);

/// @note ゲームの static 初期化・constructor・AssemblyLoadContext を起動せず、生成した assembly 属性だけを読む。
/// @see https://learn.microsoft.com/en-us/dotnet/api/system.reflection.metadata.metadatareader PE metadata reader。
public static class ScriptManifest
{
    public static IReadOnlyList<ScriptSourceEntry> ReadSources(string assemblyPath)
    {
        if (!Path.IsPathFullyQualified(assemblyPath)) throw new ArgumentException("Source discovery requires an absolute assembly path.");
        using FileStream stream = File.OpenRead(assemblyPath);
        using PEReader image = new(stream);
        MetadataReader metadata = image.GetMetadataReader();
        Dictionary<string, List<ScriptSourceInfo>> entries = new(StringComparer.Ordinal);
        foreach (CustomAttributeHandle handle in metadata.GetAssemblyDefinition().GetCustomAttributes())
        {
            CustomAttribute attribute = metadata.GetCustomAttribute(handle);
            if (!IsSourceAttribute(metadata, attribute.Constructor)) continue;
            BlobReader blob = metadata.GetBlobReader(attribute.Value);
            if (blob.ReadUInt16() != 1) throw new InvalidDataException("ScriptSource attribute prolog is invalid.");
            string clrName = blob.ReadSerializedString() ?? "";
            string sourcePath = blob.ReadSerializedString() ?? "";
            int line = blob.ReadInt32();
            string contentHash = blob.ReadSerializedString() ?? "";
            if (blob.ReadUInt16() != 0 || blob.RemainingBytes != 0 || string.IsNullOrWhiteSpace(clrName))
                throw new InvalidDataException("ScriptSource attribute payload is invalid.");
            ScriptSourceInfo source = new(sourcePath, line, contentHash);
            ValidateSource(source);
            if (!entries.TryGetValue(clrName, out List<ScriptSourceInfo>? sources)) entries.Add(clrName, sources = new());
            if (sources.Any((ScriptSourceInfo existing) => existing.Path == source.Path && existing.Line == source.Line))
                throw new InvalidDataException($"Duplicate ScriptSource declaration: {clrName} {source.Path}:{source.Line}.");
            sources.Add(source);
        }
        return entries.OrderBy((KeyValuePair<string, List<ScriptSourceInfo>> entry) => entry.Key, StringComparer.Ordinal)
            .Select((KeyValuePair<string, List<ScriptSourceInfo>> entry) => new ScriptSourceEntry(entry.Key, SortSources(entry.Value))).ToArray();
    }

    public static string SourcesJson(IReadOnlyList<ScriptSourceEntry> entries)
        => JsonSerializer.Serialize(entries.Select((ScriptSourceEntry entry) => new
        {
            clrName = entry.ClrName,
            sources = entry.Sources.Select((ScriptSourceInfo source) => new { path = source.Path, line = source.Line, contentHash = source.ContentHash })
        }));

    internal static void Apply(ScriptCatalog catalog, string assemblyPath, string assemblyName, bool internalMode)
    {
        IReadOnlyList<ScriptSourceEntry> sourceEntries = ReadSources(assemblyPath);
        Dictionary<string, ScriptSourceEntry> sourceMap = sourceEntries.ToDictionary((ScriptSourceEntry entry) => entry.ClrName, StringComparer.Ordinal);
        string manifestPath = assemblyPath + ".scripts.json";
        if (!File.Exists(manifestPath))
        {
            if (!internalMode) throw new InvalidDataException("A published game assembly requires its .scripts.json manifest.");
            foreach (ManagedTypeDescriptor type in catalog.AllTypes)
            {
                type.PersistentId = "cs-internal:" + type.TypeName;
                if (sourceMap.TryGetValue(type.TypeName, out ScriptSourceEntry? entry)) type.Sources = entry.Sources;
            }
            return;
        }
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(manifestPath));
        JsonElement root = document.RootElement;
        RequireObject(root, "manifest");
        if (root.GetProperty("schemaVersion").GetInt32() != 1 || root.GetProperty("assemblyName").GetString() != assemblyName)
            throw new InvalidDataException("Manifest schema or logical assembly name is incompatible.");
        JsonElement types = root.GetProperty("types");
        if (types.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Manifest types must be an array.");
        HashSet<string> ids = new(StringComparer.Ordinal);
        HashSet<string> names = new(StringComparer.Ordinal);
        foreach (JsonElement record in types.EnumerateArray())
        {
            RequireObject(record, "type");
            string name = record.GetProperty("clrName").GetString() ?? "";
            string id = record.GetProperty("id").GetString() ?? "";
            string primary = record.GetProperty("primarySource").GetString() ?? "";
            if (!names.Add(name) || !ids.Add(id) || !id.StartsWith("cs:", StringComparison.Ordinal) || !IsLowerHex(id[3..], 32))
                throw new InvalidDataException("Manifest CLR names and persistent type IDs must be unique.");
            ManagedTypeDescriptor type = catalog.AllTypes.FirstOrDefault((ManagedTypeDescriptor type) => type.TypeName == name)
                ?? throw new InvalidDataException($"Manifest type is absent: {name}.");
            if (!sourceMap.TryGetValue(name, out ScriptSourceEntry? declarations))
                throw new InvalidDataException($"Compiled ScriptSource is absent: {name}.");
            JsonElement sources = record.GetProperty("sources");
            if (sources.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Manifest sources must be an array.");
            List<ScriptSourceInfo> configured = new();
            foreach (JsonElement value in sources.EnumerateArray())
            {
                RequireObject(value, "source");
                ScriptSourceInfo source = new(value.GetProperty("path").GetString() ?? "", value.GetProperty("line").GetInt32(), value.GetProperty("contentHash").GetString() ?? "");
                ValidateSource(source);
                configured.Add(source);
            }
            ScriptSourceInfo[] ordered = SortSources(configured);
            if (ordered.Length == 0 || !ordered.SequenceEqual(declarations.Sources) || !ordered.Any((ScriptSourceInfo source) => source.Path == primary))
                throw new InvalidDataException($"Manifest declarations differ from compiled source metadata: {name}.");
            type.PersistentId = id;
            type.Sources = ordered;
        }
        if (names.Count != catalog.AllTypes.Count() || sourceMap.Count != catalog.AllTypes.Count()
            || catalog.AllTypes.Any((ManagedTypeDescriptor type) => !names.Contains(type.TypeName)))
            throw new InvalidDataException("Manifest and compiled catalog must describe exactly the same concrete Scripts.");
    }

    static bool IsSourceAttribute(MetadataReader reader, EntityHandle constructor)
    {
        if (constructor.Kind != HandleKind.MemberReference) return false;
        EntityHandle parent = reader.GetMemberReference((MemberReferenceHandle)constructor).Parent;
        if (parent.Kind != HandleKind.TypeReference) return false;
        TypeReference type = reader.GetTypeReference((TypeReferenceHandle)parent);
        return reader.GetString(type.Namespace) == "Fbzz" && reader.GetString(type.Name) == "ScriptSourceAttribute";
    }
    static ScriptSourceInfo[] SortSources(IEnumerable<ScriptSourceInfo> sources)
        => sources.OrderBy((ScriptSourceInfo source) => source.Path, StringComparer.Ordinal).ThenBy((ScriptSourceInfo source) => source.Line).ToArray();
    static void ValidateSource(ScriptSourceInfo source)
    {
        string[] segments = source.Path.Split('/');
        if (source.Line < 1 || !IsLowerHex(source.ContentHash, 64) || source.Path.Contains('\\') || source.Path.Contains(':')
            || segments.Length == 0 || segments.Any((string segment) => segment is "" or "." or "..")
            || !source.Path.EndsWith(".cs", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Script source requires a relative .cs path, positive line and SHA256 checksum.");
    }
    static bool IsLowerHex(string value, int length)
        => value.Length == length && value.All((char item) => item is >= '0' and <= '9' or >= 'a' and <= 'f');
    static void RequireObject(JsonElement value, string name)
    {
        if (value.ValueKind != JsonValueKind.Object) throw new InvalidDataException($"{name} must be an object.");
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (JsonProperty property in value.EnumerateObject())
            if (!keys.Add(property.Name)) throw new InvalidDataException($"Duplicate {name} key: {property.Name}.");
    }
}
