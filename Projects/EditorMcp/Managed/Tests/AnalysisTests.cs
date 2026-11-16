/// @file    AnalysisTests.cs
/// @brief   Semantic identity, persisted-symbol rejection and rename binding checks.
/// @author  Hasegawa Jin
/// @date    2026-11-06
using System;
using System.Text.Json;
using Fbzz.EditorAnalysis;

int failures = 0;
/// @brief An unresolved call produces one error attached to the source file path.
Check("diagnostic source position", () =>
{
    using JsonDocument result = Run("diagnostics", "class A { void M() { missing(); } }");
    return result.RootElement.GetProperty("errorCount").GetInt32() == 1
        && result.RootElement.GetProperty("diagnostics")[0].GetProperty("file").GetString() == "Assets/Scripts/Test.cs";
});
/// @brief Same name on a different type is not a reference.
Check("same name on a different type is not a reference", () =>
{
    string source = "class A { int value; int Read() => value; } class B { int value; int Read() => value; }";
    using JsonDocument result = Run("references", source, source.IndexOf("value", StringComparison.Ordinal) + 1);
    return result.RootElement.GetProperty("total").GetInt32() == 2;
});
/// @brief Private field rename edits only matching symbol.
Check("private field rename edits only matching symbol", () =>
{
    string source = "class A { int value; int Read() => value; } class B { int value; int Read() => value; }";
    using JsonDocument result = Run("rename", source, source.IndexOf("value", StringComparison.Ordinal) + 1, "renamed");
    return !result.RootElement.GetProperty("blocked").GetBoolean() && result.RootElement.GetProperty("edits").GetArrayLength() == 2;
});
/// @brief Public persisted field rename is blocked.
Check("public persisted field rename is blocked", () =>
{
    string source = "class A { public int speed; }";
    using JsonDocument result = Run("rename", source, source.IndexOf("speed", StringComparison.Ordinal) + 1, "velocity");
    return HasBlocker(result, "PERSISTED_SYMBOL_REQUIRES_FBZZ_MIGRATION");
});
/// @brief Setting field rename is blocked.
Check("Setting field rename is blocked", () =>
{
    string source = "class SettingAttribute : System.Attribute {} class A { [Setting] int speed; }";
    using JsonDocument result = Run("rename", source, source.IndexOf("speed", StringComparison.Ordinal) + 1, "velocity");
    return HasBlocker(result, "PERSISTED_SYMBOL_REQUIRES_FBZZ_MIGRATION");
});
/// @brief Script type rename is blocked.
Check("script type rename is blocked", () =>
{
    string source = "namespace Fbzz { class Script {} } class Enemy : Fbzz.Script {}";
    using JsonDocument result = Run("rename", source, source.IndexOf("Enemy", StringComparison.Ordinal) + 1, "Foe");
    return HasBlocker(result, "PERSISTED_SYMBOL_REQUIRES_FBZZ_MIGRATION");
});
/// @brief Local rename cannot capture an existing field reference.
Check("local rename cannot capture an existing field reference", () =>
{
    string source = "class A { int field; int Read() { int local = 1; return field + local; } }";
    using JsonDocument result = Run("rename", source, source.IndexOf("local", StringComparison.Ordinal) + 1, "field");
    return result.RootElement.GetProperty("blocked").GetBoolean();
});
/// @brief Ordinary local rename keeps references.
Check("ordinary local rename keeps references", () =>
{
    string source = "class A { int Read() { int local = 1; return local; } }";
    using JsonDocument result = Run("rename", source, source.IndexOf("local", StringComparison.Ordinal) + 1, "count");
    return !result.RootElement.GetProperty("blocked").GetBoolean() && result.RootElement.GetProperty("edits").GetArrayLength() == 2;
});
/// @brief Escaped identifiers are replaced by full token span.
Check("escaped identifiers are replaced by full token span", () =>
{
    string source = "class A { int @event; int Read() => @event; }";
    using JsonDocument result = Run("rename", source, source.IndexOf("@event", StringComparison.Ordinal) + 2, "count");
    return !result.RootElement.GetProperty("blocked").GetBoolean() && result.RootElement.GetProperty("edits")[0].GetProperty("length").GetInt32() == 6;
});
/// @brief Snapshot scope is explicit.
Check("snapshot scope is explicit", () =>
{
    using JsonDocument result = Run("symbols", "class A { int value; }");
    return result.RootElement.GetProperty("metadata").GetProperty("mode").GetString() == "source-snapshot"
        && !result.RootElement.GetProperty("metadata").GetProperty("projectEvaluated").GetBoolean();
});
Console.WriteLine($"RESULT {(failures == 0 ? "success" : "failure")} Roslyn analysis tests; failures={failures}");
return failures == 0 ? 0 : 1;

void Check(string name, Func<bool> test)
{
    try
    {
        if (test()) { Console.WriteLine("PASS " + name); return; }
    }
    catch (Exception error) { Console.Error.WriteLine(error); }
    ++failures;
    Console.Error.WriteLine("FAIL " + name);
}
static JsonDocument Run(string operation, string source, int column = 1, string newName = "")
    => JsonDocument.Parse(Analysis.Handle(JsonSerializer.Serialize(new { operation, sources = new[] { new { file = "Assets/Scripts/Test.cs", content = source } }, file = "Assets/Scripts/Test.cs", line = 1, column, newName })));
static bool HasBlocker(JsonDocument result, string value)
{
    foreach (JsonElement blocker in result.RootElement.GetProperty("blockers").EnumerateArray()) if (blocker.GetString() == value) return true;
    return false;
}
