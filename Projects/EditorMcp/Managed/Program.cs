/// @file    Program.cs
/// @brief   One JSON request per process for the Editor MCP Roslyn bridge.
/// @author  Hasegawa Jin
/// @date    2026-11-06
using System;
using System.IO;
using System.Text;
using Fbzz.EditorAnalysis;

/// @note Node sends UTF-8 JSON through pipes; Windows console code pages must not reinterpret source text or UTF-16 positions.
/// @see https://learn.microsoft.com/dotnet/api/system.console.inputencoding Console input encoding.
Console.InputEncoding = new UTF8Encoding(false, true);
Console.OutputEncoding = new UTF8Encoding(false, true);
try
{
    string input = Console.In.ReadToEnd();
    if (input.Length > 32 * 1024 * 1024)
    {
        Console.Out.WriteLine("{\"ok\":false,\"error\":\"REQUEST_TOO_LARGE\"}");
        return 1;
    }
    Console.Out.WriteLine(Analysis.Handle(input));
    return 0;
}
catch (Exception error) when (error is ArgumentException or InvalidOperationException or IOException or UnauthorizedAccessException or System.Text.Json.JsonException or BadImageFormatException)
{
    Console.Out.WriteLine(System.Text.Json.JsonSerializer.Serialize(new { ok = false, error = error.Message }));
    return 1;
}
