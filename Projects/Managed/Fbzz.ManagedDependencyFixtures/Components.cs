/// @file    Components.cs
/// @brief   外部 Script 基底への型検索を試す同期 callback と実体。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;
using ManagedFixtureUtility;

namespace ManagedDependencyFixtures;

class DependencyHealthComponent : HealthBaseComponent { }

class DependencyCallerComponent : Script
{
    protected override void Update() { Transform.TranslateLocal(Vector3.ZERO); }
}
