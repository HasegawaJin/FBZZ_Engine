/// @file    Attributes.cs
/// @brief   private 設定と保存キーを宣言する標準 C# 属性。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Fbzz.ManagedHost")]
[assembly: InternalsVisibleTo("Fbzz.ManagedTests")]

namespace Fbzz;

/// @brief public と同じ保存・編集対象へ private field を追加する。
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class SettingAttribute : Attribute
{
    public string? Label { get; set; }
    public string? Help { get; set; }
    public string? Group { get; set; }
    public string? Unit { get; set; }
    public bool Visible { get; set; } = true;
}

/// @brief メンバー名から独立した保存キー。
/// @note Serializable の入れ子 field では内部参照 token の "$go" を保存キーに使えない。
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class FieldKeyAttribute : Attribute
{
    public FieldKeyAttribute(string key) { Key = key; }
    public string Key { get; }
}

/// @brief 改名前の保存キーを現在の field へ対応付ける。
/// @note Serializable の入れ子 field では内部参照 token の "$go" を旧保存キーに使えない。
[AttributeUsage(AttributeTargets.Field, AllowMultiple = true, Inherited = false)]
public sealed class PreviousFieldKeyAttribute : Attribute
{
    public PreviousFieldKeyAttribute(string key) { Key = key; }
    public string Key { get; }
}

/// @brief Play 差し替えで非保存の状態 field を引き継ぐ。static・delegate・実行途中の Task は保持しない。
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class KeepOnReloadAttribute : Attribute { }

/// @brief 非物理 callback を編集中にも配送する opt-in。
[AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = true)]
public sealed class ExecuteAlwaysAttribute : Attribute { }

/// @brief 同じ GameObject に必要な型を宣言する。Inspector と Play は欠損を報告し、自動追加しない。
[AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = true)]
public sealed class RequireComponentAttribute : Attribute
{
    public RequireComponentAttribute(params Type[] types) { Types = types; }
    public Type[] Types { get; }
}

/// @brief 小さい値を先に配送する。省略は 0、同値は既存の個体順を保持する。
[AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = true)]
public sealed class ExecutionOrderAttribute : Attribute
{
    public ExecutionOrderAttribute(int order) { Order = order; }
    public int Order { get; }
}

/// @brief 設定候補へ適用する編集 action。Engine・実行時 cache・static・外部 I/O は使わない。
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = true)]
public sealed class EditorActionAttribute : Attribute
{
    public EditorActionAttribute(string key, string? label = null) { Key = key; Label = label; }
    public string Key { get; }
    public string? Label { get; }
}

/// @brief 明示した有限範囲だけを保存・編集・候補復元に要求する。
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class ValueRangeAttribute : Attribute
{
    public ValueRangeAttribute(float minimum, float maximum) { Minimum = minimum; Maximum = maximum; }
    public ValueRangeAttribute(double minimum, double maximum) { Minimum = minimum; Maximum = maximum; }
    public ValueRangeAttribute(int minimum, int maximum) { Minimum = minimum; Maximum = maximum; }
    public ValueRangeAttribute(uint minimum, uint maximum) { Minimum = minimum; Maximum = maximum; }
    public ValueRangeAttribute(long minimum, long maximum) { Minimum = minimum; Maximum = maximum; }
    public ValueRangeAttribute(ulong minimum, ulong maximum) { Minimum = minimum; Maximum = maximum; }
    /// @note Decimal bounds use invariant strings because decimal is not a C# attribute argument type.
    public ValueRangeAttribute(string minimum, string maximum) { Minimum = minimum; Maximum = maximum; }
    public object Minimum { get; }
    public object Maximum { get; }
}

/// @brief Play 開始・実行前検証で GameObject 単体・非空配列・非空 List に生存する参照を要求する。
/// @note 編集・保存・Undo では未接続の設定を保持できる。コレクションは実行前に全要素の接続を要求する。
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = false)]
public sealed class RequiredReferenceAttribute : Attribute { }

/// @note コンパイラーが型と宣言元を対応付ける。ゲーム作者の手書き登録ではない。
[AttributeUsage(AttributeTargets.Assembly, AllowMultiple = true)]
public sealed class ScriptSourceAttribute : Attribute
{
    public ScriptSourceAttribute(string clrName, string sourcePath, int line, string contentHash)
    { ClrName = clrName; SourcePath = sourcePath; Line = line; ContentHash = contentHash; }
    public string ClrName { get; }
    public string SourcePath { get; }
    public int Line { get; }
    public string ContentHash { get; }
}
