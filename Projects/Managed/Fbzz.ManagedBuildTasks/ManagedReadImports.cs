/// @file    ManagedReadImports.cs
/// @brief   実行中 project と同じ global property で実 import と欠損候補を列挙する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Collections;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Build.Construction;
using Microsoft.Build.Evaluation;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

namespace Fbzz.ManagedBuildTasks;

/// @note 独立した ProjectCollection は評価後に破棄し、実行中の build instance を変更しない。
/// @see https://learn.microsoft.com/dotnet/api/microsoft.build.evaluation.project.imports Project.Imports
/// @see https://learn.microsoft.com/dotnet/api/microsoft.build.framework.ibuildengine6.getglobalproperties IBuildEngine6.GetGlobalProperties
public sealed class ManagedReadImports : Task
{
    [Required] public string ProjectPath { get; set; } = "";
    [Required] public string CacheDirectory { get; set; } = "";
    [Output] public ITaskItem[] Imports { get; set; } = [];
    [Output] public ITaskItem[] ImportDirectories { get; set; } = [];
    [Output] public ITaskItem[] RecursiveImportDirectories { get; set; } = [];

    public override bool Execute()
    {
        Dictionary<string, string> properties = new(StringComparer.OrdinalIgnoreCase);
        foreach (KeyValuePair<string, string> property in ((IBuildEngine6)BuildEngine).GetGlobalProperties())
            if (!property.Key.Equals("CustomAfterMicrosoftCommonTargets", StringComparison.OrdinalIgnoreCase))
                properties[property.Key] = property.Value;
        string cachePath = Path.Combine(CacheDirectory, "fbzz-imports-cache.json");
        string signature = BuildSignature(properties);
        ImportCache? cache = ReadCache(cachePath);
        if (cache is not null && cache.Signature == signature && cache.ContentVersion == ContentVersion(cache.Files, cache.Directories, cache.RecursiveDirectories))
        {
            Log.LogMessage(MessageImportance.High, "FBZZ managed input graph: reused");
            Imports = cache.Files.Select(path => (ITaskItem)new TaskItem(path)).ToArray();
            ImportDirectories = cache.Directories.Select(path => (ITaskItem)new TaskItem(path)).ToArray();
            RecursiveImportDirectories = cache.RecursiveDirectories.Select(path => (ITaskItem)new TaskItem(path)).ToArray();
            return true;
        }
        using ProjectCollection collection = new(properties);
        Log.LogMessage(MessageImportance.High, "FBZZ managed input graph: evaluated");
        Project evaluated = collection.LoadProject(ProjectPath);
        string previousHook = evaluated.GetPropertyValue("CustomAfterMicrosoftCommonTargets");
        string defaultHook = Path.GetFullPath(Path.Combine(evaluated.GetPropertyValue("MSBuildExtensionsPath"),
            "v" + evaluated.GetPropertyValue("MSBuildToolsVersion"), "Custom.After.Microsoft.Common.targets"));
        string resolvedHook = string.IsNullOrEmpty(previousHook) ? "" : Path.GetFullPath(previousHook, Path.GetDirectoryName(ProjectPath)!);
        if (!string.IsNullOrEmpty(resolvedHook) &&
            (!resolvedHook.Equals(defaultHook, StringComparison.OrdinalIgnoreCase) || File.Exists(resolvedHook)))
        {
            Log.LogError(null, "FBZZ0101", null, ProjectPath, 0, 0, 0, 0,
                "AgentBuild incremental targets require the CustomAfterMicrosoftCommonTargets hook; move the existing hook to an explicit project Import: " + resolvedHook);
            return false;
        }
        HashSet<string> inputs = new(StringComparer.OrdinalIgnoreCase) { ProjectPath };
        HashSet<string> directories = new(StringComparer.OrdinalIgnoreCase);
        HashSet<string> recursiveDirectories = new(StringComparer.OrdinalIgnoreCase);
        List<ProjectRootElement> roots = [evaluated.Xml];
        foreach (ResolvedImport import in evaluated.Imports)
        {
            inputs.Add(import.ImportedProject.FullPath);
            roots.Add(import.ImportedProject);
        }
        foreach (ProjectRootElement root in roots)
        {
            /// @note import 時点と最終 property 値の差を、作者側 import ディレクトリの保守的な監視で補う。
            string sdkRoot = Path.GetFullPath(evaluated.GetPropertyValue("MSBuildToolsPath")).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            if (!root.FullPath.StartsWith(sdkRoot, StringComparison.OrdinalIgnoreCase))
                directories.Add(Path.GetDirectoryName(root.FullPath)!);
            foreach (ProjectImportElement import in root.AllChildren.OfType<ProjectImportElement>())
            {
                string importPath = import.Project.Replace("$(MSBuildThisFileDirectory)", Path.GetDirectoryName(root.FullPath) + Path.DirectorySeparatorChar)
                    .Replace("$(MSBuildThisFileFullPath)", root.FullPath)
                    .Replace("$(MSBuildThisFileName)", Path.GetFileNameWithoutExtension(root.FullPath))
                    .Replace("$(MSBuildThisFileExtension)", Path.GetExtension(root.FullPath));
                string expanded = evaluated.ExpandString(importPath);
                if (string.IsNullOrEmpty(expanded)) continue;
                foreach (string path in expanded.Split(';', StringSplitOptions.RemoveEmptyEntries))
                {
                    int wildcard = path.IndexOfAny(['*', '?']);
                    if (wildcard >= 0)
                    {
                        string prefix = path[..wildcard];
                        string directory = Path.GetDirectoryName(prefix) ?? "";
                        (path.Contains("**", StringComparison.Ordinal) ? recursiveDirectories : directories)
                            .Add(Path.GetFullPath(directory, Path.GetDirectoryName(root.FullPath)!));
                    }
                    else inputs.Add(Path.GetFullPath(path, Path.GetDirectoryName(root.FullPath)!));
                }
            }
        }
        Imports = inputs.OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .Select(path => (ITaskItem)new TaskItem(path)).ToArray();
        ImportDirectories = directories.OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .Select(path => (ITaskItem)new TaskItem(path)).ToArray();
        RecursiveImportDirectories = recursiveDirectories.OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .Select(path => (ITaskItem)new TaskItem(path)).ToArray();
        ImportCache updated = new()
        {
            Signature = signature,
            Files = inputs.OrderBy(path => path, StringComparer.OrdinalIgnoreCase).ToArray(),
            Directories = directories.OrderBy(path => path, StringComparer.OrdinalIgnoreCase).ToArray(),
            RecursiveDirectories = recursiveDirectories.OrderBy(path => path, StringComparer.OrdinalIgnoreCase).ToArray()
        };
        updated.ContentVersion = ContentVersion(updated.Files, updated.Directories, updated.RecursiveDirectories);
        Directory.CreateDirectory(CacheDirectory);
        string pendingCache = cachePath + ".pending";
        File.WriteAllText(pendingCache, JsonSerializer.Serialize(updated));
        File.Move(pendingCache, cachePath, true);
        return true;
    }

    /// @note 環境由来の MSBuild property も鍵へ含め、評価結果を source timestamp で再利用しない。
    private static string BuildSignature(Dictionary<string, string> properties)
    {
        List<string> values = properties.Where(pair => pair.Key is not
            ("FbzzManagedInputManifest" or "FbzzManagedInputReceipt" or "FbzzManagedCandidateDirectory"))
            .Select(pair => pair.Key + "=" + pair.Value).ToList();
        foreach (DictionaryEntry variable in Environment.GetEnvironmentVariables())
            values.Add(variable.Key + "=" + variable.Value);
        values.Sort(StringComparer.Ordinal);
        values.Add(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(typeof(ManagedReadImports).Assembly.Location))));
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', values))));
    }

    private static ImportCache? ReadCache(string path)
    {
        try { return File.Exists(path) ? JsonSerializer.Deserialize<ImportCache>(File.ReadAllText(path)) : null; }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (JsonException) { return null; }
    }

    /// @note 欠損 import と wildcard 探索の追加・削除も内容版へ含める。
    private static string ContentVersion(string[] files, string[] directories, string[] recursiveDirectories)
    {
        HashSet<string> inputs = new(files, StringComparer.OrdinalIgnoreCase);
        foreach (string directory in directories)
            foreach (string path in EnumerateConfiguration(directory, false)) inputs.Add(path);
        foreach (string directory in recursiveDirectories)
            foreach (string path in EnumerateConfiguration(directory, true)) inputs.Add(path);
        using IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (string path in inputs.OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
        {
            hash.AppendData(Encoding.UTF8.GetBytes(path));
            hash.AppendData([0]);
            bool exists = File.Exists(path);
            hash.AppendData([exists ? (byte)1 : (byte)0]);
            if (exists) hash.AppendData(File.ReadAllBytes(path));
        }
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    /// @note 保守的な親監視は直下だけ。明示 ** のみ再帰し、生成物と reparse point は探索しない。
    private static IEnumerable<string> EnumerateConfiguration(string root, bool recursive)
    {
        if (!Directory.Exists(root) || (File.GetAttributes(root) & FileAttributes.ReparsePoint) != 0) yield break;
        Stack<string> pending = new();
        pending.Push(root);
        while (pending.Count != 0)
        {
            string directory = pending.Pop();
            foreach (string path in Directory.EnumerateFileSystemEntries(directory))
            {
                FileAttributes attributes = File.GetAttributes(path);
                if ((attributes & FileAttributes.ReparsePoint) != 0) continue;
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    string name = Path.GetFileName(path).ToLowerInvariant();
                    if (recursive && name is not ("obj" or "bin" or "library" or "build" or ".git" or ".vs" or "scratch")) pending.Push(path);
                }
                else
                {
                    string name = Path.GetFileName(path).ToLowerInvariant();
                    if (Path.GetExtension(name) is ".props" or ".targets" or ".csproj" ||
                        name is "global.json" or "nuget.config" or ".editorconfig") yield return path;
                }
            }
        }
    }

    private sealed class ImportCache
    {
        public string Signature { get; set; } = "";
        public string ContentVersion { get; set; } = "";
        public string[] Files { get; set; } = [];
        public string[] Directories { get; set; } = [];
        public string[] RecursiveDirectories { get; set; } = [];
    }
}
