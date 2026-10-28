/// @file    Fixtures.cs
/// @brief   不正 constructor と候補の設定復元失敗を検証する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

namespace ManagedFixtures;

class LifecycleComponent : Script
{
    public float speed = 99f;
    LifecycleComponent() { Transform.LocalPosition = Vector3.ZERO; }
}

/// @note constructor は正常。旧世代の awakeCount=1 を復元すると候補の設定契約に違反する。
class CompletionComponent : Script
{
    [ValueRange(0, 0)] public int awakeCount = 0;
}
