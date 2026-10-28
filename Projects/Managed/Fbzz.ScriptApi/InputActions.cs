/// @file    InputActions.cs
/// @brief   入力名だけを保持し、現在の callback でボタンと軸を型付きで読む。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;

namespace Fbzz;

/// @note Input.Button で取得する非所有の名前参照。実行権限を保持せず、入力設定変更後も読取時に存在を検証する。
public readonly struct ButtonAction
{
    readonly string? m_name;
    internal ButtonAction(string name) { m_name = name; }
    [ApiContract(Summary = "The immutable bound button name; empty for the default value.", Read = "any", RunModes = new[] { "any" }, Purity = ApiPurity.MANAGED)]
    public string Name => m_name ?? string.Empty;
    [ApiContract(Summary = "Whether the bound button is currently held.", Read = "input", Purity = ApiPurity.ENGINE,
        Failure = "unbound_or_removed_button_name_or_invalid_scope_throws", Lifetime = "non_owning_action_name; current_callback_revalidates_mapping")]
    public bool Held => InputActionAccess.Read(m_name, ScriptInputValue.HELD) != 0f;
    /// @note フレーム単位の押下遷移。FixedUpdate で一度消費する処理には Update でラッチする。
    [ApiContract(Summary = "The frame's press transition; latch in Update before consuming once in FixedUpdate.", Read = "input", Purity = ApiPurity.ENGINE,
        Failure = "unbound_or_removed_button_name_or_invalid_scope_throws", Lifetime = "non_owning_action_name; current_callback_revalidates_mapping")]
    public bool Pressed => InputActionAccess.Read(m_name, ScriptInputValue.DOWN) != 0f;
    [ApiContract(Summary = "The frame's release transition.", Read = "input", Purity = ApiPurity.ENGINE,
        Failure = "unbound_or_removed_button_name_or_invalid_scope_throws", Lifetime = "non_owning_action_name; current_callback_revalidates_mapping")]
    public bool Released => InputActionAccess.Read(m_name, ScriptInputValue.UP) != 0f;
}

/// @note Input.Axis で取得する非所有の名前参照。実行権限を保持せず、入力設定変更後も読取時に存在を検証する。
public readonly struct AxisAction
{
    readonly string? m_name;
    internal AxisAction(string name) { m_name = name; }
    [ApiContract(Summary = "The immutable bound axis name; empty for the default value.", Read = "any", RunModes = new[] { "any" }, Purity = ApiPurity.MANAGED)]
    public string Name => m_name ?? string.Empty;
    [ApiContract(Summary = "The current axis value; scale and units follow the configured axis bindings.", Units = "mapping_defined", Read = "input", Purity = ApiPurity.ENGINE,
        Failure = "unbound_or_removed_axis_name_or_invalid_scope_throws", Lifetime = "non_owning_action_name; current_callback_revalidates_mapping")]
    public float Value => InputActionAccess.Read(m_name, ScriptInputValue.AXIS);
}

internal static class InputActionAccess
{
    internal static float Read(string? name, ScriptInputValue value)
    {
        if (string.IsNullOrEmpty(name))
            throw new InvalidOperationException($"The input action is unbound. Obtain it through Input.{(value == ScriptInputValue.AXIS ? "Axis" : "Button")}(name) in an engine callback.");
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        return scope.Bridge.ReadAction(scope, name, (uint)value);
    }

    internal static ArgumentException Unknown(string name, bool axis)
        => new($"Unknown input {(axis ? "axis" : "button")} '{name}'. Use Input.{(axis ? "GetAxisNames" : "GetButtonNames")}() to inspect the current input settings.", "name");
}
