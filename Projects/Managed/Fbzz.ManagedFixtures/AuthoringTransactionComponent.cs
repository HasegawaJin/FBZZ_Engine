/// @file    AuthoringTransactionComponent.cs
/// @brief   Real native preparation permissions and detached normalization regression fixture.
/// @author  Hasegawa Jin
/// @date    2026-10-05
#nullable enable
using System;
#nullable enable
using Fbzz;

namespace ManagedFixtures;

public sealed class AuthoringTransactionComponent : Script
{
    [ValueRange(0, 10)] public int amount = 2;
    public int coupled = 4;
    public GameObject? target = null;
    public bool touchEngine = false;
    protected override void OnValidate()
    {
        amount = Math.Clamp(amount, 0, 10); coupled = amount * 2;
        if (touchEngine) ReadWorld();
    }
    /// @note Deliberately tests a helper beyond direct analyzer diagnostics; reference preparation must still confer no Engine authority.
    void ReadWorld() { _ = Transform.WorldPosition; }
}
