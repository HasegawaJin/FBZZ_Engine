/// @file    ApiContractAttribute.cs
/// @brief   SDK 宣言の契約を API catalog と配布 XML の共通入力にする。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;

namespace Fbzz;

/// @note 未指定項目は unknown のまま。単位や失敗動作を型名・メソッド名から推定しない。
/// @note ParameterUnits / ParameterCoordinates は parameter=value の列。overload の宣言に直接属する。
/// @note callback alias は live=変更可能、read/input/subscription=live と Connect、query=read と readonly draw、fixed=FixedUpdate、draw=DrawGizmos/Selected、time=時間差 callback、physics=固定更新と物理通知、any=純 managed。
[AttributeUsage(AttributeTargets.Method | AttributeTargets.Constructor | AttributeTargets.Property | AttributeTargets.Field,
    AllowMultiple = false, Inherited = false)]
internal sealed class ApiContractAttribute : Attribute
{
    public string? Summary { get; set; }
    public string? Units { get; set; }
    public string? Coordinate { get; set; }
    public string? Failure { get; set; }
    public string? Returns { get; set; }
    public string[]? ParameterUnits { get; set; }
    public string[]? ParameterCoordinates { get; set; }
    public string[]? ParameterHelp { get; set; }
    public string? Read { get; set; }
    public string? Write { get; set; }
    public string? Call { get; set; }
    /// @note 条件を満たす native 型/個体だけが追加権限を持つ。通常の Read/Call の権限は変更しない。
    public string? AccessCondition { get; set; }
    public string? ConditionalRead { get; set; }
    public string? ConditionalCall { get; set; }
    public string[]? RunModes { get; set; }
    public string[]? ReadRunModes { get; set; }
    public string[]? WriteRunModes { get; set; }
    public string[]? CallRunModes { get; set; }
    public ApiPurity Purity { get; set; }
    public string? Lifetime { get; set; }
}

internal enum ApiPurity { UNKNOWN, MANAGED, ENGINE }
