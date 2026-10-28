/// @file    ContractAnalyzerTests.cs
/// @brief   Compile real SDK authoring examples and check contract diagnostics and source locations.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Text;
using Fbzz;
using Fbzz.ScriptGenerator;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Fbzz.ScriptGeneratorTests;

internal static class ContractAnalyzerTests
{
    public static void RunAll()
    {
        (string Name, Action Test)[] tests =
        [
            ("EveryLifecycleOverrideCompiles", EveryLifecycleOverrideCompiles),
            ("SequenceCompletionStringAliasesUseExactSystemType", SequenceCompletionStringAliasesUseExactSystemType),
            ("HighFacadeRequirementsAndModelReferencesRemainAuthorable", HighFacadeRequirementsAndModelReferencesRemainAuthorable),
            ("DrawPropertyReadsDistinguishAssignmentTargets", DrawPropertyReadsDistinguishAssignmentTargets),
            ("InvalidOverridesAndTyposHaveLocations", InvalidOverridesAndTyposHaveLocations),
            ("EveryReservedNameRejectsHiding", EveryReservedNameRejectsHiding),
            ("AsyncEngineDelegatesAreRejected", AsyncEngineDelegatesAreRejected),
            ("OrdinaryAsyncWorkRemainsAvailable", OrdinaryAsyncWorkRemainsAvailable),
            ("PersistedFieldMistakesAreDiagnosed", PersistedFieldMistakesAreDiagnosed),
            ("SupportedSettingsRemainAccepted", SupportedSettingsRemainAccepted),
            ("PrimitiveWidthsAndRangeOverloadsRemainExact", PrimitiveWidthsAndRangeOverloadsRemainExact),
            ("ConcreteTypeRequirementsAreDiagnosed", ConcreteTypeRequirementsAreDiagnosed),
            ("AuthoringCallbacksRejectRuntimeAndEngineDependencies", AuthoringCallbacksRejectRuntimeAndEngineDependencies),
            ("TypedComponentRequirementsHaveLocatedDiagnostics", TypedComponentRequirementsHaveLocatedDiagnostics),
            ("EditorActionsValidateSignatureAndSettingsOnlyDependencies", EditorActionsValidateSignatureAndSettingsOnlyDependencies)
            , ("TypedAssetValuesArePureAuthoringDependencies", TypedAssetValuesArePureAuthoringDependencies)
            , ("WorldQueriesAllowDrawWhilePlaybackAndGeometryMutationsAreDiagnosed", WorldQueriesAllowDrawWhilePlaybackAndGeometryMutationsAreDiagnosed)
        ];
        foreach ((string name, Action test) in tests) { test(); Console.WriteLine("[PASS] ContractAnalyzer." + name); }
        Console.WriteLine($"CONTRACT ANALYZER TESTS passed={tests.Length} failed=0");
    }

    private static void TypedAssetValuesArePureAuthoringDependencies()
    {
        Compilation compilation = Compile("using Fbzz; public class Profile : DataAsset { public MaterialAssetRef material; public TextureAssetRef texture; protected override void OnValidate() { material = new MaterialAssetRef(\"material-guid\"); texture = new TextureAssetRef(material.Reference); _ = texture.IsEmpty; } }");
        Check(!compilation.GetDiagnostics().Any(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error) && Analyze(compilation).Length == 0,
            "Pure typed asset GUID constructors/properties must be usable in settings-only authoring callbacks.");
    }
    private static void WorldQueriesAllowDrawWhilePlaybackAndGeometryMutationsAreDiagnosed()
    {
        const string accepted = "using Fbzz; [RequireComponent(typeof(Terrain),typeof(Water),typeof(VFX),typeof(SequencePlayer))] public class Actor : Script { protected override void DrawGizmos() { _ = GetComponent<Terrain>()!.TrySampleWorld(1,2,out var surface); _ = surface.Position; _ = GetComponent<Water>()!.GetSurfaceHeightWorld(1,2,0); } }";
        Compilation valid = Compile(accepted);
        Check(!valid.GetDiagnostics().Any(value => value.Severity == DiagnosticSeverity.Error) && Analyze(valid).Length == 0,
            "Exact native world queries and all new typed requirements must compile in readonly draw callbacks.");
        const string forbidden = "using Fbzz; public class Actor : Script { protected override void DrawGizmos() { GetComponent<VFX>()!.Play(); GetComponent<Terrain>()!.SetHeightAtGrid(1,2,3); } }";
        Check(Analyze(Compile(forbidden)).Count(value => value.Id == "FBZZ2010") == 2,
            "Readonly draw must still reject actual playback and geometry writes.");
    }
    private static void EveryLifecycleOverrideCompiles()
    {
        string methods = string.Join("\n", ScriptLifecycleContract.Callbacks.Select(static callback =>
            "protected override void " + callback.Name + "(" + (callback.ParameterTypeName is null ? "" : callback.ParameterTypeName + " value") + ") { }"));
        Compilation compilation = Compile("using Fbzz; public class Actor : Script { " + methods + " }");
        Check(!compilation.GetDiagnostics().Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "Every shared lifecycle must expose an overrideable SDK method.");
        Check(Analyze(compilation).Length == 0, "Valid SDK overrides must not receive contract diagnostics.");
    }

    private static void SequenceCompletionStringAliasesUseExactSystemType()
    {
        foreach (string spelling in new[] { "string", "System.String" }) {
            Compilation compilation = Compile("using Fbzz; public class Actor : Script { protected override void OnSequenceFinished(" + spelling + " value) { } }");
            Check(!compilation.GetDiagnostics().Any(value => value.Severity == DiagnosticSeverity.Error) && Analyze(compilation).Length == 0,
                "The canonical System.String notification must accept either C# spelling of the same runtime type.");
        }
        Check(Analyze(Compile("using Fbzz; public class Actor : Script { protected override void OnSequenceFinished(object value) { } }")).Any(value => value.Id == "FBZZ2001"),
            "An object payload must still be rejected as a different notification type.");
    }
    private static void HighFacadeRequirementsAndModelReferencesRemainAuthorable()
    {
        const string source = "using Fbzz; [RequireComponent(typeof(LineRenderer),typeof(MotionWarp),typeof(FlowField))] public class Actor : Script { public ComponentRef<LineRenderer> line; public ComponentRef<MotionWarp> warp; public ModelAssetRef model; protected override void OnValidate() { model = new ModelAssetRef(\"model-guid\"); } protected override void DrawGizmos() { if (line.TryGet(out var renderer)) _ = renderer.PositionCount; } }";
        Compilation compilation = Compile(source);
        Diagnostic[] errors = compilation.GetDiagnostics().Where(value => value.Severity == DiagnosticSeverity.Error).Concat(Analyze(compilation)).ToArray();
        Check(errors.Length == 0,
            "High facade requirements, typed saved references, pure model GUID authoring and readonly Line queries must compile without special authoring declarations.\n"
                + string.Join("\n", errors.Select(value => value.ToString())));
    }
    private static void DrawPropertyReadsDistinguishAssignmentTargets()
    {
        Compilation accepted = Compile("using Fbzz; public class Actor : Script { public ComponentRef<LineRenderer> line; protected override void DrawGizmos() { if (line.TryGet(out var renderer)) { _ = renderer.PositionCount; int count = renderer.PositionCount; Gizmos.Sphere(Vector3.ZERO, count); } } }");
        Diagnostic[] errors = accepted.GetDiagnostics().Where(value => value.Severity == DiagnosticSeverity.Error).Concat(Analyze(accepted)).ToArray();
        Check(errors.Length == 0, "A native property used as the value of a discard assignment remains a readonly query.\n" + string.Join("\n", errors.Select(value => value.ToString())));
        Compilation rejected = Compile("using Fbzz; public class Actor : Script { public ComponentRef<LineRenderer> line; protected override void DrawGizmos() { if (line.TryGet(out var renderer)) renderer.Enabled = false; } }");
        Check(Analyze(rejected).Count(value => value.Id == "FBZZ2010") == 1,
            "Assigning a native property's target must still reject mutation inside DrawGizmos.");
    }

    private static void InvalidOverridesAndTyposHaveLocations()
    {
        const string source = """
            using System.Threading.Tasks;
            using Fbzz;
            public class Actor : Script
            {
                void Update() { }
                protected override async void FixedUpdate() { await Task.Yield(); }
                protected override void OnCollisionEnter() { }
                void Updat() { }
                public void Initializ() { }
            }
            """;
        Compilation compilation = Compile(source);
        ImmutableArray<Diagnostic> diagnostics = Analyze(compilation);
        Check(diagnostics.Count(static value => value.Id == "FBZZ2001") == 3, "Hidden, async and incorrectly parameterized lifecycles need early errors.");
        Check(diagnostics.Count(static value => value.Id == "FBZZ2003" && value.Severity == DiagnosticSeverity.Warning) == 1, "A likely spelling mistake should receive one warning.");
        Check(compilation.GetDiagnostics().Any(static value => value.Id == "CS0115"), "The C# compiler must reject an invalid override signature itself.");
        Diagnostic update = diagnostics.Single(value => value.Id == "FBZZ2001" && source.Substring(value.Location.SourceSpan.Start, value.Location.SourceSpan.Length) == "Update");
        Check(update.Location.GetLineSpan().Path == "Assets/Scripts/ContractActor.cs" && update.Location.GetLineSpan().StartLinePosition.Line == 4,
            "Authoring diagnostics must identify the actual member's file and line.");
    }

    private static void EveryReservedNameRejectsHiding()
    {
        string methods = string.Join("\n", ScriptLifecycleContract.Callbacks.Select(static callback =>
            "void " + callback.Name + "(" + (callback.ParameterTypeName is null ? "" : callback.ParameterTypeName + " value") + ") { }"));
        ImmutableArray<Diagnostic> diagnostics = Analyze(Compile("using Fbzz; public class Actor : Script { " + methods + " }"));
        Check(diagnostics.Count(static value => value.Id == "FBZZ2001") == ScriptLifecycleContract.Callbacks.Count,
            "The analyzer must consume every generated SDK lifecycle rather than a partial handwritten name list.");
    }

    private static void AsyncEngineDelegatesAreRejected()
    {
        const string source = """
            using System;
            using System.Threading.Tasks;
            using Fbzz;
            public class Actor : Script
            {
                protected override void Start()
                {
                    Jobs.Wait(0, async () => await Task.Yield());
                    Jobs.WaitFrames(1, async () => await Task.Yield());
                    Jobs.Repeat(1, async () => await Task.Yield());
                    Jobs.Tween(0, 1, 1, async value => await Task.Yield(), completed: async () => await Task.Yield());
                    Events.Subscribe<int>(async value => await Task.Yield());
                    Jobs.Timeline().Then(async () => await Task.Yield());
                    Jobs.Timeline().Tween(0, 1, 1, async value => await Task.Yield());
                    Action local = async () => await Task.Yield();
                    Jobs.Wait(0, local);
                    Jobs.Wait(0, Deferred);
                }
                async void Deferred() { await Task.Yield(); }
            }
            """;
        Compilation compilation = Compile(source);
        Check(!compilation.GetDiagnostics().Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "This probe must be ordinary legal C# so the FBZZ analyzer adds the missing safety contract.");
        ImmutableArray<Diagnostic> diagnostics = Analyze(compilation);
        Check(diagnostics.Count(static value => value.Id == "FBZZ2002") == 10, "Every annotated engine callback must reject direct async lambdas, method groups and local delegate initializers.");
        Check(diagnostics.All(static value => value.Location.IsInSource), "Each asynchronous callback diagnostic needs an editable source location.");
    }

    private static void OrdinaryAsyncWorkRemainsAvailable()
    {
        const string source = """
            using System;
            using System.Threading.Tasks;
            using Fbzz;
            public class Actor : Script
            {
                protected override void Start()
                {
                    _ = Task.Run(async () => { await Task.Yield(); return 3; });
                    Jobs.Wait(1, () => Transform.WorldPosition = Vector3.ZERO);
                    Jobs.Tween(0, 1, 1, value => { }, completed: () => { });
                    Events.Subscribe<int>(value => { });
                }
                public async Task<int> Compute() { await Task.Yield(); return 3; }
            }
            public class Ordinary { void Update() { } void Updat() { } }
            """;
        Compilation compilation = Compile(source);
        Check(!compilation.GetDiagnostics().Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), "Supported async pure work and synchronous Engine callbacks must compile.");
        Check(Analyze(compilation).Length == 0, "The analyzer must leave ordinary C# tasks and unrelated helper methods available.");
    }

    private static void PersistedFieldMistakesAreDiagnosed()
    {
        const string source = """
            using System;
            using System.Collections.Generic;
            using Fbzz;
            public class Actor : Script
            {
                [Setting] Dictionary<string,int> map = new();
                [KeepOnReload] Action retained = () => { };
                [Setting] readonly float fixedValue = 1;
                [Setting, NonSerialized] float conflict;
                [RequiredReference] public int invalidReference;
                [ValueRange(0, 1)] public Vector3 invalidRange;
                public Dictionary<string,int> runtimeMap = new();
                [NonSerialized] public Dictionary<string,int> transient = new();
            }
            """;
        ImmutableArray<Diagnostic> diagnostics = Analyze(Compile(source));
        Check(diagnostics.Count(static value => value.Id == "FBZZ2004") == 3, "Explicit unsupported or immutable settings require errors.");
        Check(diagnostics.Count(static value => value.Id == "FBZZ2005") == 1 && diagnostics.Count(static value => value.Id == "FBZZ2006") == 1
            && diagnostics.Count(static value => value.Id == "FBZZ2007") == 1, "Conflicting save flags, incorrect required references and nonnumeric ranges require early errors.");
        Check(diagnostics.Count(static value => value.Id == "FBZZ2008") == 1, "Only the unsupported public persisted candidate should warn; explicit transient state remains valid.");
    }

    private static void SupportedSettingsRemainAccepted()
    {
        const string source = """
            using System;
            using System.Collections.Generic;
            using Fbzz;
            [Serializable] public class Options { public float speed = 1; [Setting] int count; }
            public class Actor : Script
            {
                public Options options = new();
                [RequiredReference] public GameObject? target;
                [RequiredReference] public GameObject[] targets = [];
                [RequiredReference] public List<GameObject> path = [];
                [ValueRange(0, 10)] public float[] weights = [];
                public Quaternion rotation;
                public Vector2 screen;
                public Vector4 color;
                public Vector2[] screenPath = [];
                public List<Vector4> colors = [];
                public uint mask;
                [ValueRange(0, 10)] public List<uint> counts = [];
                [KeepOnReload] float state;
                [NonSerialized] public Action? runtimeCallback;
                protected override void Update() { }
            }
            """;
        Check(!Analyze(Compile(source)).Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error),
            "Supported structured settings, nullable unconnected defaults and runtime-only state must retain their contracts.");
    }

    private static void ConcreteTypeRequirementsAreDiagnosed()
    {
        const string source = """
            using Fbzz;
            public class Generic<T> : Script { }
            public class Outer { public class Nested : Script { } }
            public class Arguments : DataAsset { public Arguments(int number) { } }
            public abstract class Abstract<T> : Script { }
            public class Concrete : Abstract<int> { }
            """;
        Check(Analyze(Compile(source)).Count(static diagnostic => diagnostic.Id == "FBZZ2009") == 3,
            "Concrete invalid types should be diagnosed while abstract generic bases and their valid concrete descendants remain supported.");
    }

    private static void PrimitiveWidthsAndRangeOverloadsRemainExact()
    {
        const string valid = """
            using System;
            using System.Collections.Generic;
            using Fbzz;
            public enum Wide : ulong { HIGH = ulong.MaxValue }
            public class Actor : Script
            {
                public sbyte signed8; public byte unsigned8; public short signed16; public ushort unsigned16;
                public long[] signedPath = []; public List<ulong> unsignedPath = []; public char code;
                [ValueRange(0, 100)] public int amount = 2;
                [ValueRange(0u, uint.MaxValue)] public uint mask;
                [ValueRange(long.MaxValue - 1, long.MaxValue)] public long signed = long.MaxValue;
                [ValueRange(ulong.MaxValue - 1, ulong.MaxValue)] public ulong unsigned = ulong.MaxValue;
                [ValueRange(0f, 100f)] public float single;
                [ValueRange(1d, double.MaxValue)] public double real = 2;
                [ValueRange("0.1", "0.5")] public decimal money = 0.3m;
                public Wide mode = Wide.HIGH;
            }
            """;
        Compilation compilation = Compile(valid);
        Check(!compilation.GetDiagnostics().Any(value => value.Severity == DiagnosticSeverity.Error), "Legal primitive and exact ValueRange overloads must compile against the real SDK.");
        Check(Analyze(compilation).Length == 0, "The analyzer must accept every common primitive, recursive collections and typed bound overloads.");
        const string invalid = """
            using Fbzz;
            public class Bad : Script
            {
                [ValueRange(9007199254740995L, 9007199254740993L)] public long reversed;
                [ValueRange(ulong.MaxValue, 0UL)] public ulong unsigned;
                [ValueRange("0.5", "0.1")] public decimal reversedMoney;
                [ValueRange(0, 1)] public decimal binaryBounds;
                [ValueRange("oops", "2")] public decimal malformed;
                [ValueRange(double.NaN, 2d)] public double nonfinite;
            }
            """;
        ImmutableArray<Diagnostic> diagnostics = Analyze(Compile(invalid)).Where(value => value.Id == "FBZZ2007").ToImmutableArray();
        Check(diagnostics.Length == 6 && diagnostics.All(value => value.Location.GetLineSpan().Path == "Assets/Scripts/ContractActor.cs"),
            "Invalid exact wide/string/nonfinite bounds must retain located errors without double rounding or diagnostic suppression.");
    }

    private static void AuthoringCallbacksRejectRuntimeAndEngineDependencies()
    {
        const string source = """
            using System;
            using System.IO;
            using Fbzz;
            public class Actor : Script
            {
                public int amount = 2;
                private int cache = 99;
                private static int shared;
                private const int LIMIT = 10;
                protected override void Reset() { amount = cache; shared++; File.WriteAllText("bad", "bad"); }
                protected override void OnValidate() { amount = Math.Clamp(amount, 0, LIMIT); _ = Transform.WorldPosition; }
            }
            """;
        Compilation compilation = Compile(source);
        Check(!compilation.GetDiagnostics().Any(value => value.Severity == DiagnosticSeverity.Error), "The probe must otherwise be valid C#.");
        ImmutableArray<Diagnostic> errors = Analyze(compilation).Where(value => value.Id == "FBZZ2010").ToImmutableArray();
        Check(errors.Length == 5 && errors.All(value => value.Severity == DiagnosticSeverity.Error && value.Location.GetLineSpan().Path == "Assets/Scripts/ContractActor.cs"),
            "Private cache, mutable static, I/O and both Engine property accesses must receive located authoring dependency errors; settings/constants/math remain accepted.");
    }

    private static void TypedComponentRequirementsHaveLocatedDiagnostics()
    {
        const string source = """
            using Fbzz;
            [RequireComponent(typeof(Camera))] [ExecutionOrder(-100)] public abstract class ValidBase : Script { }
            public abstract class PeerBase : Script { }
            [RequireComponent(typeof(PeerBase))] public class Valid : ValidBase { }
            [RequireComponent(typeof(string))] public class WrongType : Script { }
            [RequireComponent] public class Empty : Script { }
            [ExecutionOrder(2)] public class WrongOwner : DataAsset { }
            """;
        Compilation compilation = Compile(source);
        Check(!compilation.GetDiagnostics().Any(value => value.Severity == DiagnosticSeverity.Error), "The attribute probe must compile as ordinary C#.");
        ImmutableArray<Diagnostic> errors = Analyze(compilation).Where(value => value.Id == "FBZZ2011").ToImmutableArray();
        Check(errors.Length == 3 && errors.All(value => value.Location.GetLineSpan().Path == "Assets/Scripts/ContractActor.cs"),
            "Unsupported type, empty requirement and non-Script execution order must have exact source diagnostics; typed inherited requirements remain valid.");
    }

    private static void EditorActionsValidateSignatureAndSettingsOnlyDependencies()
    {
        const string source = """
            using System.IO;
            using System.Threading.Tasks;
            using Fbzz;
            public class Actor : Script
            {
                public int amount;
                int cache;
                [EditorAction("valid")] void Valid() { amount *= 2; }
                [EditorAction("bad")] void Bad() { amount = cache; File.WriteAllText("bad", "bad"); }
                [EditorAction("async")] async void Async() { await Task.Yield(); }
                [EditorAction("args")] void Args(int value) { }
                [EditorAction("static")] static void Static() { }
            }
            """;
        Compilation compilation = Compile(source);
        Check(!compilation.GetDiagnostics().Any(value => value.Severity == DiagnosticSeverity.Error), "The action probe must compile as ordinary C#.");
        ImmutableArray<Diagnostic> diagnostics = Analyze(compilation);
        Check(diagnostics.Count(value => value.Id == "FBZZ2012") == 3, "Async, argument-taking and static actions must have located signature errors.");
        Check(diagnostics.Count(value => value.Id == "FBZZ2010") >= 2, "Action runtime cache and external I/O must use the existing authoring dependency errors.");
    }

    private static CSharpCompilation Compile(string source)
    {
        string platform = AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string ?? throw new InvalidOperationException("Platform references are required.");
        ImmutableArray<MetadataReference> references = platform.Split(Path.PathSeparator).Append(typeof(Script).Assembly.Location)
            .Distinct(StringComparer.OrdinalIgnoreCase).Select(static path => (MetadataReference)MetadataReference.CreateFromFile(path)).ToImmutableArray();
        return CSharpCompilation.Create("Contract.Analyzer.Probe", [CSharpSyntaxTree.ParseText(source, new CSharpParseOptions(LanguageVersion.Latest), "Assets/Scripts/ContractActor.cs", Encoding.UTF8)], references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, nullableContextOptions: NullableContextOptions.Enable));
    }

    private static ImmutableArray<Diagnostic> Analyze(Compilation compilation)
    {
        ImmutableArray<Diagnostic> diagnostics = compilation.WithAnalyzers(ImmutableArray.Create<DiagnosticAnalyzer>(new ScriptContractAnalyzer()))
            .GetAnalyzerDiagnosticsAsync().GetAwaiter().GetResult();
        Check(!diagnostics.Any(static diagnostic => diagnostic.Id == "AD0001"), "The analyzer itself must not fail: " + string.Join("; ", diagnostics));
        return diagnostics;
    }

    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
