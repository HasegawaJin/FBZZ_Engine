/// @file    NativeComponentFactory.cs
/// @brief   公開 facade 型と共通 native Component 識別子の対応。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;

namespace Fbzz;

internal static class NativeComponentFactory
{
    /// @note 7 は旧 aggregate UI の予約番号。UI は所有者サービスであり Component ではない。
    static readonly Type?[] s_types = [typeof(Transform), typeof(RigidBody), typeof(BoxCollider), typeof(SphereCollider),
        typeof(CapsuleCollider), typeof(Animator), typeof(AudioSource), null, typeof(Camera), typeof(NavMeshAgent),
        typeof(CharacterController), typeof(UIText), typeof(UIImage), typeof(UIButton), typeof(Canvas), typeof(CylinderCollider),
        typeof(AabbCollider), typeof(MeshCollider), typeof(ConvexHullCollider), typeof(TerrainCollider), typeof(Slider),
        typeof(Toggle), typeof(ScrollView), typeof(TextField), typeof(CanvasGroup), typeof(Draggable), typeof(DropTarget), typeof(Light), typeof(ParticleSystem), typeof(Material),
        typeof(VFX), typeof(SequencePlayer), typeof(Terrain), typeof(Water), typeof(Joint),
        typeof(LineRenderer), typeof(SpriteRenderer), typeof(SortingGroup), typeof(TrailRenderer), typeof(MeshTrail), typeof(MeshRenderer), typeof(SkinnedMeshRenderer), typeof(MotionWarp), typeof(IKSolver), typeof(Ragdoll), typeof(SpringBone), typeof(UIAnimator), typeof(NavMeshPatrol), typeof(SplineFollower), typeof(CameraShake), typeof(VirtualCamera), typeof(AudioMixerSend), typeof(FlowField), typeof(InstancedMeshes)];
    internal static Type? GetType(uint kind) => kind < s_types.Length ? s_types[kind] : null;
    internal static uint GetKind(Type type)
    {
        for (uint kind = 0; kind < s_types.Length; kind++) if (s_types[kind] == type) return kind;
        return uint.MaxValue;
    }
    internal static ulong Lifetime(Component value) => value switch
    { NativeComponent component => component.LifetimeToken, RigidBody body => body.LifetimeToken, Transform => value.GameObject.ObjectId, _ => 0 };
    internal static ulong CaptureLifetime(GameObject owner, uint kind)
    {
        ScriptExecutionScope scope = ScriptApiRuntime.RequireScope();
        if (owner.SceneLifetime != scope.SceneLifetime) throw new InvalidOperationException("The Component belongs to another scene.");
        return scope.Bridge.ReadComponentLifetime(scope, owner.ObjectId, kind);
    }
    /// @note 保存済み runtime token の復元は Engine を照会しない。
    internal static Component? Create(Type type, GameObject owner, ulong token) => token == 0 ? null : GetKind(type) switch
    {
        0 => token == owner.ObjectId ? owner.Transform : null, 1 => new RigidBody(owner, token), 2 => new BoxCollider(owner, token),
        3 => new SphereCollider(owner, token), 4 => new CapsuleCollider(owner, token), 5 => new Animator(owner, token),
        6 => new AudioSource(owner, token), 8 => new Camera(owner, token), 9 => new NavMeshAgent(owner, token),
        10 => new CharacterController(owner, token), 11 => new UIText(owner, token), 12 => new UIImage(owner, token),
        13 => new UIButton(owner, token), 14 => new Canvas(owner, token), 15 => new CylinderCollider(owner, token),
        16 => new AabbCollider(owner, token), 17 => new MeshCollider(owner, token), 18 => new ConvexHullCollider(owner, token),
        19 => new TerrainCollider(owner, token), 20 => new Slider(owner, token), 21 => new Toggle(owner, token),
        22 => new ScrollView(owner, token), 23 => new TextField(owner, token), 24 => new CanvasGroup(owner, token),
        25 => new Draggable(owner, token), 26 => new DropTarget(owner, token),
        (uint)NativeComponentKind.LIGHT => new Light(owner, token),
        (uint)NativeComponentKind.PARTICLE_SYSTEM => new ParticleSystem(owner, token),
        (uint)NativeComponentKind.MATERIAL => new Material(owner, token),
        (uint)NativeComponentKind.VFX => new VFX(owner, token),
        (uint)NativeComponentKind.SEQUENCE => new SequencePlayer(owner, token),
        (uint)NativeComponentKind.TERRAIN => new Terrain(owner, token),
        (uint)NativeComponentKind.WATER => new Water(owner, token), (uint)NativeComponentKind.JOINT => new Joint(owner, token),
        (uint)NativeComponentKind.LINE_RENDERER => new LineRenderer(owner, token),
        (uint)NativeComponentKind.SPRITE_RENDERER => new SpriteRenderer(owner, token),
        (uint)NativeComponentKind.SORTING_GROUP => new SortingGroup(owner, token),
        (uint)NativeComponentKind.TRAIL => new TrailRenderer(owner, token),
        (uint)NativeComponentKind.MESH_TRAIL => new MeshTrail(owner, token),
        (uint)NativeComponentKind.MESH_RENDERER => new MeshRenderer(owner, token),
        (uint)NativeComponentKind.SKINNED_MESH_RENDERER => new SkinnedMeshRenderer(owner, token),
        (uint)NativeComponentKind.MOTION_WARP => new MotionWarp(owner, token),
        (uint)NativeComponentKind.IK_SOLVER => new IKSolver(owner, token),
        (uint)NativeComponentKind.RAGDOLL => new Ragdoll(owner, token),
        (uint)NativeComponentKind.SPRING_BONE => new SpringBone(owner, token),
        (uint)NativeComponentKind.UI_ANIMATOR => new UIAnimator(owner, token),
        (uint)NativeComponentKind.NAV_MESH_PATROL => new NavMeshPatrol(owner, token),
        (uint)NativeComponentKind.SPLINE_FOLLOWER => new SplineFollower(owner, token),
        (uint)NativeComponentKind.CAMERA_SHAKE => new CameraShake(owner, token),
        (uint)NativeComponentKind.VIRTUAL_CAMERA => new VirtualCamera(owner, token),
        (uint)NativeComponentKind.AUDIO_MIXER_SEND => new AudioMixerSend(owner, token),
        (uint)NativeComponentKind.FLOW_FIELD => new FlowField(owner, token),
        (uint)NativeComponentKind.INSTANCED_MESH => new InstancedMeshes(owner, token),
        _ => null
    };
}
