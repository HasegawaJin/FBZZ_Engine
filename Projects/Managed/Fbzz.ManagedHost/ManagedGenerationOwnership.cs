/// @file    ManagedGenerationOwnership.cs
/// @brief   parked Edit 等が保持するコード世代の lease と候補用資源枠を分離する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using System.Collections.Generic;
using System.Linq;
using Fbzz;

namespace Fbzz.ManagedHost;

public sealed partial class ManagedScriptRuntime
{
    readonly Dictionary<ulong, uint> m_generationLeases = new();
    /// @note 所有世代と current/candidate の一時世代は別の資源枠。配列等の保存容量とは無関係。
    public int RetainedGenerationCapacity { get; set; } = 4;
    public int TransientGenerationCapacity { get; set; } = 2;
    public ManagedRuntimeResult RetainGeneration(ulong generation)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_generations.ContainsKey(generation)) return new(BridgeStatus.STALE_GENERATION);
        uint count = m_generationLeases.GetValueOrDefault(generation);
        if (count == uint.MaxValue || (count == 0 && m_generationLeases.Count >= RetainedGenerationCapacity))
            return new(BridgeStatus.PENDING_LIMIT, "Retained generation capacity is exhausted; release an owned generation lease or configure more retained capacity.");
        m_generationLeases[generation] = count + 1;
        return new(BridgeStatus.OK);
    }
    /// @note lease 解放だけで CLR 個体を退役しない。native の個体所有と active 選択が退役時点を決める。
    public ManagedRuntimeResult ReleaseGeneration(ulong generation)
    {
        if (!IsPreparationThread()) return new(BridgeStatus.INVALID_SCOPE);
        if (!m_generationLeases.TryGetValue(generation, out uint count)) return new(BridgeStatus.INVALID_ARGUMENT, "Generation has no owned lease to release.");
        if (count == 1) m_generationLeases.Remove(generation); else m_generationLeases[generation] = count - 1;
        return new(BridgeStatus.OK);
    }
    bool HasTransientGenerationCapacity() => TransientGenerationCapacity > 0
        && m_generations.Keys.Count(generation => !m_generationLeases.ContainsKey(generation)) < TransientGenerationCapacity;
}
