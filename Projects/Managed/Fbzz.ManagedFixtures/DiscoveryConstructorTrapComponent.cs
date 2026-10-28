/// @file    DiscoveryConstructorTrapComponent.cs
/// @brief   Passive Host/bus catalog must enumerate this type without executing its throwing constructor.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using System;
using Fbzz;

namespace ManagedFixtures;

public sealed class DiscoveryConstructorTrapComponent : Script
{
    public int value = 23;
    public DiscoveryConstructorTrapComponent() => throw new InvalidOperationException("Explicit discovery constructor trap.");
}

public sealed class DiscoveryStaticConstructorTrapComponent : Script
{
    public int value = 41;
    static DiscoveryStaticConstructorTrapComponent() => throw new InvalidOperationException("Passive discovery must never initialize this type.");
}
