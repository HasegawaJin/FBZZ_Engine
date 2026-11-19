/// @file    RagdollStateController.cs
/// @brief   通常アニメーションと Active Ragdoll の適用ステートを C# で選択する。
/// @author  Hasegawa Jin
/// @date    2026-11-18
using System;
using System.Collections.Generic;

namespace Fbzz;

public enum RagdollUsage
{
    ANIMATION = 0,
    SELECTED_STATES = 1,
    ALWAYS = 2
}

/// @note 一つの Ragdoll の起動・終了をこの制御元にまとめ、FixedUpdate から Update を呼ぶ。
/// @note ステート名はゲームが渡す。Animator と独立した歩行・被弾ステートにも使える。
public sealed class RagdollStateController
{
    private readonly Ragdoll m_ragdoll;
    private HashSet<string> m_activeStates = new(StringComparer.Ordinal);
    private RagdollUsage m_usage;
    private float m_blendInSeconds = 0.15f;
    private float m_blendOutSeconds = 0.25f;
    private bool m_configurationChanged = true;
    private bool m_hasRequested;
    private bool m_activeRequested;

    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED)]
    public RagdollStateController(Ragdoll ragdoll)
    {
        ArgumentNullException.ThrowIfNull(ragdoll);
        m_ragdoll = ragdoll;
    }

    /// @note ステートは完全一致・大文字小文字を区別する。一覧をコピーし、空の一覧は選択なしを表す。
    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED,
        ParameterUnits = new[] { "blendInSeconds=s", "blendOutSeconds=s" },
        Summary = "Stages animation-only, selected-state or always-active usage. Changes apply on the next fixed Update.",
        Failure = "invalid_mode_state_name_or_transition_time_throws_without_changes")]
    public void Configure(RagdollUsage usage, string[]? activeStates = null,
        float blendInSeconds = 0.15f, float blendOutSeconds = 0.25f)
    {
        if (!Enum.IsDefined(usage)) throw new ArgumentOutOfRangeException(nameof(usage));
        if (!float.IsFinite(blendInSeconds) || blendInSeconds < 0f)
            throw new ArgumentOutOfRangeException(nameof(blendInSeconds));
        if (!float.IsFinite(blendOutSeconds) || blendOutSeconds < 0f)
            throw new ArgumentOutOfRangeException(nameof(blendOutSeconds));
        var states = new HashSet<string>(StringComparer.Ordinal);
        foreach (string state in activeStates ?? [])
        {
            if (string.IsNullOrEmpty(state)) throw new ArgumentException("State names must be nonempty.", nameof(activeStates));
            states.Add(state);
        }
        m_usage = usage;
        m_activeStates = states;
        m_blendInSeconds = blendInSeconds;
        m_blendOutSeconds = blendOutSeconds;
        m_configurationChanged = true;
    }

    [ApiContract(Call = "any", Purity = ApiPurity.MANAGED,
        Summary = "Evaluates the configured usage for an exact state name without accessing the Engine.")]
    public bool UsesActiveRagdoll(string currentState)
    {
        ArgumentNullException.ThrowIfNull(currentState);
        return m_usage == RagdollUsage.ALWAYS ||
            (m_usage == RagdollUsage.SELECTED_STATES && m_activeStates.Contains(currentState));
    }

    /// @note 要求が変わったときだけ BeginActive / End を呼び、補間中の再開は現在の適用率からつなぐ。
    /// @note 対象部位は Ragdoll.SetRoots / SetExcludedBranches、駆動の効き方は ApplyDriveProfile が担当する。
    [ApiContract(RunModes = new[] { "Play" }, Call = "fixed",
        Summary = "Applies the requested usage in FixedUpdate and returns whether Active Ragdoll is requested. Repeated states preserve the running blend.",
        Failure = "invalid_callback_thread_scene_or_component_lifetime_throws")]
    public bool Update(string currentState)
    {
        bool active = UsesActiveRagdoll(currentState);
        var scope = ScriptApiRuntime.RequireScope();
        if (scope.Callback != ScriptCallback.FIXED_UPDATE)
            throw new InvalidOperationException("Ragdoll state changes require FixedUpdate.");
        if (!m_ragdoll.IsAlive) throw new InvalidOperationException("The Ragdoll component lifetime has expired.");
        if (m_configurationChanged)
        {
            m_ragdoll.SetBlend(m_blendInSeconds, m_blendOutSeconds);
            m_configurationChanged = false;
        }
        if (!m_hasRequested || active != m_activeRequested)
        {
            if (active) m_ragdoll.BeginActive();
            else m_ragdoll.End();
            m_activeRequested = active;
            m_hasRequested = true;
        }
        return active;
    }
}
