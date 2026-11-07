/// @file    Analysis.cs
/// @brief   Semantic C# diagnostics, symbol references and non-persistent rename plans.
/// @author  Hasegawa Jin
/// @date    2026-11-06
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Fbzz.EditorAnalysis;

/// @note No project, analyzer, generator or game assembly is executed; callers supply an immutable source snapshot.
/// @see https://learn.microsoft.com/dotnet/csharp/roslyn-sdk/compiler-api-model Roslyn compiler and semantic model.
public static class Analysis
{
    private static readonly JsonSerializerOptions OPTIONS = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    private sealed record Source(string File, string Content);
    private sealed record Position(string File, int Line, int Column, int Start, int Length, bool Declaration);
    private sealed record Edit(string File, int Start, int Length, string Before, string After);

    public static string Handle(string input)
    {
        using JsonDocument document = JsonDocument.Parse(input);
        JsonElement request = document.RootElement;
        string operation = request.GetProperty("operation").GetString() ?? "";
        if (operation is not ("diagnostics" or "symbols" or "references" or "rename"))
            return Serialize(new { ok = false, error = "UNKNOWN_OPERATION" });
        Source[] sources = request.GetProperty("sources").EnumerateArray().Select(source => new Source(
            source.GetProperty("file").GetString() ?? "", source.GetProperty("content").GetString() ?? "")).ToArray();
        if (sources.Length is < 1 or > 512 || sources.Select(source => source.File).Distinct(StringComparer.OrdinalIgnoreCase).Count() != sources.Length)
            return Serialize(new { ok = false, error = "INVALID_SOURCE_SET" });
        SyntaxTree[] trees = sources.Select(source => CSharpSyntaxTree.ParseText(source.Content,
            new CSharpParseOptions(LanguageVersion.CSharp14), source.File)).ToArray();
        List<MetadataReference> references = new();
        List<string> referenceErrors = new();
        HashSet<string> paths = new(StringComparer.OrdinalIgnoreCase);
        foreach (string path in ((string?)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            if (paths.Add(path)) references.Add(MetadataReference.CreateFromFile(path));
        }
        if (request.TryGetProperty("references", out JsonElement requestedReferences))
        {
            foreach (JsonElement reference in requestedReferences.EnumerateArray())
            {
                string path = reference.GetString() ?? "";
                if (!paths.Add(path)) continue;
                try { references.Add(MetadataReference.CreateFromFile(path)); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or BadImageFormatException or ArgumentException)
                { referenceErrors.Add(path + ": " + error.Message); }
            }
        }
        CSharpCompilation compilation = CSharpCompilation.Create("Fbzz.EditorSourceSnapshot", trees, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true, nullableContextOptions: NullableContextOptions.Enable));
        int limit = request.TryGetProperty("limit", out JsonElement requestedLimit) ? Math.Clamp(requestedLimit.GetInt32(), 1, 1000) : 250;
        Diagnostic[] diagnostics = compilation.GetDiagnostics().Where(diagnostic => diagnostic.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning).ToArray();
        object metadata = new
        {
            mode = "source-snapshot", languageVersion = "CSharp14", framework = "net10.0",
            sourceFiles = sources.Select(source => source.File), referenceErrors,
            projectEvaluated = false, analyzersExecuted = false, generatorsExecuted = false,
            warning = "Assets/Scripts source snapshot with framework and supplied metadata references; this does not replace the managed project build."
        };
        if (operation == "diagnostics") return Serialize(new
        {
            ok = true, metadata, diagnostics = diagnostics.Take(limit).Select(DiagnosticValue), total = diagnostics.Length,
            errorCount = diagnostics.Count(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error), truncated = diagnostics.Length > limit
        });
        if (operation == "symbols")
        {
            string filter = request.TryGetProperty("filter", out JsonElement requestedFilter) ? requestedFilter.GetString() ?? "" : "";
            List<object> values = new();
            foreach (SyntaxTree tree in trees)
            {
                SemanticModel model = compilation.GetSemanticModel(tree);
                foreach (SyntaxToken token in tree.GetRoot().DescendantTokens().Where(token => token.IsKind(SyntaxKind.IdentifierToken)))
                {
                    ISymbol? symbol = DeclarationSymbol(model, token);
                    if (symbol is null || !symbol.Name.Contains(filter, StringComparison.OrdinalIgnoreCase)) continue;
                    values.Add(new { name = symbol.Name, kind = symbol.Kind.ToString(), display = symbol.ToDisplayString(), location = Locate(tree, token.Span, true), potentiallyPersisted = IsPersisted(symbol) });
                }
            }
            return Serialize(new { ok = true, metadata, symbols = values.Take(limit), total = values.Count, truncated = values.Count > limit });
        }
        string file = request.GetProperty("file").GetString() ?? "";
        SyntaxTree? selected = trees.FirstOrDefault(tree => string.Equals(tree.FilePath, file, StringComparison.OrdinalIgnoreCase));
        if (selected is null) return Serialize(new { ok = false, error = "SOURCE_NOT_IN_SNAPSHOT" });
        SourceText text = selected.GetText();
        int line = request.GetProperty("line").GetInt32() - 1;
        int column = request.GetProperty("column").GetInt32() - 1;
        if (line < 0 || line >= text.Lines.Count || column < 0 || column >= text.Lines[line].Span.Length)
            return Serialize(new { ok = false, error = "POSITION_OUT_OF_RANGE" });
        SyntaxToken selectedToken = selected.GetRoot().FindToken(text.Lines[line].Start + column);
        ISymbol? selectedSymbol = ResolveSymbol(compilation.GetSemanticModel(selected), selectedToken);
        if (selectedSymbol is null || !selectedToken.IsKind(SyntaxKind.IdentifierToken) || selectedSymbol.DeclaringSyntaxReferences.Length == 0)
            return Serialize(new { ok = false, error = "SOURCE_SYMBOL_NOT_FOUND" });
        selectedSymbol = Normalize(selectedSymbol);
        List<Position> positions = new();
        foreach (SyntaxTree tree in trees)
        {
            SemanticModel model = compilation.GetSemanticModel(tree);
            foreach (SyntaxToken token in tree.GetRoot().DescendantTokens().Where(token => token.IsKind(SyntaxKind.IdentifierToken) && token.ValueText == selectedSymbol.Name))
            {
                ISymbol? symbol = ResolveSymbol(model, token);
                if (symbol is not null && SymbolEqualityComparer.Default.Equals(Normalize(symbol), selectedSymbol))
                    positions.Add(Locate(tree, token.Span, DeclarationSymbol(model, token) is not null));
            }
        }
        object symbolValue = new { name = selectedSymbol.Name, kind = selectedSymbol.Kind.ToString(), display = selectedSymbol.ToDisplayString(), potentiallyPersisted = IsPersisted(selectedSymbol) };
        if (operation == "references") return Serialize(new { ok = true, metadata, symbol = symbolValue, references = positions.Take(limit), total = positions.Count, truncated = positions.Count > limit });

        string newName = request.GetProperty("newName").GetString() ?? "";
        List<string> blockers = new();
        if (!SyntaxFacts.IsValidIdentifier(newName) || SyntaxFacts.GetKeywordKind(newName) != SyntaxKind.None || newName.StartsWith('@')) blockers.Add("INVALID_IDENTIFIER");
        if (newName == selectedSymbol.Name) blockers.Add("UNCHANGED_NAME");
        if (IsPersisted(selectedSymbol)) blockers.Add("PERSISTED_SYMBOL_REQUIRES_FBZZ_MIGRATION");
        if (selectedSymbol is IMethodSymbol method && (method.IsOverride || method.IsVirtual || method.IsAbstract || method.ExplicitInterfaceImplementations.Length != 0)) blockers.Add("DISPATCH_CONTRACT_RENAME_UNSUPPORTED");
        if (selectedSymbol is IMethodSymbol && selectedSymbol.ContainingType?.GetMembers(selectedSymbol.Name).OfType<IMethodSymbol>().Count() > 1) blockers.Add("OVERLOADED_METHOD_RENAME_UNSUPPORTED");
        if (selectedSymbol.DeclaredAccessibility is Accessibility.Public or Accessibility.Protected or Accessibility.ProtectedOrInternal || selectedSymbol.ContainingType?.TypeKind == TypeKind.Interface)
            blockers.Add("EXTERNALLY_VISIBLE_SYMBOL_RENAME_UNSUPPORTED");
        if (selectedSymbol.Kind is not (SymbolKind.Local or SymbolKind.Parameter or SymbolKind.Field or SymbolKind.Method or SymbolKind.Property or SymbolKind.NamedType or SymbolKind.TypeParameter or SymbolKind.Event)) blockers.Add("SYMBOL_KIND_RENAME_UNSUPPORTED");
        if (diagnostics.Any(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error) || referenceErrors.Count != 0) blockers.Add("SNAPSHOT_HAS_COMPILATION_ERRORS");
        if (selectedSymbol.ContainingType?.GetMembers(newName).Any(member => !SymbolEqualityComparer.Default.Equals(member, selectedSymbol)) == true) blockers.Add("NAME_ALREADY_EXISTS");
        List<Edit> edits = positions.Select(position => new Edit(position.File, position.Start, position.Length,
            sources.First(source => source.File == position.File).Content.Substring(position.Start, position.Length), newName)).ToList();
        if (blockers.Count == 0)
        {
            SyntaxTree[] changedTrees = sources.Select(source =>
            {
                string changed = source.Content;
                foreach (Edit edit in edits.Where(edit => edit.File == source.File).OrderByDescending(edit => edit.Start))
                    changed = changed.Remove(edit.Start, edit.Length).Insert(edit.Start, edit.After);
                return CSharpSyntaxTree.ParseText(changed, new CSharpParseOptions(LanguageVersion.CSharp14), source.File);
            }).ToArray();
            CSharpCompilation changed = compilation.RemoveAllSyntaxTrees().AddSyntaxTrees(changedTrees);
            if (changed.GetDiagnostics().Any(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)) blockers.Add("RENAMED_SNAPSHOT_HAS_COMPILATION_ERRORS");
            else if (!BindingsUnchanged(compilation, changed)) blockers.Add("RENAMED_SNAPSHOT_CHANGES_OTHER_BINDINGS");
        }
        return Serialize(new { ok = true, metadata, symbol = symbolValue, blocked = blockers.Count != 0, blockers, edits = blockers.Count == 0 ? edits : new List<Edit>(), totalReferences = positions.Count });
    }

    private static string Serialize(object value) => JsonSerializer.Serialize(value, OPTIONS);
    private static object DiagnosticValue(Diagnostic diagnostic)
    {
        FileLinePositionSpan location = diagnostic.Location.GetLineSpan();
        return new { code = diagnostic.Id, severity = diagnostic.Severity.ToString().ToLowerInvariant(), message = diagnostic.GetMessage(), file = location.Path,
            line = location.StartLinePosition.Line + 1, column = location.StartLinePosition.Character + 1 };
    }
    private static Position Locate(SyntaxTree tree, TextSpan span, bool declaration)
    {
        LinePosition position = tree.GetText().Lines.GetLinePosition(span.Start);
        return new Position(tree.FilePath, position.Line + 1, position.Character + 1, span.Start, span.Length, declaration);
    }
    private static ISymbol Normalize(ISymbol symbol)
    {
        if (symbol is IMethodSymbol method)
        {
            if (method.MethodKind is MethodKind.Constructor or MethodKind.StaticConstructor or MethodKind.Destructor) return method.ContainingType.OriginalDefinition;
            return (method.PartialDefinitionPart ?? method.ReducedFrom ?? method).OriginalDefinition;
        }
        return symbol.OriginalDefinition;
    }
    private static ISymbol? ResolveSymbol(SemanticModel model, SyntaxToken token)
    {
        ISymbol? declaration = DeclarationSymbol(model, token);
        if (declaration is not null) return declaration;
        if (token.Parent is not IdentifierNameSyntax and not GenericNameSyntax) return null;
        SymbolInfo info = model.GetSymbolInfo(token.Parent);
        return info.Symbol ?? (info.CandidateSymbols.Length == 1 ? info.CandidateSymbols[0] : null);
    }
    private static ISymbol? DeclarationSymbol(SemanticModel model, SyntaxToken token)
    {
        SyntaxNode? node = token.Parent;
        bool matches = node switch
        {
            VariableDeclaratorSyntax value => value.Identifier == token,
            ParameterSyntax value => value.Identifier == token,
            TypeParameterSyntax value => value.Identifier == token,
            BaseTypeDeclarationSyntax value => value.Identifier == token,
            DelegateDeclarationSyntax value => value.Identifier == token,
            MethodDeclarationSyntax value => value.Identifier == token,
            ConstructorDeclarationSyntax value => value.Identifier == token,
            DestructorDeclarationSyntax value => value.Identifier == token,
            PropertyDeclarationSyntax value => value.Identifier == token,
            EventDeclarationSyntax value => value.Identifier == token,
            LocalFunctionStatementSyntax value => value.Identifier == token,
            _ => false
        };
        return matches ? model.GetDeclaredSymbol(node!) : null;
    }
    private static bool IsPersisted(ISymbol symbol)
    {
        if (symbol is IFieldSymbol field && !field.IsStatic && !field.IsConst && (field.DeclaredAccessibility == Accessibility.Public
            || field.GetAttributes().Any(attribute => attribute.AttributeClass?.Name is "SettingAttribute" or "Setting"))) return true;
        if (symbol is INamedTypeSymbol type)
        {
            for (INamedTypeSymbol? current = type; current is not null; current = current.BaseType)
                if (current.ContainingNamespace.ToDisplayString() == "Fbzz" && current.Name is "Script" or "DataAsset") return true;
        }
        return false;
    }

    /// @note Renaming must preserve source-symbol bindings, including identifiers which were not edited and could become shadowed.
    private static bool BindingsUnchanged(CSharpCompilation before, CSharpCompilation after)
    {
        Dictionary<SyntaxTree, Dictionary<(int, int), int>> beforeNodes = NodeOrdinals(before);
        Dictionary<SyntaxTree, Dictionary<(int, int), int>> afterNodes = NodeOrdinals(after);
        SyntaxTree[] oldTrees = before.SyntaxTrees.ToArray();
        SyntaxTree[] newTrees = after.SyntaxTrees.ToArray();
        for (int index = 0; index < oldTrees.Length; ++index)
        {
            SyntaxToken[] oldTokens = oldTrees[index].GetRoot().DescendantTokens().Where(token => token.IsKind(SyntaxKind.IdentifierToken)).ToArray();
            SyntaxToken[] newTokens = newTrees[index].GetRoot().DescendantTokens().Where(token => token.IsKind(SyntaxKind.IdentifierToken)).ToArray();
            if (oldTokens.Length != newTokens.Length) return false;
            SemanticModel oldModel = before.GetSemanticModel(oldTrees[index]);
            SemanticModel newModel = after.GetSemanticModel(newTrees[index]);
            for (int token = 0; token < oldTokens.Length; ++token)
            {
                if (BindingIdentity(ResolveSymbol(oldModel, oldTokens[token]), beforeNodes)
                    != BindingIdentity(ResolveSymbol(newModel, newTokens[token]), afterNodes)) return false;
            }
        }
        return true;
    }
    private static Dictionary<SyntaxTree, Dictionary<(int, int), int>> NodeOrdinals(CSharpCompilation compilation)
    {
        Dictionary<SyntaxTree, Dictionary<(int, int), int>> result = new();
        foreach (SyntaxTree tree in compilation.SyntaxTrees)
        {
            Dictionary<(int, int), int> ordinals = new();
            int ordinal = 0;
            foreach (SyntaxNode node in tree.GetRoot().DescendantNodesAndSelf()) ordinals[(node.SpanStart, node.RawKind)] = ordinal++;
            result[tree] = ordinals;
        }
        return result;
    }
    private static string BindingIdentity(ISymbol? symbol, Dictionary<SyntaxTree, Dictionary<(int, int), int>> nodes)
    {
        if (symbol is null) return "";
        symbol = Normalize(symbol);
        if (symbol.DeclaringSyntaxReferences.Length == 0)
            return "metadata:" + symbol.ContainingAssembly?.Identity + ":" + symbol.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
        return symbol.Kind + ":" + string.Join(";", symbol.DeclaringSyntaxReferences.Select(reference =>
        {
            SyntaxNode node = reference.GetSyntax();
            return reference.SyntaxTree.FilePath + ":" + nodes[reference.SyntaxTree][(node.SpanStart, node.RawKind)];
        }).OrderBy(value => value, StringComparer.Ordinal));
    }
}
