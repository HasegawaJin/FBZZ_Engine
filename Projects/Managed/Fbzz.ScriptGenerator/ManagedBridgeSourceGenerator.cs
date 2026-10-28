/// @file    ManagedBridgeSourceGenerator.cs
/// @brief   C++ と共有する X macro から固定 ABI と単純な managed marshaller を生成する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Fbzz.ScriptGenerator;

/// @note SDK / Host の明示指定だけを対象とし、ゲーム AdditionalFiles の内容は実行しない。
/// @see https://github.com/dotnet/roslyn/blob/main/docs/features/incremental-generators.md Incremental generators
[Generator(LanguageNames.CSharp)]
public sealed class ManagedBridgeSourceGenerator : IIncrementalGenerator
{
    private const string CONTRACT_FILE = "ManagedBridgeContract.inl";
    private const string PREFIX = "FBZZ_MANAGED_";
    private static readonly DiagnosticDescriptor INVALID_CONTRACT = new(
        "FBZZ1010", "Managed bridge contract is invalid", "{0}", "FBZZ", DiagnosticSeverity.Error, true);
    private static readonly DiagnosticDescriptor MISSING_CONTRACT = new(
        "FBZZ1011", "Managed bridge contract is required", "{0}", "FBZZ", DiagnosticSeverity.Error, true);
    private static readonly DiagnosticDescriptor INVALID_TARGET = new(
        "FBZZ1012", "Managed bridge target is invalid", "FbzzBindingTarget must be Host or Sdk: {0}", "FBZZ", DiagnosticSeverity.Error, true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValueProvider<string?> target = context.AnalyzerConfigOptionsProvider.Select(
            static (options, _) => options.GlobalOptions.TryGetValue("build_property.FbzzBindingTarget", out string? value) ? value : null);
        IncrementalValuesProvider<AdditionalText> contracts = context.AdditionalTextsProvider.Where(
            static file => string.Equals(Path.GetFileName(file.Path), CONTRACT_FILE, StringComparison.OrdinalIgnoreCase));
        context.RegisterSourceOutput(target.Combine(contracts.Collect()),
            static (production, input) => Emit(production, input.Left, input.Right));
    }

    private static void Emit(SourceProductionContext context, string? target, ImmutableArray<AdditionalText> files)
    {
        if (string.IsNullOrEmpty(target)) return;
        if (target is not ("Host" or "Sdk"))
        {
            context.ReportDiagnostic(Diagnostic.Create(INVALID_TARGET, Location.None, target));
            return;
        }
        if (files.Length != 1)
        {
            context.ReportDiagnostic(Diagnostic.Create(MISSING_CONTRACT, Location.None,
                "Expose exactly one " + CONTRACT_FILE + " using AdditionalFiles."));
            return;
        }
        SourceText? text = files[0].GetText(context.CancellationToken);
        if (text is null)
        {
            context.ReportDiagnostic(Diagnostic.Create(MISSING_CONTRACT, Location.None, "Cannot read " + files[0].Path));
            return;
        }
        Contract contract = new();
        ContractParser parser = new(context, files[0].Path, text, contract);
        if (!parser.Parse()) return;
        context.AddSource("ManagedBridgeContract.g.cs", SourceText.From(Generate(contract, target), Encoding.UTF8));
        if (target == "Host" && contract.Calls.Count != 0)
            context.AddSource("ManagedBridgeCalls.g.cs", SourceText.From(GenerateCalls(contract), Encoding.UTF8));
    }

    private static StringBuilder Header(string name, string description, bool host)
    {
        StringBuilder output = new();
        output.Append("/// @file    ").AppendLine(name);
        output.Append("/// @brief   ").AppendLine(description);
        output.AppendLine("/// @author  Hasegawa Jin");
        output.AppendLine("/// @date    2026-10-04");
        output.AppendLine("#nullable enable");
        if (host) output.AppendLine("using System.Runtime.InteropServices;\nusing Fbzz;");
        output.Append("namespace ").Append(host ? "Fbzz.ManagedHost" : "Fbzz").AppendLine(";\n");
        return output;
    }

    private static string Generate(Contract contract, string target)
    {
        bool host = target == "Host";
        StringBuilder output = Header("ManagedBridgeContract.g.cs", "共通定義から投影した数値 ID と ABI 配置。", host);
        foreach (EnumDefinition definition in contract.Enums.Where(value => value.Target == (host ? "HOST" : "SDK")))
        {
            output.Append(definition.Visibility).Append(" enum ").Append(definition.CsName).Append(" : ").Append(definition.Underlying).AppendLine("\n{");
            foreach (EnumValue value in definition.Values)
            {
                CallbackDefinition? callback = definition.CsName == "ScriptCallback"
                    ? contract.Callbacks.SingleOrDefault(item => item.NativeName == value.NativeName) : null;
                if (callback is not null)
                    output.Append("    [ScriptLifecycle(").Append(Quote(callback.Name)).Append(", ")
                        .Append(callback.Parameter.Length == 0 ? "null" : Quote(callback.Parameter)).Append(", ")
                        .Append(Quote(callback.Policy)).AppendLine(")]");
                output.Append("    ").Append(value.CsName).Append(" = ").Append(value.Literal).AppendLine(",");
            }
            output.AppendLine("}\n");
        }
        if (!host)
        {
            /// @note SDK が wire ID を別表に複製せず、Host op 登録簿のサービス部分だけを内部 enum に投影する。
            output.AppendLine("internal enum ScriptServiceKind : uint\n{");
            foreach (EnumValue value in contract.Enums.Single(definition => definition.Target == "HOST" && definition.CsName == "BridgeOp").Values
                .Where(value => value.CsName.StartsWith("SERVICE_", StringComparison.Ordinal)))
                output.Append("    ").Append(value.CsName).Append(" = ").Append(value.Literal).AppendLine(",");
            output.AppendLine("}\n");
            GenerateLifecycles(output, contract);
            return output.ToString();
        }
        output.AppendLine("public static partial class BridgeAbi\n{");
        foreach (Constant constant in contract.Constants)
            output.Append("    public const ").Append(constant.CsType).Append(' ').Append(constant.CsName).Append(" = ").Append(constant.Literal).AppendLine(";");
        foreach (StructDefinition definition in contract.Structs)
        {
            string prefix = LayoutPrefix(definition.CsName);
            output.Append("    public const uint ").Append(prefix).Append("_SIZE = ").Append(definition.Size).AppendLine("U;");
            foreach (FieldDefinition field in definition.Fields)
                output.Append("    public const uint ").Append(prefix).Append('_').Append(SnakeCase(field.CsName)).Append("_OFFSET = ").Append(field.Offset).AppendLine("U;");
        }
        output.AppendLine("}\n");
        foreach (StructDefinition definition in contract.Structs)
        {
            output.AppendLine("[StructLayout(LayoutKind.Sequential, Pack = 8)]");
            output.Append("public unsafe struct ").Append(definition.CsName).AppendLine("\n{");
            foreach (FieldDefinition field in definition.Fields)
                output.Append("    public ").Append(field.CsType).Append(' ').Append(field.CsName).AppendLine(";");
            output.AppendLine("}\n");
        }
        return output.ToString();
    }

    private static string Quote(string value) => SymbolDisplay.FormatLiteral(value, quote: true);

    private static void GenerateLifecycles(StringBuilder output, Contract contract)
    {
        if (contract.Callbacks.Count == 0) return;
        EnumDefinition definition = contract.Enums.Single(value => value.CsName == "ScriptCallback" && value.Target == "SDK");
        output.AppendLine("[System.AttributeUsage(System.AttributeTargets.Field, AllowMultiple = false)]");
        output.AppendLine("public sealed class ScriptLifecycleAttribute : System.Attribute\n{");
        output.AppendLine("    public ScriptLifecycleAttribute(string name, string? parameterTypeName, string executionPolicy)");
        output.AppendLine("    { Name = name; ParameterTypeName = parameterTypeName; ExecutionPolicy = executionPolicy; }");
        output.AppendLine("    public string Name { get; }\n    public string? ParameterTypeName { get; }\n    public string ExecutionPolicy { get; }\n}\n");
        output.AppendLine("public readonly record struct ScriptLifecycleDefinition(ScriptCallback Callback, string Name, string? ParameterTypeName, string ExecutionPolicy);\n");
        output.AppendLine("public static class ScriptLifecycleContract\n{");
        output.AppendLine("    public static System.Collections.Generic.IReadOnlyList<ScriptLifecycleDefinition> Callbacks { get; } = System.Array.AsReadOnly(new ScriptLifecycleDefinition[]\n    {");
        foreach (CallbackDefinition callback in contract.Callbacks)
        {
            string id = definition.Values.Single(value => value.NativeName == callback.NativeName).CsName;
            output.Append("        new(ScriptCallback.").Append(id).Append(", ").Append(Quote(callback.Name)).Append(", ")
                .Append(callback.Parameter.Length == 0 ? "null" : Quote(callback.Parameter)).Append(", ").Append(Quote(callback.Policy)).AppendLine("),");
        }
        output.AppendLine("    });");
        output.AppendLine("    /// @brief Queries the shared Engine access policy without accessing Engine state.");
        output.AppendLine("    /// @return live, subscription_only, draw_readonly or no_engine_api; unknown callbacks return no_engine_api.");
        output.AppendLine("    /// @note Available outside Host callbacks and in every run mode.");
        output.AppendLine("    public static string GetExecutionPolicy(ScriptCallback callback) => callback switch\n    {");
        foreach (CallbackDefinition callback in contract.Callbacks)
        {
            string id = definition.Values.Single(value => value.NativeName == callback.NativeName).CsName;
            output.Append("        ScriptCallback.").Append(id).Append(" => ").Append(Quote(callback.Policy)).AppendLine(",");
        }
        output.AppendLine("        ScriptCallback.EVENT or ScriptCallback.JOB => \"live\",\n        _ => \"no_engine_api\"\n    };\n}\n");
        output.AppendLine("public abstract partial class Script\n{");
        foreach (CallbackDefinition callback in contract.Callbacks)
        {
            string policyDescription = callback.Policy switch
            {
                "subscription_only" => "Read Engine state and register/cancel owned jobs or subscriptions; mutation and event publication are forbidden.",
                "no_engine_api" => "Engine state is unavailable; update managed data only.",
                "draw_readonly" => "A detached settings candidate may read native state and record bounded Gizmos; mutation, jobs and managed Script lookup are forbidden.",
                _ => "Engine access requires this live callback and each API's phase/run-mode contract."
            };
            output.Append("    /// @note Synchronous override. ").AppendLine(policyDescription);
            output.Append("    protected virtual void ").Append(callback.Name).Append('(');
            if (callback.Parameter.Length != 0)
                output.Append(callback.Parameter).Append(callback.Parameter switch {
                    "Fbzz.GameObject" => "? other", "Fbzz.Collision" => " collision",
                    "System.String" => " sequenceName", _ => " info" });
            output.AppendLine(") { }");
        }
        output.AppendLine("}\n");
    }

    private static string GenerateCalls(Contract contract)
    {
        StringBuilder output = Header("ManagedBridgeCalls.g.cs", "共通定義の単純 API を一回の dispatch に変換する。", true);
        output.AppendLine("public static unsafe partial class EntryPoint\n{\n    sealed partial class NativeBridge\n    {");
        foreach (CallDefinition call in contract.Calls)
        {
            string operation = contract.Enums.Single(value => value.CsName == "BridgeOp" && value.Target == "HOST").Values.Single(value => value.NativeName == call.Operation).CsName;
            output.Append("        ").Append(call.Visibility).Append(' ').Append(call.ReturnType).Append(' ').Append(call.Name)
                .Append("(ScriptExecutionScope scope").Append(call.Arguments.Length == 0 ? string.Empty : ", " + call.Arguments).AppendLine(")\n        {");
            output.Append("            BridgeRequest request = Request(scope, ").Append(call.Target).Append(", BridgeOp.").Append(operation).AppendLine(");");
            if (call.Setup.Length != 0) output.Append("            ").AppendLine(call.Setup);
            output.AppendLine(call.ReturnType == "void" ? "            Call(ref request);" : "            BridgeReply reply = Call(ref request);");
            if (call.Return.Length != 0) output.Append("            ").AppendLine(call.Return);
            output.AppendLine("        }\n");
        }
        output.AppendLine("    }\n}");
        return output.ToString();
    }

    private static string LayoutPrefix(string name) => SnakeCase(name.StartsWith("Bridge", StringComparison.Ordinal) ? name[6..] : name);

    private static string SnakeCase(string name)
    {
        StringBuilder result = new();
        for (int index = 0; index < name.Length; index++)
        {
            if (index > 0 && char.IsUpper(name[index]) && (!char.IsUpper(name[index - 1]) || index + 1 < name.Length && char.IsLower(name[index + 1]))) result.Append('_');
            result.Append(char.ToUpperInvariant(name[index]));
        }
        return result.ToString();
    }

    private sealed class Contract
    {
        public List<Constant> Constants { get; } = [];
        public List<EnumDefinition> Enums { get; } = [];
        public List<StructDefinition> Structs { get; } = [];
        public List<CallDefinition> Calls { get; } = [];
        public List<CallbackDefinition> Callbacks { get; } = [];
    }

    private sealed record CallbackDefinition(string NativeName, string Name, string Parameter, string Policy);
    private sealed record Constant(string NativeName, string CsType, string CsName, string Literal);
    private sealed record EnumValue(string NativeName, string CsName, string Literal, string Numeric);
    private sealed record CallDefinition(string Visibility, string ReturnType, string Name, string Arguments, string Operation, string Target, string Setup, string Return);
    private sealed class EnumDefinition(string nativeName, string csName, string underlying, string target, string visibility)
    {
        public string NativeName { get; } = nativeName;
        public string CsName { get; } = csName;
        public string Underlying { get; } = underlying;
        public string Target { get; } = target;
        public string Visibility { get; } = visibility;
        public List<EnumValue> Values { get; } = [];
    }

    private sealed class StructDefinition(string nativeName, string csName)
    {
        public string NativeName { get; } = nativeName;
        public string CsName { get; } = csName;
        public int Size { get; set; } = -1;
        public List<FieldDefinition> Fields { get; } = [];
    }

    private sealed class FieldDefinition(string nativeName, string csType, string csName)
    {
        public string NativeName { get; } = nativeName;
        public string CsType { get; } = csType;
        public string CsName { get; } = csName;
        public int Offset { get; set; } = -1;
    }

    private sealed class ContractParser(SourceProductionContext context, string path, SourceText text, Contract contract)
    {
        private readonly HashSet<string> m_nativeNames = new(StringComparer.Ordinal);
        private readonly HashSet<string> m_csNames = new(StringComparer.Ordinal);
        private EnumDefinition? m_enum;
        private StructDefinition? m_struct;
        private int m_line;

        public bool Parse()
        {
            for (m_line = 0; m_line < text.Lines.Count; m_line++)
            {
                string line = text.Lines[m_line].ToString().Trim();
                if (line.Length == 0 || line.StartsWith("///", StringComparison.Ordinal)) continue;
                if (!Tokens(line, out string name, out string[] arguments)) return Error("Expected one complete FBZZ_MANAGED_ macro per line.");
                if (!ParseMacro(name, arguments)) return false;
            }
            m_line = Math.Max(0, text.Lines.Count - 1);
            if (m_enum is not null || m_struct is not null) return Error("Unclosed enum or struct definition.");
            if (contract.Enums.Count == 0) return Error("The contract must contain enum definitions.");
            foreach (StructDefinition definition in contract.Structs)
            {
                int end = 0;
                int largestAlignment = 1;
                foreach (FieldDefinition field in definition.Fields)
                {
                    int size = FieldSize(field.CsType);
                    if (size == 0) return Error("Unsupported ABI field type: " + field.CsType);
                    int alignment = Math.Min(size, 8);
                    largestAlignment = Math.Max(alignment, largestAlignment);
                    int offset = (end + alignment - 1) / alignment * alignment;
                    if (field.Offset != offset) return Error("Missing or inconsistent offset for " + definition.CsName + "." + field.CsName);
                    end = offset + size;
                }
                if (definition.Fields.Count == 0 || definition.Size != (end + largestAlignment - 1) / largestAlignment * largestAlignment)
                    return Error("Missing or inconsistent size for " + definition.CsName);
            }
            EnumDefinition? operations = contract.Enums.SingleOrDefault(value => value.CsName == "BridgeOp" && value.Target == "HOST");
            foreach (CallDefinition call in contract.Calls)
                if (operations is null || !operations.Values.Any(value => value.NativeName == call.Operation)) return Error("Unknown call operation: " + call.Operation);
            EnumDefinition? callbacks = contract.Enums.SingleOrDefault(value => value.CsName == "ScriptCallback" && value.Target == "SDK");
            if (callbacks is not null && callbacks.Values.Any(value => value.NativeName is not ("EVENT" or "JOB")
                && !contract.Callbacks.Any(callback => callback.NativeName == value.NativeName)))
                return Error("Every authorable lifecycle enum value requires one CALLBACK declaration.");
            foreach (CallbackDefinition callback in contract.Callbacks)
                if (callbacks is null || !callbacks.Values.Any(value => value.NativeName == callback.NativeName))
                    return Error("Unknown lifecycle callback: " + callback.NativeName);
            return true;
        }

        private bool ParseMacro(string name, string[] arguments)
        {
            if (m_enum is not null && name is not ("ENUM_VALUE" or "ENUM_END")) return Error("Only enum values are allowed inside an enum.");
            if (m_struct is not null && name is not ("FIELD" or "STRUCT_END")) return Error("Only fields are allowed inside a struct.");
            switch (name)
            {
                case "CONST":
                    if (!Count(arguments, 5) || !NativeType(arguments[0], arguments[1]) || !Names(arguments[2], "HOST/BridgeAbi/" + arguments[3]) ||
                        !Integer(arguments[4], arguments[1], out string constant, out _)) return Error("Invalid or duplicate constant.");
                    contract.Constants.Add(new(arguments[2], arguments[1], arguments[3], constant));
                    return true;
                case "ENUM_BEGIN":
                    if (!Count(arguments, 6) || arguments[4] is not ("HOST" or "SDK") || arguments[5] is not ("public" or "internal") ||
                        !NativeType(arguments[2], arguments[3]) || !Integer("0", arguments[3], out _, out _) || !Names(arguments[0], arguments[4] + "/" + arguments[1])) return Error("Invalid or duplicate enum.");
                    m_enum = new(arguments[0], arguments[1], arguments[3], arguments[4], arguments[5]);
                    contract.Enums.Add(m_enum);
                    return true;
                case "ENUM_VALUE":
                    if (!Count(arguments, 3) || m_enum is null || !Identifier(arguments[0]) || !Identifier(arguments[1]) ||
                        !Integer(arguments[2], m_enum.Underlying, out string literal, out string numeric) ||
                        m_enum.Values.Any(value => value.NativeName == arguments[0] || value.CsName == arguments[1] || value.Numeric == numeric)) return Error("Invalid or duplicate enum value.");
                    m_enum.Values.Add(new(arguments[0], arguments[1], literal, numeric));
                    return true;
                case "ENUM_END":
                    if (!Count(arguments, 0) || m_enum is null || m_enum.Values.Count == 0) return Error("Unexpected or empty enum end.");
                    m_enum = null;
                    return true;
                case "STRUCT_BEGIN":
                    if (!Count(arguments, 2) || !Names(arguments[0], "HOST/" + arguments[1])) return Error("Invalid or duplicate struct.");
                    m_struct = new(arguments[0], arguments[1]);
                    contract.Structs.Add(m_struct);
                    return true;
                case "FIELD":
                    if (!Count(arguments, 5) || m_struct is null || !NativeType(arguments[0], arguments[1]) || !Identifier(arguments[2]) || !Identifier(arguments[3]) ||
                        m_struct.Fields.Any(field => field.NativeName == arguments[2] || field.CsName == arguments[3])) return Error("Invalid or duplicate struct field.");
                    m_struct.Fields.Add(new(arguments[2], arguments[1], arguments[3]));
                    return true;
                case "STRUCT_END":
                    if (!Count(arguments, 0) || m_struct is null) return Error("Unexpected struct end.");
                    m_struct = null;
                    return true;
                case "SIZE":
                    if (!Count(arguments, 3) || FindStruct(arguments[0], arguments[1]) is not StructDefinition sized || sized.Size != -1 ||
                        !LayoutNumber(arguments[2], out int size) || size == 0) return Error("Invalid or duplicate struct size.");
                    sized.Size = size;
                    return true;
                case "OFFSET":
                    if (!Count(arguments, 5) || FindStruct(arguments[0], arguments[1]) is not StructDefinition structure ||
                        structure.Fields.SingleOrDefault(field => field.NativeName == arguments[2] && field.CsName == arguments[3]) is not FieldDefinition offsetField ||
                        offsetField.Offset != -1 || !LayoutNumber(arguments[4], out int offset)) return Error("Invalid or duplicate field offset.");
                    offsetField.Offset = offset;
                    return true;
                case "CALLBACK":
                    if (!Count(arguments, 4) || !Identifier(arguments[0]) || !String(arguments[1], out string callbackName) || !Identifier(callbackName)
                        || !String(arguments[2], out string parameter) || parameter is not ("" or "Fbzz.Collision" or "Fbzz.GameObject"
                            or "Fbzz.AnimationEvent" or "Fbzz.RootMotionInfo" or "Fbzz.SequenceEvent" or "System.String")
                        || !String(arguments[3], out string policy) || policy is not ("live" or "no_engine_api" or "subscription_only" or "draw_readonly")
                        || contract.Callbacks.Any(value => value.NativeName == arguments[0] || value.Name == callbackName))
                        return Error("Invalid or duplicate lifecycle callback.");
                    contract.Callbacks.Add(new(arguments[0], callbackName, parameter, policy));
                    return true;
                case "CALL":
                    if (!Count(arguments, 8) || !String(arguments[0], out string visibility) || visibility is not ("public" or "private") ||
                        !String(arguments[1], out string returnType) || returnType != "void" && SyntaxFactory.ParseTypeName(returnType).ContainsDiagnostics || !Identifier(arguments[2]) ||
                        !String(arguments[3], out string parameters) || !Identifier(arguments[4]) || !String(arguments[5], out string target) || target.Length == 0 ||
                        !String(arguments[6], out string setup) || !String(arguments[7], out string result) ||
                        contract.Calls.Any(call => call.Name == arguments[2])) return Error("Invalid or duplicate call.");
                    contract.Calls.Add(new(visibility, returnType, arguments[2], parameters, arguments[4], target, setup, result));
                    return true;
                default: return Error("Unknown contract macro: " + name);
            }
        }

        private StructDefinition? FindStruct(string nativeName, string csName) => contract.Structs.SingleOrDefault(
            value => value.NativeName == nativeName && value.CsName == csName);

        private bool NativeType(string nativeType, string csType)
        {
            string native = string.Concat(nativeType.Where(static character => !char.IsWhiteSpace(character)));
            string? primitive = native switch
            {
                "uint8_t" => "byte", "int8_t" => "sbyte", "uint16_t" => "ushort", "int16_t" => "short",
                "uint32_t" => "uint", "int32_t" => "int", "uint64_t" => "ulong", "int64_t" => "long",
                "float" => "float", "double" => "double", "char*" or "constchar*" => "byte*", _ => null
            };
            if (primitive is not null) return primitive == csType;
            return contract.Enums.Any(value => value.NativeName == native && value.CsName == csType && value.Target == "HOST");
        }

        private int FieldSize(string type) => type switch
        {
            "uint" or "int" or "float" => 4,
            "ulong" or "long" or "double" or "byte*" => 8,
            "byte" or "sbyte" => 1,
            "short" or "ushort" => 2,
            _ => contract.Enums.FirstOrDefault(value => value.CsName == type && value.Target == "HOST") is EnumDefinition definition ? FieldSize(definition.Underlying) : 0
        };

        private bool Names(string nativeName, string csName) => Identifier(nativeName) && Identifier(csName[(csName.LastIndexOf('/') + 1)..]) &&
            m_nativeNames.Add(nativeName) && m_csNames.Add(csName);

        private bool Error(string message)
        {
            TextLine line = text.Lines[m_line];
            context.ReportDiagnostic(Diagnostic.Create(INVALID_CONTRACT,
                Location.Create(path, line.Span, text.Lines.GetLinePositionSpan(line.Span)), message));
            return false;
        }
    }

    private static bool Identifier(string value) => SyntaxFacts.IsValidIdentifier(value);
    private static bool Count(string[] values, int count) => values.Length == count;
    private static bool LayoutNumber(string value, out int number) => int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out number);

    private static bool Integer(string value, string type, out string literal, out string numeric)
    {
        literal = numeric = string.Empty;
        string digits = value.TrimEnd('U', 'u', 'L', 'l');
        bool hex = digits.StartsWith("0x", StringComparison.OrdinalIgnoreCase);
        string magnitude = digits.StartsWith('-') || digits.StartsWith('+') ? digits[1..] : digits;
        /// @note 同じ正本を C++ も読むため、C++ で八進数になる非 hex の先頭 0 は受け入れない。
        if (!hex && magnitude.Length > 1 && magnitude[0] == '0') return false;
        bool signed = type is "int" or "long" or "short" or "sbyte";
        if (type is not ("uint" or "ulong" or "int" or "long" or "ushort" or "short" or "byte" or "sbyte")) return false;
        if (signed && !hex)
        {
            if (!long.TryParse(digits, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long number)) return false;
            if (type == "int" && number is < int.MinValue or > int.MaxValue || type == "short" && number is < short.MinValue or > short.MaxValue || type == "sbyte" && number is < sbyte.MinValue or > sbyte.MaxValue) return false;
            numeric = number.ToString(CultureInfo.InvariantCulture);
        }
        else
        {
            if (!ulong.TryParse(hex ? digits[2..] : digits, hex ? NumberStyles.AllowHexSpecifier : NumberStyles.None, CultureInfo.InvariantCulture, out ulong number)) return false;
            ulong maximum = type switch { "uint" => uint.MaxValue, "int" => (ulong)int.MaxValue, "long" => (ulong)long.MaxValue, "ushort" => ushort.MaxValue, "short" => (ulong)short.MaxValue, "byte" => byte.MaxValue, "sbyte" => (ulong)sbyte.MaxValue, _ => ulong.MaxValue };
            if (number > maximum) return false;
            numeric = number.ToString(CultureInfo.InvariantCulture);
        }
        literal = (hex ? "0x" + digits[2..] : numeric) + (type == "ulong" ? "UL" : type == "uint" ? "U" : type == "long" ? "L" : string.Empty);
        return true;
    }

    private static bool String(string token, out string value)
    {
        ExpressionSyntax expression = SyntaxFactory.ParseExpression(token);
        if (expression is LiteralExpressionSyntax literal && literal.IsKind(SyntaxKind.StringLiteralExpression) && !literal.ContainsDiagnostics)
        {
            value = literal.Token.ValueText;
            return true;
        }
        value = string.Empty;
        return false;
    }

    /// @note CALL の C# 断片はリポジトリの正本だけから取り、引用符内の comma / escaped quote を区切りとしない。
    private static bool Tokens(string line, out string name, out string[] arguments)
    {
        name = string.Empty;
        arguments = [];
        int open = line.IndexOf('(');
        string trimmed = line.EndsWith(';') ? line[..^1].TrimEnd() : line;
        if (open <= PREFIX.Length || !trimmed.EndsWith(')') || !trimmed.StartsWith(PREFIX, StringComparison.Ordinal)) return false;
        name = trimmed[PREFIX.Length..open];
        if (!Identifier(name)) return false;
        string content = trimmed[(open + 1)..^1];
        if (content.Trim().Length == 0) return true;
        List<string> tokens = [];
        bool quoted = false;
        bool escaped = false;
        int start = 0;
        for (int index = 0; index < content.Length; index++)
        {
            char character = content[index];
            if (quoted)
            {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') quoted = false;
            }
            else if (character == '"') quoted = true;
            else if (character == ',') { tokens.Add(content[start..index].Trim()); start = index + 1; }
            else if (character is '(' or ')') return false;
        }
        if (quoted || escaped) return false;
        tokens.Add(content[start..].Trim());
        if (tokens.Any(static token => token.Length == 0)) return false;
        arguments = tokens.ToArray();
        return true;
    }
}
