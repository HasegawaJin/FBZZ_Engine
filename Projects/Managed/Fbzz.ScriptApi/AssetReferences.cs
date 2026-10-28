/// @file    AssetReferences.cs
/// @brief   Material・Texture・Model の GUID を保持する型付き値参照。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;

namespace Fbzz;

internal interface IAssetReference { string Reference { get; } string AssetKind { get; } }
internal static class AssetReferenceGuid
{
    internal static string Canonical(string guid)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(guid);
        string value = guid.StartsWith("guid:", StringComparison.Ordinal) ? guid[5..] : guid;
        if (value.Length == 0 || value.Contains('\0') || value.Contains('/') || value.Contains('\\') || value.Contains(':'))
            throw new ArgumentException("Typed asset references require an asset GUID, not a file path.", nameof(guid));
        return "guid:" + value;
    }
}
public readonly struct MaterialAssetRef : IAssetReference
{
    readonly string? m_reference;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "empty_or_path_like_GUID_throws_ArgumentException", Lifetime = "managed_asset_GUID_value")]
    public MaterialAssetRef(string guid) { m_reference = AssetReferenceGuid.Canonical(guid); }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Failure = "default_returns_empty_string", Lifetime = "managed_asset_GUID_value")]
    public string Reference => m_reference ?? "";
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Failure = "no_asset_resolution")]
    public bool IsEmpty => string.IsNullOrEmpty(Reference);
    string IAssetReference.AssetKind => "material";
}
public readonly struct TextureAssetRef : IAssetReference
{
    readonly string? m_reference;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "empty_or_path_like_GUID_throws_ArgumentException", Lifetime = "managed_asset_GUID_value")]
    public TextureAssetRef(string guid) { m_reference = AssetReferenceGuid.Canonical(guid); }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Failure = "default_returns_empty_string", Lifetime = "managed_asset_GUID_value")]
    public string Reference => m_reference ?? "";
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Failure = "no_asset_resolution")]
    public bool IsEmpty => string.IsNullOrEmpty(Reference);
    string IAssetReference.AssetKind => "texture";
}
public readonly struct ModelAssetRef : IAssetReference
{
    readonly string? m_reference;
    [ApiContract(Purity = ApiPurity.MANAGED, Call = "any", Failure = "empty_or_path_like_GUID_throws_ArgumentException", Lifetime = "managed_asset_GUID_value")]
    public ModelAssetRef(string guid) { m_reference = AssetReferenceGuid.Canonical(guid); }
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any", Failure = "default_returns_empty_string", Lifetime = "managed_asset_GUID_value")]
    public string Reference => m_reference ?? "";
    [ApiContract(Purity = ApiPurity.MANAGED, Read = "any")]
    public bool IsEmpty => string.IsNullOrEmpty(Reference);
    string IAssetReference.AssetKind => "model";
}
