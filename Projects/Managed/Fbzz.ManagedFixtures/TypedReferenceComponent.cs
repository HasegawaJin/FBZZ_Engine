/// @file    TypedReferenceComponent.cs
/// @brief   型付き参照の保存、正確な寿命、同型 Script と世代移行を検証する fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-05
using Fbzz;

namespace ManagedFixtures;

public sealed class TypedReferenceComponent : Script
{
    public ComponentRef<Component> target;
    public ComponentRef<RigidBody>[] bodies = [];
    public MaterialAssetRef material;
    public TextureAssetRef texture;
    public ModelAssetRef model;
    public ComponentRef<LineRenderer> line;
    public ComponentRef<MotionWarp> motionWarp;
}
