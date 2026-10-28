/// @file    ConnectionPolicyComponent.cs
/// @brief   Connect の読取りと world 変更禁止を実 bridge で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using Fbzz;

namespace ManagedFixtures;

public sealed class ConnectionPolicyComponent : Script
{
    public bool mutate;
    bool m_readSucceeded;

    protected override void Connect()
    {
        m_readSucceeded = Transform.WorldPosition.IsFinite;
        if (mutate) Scene.Create("ForbiddenConnectMutation");
    }
}
