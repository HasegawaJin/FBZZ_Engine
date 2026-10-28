/// @file    ScriptSourceGenerator.cs
/// @brief   Script の全宣言位置とコンパイル対象の checksum を assembly 属性へ記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Fbzz.ScriptGenerator;

/// @note SDK と同じ .NET / Roslyn host で実行し、ゲームの型や constructor は実行しない。
/// @see https://github.com/dotnet/roslyn/blob/main/docs/features/incremental-generators.md Incremental generators
[Generator(LanguageNames.CSharp)]
public sealed class ScriptSourceGenerator : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor INVALID_SOURCE = new(
        "FBZZ1001", "Script source metadata cannot be generated", "{0}",
        "FBZZ", DiagnosticSeverity.Error, isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<INamedTypeSymbol?> declarations = context.SyntaxProvider.CreateSyntaxProvider(
            static (node, _) => node is ClassDeclarationSyntax { BaseList: not null },
            static (syntax, cancellation) => syntax.SemanticModel.GetDeclaredSymbol(
                (ClassDeclarationSyntax)syntax.Node, cancellation));
        IncrementalValueProvider<string?> projectDirectory = context.AnalyzerConfigOptionsProvider.Select(
            static (options, _) => options.GlobalOptions.TryGetValue(
                "build_property.MSBuildProjectDirectory", out string? directory) ? directory : null);
        context.RegisterSourceOutput(declarations.Collect().Combine(projectDirectory),
            static (production, input) => Emit(production, input.Left, input.Right));
    }

    private static void Emit(SourceProductionContext context, ImmutableArray<INamedTypeSymbol?> declarations,
        string? projectDirectory)
    {
        if (string.IsNullOrEmpty(projectDirectory))
        {
            context.ReportDiagnostic(Diagnostic.Create(INVALID_SOURCE, Location.None,
                "Expose MSBuildProjectDirectory using CompilerVisibleProperty in the Scripts project."));
            return;
        }
        HashSet<INamedTypeSymbol> unique = new(SymbolEqualityComparer.Default);
        foreach (INamedTypeSymbol? declaration in declarations)
        {
            if (declaration is not null && !declaration.IsAbstract && declaration.TypeParameters.Length == 0 &&
                declaration.ContainingType is null && (IsScript(declaration) || IsDataAsset(declaration))) unique.Add(declaration);
        }
        StringBuilder output = new();
        output.AppendLine("/// @file    ScriptSources.g.cs");
        output.AppendLine("/// @brief   コンパイル済み Script と宣言元の対応。");
        output.AppendLine("/// @author  Hasegawa Jin");
        output.AppendLine("/// @date    2026-10-04");
        foreach (INamedTypeSymbol type in unique.OrderBy(static value => ClrTypeName(value), StringComparer.Ordinal))
        {
            string clrName = ClrTypeName(type);
            HashSet<(string Path, int Line, string Hash)> emitted = new();
            foreach (SyntaxReference declaration in type.DeclaringSyntaxReferences.OrderBy(
                static value => value.SyntaxTree.FilePath, StringComparer.Ordinal).ThenBy(static value => value.Span.Start))
            {
                SyntaxTree tree = declaration.SyntaxTree;
                SourceText text = tree.GetText(context.CancellationToken);
                string relative = Path.GetRelativePath(projectDirectory, tree.FilePath).Replace('\\', '/');
                if (relative.StartsWith("../", StringComparison.Ordinal) || Path.IsPathRooted(relative) ||
                    !relative.EndsWith(".cs", StringComparison.OrdinalIgnoreCase) ||
                    text.ChecksumAlgorithm != SourceHashAlgorithm.Sha256)
                {
                    context.ReportDiagnostic(Diagnostic.Create(INVALID_SOURCE, tree.GetLocation(declaration.Span),
                        "Script declarations must be project-relative *.cs sources with ChecksumAlgorithm=SHA256: " + relative));
                    continue;
                }
                /// @note SourceText の checksum はコンパイラが読んだ元のバイト列を対象とし、BOM も含む。
                /// @see https://learn.microsoft.com/dotnet/api/microsoft.codeanalysis.text.sourcetext.getchecksum SourceText.GetChecksum
                string hash = Convert.ToHexString(text.GetChecksum().AsSpan()).ToLowerInvariant();
                int line = tree.GetLineSpan(declaration.Span, context.CancellationToken).StartLinePosition.Line + 1;
                /// @note 属性に column は無いため、同じ行の合法 partial 宣言は一つの位置として記録する。
                if (!emitted.Add((relative, line, hash))) continue;
                output.Append("[assembly: global::Fbzz.ScriptSourceAttribute(")
                    .Append(SymbolDisplay.FormatLiteral(clrName, quote: true)).Append(", ")
                    .Append(SymbolDisplay.FormatLiteral(relative, quote: true)).Append(", ")
                    .Append(line).Append(", ")
                    .Append(SymbolDisplay.FormatLiteral(hash, quote: true)).AppendLine(")]");
            }
        }
        context.AddSource("ScriptSources.g.cs", SourceText.From(output.ToString(), Encoding.UTF8));
    }

    internal static bool IsScript(INamedTypeSymbol type)
    {
        for (INamedTypeSymbol? parent = type.BaseType; parent is not null; parent = parent.BaseType)
        {
            if (parent.Name == "Script" && parent.ContainingNamespace.ToDisplayString() == "Fbzz" &&
                parent.ContainingAssembly.Name == "Fbzz.ScriptApi") return true;
        }
        return false;
    }

    internal static bool IsDataAsset(INamedTypeSymbol type)
    {
        for (INamedTypeSymbol? parent = type.BaseType; parent is not null; parent = parent.BaseType)
            if (parent.Name == "DataAsset" && parent.ContainingNamespace.ToDisplayString() == "Fbzz"
                && parent.ContainingAssembly.Name == "Fbzz.ScriptApi") return true;
        return false;
    }

    private static string ClrTypeName(INamedTypeSymbol type)
    {
        Stack<string> parts = new();
        parts.Push(type.MetadataName);
        for (INamespaceSymbol? space = type.ContainingNamespace;
            space is not null && !space.IsGlobalNamespace; space = space.ContainingNamespace)
            parts.Push(space.Name);
        return string.Join(".", parts);
    }
}
