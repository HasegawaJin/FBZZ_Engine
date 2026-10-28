/// @file    ScriptFieldContract.cs
/// @brief   Shared Roslyn checks for persisted field diagnostics and Inspector warning suppression.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace Fbzz.ScriptGenerator;

internal static class ScriptFieldContract
{
    internal static bool HasAttribute(IFieldSymbol field, string fullName)
    {
        foreach (AttributeData attribute in field.GetAttributes())
            if (attribute.AttributeClass?.ToDisplayString() == fullName &&
                (fullName != "Fbzz.SettingAttribute" || attribute.AttributeClass.ContainingAssembly.Name == "Fbzz.ScriptApi"))
                return true;
        return false;
    }

    internal static bool HasReservedKey(IFieldSymbol field)
        => field.GetAttributes().Any(static attribute => attribute.AttributeClass?.ContainingAssembly.Name == "Fbzz.ScriptApi"
            && attribute.AttributeClass.ToDisplayString() is "Fbzz.FieldKeyAttribute" or "Fbzz.PreviousFieldKeyAttribute"
            && attribute.ConstructorArguments.Length == 1 && attribute.ConstructorArguments[0].Value is "$go");

    /// @note Mirror the supported persisted field types; suppress only warnings for fields the Host saves.
    internal static bool SupportsField(ITypeSymbol type) => SupportsField(type, new HashSet<ITypeSymbol>(SymbolEqualityComparer.Default), 0);
    internal static bool SupportsField(ITypeSymbol type, HashSet<ITypeSymbol> visited, int depth)
    {
        if (depth > 32) return false;
        if (type is IArrayTypeSymbol array)
            return array.IsSZArray && array.Rank == 1 && SupportsField(array.ElementType, visited, depth + 1);
        if (SupportsScalar(type)) return true;
        if (type is not INamedTypeSymbol named) return false;
        if (named.OriginalDefinition.ToDisplayString() == "System.Collections.Generic.List<T>") return SupportsField(named.TypeArguments[0], visited, depth + 1);
        if (named.OriginalDefinition.ToDisplayString() is "Fbzz.DataAssetRef<T>" or "Fbzz.ComponentRef<T>" && named.ContainingAssembly.Name == "Fbzz.ScriptApi") return true;
        if (named.SpecialType == SpecialType.System_Object) return false;
        for (INamedTypeSymbol? parent = named; parent is not null && parent.SpecialType != SpecialType.System_Object; parent = parent.BaseType)
        {
            if (named.IsValueType && parent.SpecialType == SpecialType.System_ValueType) continue;
            string assembly = parent.ContainingAssembly.Name;
            if (assembly == "Fbzz.ScriptApi" && parent.ToDisplayString() == "Fbzz.SequenceEvent") continue;
            if (assembly is "System.Private.CoreLib" or "mscorlib" or "netstandard" or "Fbzz.ScriptApi"
                || assembly.StartsWith("System.", System.StringComparison.Ordinal)
                || assembly.StartsWith("Microsoft.", System.StringComparison.Ordinal)) return false;
        }
        if (named.AllInterfaces.Any(static value => value.ToDisplayString() == "System.Collections.IEnumerable")) return false;
        if (named.IsAbstract || !named.GetAttributes().Any(static attribute => attribute.AttributeClass?.ToDisplayString() == "System.SerializableAttribute")) return false;
        if (!named.IsValueType && !named.InstanceConstructors.Any(static constructor => constructor.Parameters.Length == 0)) return false;
        if (!visited.Add(type)) return true;
        for (INamedTypeSymbol? parent = named; parent is not null && parent.SpecialType != SpecialType.System_Object; parent = parent.BaseType)
            foreach (IFieldSymbol field in parent.GetMembers().OfType<IFieldSymbol>())
                if (!field.IsStatic && !field.IsReadOnly && !field.IsConst && !HasAttribute(field, "System.NonSerializedAttribute")
                    && (field.DeclaredAccessibility == Accessibility.Public || HasAttribute(field, "Fbzz.SettingAttribute"))
                    && (HasReservedKey(field) || !SupportsField(field.Type, visited, depth + 1))) { visited.Remove(type); return false; }
        visited.Remove(type);
        return true;
    }

    internal static bool SupportsScalar(ITypeSymbol type)
    {
        if (type.SpecialType is SpecialType.System_SByte or SpecialType.System_Byte or SpecialType.System_Int16 or SpecialType.System_UInt16
            or SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Int64 or SpecialType.System_UInt64
            or SpecialType.System_Single or SpecialType.System_Double or SpecialType.System_Decimal or SpecialType.System_Char or
            SpecialType.System_Boolean or SpecialType.System_String) return true;
        if (type is INamedTypeSymbol { TypeKind: TypeKind.Enum } enumeration)
            return enumeration.EnumUnderlyingType is not null;
        return type is INamedTypeSymbol named && named.ContainingAssembly.Name == "Fbzz.ScriptApi" &&
            named.ContainingNamespace.ToDisplayString() == "Fbzz" && named.Name is "GameObject" or "Vector2" or "Vector3" or "Vector4" or "Quaternion" or "MaterialAssetRef" or "TextureAssetRef" or "ModelAssetRef";
    }
}
