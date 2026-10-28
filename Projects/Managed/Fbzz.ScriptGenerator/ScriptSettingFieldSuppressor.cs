/// @file    ScriptSettingFieldSuppressor.cs
/// @brief   Inspector が保存・設定する field に限り compiler の未代入警告を抑制する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System.Collections.Immutable;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Diagnostics;
using static Fbzz.ScriptGenerator.ScriptFieldContract;

namespace Fbzz.ScriptGenerator;

/// @note C# のアクセス・既定値・nullable 規則は変更せず、外部設定対象の CS0649 だけを抑制する。
/// @see https://learn.microsoft.com/dotnet/api/microsoft.codeanalysis.diagnostics.diagnosticsuppressor DiagnosticSuppressor
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class ScriptSettingFieldSuppressor : DiagnosticSuppressor
{
    private static readonly SuppressionDescriptor SETTING_ASSIGNMENT = new(
        "FBZZSPR001", "CS0649", "The Inspector assigns this persisted Script setting field.");

    public override ImmutableArray<SuppressionDescriptor> SupportedSuppressions => [SETTING_ASSIGNMENT];

    public override void ReportSuppressions(SuppressionAnalysisContext context)
    {
        foreach (Diagnostic diagnostic in context.ReportedDiagnostics)
        {
            if (diagnostic.Id != "CS0649" || diagnostic.Location.SourceTree is not SyntaxTree tree) continue;
            SyntaxNode node = tree.GetRoot(context.CancellationToken).FindNode(diagnostic.Location.SourceSpan);
            VariableDeclaratorSyntax? declaration = node as VariableDeclaratorSyntax
                ?? node.FirstAncestorOrSelf<VariableDeclaratorSyntax>();
            if (declaration is null) continue;
            SemanticModel model = context.GetSemanticModel(tree);
            if (model.GetDeclaredSymbol(declaration, context.CancellationToken) is not IFieldSymbol field ||
                field.IsStatic || field.IsReadOnly || field.IsConst ||
                (!ScriptSourceGenerator.IsScript(field.ContainingType) && !ScriptSourceGenerator.IsDataAsset(field.ContainingType)) ||
                HasAttribute(field, "System.NonSerializedAttribute") ||
                (field.DeclaredAccessibility != Accessibility.Public && !HasAttribute(field, "Fbzz.SettingAttribute")) ||
                !SupportsField(field.Type)) continue;
            context.ReportSuppression(Suppression.Create(SETTING_ASSIGNMENT, diagnostic));
        }
    }

}
