/// @file    ManagedHashInputs.cs
/// @brief   内容を逐次 SHA256 化し、増分 stamp と検証 receipt を同じ入力へ結び付ける。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

namespace Fbzz.ManagedBuildTasks;

/// @note timestamp の一致では再利用せず、検証時も全入力を読み直す。
/// @see https://learn.microsoft.com/dotnet/api/system.security.cryptography.sha256.hashdata SHA256.HashData(Stream)
public sealed class ManagedHashInputs : Task
{
    [Required] public ITaskItem[] Files { get; set; } = [];
    public ITaskItem[] Expected { get; set; } = [];
    [Output] public ITaskItem[] Items { get; set; } = [];

    public override bool Execute()
    {
        Stopwatch elapsed = Stopwatch.StartNew();
        Dictionary<string, string> expected = new(StringComparer.OrdinalIgnoreCase);
        foreach (ITaskItem item in Expected) expected[item.ItemSpec] = item.GetMetadata("FileHash");
        SortedDictionary<string, string> hashes = new(StringComparer.OrdinalIgnoreCase);
        long bytes = 0;
        try
        {
            foreach (ITaskItem item in Files)
            {
                string path = Path.GetFullPath(item.ItemSpec);
                if (hashes.ContainsKey(path)) continue;
                using FileStream stream = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                bytes += stream.Length;
                string digest = Convert.ToHexString(SHA256.HashData(stream));
                if (expected.Count != 0 && (!expected.TryGetValue(path, out string? original) || original != digest))
                {
                    Log.LogError(null, "FBZZ0102", null, path, 0, 0, 0, 0,
                        "Managed inputs changed during compilation; retry the latest source version.");
                    return false;
                }
                hashes.Add(path, digest);
            }
        }
        catch (IOException error) { Log.LogError(null, "FBZZ0102", null, null, 0, 0, 0, 0, error.Message); return false; }
        catch (UnauthorizedAccessException error) { Log.LogErrorFromException(error); return false; }
        List<ITaskItem> outputs = [];
        foreach ((string path, string digest) in hashes)
        {
            TaskItem item = new(path);
            item.SetMetadata("FileHash", digest);
            outputs.Add(item);
        }
        Items = outputs.ToArray();
        Log.LogMessage(MessageImportance.High, "FBZZ managed input hash: files={0} bytes={1} elapsed_ms={2} verification={3}",
            hashes.Count, bytes, elapsed.ElapsedMilliseconds, Expected.Length != 0);
        return true;
    }
}
