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
    readonly Dictionary<ulong, uint> m_generationLeases = new(); ///< 保持中のコード世代 ID と、各世代の未解放 lease 数。
    /// @note 所有世代と current/candidate の一時世代は別の資源枠。配列等の保存容量とは無関係。
    public int RetainedGenerationCapacity { get; set; } = 4;
    public int TransientGenerationCapacity { get; set; } = 2;
    public ManagedRuntimeResult RetainGeneration(ulong generation)
    {
        if (!IsPreparationThread())
        {
            return new(BridgeStatus.INVALID_SCOPE);
        }
        if (!m_generations.ContainsKey(generation))
        {
            return new(BridgeStatus.STALE_GENERATION);
        }

        // 世代スロットを消費するのは、その世代の最初の lease だけ。
        uint leaseCount = m_generationLeases.GetValueOrDefault(generation);
        bool retainedSlotsExhausted = leaseCount == 0 && m_generationLeases.Count >= RetainedGenerationCapacity;
        if (leaseCount == uint.MaxValue || retainedSlotsExhausted)
        {
            return new(BridgeStatus.PENDING_LIMIT, "Retained generation capacity is exhausted; release an owned generation lease or configure more retained capacity.");
        }

        m_generationLeases[generation] = leaseCount + 1;
        return new(BridgeStatus.OK);
    }
    /// @note lease 解放だけで CLR 個体を退役しない。native の個体所有と active 選択が退役時点を決める。
    public ManagedRuntimeResult ReleaseGeneration(ulong generation)
    {
        if (!IsPreparationThread())
        {
            return new(BridgeStatus.INVALID_SCOPE);
        }
        if (!m_generationLeases.TryGetValue(generation, out uint leaseCount))
        {
            return new(BridgeStatus.INVALID_ARGUMENT, "Generation has no owned lease to release.");
        }

        // 最後の lease を解放すると、その世代が占めていた保持枠を返す。
        if (leaseCount == 1)
        {
            m_generationLeases.Remove(generation);
        }
        else
        {
            m_generationLeases[generation] = leaseCount - 1;
        }

        return new(BridgeStatus.OK);
    }
    bool HasTransientGenerationCapacity() => TransientGenerationCapacity > 0
        && m_generations.Keys.Count(generation => !m_generationLeases.ContainsKey(generation)) < TransientGenerationCapacity;
}
