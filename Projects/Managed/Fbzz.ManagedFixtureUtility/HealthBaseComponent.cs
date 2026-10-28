/// @file    HealthBaseComponent.cs
/// @brief   ゲーム依存 DLL に置いた通常 C# の Script 基底型。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

namespace ManagedFixtureUtility;

public abstract class HealthBaseComponent : Script
{
    public int health = 10;
}
