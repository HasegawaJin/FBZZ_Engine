/// @file    ScriptContractAnalyzer.cs
/// @brief   Diagnose Script overrides, persisted fields and synchronous Engine delegates before Host loading.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Globalization;
using System.Linq;
using System.Numerics;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;
using static Fbzz.ScriptGenerator.ScriptFieldContract;

namespace Fbzz.ScriptGenerator;

[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class ScriptContractAnalyzer : DiagnosticAnalyzer
{
    private static readonly DiagnosticDescriptor LIFECYCLE = Rule("FBZZ2001", "Invalid Script lifecycle override",
        "'{0}' requires a synchronous protected override void method with {1}", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor ASYNC_CALLBACK = Rule("FBZZ2002", "Engine callbacks must be synchronous",
        "Parameter '{0}' cannot receive an async delegate; use owned Jobs waits or routines and apply Task results in Update", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor POSSIBLE_TYPO = Rule("FBZZ2003", "Possible lifecycle spelling mistake",
        "'{0}' is not a lifecycle callback; override '{1}' if this method should be dispatched", DiagnosticSeverity.Warning);
    private static readonly DiagnosticDescriptor UNSUPPORTED_FIELD = Rule("FBZZ2004", "Unsupported persisted field",
        "'{0}' requires a supported mutable instance field for Setting or KeepOnReload; use ComponentRef<T> for saved Component/Script references", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor FIELD_CONFLICT = Rule("FBZZ2005", "Conflicting field persistence attributes",
        "'{0}' cannot combine Setting and NonSerialized", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor REQUIRED_REFERENCE = Rule("FBZZ2006", "Invalid required scene reference",
        "'{0}' requires a saved GameObject or ComponentRef<T>, or a collection of these references, for RequiredReference", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor FIELD_RANGE = Rule("FBZZ2007", "Invalid persisted value range",
        "'{0}' requires numeric primitive values and ordered finite ValueRange bounds; decimal bounds use invariant strings", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor UNSAVED_FIELD = Rule("FBZZ2008", "Public field is not persisted",
        "'{0}' is not supported by the save codec; use NonSerialized for runtime state or a supported setting type", DiagnosticSeverity.Warning);
    private static readonly DiagnosticDescriptor SCRIPT_TYPE = Rule("FBZZ2009", "Invalid concrete managed type",
        "'{0}' must be unnested, non-generic and have a parameterless constructor", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor AUTHORING_DEPENDENCY = Rule("FBZZ2010", "Authoring callback requires isolated settings",
        "'{0}' cannot depend on '{1}'; authoring uses settings/constants/pure math, draw callbacks additionally allow read-only native state and Gizmos, with no runtime cache, mutable static state or external I/O", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor COMPONENT_REQUIREMENT = Rule("FBZZ2011", "Invalid Component requirement",
        "'{0}' requires at least one supported native Component or same-assembly Script type; RequireComponent/ExecutionOrder are Script-only", DiagnosticSeverity.Error);
    private static readonly DiagnosticDescriptor EDITOR_ACTION = Rule("FBZZ2012", "Invalid Editor action",
        "'{0}' requires a unique nonempty EditorAction ID and a synchronous zero-argument instance void Script method outside lifecycle names", DiagnosticSeverity.Error);

    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics =>
        [LIFECYCLE, ASYNC_CALLBACK, POSSIBLE_TYPO, UNSUPPORTED_FIELD, FIELD_CONFLICT, REQUIRED_REFERENCE, FIELD_RANGE, UNSAVED_FIELD, SCRIPT_TYPE, AUTHORING_DEPENDENCY, COMPONENT_REQUIREMENT, EDITOR_ACTION, ASSET_SCENE_REFERENCE];
    private static readonly DiagnosticDescriptor ASSET_SCENE_REFERENCE = Rule("FBZZ2013", "DataAsset must be portable",
        "'{0}' cannot persist GameObject/Component/Script references in a DataAsset; use asset GUIDs or ordinary saved values, and keep Scene references in private runtime caches", DiagnosticSeverity.Error);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterCompilationStartAction(start =>
        {
            Dictionary<string, Lifecycle> callbacks = ReadLifecycles(start.Compilation);
            start.RegisterSymbolAction(symbol => AnalyzeMethod(symbol, callbacks), SymbolKind.Method);
            start.RegisterSymbolAction(AnalyzeField, SymbolKind.Field);
            start.RegisterSymbolAction(AnalyzeType, SymbolKind.NamedType);
            start.RegisterOperationAction(AnalyzeInvocation, OperationKind.Invocation);
            start.RegisterOperationAction(AnalyzeAuthoringDependency, OperationKind.FieldReference, OperationKind.PropertyReference, OperationKind.Invocation);
        });
    }

    private static DiagnosticDescriptor Rule(string id, string title, string message, DiagnosticSeverity severity)
        => new(id, title, message, "FBZZ", severity, isEnabledByDefault: true);

    /// @note Read generated SDK metadata rather than duplicating lifecycle names or signatures in the analyzer.
    private static Dictionary<string, Lifecycle> ReadLifecycles(Compilation compilation)
    {
        Dictionary<string, Lifecycle> callbacks = new(StringComparer.Ordinal);
        INamedTypeSymbol? enumeration = compilation.GetTypeByMetadataName("Fbzz.ScriptCallback");
        if (enumeration?.ContainingAssembly.Name != "Fbzz.ScriptApi") return callbacks;
        foreach (IFieldSymbol field in enumeration.GetMembers().OfType<IFieldSymbol>())
            foreach (AttributeData attribute in field.GetAttributes())
                if (SdkAttribute(attribute, "Fbzz.ScriptLifecycleAttribute") && attribute.ConstructorArguments.Length == 3
                    && attribute.ConstructorArguments[0].Value is string name)
                    callbacks.Add(name, new(attribute.ConstructorArguments[1].Value as string));
        return callbacks;
    }

    private static void AnalyzeMethod(SymbolAnalysisContext context, IReadOnlyDictionary<string, Lifecycle> callbacks)
    {
        IMethodSymbol method = (IMethodSymbol)context.Symbol;
        if (method.MethodKind != MethodKind.Ordinary) return;
        AttributeData? action = EditorAction(method);
        if (action is not null && (!ScriptSourceGenerator.IsScript(method.ContainingType) || method.IsStatic || method.IsGenericMethod
            || method.IsAsync || !method.ReturnsVoid || method.Parameters.Length != 0 || callbacks.ContainsKey(method.Name)
            || action.ConstructorArguments.Length == 0 || action.ConstructorArguments[0].Value is not string key || string.IsNullOrWhiteSpace(key)
            || key.Contains('\0') || method.ContainingType.GetMembers().OfType<IMethodSymbol>().Any(other => !SymbolEqualityComparer.Default.Equals(other, method)
                && other.GetAttributes().Any(metadata => SdkAttribute(metadata, "Fbzz.EditorActionAttribute") && metadata.ConstructorArguments[0].Value is string otherKey && otherKey == key))))
            context.ReportDiagnostic(Diagnostic.Create(EDITOR_ACTION, action.ApplicationSyntaxReference?.GetSyntax(context.CancellationToken).GetLocation() ?? FirstLocation(method), method.Name));
        if (ScriptSourceGenerator.IsDataAsset(method.ContainingType)) {
            if (method.Name is not ("Reset" or "OnValidate")) return;
            IMethodSymbol root = method;
            while (root.OverriddenMethod is IMethodSymbol parent) root = parent;
            if (!method.IsOverride || method.DeclaredAccessibility != Accessibility.Protected || method.IsStatic || method.IsGenericMethod
                || method.IsAsync || !method.ReturnsVoid || method.Parameters.Length != 0 || root.ContainingType.ToDisplayString() != "Fbzz.DataAsset")
                context.ReportDiagnostic(Diagnostic.Create(LIFECYCLE, FirstLocation(method), method.Name, "no parameters"));
            return;
        }
        if (!ScriptSourceGenerator.IsScript(method.ContainingType)) return;
        if (callbacks.TryGetValue(method.Name, out Lifecycle? callback))
        {
            IMethodSymbol root = method;
            while (root.OverriddenMethod is IMethodSymbol parent) root = parent;
            bool parameters = callback.Parameter is null ? method.Parameters.Length == 0
                : method.Parameters.Length == 1 && method.Parameters[0].RefKind == RefKind.None && !method.Parameters[0].IsOptional
                    && (callback.Parameter == "System.String" ? method.Parameters[0].Type.SpecialType == SpecialType.System_String
                        : method.Parameters[0].Type.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat).TrimEnd('?') == callback.Parameter);
            bool valid = method.IsOverride && method.DeclaredAccessibility == Accessibility.Protected && !method.IsStatic && !method.IsGenericMethod && !method.IsAsync && method.ReturnsVoid
                && root.ContainingType.ToDisplayString() == "Fbzz.Script" && root.ContainingAssembly.Name == "Fbzz.ScriptApi" && parameters;
            if (!valid)
                context.ReportDiagnostic(Diagnostic.Create(LIFECYCLE, FirstLocation(method), method.Name,
                    callback.Parameter is null ? "no parameters" : "one " + callback.Parameter + " parameter passed by value"));
            return;
        }
        if (method.IsOverride || method.IsStatic || method.IsGenericMethod || !method.ReturnsVoid || method.Parameters.Length != 0
            || method.DeclaredAccessibility == Accessibility.Public || method.Name.Length < 5) return;
        string? closest = callbacks.Keys.FirstOrDefault(name => OneEditApart(method.Name, name));
        if (closest is not null) context.ReportDiagnostic(Diagnostic.Create(POSSIBLE_TYPO, FirstLocation(method), method.Name, closest));
    }

    private static void AnalyzeType(SymbolAnalysisContext context)
    {
        INamedTypeSymbol type = (INamedTypeSymbol)context.Symbol;
        foreach (AttributeData attribute in type.GetAttributes()) {
            bool required = SdkAttribute(attribute, "Fbzz.RequireComponentAttribute");
            bool order = SdkAttribute(attribute, "Fbzz.ExecutionOrderAttribute");
            if (!required && !order) continue;
            bool valid = ScriptSourceGenerator.IsScript(type);
            if (required) {
                valid = valid && attribute.ConstructorArguments.Length == 1
                    && attribute.ConstructorArguments[0].Kind == TypedConstantKind.Array
                    && !attribute.ConstructorArguments[0].IsNull && attribute.ConstructorArguments[0].Values.Length != 0;
                if (valid) foreach (TypedConstant value in attribute.ConstructorArguments[0].Values)
                    valid = valid && value.Value is INamedTypeSymbol target && !target.IsUnboundGenericType
                        && !target.TypeArguments.Any(static argument => argument.TypeKind == TypeKind.TypeParameter)
                        && ((ScriptSourceGenerator.IsScript(target) && SymbolEqualityComparer.Default.Equals(target.ContainingAssembly, type.ContainingAssembly))
                            || IsSdkNativeFacade(target));
            }
            if (!valid) context.ReportDiagnostic(Diagnostic.Create(COMPONENT_REQUIREMENT,
                attribute.ApplicationSyntaxReference?.GetSyntax(context.CancellationToken).GetLocation() ?? FirstLocation(type), type.Name));
        }
        if (type.IsAbstract || (!ScriptSourceGenerator.IsScript(type) && !ScriptSourceGenerator.IsDataAsset(type))) return;
        if (type.ContainingType is not null || type.IsGenericType || !type.InstanceConstructors.Any(static constructor => constructor.Parameters.Length == 0))
            context.ReportDiagnostic(Diagnostic.Create(SCRIPT_TYPE, FirstLocation(type), type.Name));
    }

    /// @note native facade の継承境界をSDK assemblyで確認し、個別型名の古い登録表を持たない。Hostはfactoryのexact kindも検証する。
    private static bool IsSdkNativeFacade(INamedTypeSymbol type)
    {
        if (type.ContainingAssembly.Name != "Fbzz.ScriptApi" || type.ContainingNamespace.ToDisplayString() != "Fbzz" || type.IsAbstract) return false;
        if (type.Name is "Transform" or "RigidBody") return true;
        for (INamedTypeSymbol? parent = type.BaseType; parent is not null; parent = parent.BaseType)
            if (parent.ContainingAssembly.Name == "Fbzz.ScriptApi" && parent.ToDisplayString() == "Fbzz.NativeComponent") return true;
        return false;
    }

    private static void AnalyzeField(SymbolAnalysisContext context)
    {
        IFieldSymbol field = (IFieldSymbol)context.Symbol;
        if (field.IsImplicitlyDeclared || (!ScriptSourceGenerator.IsScript(field.ContainingType) && !ScriptSourceGenerator.IsDataAsset(field.ContainingType))) return;
        bool setting = HasAttribute(field, "Fbzz.SettingAttribute"), reload = HasAttribute(field, "Fbzz.KeepOnReloadAttribute");
        bool nonSerialized = HasAttribute(field, "System.NonSerializedAttribute"), mutable = !field.IsStatic && !field.IsConst && !field.IsReadOnly;
        bool supported = SupportsField(field.Type), saved = mutable && !nonSerialized && supported && (setting || field.DeclaredAccessibility == Accessibility.Public);
        void Report(DiagnosticDescriptor rule) => context.ReportDiagnostic(Diagnostic.Create(rule, FirstLocation(field), field.Name));
        if ((setting || reload) && (!mutable || !supported)) Report(UNSUPPORTED_FIELD);
        if (ScriptSourceGenerator.IsDataAsset(field.ContainingType) && saved && ContainsSceneReference(field.Type, new(SymbolEqualityComparer.Default))) Report(ASSET_SCENE_REFERENCE);
        if (setting && nonSerialized) Report(FIELD_CONFLICT);
        if (HasAttribute(field, "Fbzz.RequiredReferenceAttribute") && (!saved || !IsReferenceType(field.Type))) Report(REQUIRED_REFERENCE);
        AttributeData? range = field.GetAttributes().FirstOrDefault(attribute => SdkAttribute(attribute, "Fbzz.ValueRangeAttribute"));
        if (range is not null)
        {
            ITypeSymbol scalar = field.Type is IArrayTypeSymbol array ? array.ElementType
                : field.Type is INamedTypeSymbol list && list.OriginalDefinition.ToDisplayString() == "System.Collections.Generic.List<T>" ? list.TypeArguments[0] : field.Type;
            if (scalar is INamedTypeSymbol { TypeKind: TypeKind.Enum } enumeration) scalar = enumeration.EnumUnderlyingType!;
            bool numeric = scalar.SpecialType is SpecialType.System_SByte or SpecialType.System_Byte or SpecialType.System_Int16 or SpecialType.System_UInt16
                or SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Int64 or SpecialType.System_UInt64
                or SpecialType.System_Single or SpecialType.System_Double or SpecialType.System_Decimal or SpecialType.System_Char;
            if (!numeric || !OrderedRange(range) || scalar.SpecialType == SpecialType.System_Decimal && range.ConstructorArguments[0].Value is not string)
                Report(FIELD_RANGE);
        }
        if (field.DeclaredAccessibility == Accessibility.Public && mutable && !nonSerialized && !supported) Report(UNSAVED_FIELD);
    }

    static bool OrderedRange(AttributeData range)
    {
        if (range.ConstructorArguments.Length != 2) return false;
        object? minimum = range.ConstructorArguments[0].Value, maximum = range.ConstructorArguments[1].Value;
        if (minimum is null || maximum is null || minimum.GetType() != maximum.GetType()) return false;
        if (minimum is float a && (!float.IsFinite(a) || !float.IsFinite((float)maximum))
            || minimum is double b && (!double.IsFinite(b) || !double.IsFinite((double)maximum))) return false;
        if (minimum is not string left) return minimum is IComparable number && number.CompareTo(maximum) <= 0;
        static bool Parse(string text, out BigInteger number, out BigInteger scale)
        {
            int dot = text.IndexOf('.'); scale = BigInteger.Pow(10, dot < 0 ? 0 : text.Length - dot - 1);
            string digits = dot < 0 ? text : text.Remove(dot, 1);
            return BigInteger.TryParse(digits, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out number);
        }
        return Parse(left, out BigInteger first, out BigInteger firstScale) && Parse((string)maximum, out BigInteger last, out BigInteger lastScale)
            && first * lastScale <= last * firstScale;
    }

    private static bool ContainsSceneReference(ITypeSymbol type, HashSet<ITypeSymbol> visited)
    {
        if (type is IArrayTypeSymbol array) return ContainsSceneReference(array.ElementType, visited);
        if (type.ToDisplayString().TrimEnd('?') == "Fbzz.GameObject") return true;
        if (type is not INamedTypeSymbol named) return false;
        if (named.OriginalDefinition.ToDisplayString() == "Fbzz.ComponentRef<T>") return true;
        for (INamedTypeSymbol? parent = named; parent is not null; parent = parent.BaseType)
            if (parent.ToDisplayString() == "Fbzz.Component") return true;
        if (named.OriginalDefinition.ToDisplayString() == "System.Collections.Generic.List<T>") return ContainsSceneReference(named.TypeArguments[0], visited);
        if (ScriptFieldContract.SupportsScalar(type) || named.OriginalDefinition.ToDisplayString() == "Fbzz.DataAssetRef<T>" || !visited.Add(type)) return false;
        return named.GetMembers().OfType<IFieldSymbol>().Any(field => !field.IsStatic && !field.IsReadOnly && !field.IsConst
            && !HasAttribute(field, "System.NonSerializedAttribute") && (field.DeclaredAccessibility == Accessibility.Public || HasAttribute(field, "Fbzz.SettingAttribute"))
            && ContainsSceneReference(field.Type, visited));
    }
    private static bool IsReferenceType(ITypeSymbol type)
    {
        if (type is IArrayTypeSymbol array) return array.IsSZArray && IsReferenceType(array.ElementType);
        if (type is INamedTypeSymbol named && named.OriginalDefinition.ToDisplayString() == "System.Collections.Generic.List<T>")
            return IsReferenceType(named.TypeArguments[0]);
        if (type is INamedTypeSymbol wrapper && wrapper.ContainingAssembly.Name == "Fbzz.ScriptApi"
            && wrapper.OriginalDefinition.ToDisplayString() == "Fbzz.ComponentRef<T>") return true;
        return type.ToDisplayString().TrimEnd('?') == "Fbzz.GameObject";
    }

    private static void AnalyzeInvocation(OperationAnalysisContext context)
    {
        IInvocationOperation invocation = (IInvocationOperation)context.Operation;
        foreach (IArgumentOperation argument in invocation.Arguments)
            if (argument.Parameter is IParameterSymbol parameter && parameter.GetAttributes().Any(attribute => SdkAttribute(attribute, "Fbzz.SynchronousCallbackAttribute"))
                && IsAsyncDelegate(argument.Value, context.Compilation, new HashSet<ISymbol>(SymbolEqualityComparer.Default)))
                context.ReportDiagnostic(Diagnostic.Create(ASYNC_CALLBACK, argument.Syntax.GetLocation(), parameter.Name));
    }

    /// @note These direct dependency diagnostics do not sandbox arbitrary helper code or .NET side effects.
    private static void AnalyzeAuthoringDependency(OperationAnalysisContext context)
    {
        if (context.ContainingSymbol is not IMethodSymbol method || (method.Name is not ("Reset" or "OnValidate" or "DrawGizmos" or "DrawGizmosSelected")
            && EditorAction(method) is null)
            || (!ScriptSourceGenerator.IsScript(method.ContainingType) && !ScriptSourceGenerator.IsDataAsset(method.ContainingType))) return;
        ISymbol? dependency = context.Operation switch {
            IFieldReferenceOperation field => field.Field,
            IPropertyReferenceOperation property => property.Property,
            IInvocationOperation invocation => invocation.TargetMethod,
            _ => null
        };
        if (dependency is null) return;
        bool drawing = method.Name is "DrawGizmos" or "DrawGizmosSelected";
        bool forbidden = false;
        if (dependency is IFieldSymbol fieldSymbol) {
            bool constant = fieldSymbol.IsConst || (fieldSymbol.IsStatic && fieldSymbol.IsReadOnly
                && (fieldSymbol.Type.IsValueType || fieldSymbol.Type.SpecialType == SpecialType.System_String));
            bool setting = !fieldSymbol.IsStatic && !fieldSymbol.IsReadOnly && !HasAttribute(fieldSymbol, "System.NonSerializedAttribute")
                && SupportsField(fieldSymbol.Type) && (fieldSymbol.DeclaredAccessibility == Accessibility.Public || HasAttribute(fieldSymbol, "Fbzz.SettingAttribute"));
            forbidden = !constant && !setting;
        } else {
            string type = dependency.ContainingType?.OriginalDefinition.ToDisplayString() ?? "";
            string ns = dependency.ContainingNamespace?.ToDisplayString() ?? "";
            bool pureSdk = type is "Fbzz.Vector2" or "Fbzz.Vector3" or "Fbzz.Vector4" or "Fbzz.Quaternion" or "Fbzz.Mathf"
                || type.StartsWith("Fbzz.DataAssetRef<", StringComparison.Ordinal)
                || dependency.GetAttributes().Any(attribute => SdkAttribute(attribute, "Fbzz.ApiContractAttribute")
                    && attribute.NamedArguments.Any(argument => argument.Key == "Purity" && argument.Value.Value is int purity && purity == 1));
            if (drawing && dependency.ContainingAssembly?.Name == "Fbzz.ScriptApi") {
                bool drawService = type == "Fbzz.ScriptGizmos" || dependency.Name == "Gizmos";
                bool writeTarget = context.Operation.Parent is IAssignmentOperation assignment && ReferenceEquals(assignment.Target, context.Operation)
                    || context.Operation.Parent is IIncrementOrDecrementOperation increment && ReferenceEquals(increment.Target, context.Operation);
                bool read = dependency is IPropertySymbol && !writeTarget
                    && dependency.Name is not ("Jobs" or "Events" or "DataAssets");
                if (dependency is IMethodSymbol target) {
                    AttributeData? contract = target.GetAttributes().FirstOrDefault(attribute => SdkAttribute(attribute, "Fbzz.ApiContractAttribute"));
                    read = contract?.NamedArguments.Any(argument => argument.Key == "Call" && argument.Value.Value is "read" or "query") == true
                        && !(target.IsGenericMethod && target.TypeArguments.Any(argument => argument is INamedTypeSymbol named && ScriptSourceGenerator.IsScript(named)));
                }
                pureSdk = pureSdk || drawService || read;
            }
            forbidden = dependency.ContainingAssembly?.Name == "Fbzz.ScriptApi" && !pureSdk
                || ns.StartsWith("System.IO", StringComparison.Ordinal) || ns.StartsWith("System.Net", StringComparison.Ordinal)
                || ns.StartsWith("System.Threading", StringComparison.Ordinal) || type == "System.Diagnostics.Process";
        }
        if (forbidden) context.ReportDiagnostic(Diagnostic.Create(AUTHORING_DEPENDENCY, context.Operation.Syntax.GetLocation(), method.Name, dependency.ToDisplayString()));
    }

    private static bool IsAsyncDelegate(IOperation operation, Compilation compilation, HashSet<ISymbol> visited)
    {
        if (operation is IConversionOperation conversion) return IsAsyncDelegate(conversion.Operand, compilation, visited);
        if (operation is IDelegateCreationOperation creation) return IsAsyncDelegate(creation.Target, compilation, visited);
        if (operation is IAnonymousFunctionOperation function) return function.Symbol.IsAsync;
        if (operation is IMethodReferenceOperation reference) return reference.Method.IsAsync
            || reference.Method.GetAttributes().Any(static attribute => attribute.AttributeClass?.ToDisplayString() == "System.Runtime.CompilerServices.AsyncStateMachineAttribute");
        if (operation is IConditionalOperation conditional)
            return IsAsyncDelegate(conditional.WhenTrue, compilation, visited) || (conditional.WhenFalse is not null && IsAsyncDelegate(conditional.WhenFalse, compilation, visited));
        if (operation is IBinaryOperation binary)
            return IsAsyncDelegate(binary.LeftOperand, compilation, visited) || IsAsyncDelegate(binary.RightOperand, compilation, visited);
        if (operation is ILocalReferenceOperation local && visited.Add(local.Local))
            foreach (SyntaxReference declaration in local.Local.DeclaringSyntaxReferences)
                if (declaration.GetSyntax() is VariableDeclaratorSyntax { Initializer.Value: var value }
                    && compilation.GetSemanticModel(value.SyntaxTree).GetOperation(value) is IOperation initial)
                    return IsAsyncDelegate(initial, compilation, visited);
        return false;
    }

    private static bool SdkAttribute(AttributeData attribute, string name)
        => attribute.AttributeClass?.ToDisplayString() == name && attribute.AttributeClass.ContainingAssembly.Name == "Fbzz.ScriptApi";
    private static AttributeData? EditorAction(IMethodSymbol method)
    {
        for (IMethodSymbol? current = method; current is not null; current = current.OverriddenMethod) {
            AttributeData? action = current.GetAttributes().FirstOrDefault(attribute => SdkAttribute(attribute, "Fbzz.EditorActionAttribute"));
            if (action is not null) return action;
        }
        return null;
    }
    private static Location FirstLocation(ISymbol symbol) => symbol.Locations.FirstOrDefault(static location => location.IsInSource) ?? Location.None;
    private sealed record Lifecycle(string? Parameter);

    private static bool OneEditApart(string left, string right)
    {
        if (Math.Abs(left.Length - right.Length) > 1 || left == right) return false;
        int a = 0, b = 0, differences = 0;
        while (a < left.Length && b < right.Length)
        {
            if (left[a] == right[b]) { a++; b++; continue; }
            if (++differences > 1) return false;
            if (left.Length >= right.Length) a++;
            if (right.Length >= left.Length) b++;
        }
        return differences + (left.Length - a) + (right.Length - b) == 1;
    }
}
