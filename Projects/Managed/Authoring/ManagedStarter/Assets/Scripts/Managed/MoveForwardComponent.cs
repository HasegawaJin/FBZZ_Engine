/// @file    MoveForwardComponent.cs
/// @brief   Minimal SDK-only example; attach it to a GameObject and enter Play.
/// @author  Hasegawa Jin
/// @date    2026-10-05
using Fbzz;

public class MoveForwardComponent : Script
{
    [ValueRange(0f, 20f)]
    public float moveSpeed = 4f;

    protected override void Update()
    {
        Transform.TranslateWorld(Vector3.FORWARD * (moveSpeed * Time.DeltaSeconds));
    }
}
