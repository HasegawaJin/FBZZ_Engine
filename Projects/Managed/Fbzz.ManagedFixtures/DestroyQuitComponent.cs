/// @file    DestroyQuitComponent.cs
/// @brief   OnDestroy quit intent remains routed through the Editor host during Play teardown.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using Fbzz;
namespace ManagedFixtures;
public sealed class DestroyQuitComponent : Script
{
    protected override void OnDestroy() { App.Quit(); }
}
