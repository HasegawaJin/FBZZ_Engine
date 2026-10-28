/// @file    DataAssets.cs
/// @brief   シーンを所有しない純設定アセットと、世代付き読み取りサービス。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;

namespace Fbzz;

/// @note public field / Setting を保存する純設定型。Authoring callbacks は設定・定数・純粋演算だけを使い、Engine API や runtime cache に依存しない。
public abstract class DataAsset
{
    /// @note 明示的なアセット作成時だけ constructor の後に一度呼ぶ。受動的な読取・編集・Undo では呼ばない。
    protected virtual void Reset() { }
    /// @note Detached な設定候補の補正。候補は非設定 instance field を CLR 既定値として持ち、外部 I/O と可変 static 状態の変更は禁止する。
    protected virtual void OnValidate() { }
}

/// @note アセットの参照だけを保存する。値は Load 時に独立したコピーとして取得し、変更してもディスクへ書かない。
public readonly struct DataAssetRef<T> : IDataAssetReference where T : DataAsset
{
    public DataAssetRef(string reference)
    {
        ArgumentException.ThrowIfNullOrEmpty(reference);
        Reference = reference;
    }
    public string Reference { get; }
    public bool IsEmpty => string.IsNullOrEmpty(Reference);
    string IAssetReference.AssetKind => "dataAsset";
}

internal interface IDataAssetReference : IAssetReference { }

public sealed class ScriptDataAssets
{
    readonly ScriptBinding m_owner;
    internal ScriptDataAssets(ScriptBinding owner) { m_owner = owner; }
    /// @return 型が一致する独立した設定値。不在は null、壊れたデータ・型不一致は診断付き例外。
    [ApiContract(Call = "read")]
    public T? Load<T>(DataAssetRef<T> reference) where T : DataAsset
        => reference.IsEmpty ? null : Load<T>(reference.Reference);
    [ApiContract(Call = "read")]
    public T? Load<T>(string reference) where T : DataAsset
    {
        ArgumentException.ThrowIfNullOrEmpty(reference);
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope(m_owner);
        if (scope.Components is not IScriptDataAssetRuntime runtime) throw new NotSupportedException("DataAsset services are unavailable.");
        return (T?)runtime.LoadDataAsset(scope, typeof(T), reference);
    }
}

internal interface IScriptDataAssetRuntime
{
    DataAsset? LoadDataAsset(ScriptExecutionScope scope, Type type, string reference);
}
