/// @file    GeneratorTests.cs
/// @brief   空型・partial 宣言・BOM checksum と Inspector field の限定警告抑制を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Fbzz.ScriptGenerator;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Text;

namespace Fbzz.ScriptGeneratorTests;

public static class GeneratorTests
{
    private static readonly CSharpParseOptions PARSE_OPTIONS = new(LanguageVersion.Latest);
    private static readonly string PROJECT_DIRECTORY = Path.Combine(AppContext.BaseDirectory, "GeneratorTestProject");
    private static readonly ImmutableArray<MetadataReference> REFERENCES = References();

    private static int Main() => RunAll();

    public static int RunAll()
    {
        (string Name, Action Test)[] tests =
        [
            ("EmptyAndPartialSourceDeclarations", EmptyAndPartialSourceDeclarations),
            ("SameLinePartialDeclarationsShareOneLocation", SameLinePartialDeclarationsShareOneLocation),
            ("CompiledChecksumIncludesUtf8Bom", CompiledChecksumIncludesUtf8Bom),
            ("ConcreteDerivedSourceAndExcludedTypes", ConcreteDerivedSourceAndExcludedTypes),
             ("DataAssetSourceDeclarations", DataAssetSourceDeclarations),
            ("InvalidSourceMetadataIsDiagnosed", InvalidSourceMetadataIsDiagnosed),
            ("InspectorSettingsSuppressOnlyCs0649", InspectorSettingsSuppressOnlyCs0649),
            ("ScriptContractDiagnostics", ContractAnalyzerTests.RunAll),
            ("BindingContracts", BindingGeneratorTests.RunAll)
        ];
        int failures = 0;
        foreach ((string name, Action test) in tests)
        {
            try { test(); Console.WriteLine($"[PASS] Generator.{name}"); }
            catch (Exception exception) { failures++; Console.WriteLine($"[FAIL] Generator.{name}: {exception.Message}"); }
        }
        Console.WriteLine($"GENERATOR TESTS passed={tests.Length - failures} failed={failures}");
        return failures == 0 ? 0 : 1;
    }

    private static void EmptyAndPartialSourceDeclarations()
    {
        SyntaxTree first = Tree("using Fbzz; namespace Actors; internal class EmptyActor : Script {} internal partial class PartialActor : Script {}", "Actors.cs");
        SyntaxTree second = Tree("namespace Actors; internal partial class PartialActor {}", "Partial.cs");
        Compilation output = Generate(Create(first, second), new ProjectOptions(PROJECT_DIRECTORY), out ImmutableArray<Diagnostic> diagnostics);
        Check(!diagnostics.Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "Valid declarations must compile metadata.");
        AttributeData[] attributes = Sources(output);
        Check(attributes.Length == 3, "Empty types and every partial declaration must produce an attribute.");
        Check(attributes.Count(static attribute => (string?)attribute.ConstructorArguments[0].Value == "Actors.PartialActor") == 2,
            "Partial declarations must retain both source paths.");
        Check(attributes.All(static attribute => (int?)attribute.ConstructorArguments[2].Value == 1), "Lines are one based.");
        Check(attributes.Select(static attribute => (string?)attribute.ConstructorArguments[1].Value)
            .All(static path => path is not null && path.StartsWith("Assets/Scripts/", StringComparison.Ordinal)),
            "Paths must be project relative with forward slashes.");
    }

    private static void SameLinePartialDeclarationsShareOneLocation()
    {
        SyntaxTree tree = Tree("using Fbzz; internal partial class Actor : Script {} internal partial class Actor {}", "Actor.cs");
        Compilation output = Generate(Create(tree), new ProjectOptions(PROJECT_DIRECTORY), out ImmutableArray<Diagnostic> diagnostics);
        Check(!diagnostics.Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "Same-line partial declarations are legal C#.");
        Check(Sources(output).Length == 1, "A location without column metadata must not produce a duplicate path/line attribute.");
    }

    private static void CompiledChecksumIncludesUtf8Bom()
    {
        byte[] content = Encoding.UTF8.GetPreamble().Concat(Encoding.UTF8.GetBytes("using Fbzz; internal class BomActor : Script {}" )).ToArray();
        using MemoryStream stream = new(content);
        SourceText source = SourceText.From(stream, Encoding.UTF8, SourceHashAlgorithm.Sha256);
        SyntaxTree tree = CSharpSyntaxTree.ParseText(source, PARSE_OPTIONS, Path.Combine(PROJECT_DIRECTORY, "Assets", "Scripts", "Bom.cs"));
        Compilation output = Generate(Create(tree), new ProjectOptions(PROJECT_DIRECTORY), out ImmutableArray<Diagnostic> diagnostics);
        Check(!diagnostics.Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "BOM source must be accepted.");
        string expected = Convert.ToHexString(SHA256.HashData(content)).ToLowerInvariant();
        Check((string?)Sources(output).Single().ConstructorArguments[3].Value == expected,
            "Metadata checksum must match the original source bytes, including the BOM.");
    }

    private static void ConcreteDerivedSourceAndExcludedTypes()
    {
        SyntaxTree source = Tree("""
            using Fbzz;
            namespace Actors;
            internal abstract class AbstractActor : Script {}
            internal class GenericActor<T> : Script {}
            internal class ConcreteActor : GenericActor<int> {}
            internal class Container { internal class NestedActor : Script {} }
            """, "Types.cs");
        Compilation output = Generate(Create(source), new ProjectOptions(PROJECT_DIRECTORY), out ImmutableArray<Diagnostic> diagnostics);
        Check(!diagnostics.Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "Supported derived type metadata must compile.");
        Check((string?)Sources(output).Single().ConstructorArguments[0].Value == "Actors.ConcreteActor",
            "Only concrete, nongeneric, unnested Script types belong to source metadata.");
    }

    private static void InvalidSourceMetadataIsDiagnosed()
    {
        CSharpCompilation compilation = Create(Tree("using Fbzz; internal class Actor : Script {}", "Actor.cs"));
        Generate(compilation, new ProjectOptions(null), out ImmutableArray<Diagnostic> noProject);
        Check(noProject.Any(static diagnostic => diagnostic.Id == "FBZZ1001"), "A missing project root must be diagnosed.");
        SyntaxTree sha1 = Tree("using Fbzz; internal class Actor : Script {}", "Actor.cs", SourceHashAlgorithm.Sha1);
        Generate(Create(sha1), new ProjectOptions(PROJECT_DIRECTORY), out ImmutableArray<Diagnostic> wrongChecksum);
        Check(wrongChecksum.Any(static diagnostic => diagnostic.Id == "FBZZ1001"), "SHA1 must not silently replace the SHA256 contract.");
    }

    private static void DataAssetSourceDeclarations()
    {
        Compilation output = Generate(Create(Tree("using Fbzz; internal class WeaponData : DataAsset {}", "WeaponData.cs")),
            new ProjectOptions(PROJECT_DIRECTORY), out ImmutableArray<Diagnostic> diagnostics);
        Check(!diagnostics.Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "DataAsset source metadata must compile.");
        Check((string?)Sources(output).Single().ConstructorArguments[0].Value == "WeaponData",
            "DataAsset identities need the same source metadata as Script identities.");
    }

    private static void InspectorSettingsSuppressOnlyCs0649()
    {
        SyntaxTree source = Tree("""
            using System;
            using System.Collections.Generic;
            using Fbzz;
            internal enum Mode { Idle }
            internal enum WideMode : long { Idle }
             [Serializable] internal class Values { public float speed = 2f; }
             [Serializable] internal class NeedsArgument { public NeedsArgument(int value) {} }
             [Serializable] internal class SavedScript : Script {}
             [Serializable] internal class DictionaryValue : Dictionary<string, int> {}
             [Serializable] internal class ListValue : List<float> {}
             [Serializable] internal class FrameworkBasedValue : EventArgs {}
             [Serializable] internal class ReservedTokenValue { [FieldKey("$go")] public string text = "source-object"; }
             [Serializable] internal class ReservedPreviousTokenValue { [PreviousFieldKey("$go")] public string text = "source-object"; }
             [Serializable] internal class SavedBase { [Setting] private float configured = 2f; public float Read() => configured; }
             [Serializable] internal class SavedDerived : SavedBase { public int child = 3; }
             [Serializable] internal struct SavedValue { public int count; }
            internal class Actor : Script
            {
                public GameObject target;
                [Setting] private GameObject configured;
                public float speed;
                public int[] counts;
                public Mode mode;
                private GameObject runtime;
                [NonSerialized] public GameObject transient;
                private readonly GameObject fixedReference;
                private static float shared;
                public object unsupported;
                 public List<GameObject> paths;
                public GameObject[][] nested;
                 public Values options;
                 public Quaternion rotation;
                 public NeedsArgument invalidConstructor;
                 public SavedScript component;
                 public Dictionary<string, int> map;
                 public DateTime timestamp;
                 public double doubleValue;
                 public decimal decimalValue;
                 public long longValue;
                  public Guid guidValue;
                  public TimeSpan durationValue;
                 public DictionaryValue dictionaryValue;
                 public ListValue listValue;
                 public FrameworkBasedValue frameworkBasedValue;
                  public ReservedTokenValue reservedTokenValue;
                  public ReservedPreviousTokenValue reservedPreviousTokenValue;
                 public SavedDerived inherited;
                 public SavedValue valueStruct;
                public WideMode wide;
                public bool Read() => target == null || configured == null || speed == 0 || counts == null || mode == Mode.Idle ||
                    runtime == null || transient == null || fixedReference == null || shared == 0 || unsupported == null ||
                     paths == null || nested == null || options == null || rotation.W == 0 || invalidConstructor == null || component == null || map == null ||
                      timestamp == default || doubleValue == 0 || decimalValue == 0 || longValue == 0 || guidValue == default || durationValue == default || dictionaryValue == null ||
                      listValue == null || frameworkBasedValue == null || reservedTokenValue == null || reservedPreviousTokenValue == null ||
                      inherited == null || valueStruct.count == 0 || wide == WideMode.Idle;
            }
            internal class Ordinary { public GameObject target; public bool Read() => target == null; }
            """, "Settings.cs");
        CSharpCompilation compilation = Create(source);
         string[] saved = ["target", "configured", "speed", "counts", "mode", "paths", "nested", "options", "rotation", "inherited", "valueStruct", "doubleValue", "decimalValue", "longValue", "wide"];
        HashSet<string> before = compilation.GetDiagnostics().Where(static diagnostic => diagnostic.Id == "CS0649")
            .Select(DiagnosticField).ToHashSet(StringComparer.Ordinal);
        Check(saved.All(before.Contains), "The test must observe the compiler warnings before suppression.");
        CompilationWithAnalyzers analysis = compilation.WithAnalyzers(
            ImmutableArray.Create<DiagnosticAnalyzer>(new ScriptSettingFieldSuppressor()),
            new CompilationWithAnalyzersOptions(new AnalyzerOptions(ImmutableArray<AdditionalText>.Empty), null, false, false, true));
        ImmutableArray<Diagnostic> after = analysis.GetAllDiagnosticsAsync().GetAwaiter().GetResult();
        Check(!after.Any(static diagnostic => diagnostic.Id == "AD0001"), "Suppressor must not fail during compiler analysis.");
        foreach (string field in saved)
            Check(after.Any(diagnostic => diagnostic.Id == "CS0649" && DiagnosticField(diagnostic) == field && diagnostic.IsSuppressed),
                "Saved field must suppress only its CS0649: " + field);
         string[] runtime = ["runtime", "transient", "fixedReference", "shared", "unsupported", "invalidConstructor", "component", "map", "timestamp", "guidValue", "durationValue", "dictionaryValue", "listValue", "frameworkBasedValue", "reservedTokenValue", "reservedPreviousTokenValue"];
        foreach (string field in runtime)
            Check(after.Any(diagnostic => diagnostic.Id == "CS0649" && DiagnosticField(diagnostic) == field && !diagnostic.IsSuppressed),
                "Runtime or unsupported fields must keep CS0649: " + field);
        Check(after.Any(diagnostic => diagnostic.Id == "CS0649" && DiagnosticField(diagnostic) == "target" && !diagnostic.IsSuppressed),
            "Ordinary classes must keep their warning even when their field name matches a Script setting.");
    }

    private static string DiagnosticField(Diagnostic diagnostic) => diagnostic.Location.SourceTree is SyntaxTree tree
        ? tree.GetText().ToString(diagnostic.Location.SourceSpan) : string.Empty;

    private static SyntaxTree Tree(string source, string file, SourceHashAlgorithm algorithm = SourceHashAlgorithm.Sha256)
        => CSharpSyntaxTree.ParseText(SourceText.From(source, Encoding.UTF8, algorithm), PARSE_OPTIONS,
            Path.Combine(PROJECT_DIRECTORY, "Assets", "Scripts", file));

    private static CSharpCompilation Create(params SyntaxTree[] sources) => CSharpCompilation.Create("Generator.Test.Scripts", sources, REFERENCES,
        new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, warningLevel: 4, nullableContextOptions: NullableContextOptions.Warnings));

    private static Compilation Generate(CSharpCompilation compilation, AnalyzerConfigOptionsProvider options, out ImmutableArray<Diagnostic> diagnostics)
    {
        GeneratorDriver driver = CSharpGeneratorDriver.Create(new ISourceGenerator[] { new ScriptSourceGenerator().AsSourceGenerator() },
            parseOptions: PARSE_OPTIONS, optionsProvider: options);
        driver.RunGeneratorsAndUpdateCompilation(compilation, out Compilation output, out diagnostics);
        return output;
    }

    private static AttributeData[] Sources(Compilation compilation) => compilation.Assembly.GetAttributes()
        .Where(static attribute => attribute.AttributeClass?.ToDisplayString() == "Fbzz.ScriptSourceAttribute").ToArray();

    private static ImmutableArray<MetadataReference> References()
    {
        string platform = AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string
            ?? throw new InvalidOperationException("The runtime must provide platform references.");
        return platform.Split(Path.PathSeparator).Append(typeof(Script).Assembly.Location).Distinct(StringComparer.OrdinalIgnoreCase)
            .Select(static path => (MetadataReference)MetadataReference.CreateFromFile(path)).ToImmutableArray();
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private sealed class ProjectOptions(string? directory) : AnalyzerConfigOptionsProvider
    {
        private readonly Options m_global = new(directory);
        private readonly Options m_empty = new(null);
        public override AnalyzerConfigOptions GlobalOptions => m_global;
        public override AnalyzerConfigOptions GetOptions(SyntaxTree tree) => m_empty;
        public override AnalyzerConfigOptions GetOptions(AdditionalText textFile) => m_empty;
    }

    private sealed class Options(string? directory) : AnalyzerConfigOptions
    {
        public override bool TryGetValue(string key, out string value)
        {
            if (directory is not null && key == "build_property.MSBuildProjectDirectory") { value = directory; return true; }
            value = string.Empty;
            return false;
        }
    }
}
