/// @file    RuntimeComponentsComponent.cs
/// @brief   実 CLR から runtime facade の row操作・phase・related参照・birth失効を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#nullable enable
using Fbzz;
using System;
namespace ManagedFixtures;
public sealed class RuntimeComponentsComponent : Script
{
    public GameObject? target;
    public GameObject? related;
    public GameObject? splineObject;
    public int operation;
    public int connectionMask;
    public float observedWarpTime;
    public float observedIKWeight;
    public float observedSpringWeight;
    public bool observedUIPlaying;
    public int observedWaypointCount;
    public Vector3 observedWaypoint;
    public float observedSplinePosition;
    public int observedShakeSeed;
    public int observedCameraPriority;
    public float observedAudioLevel;
    public int observedFlowChannels;
    public float observedFlowSpeed;
    public int observedRootCount;
    public bool fixedRequested;
    public bool wrongPhaseRejected;
    public bool relatedRejected;
    public bool parentChanged;
    public bool parentRejected;
    public bool cachedIsAlive;
    public bool savedReferenceMissing;
    public bool staleWriteRejected;
    public int observedParticleCapacity;
    public Vector3 observedParticlePosition;
    public float observedParticleDuration;
    public int observedParticleRequested;
    public bool observedParticleActual;
    public int observedParticleFallback;
    public float observedParticleNoise;
    public bool invalidMaterialRejected;
    public int invalidParticleRejected;
    public ComponentRef<FlowField> flowReference;
    public string prefab = "";
    public Vector3 spawnOffset;
    public Quaternion spawnRotationOffset = Quaternion.IDENTITY;
    public bool pointWarpStarted, pointRejected, pointModeLive, poleCleared, spawnSucceeded, spawnRejected;
    public Vector3 spawnedPosition;
    public Quaternion spawnedRotation = Quaternion.IDENTITY;
    FlowField? m_cachedFlow;
    GameObject? m_cachedPoint;
    protected override void Update()
    {
        GameObject owner = target ?? GameObject;
        switch (operation) {
        case 1: {
            MotionWarp warp = owner.GetComponent<MotionWarp>()!;
            IKSolver ik = owner.GetComponent<IKSolver>()!;
            Ragdoll ragdoll = owner.GetComponent<Ragdoll>()!;
            SpringBone spring = owner.GetComponent<SpringBone>()!;
            UIAnimator ui = owner.GetComponent<UIAnimator>()!;
            NavMeshPatrol patrol = owner.GetComponent<NavMeshPatrol>()!;
            SplineFollower spline = owner.GetComponent<SplineFollower>()!;
            CameraShake shake = owner.GetComponent<CameraShake>()!;
            VirtualCamera camera = owner.GetComponent<VirtualCamera>()!;
            AudioMixerSend audio = owner.GetComponent<AudioMixerSend>()!;
            FlowField flow = owner.GetComponent<FlowField>()!;
            connectionMask = (warp is null ? 0 : 1) | (ik is null ? 0 : 2) | (ragdoll is null ? 0 : 4)
                | (spring is null ? 0 : 8) | (ui is null ? 0 : 16) | (patrol is null ? 0 : 32) | (spline is null ? 0 : 64)
                | (shake is null ? 0 : 128) | (camera is null ? 0 : 256) | (audio is null ? 0 : 512) | (flow is null ? 0 : 1024);
            warp!.SetAxisWeight(new(0.5f, 0, 1)); warp.SetMaxSpeed(4); warp.WarpToPose(new(2, 3, 4), new(0, 0, 0, 2), 0.75f);
            observedWarpTime = warp.RemainingTime;
            ik!.SetChainWeight("Hand", 2); ik.SetChainEnabled("Hand", false); ik.SetChainTarget("Hand", related); observedIKWeight = ik.GetChainWeight("Hand");
            _ = ragdoll!.IsActive;
            spring!.EnsureChain("Tail", 3); spring.SetWeight("Tail", 0.6f); spring.SetSpring("Tail", 0.2f, 0.3f);
            spring.SetForce("Tail", new(1, 2, 3), 4); observedSpringWeight = spring.GetWeight("Tail");
            ui!.PlayPosition(new(1, 2), new(3, 4), 0.5f, UIEasing.EASE_OUT, true, true);
            ui.PlayRotation(0, 90, 0.25f); ui.PlayFill(0, 1, 0.4f); ui.PlayMaterialFloat("Progress", 0, 1, 0.7f); observedUIPlaying = ui.IsPlaying;
            patrol!.ClearWaypoints(); patrol.Mode = Fbzz.PatrolMode.PING_PONG; patrol.WaitTime = 0.25f;
            patrol.AddWaypoint(new(2, 3, 4), 0.5f, 2); patrol.AddWaypoint(new(5, 6, 7), 0.75f, 3); patrol.Restart(1);
            observedWaypointCount = patrol.WaypointCount; patrol.TryGetWaypoint(1, out observedWaypoint);
            spline!.BindSpline(splineObject); spline.Reverse = true; spline.WrapMode = SplineWrapMode.PING_PONG; spline.Speed = 2.5f;
            spline.OrientToPath = false; spline.Play(true); spline.NormalizedPosition = 0.4f; observedSplinePosition = spline.NormalizedPosition;
            shake!.Configure(2.5f, 12, 3); shake.Play(0.3f, 0.7f, int.MinValue); observedShakeSeed = shake.Seed;
            camera!.Priority = int.MaxValue; camera.FieldOfView = 200; camera.BlendInTime = -1; observedCameraPriority = camera.Priority;
            audio!.Set("SE", 2); observedAudioLevel = audio.Level;
            flow!.Type = FlowFieldType.VORTEX; flow.Speed = 4.5f; flow.SetRadius(2, 0); flow.Direction = new(0, 0, 1);
            flow.SetTurbulence(0.7f, -2); flow.Channels = 0x80000001; observedFlowChannels = unchecked((int)flow.Channels);
            observedFlowSpeed = flow.Speed; m_cachedFlow = flow; flowReference = new(flow); cachedIsAlive = flow.IsAlive;
            break;
        }
        case 2:
            try { owner.GetComponent<Ragdoll>()!.Begin(3, 0.9f, 2); }
            catch (InvalidOperationException) { wrongPhaseRejected = true; }
            break;
        case 3:
            try { owner.GetComponent<IKSolver>()!.SetChainTarget("Hand", related); }
            catch (InvalidOperationException) { relatedRejected = true; }
            try { parentChanged = owner.SetParent(related); }
            catch (InvalidOperationException) { parentRejected = true; }
            break;
        case 4:
            owner.RemoveComponent<FlowField>(); cachedIsAlive = m_cachedFlow is not null && m_cachedFlow.IsAlive;
            savedReferenceMissing = flowReference.IsMissing;
            break;
        case 5:
            FlowField fresh = owner.AddComponent<FlowField>();
            try { m_cachedFlow!.Speed = 99; }
            catch (InvalidOperationException) { staleWriteRejected = true; }
            cachedIsAlive = m_cachedFlow is not null && m_cachedFlow.IsAlive; savedReferenceMissing = flowReference.IsMissing;
            observedFlowSpeed = fresh.Speed;
            break;
        case 6: {
            ParticleSystem particle = owner.GetComponent<ParticleSystem>()!;
            particle.EmitPosition = new(1, 2, 3); particle.EmitVelocity = new(4, 5, 6); particle.VelocitySpread = -1; particle.Gravity = new(0, -3, 0);
            particle.SetColor(new(1, 0, 0, 0.8f), new(0, 0, 1, 0)); particle.SetSize(0.2f, 0.4f); particle.MaxParticles = 23;
            particle.SetPlayback(false, 2.5f, true); particle.SetConeShape(0.3f, 35); particle.SortMode = ParticleSortMode.BACK_TO_FRONT;
            particle.SimulationMode = ParticleSimulationMode.GPU; particle.SimulationSpace = ParticleSimulationSpace.WORLD;
            particle.SetRenderMode(ParticleRenderMode.STRETCHED_BILLBOARD, 3); particle.SetCollision(ParticleCollisionMode.DEPTH, ParticleCollisionResponse.KILL, 0.1f, 0.9f);
            particle.Prewarm = false; particle.SetSubEmitters("", "", "", 2); particle.FlowCoupling = 7; particle.SetAngularVelocity(-2, 3);
            particle.SetNoise(2, 0.7f, -0.3f); particle.ReceiveFlowFields = false; particle.FlowFieldChannels = 0x80000001;
            observedParticleCapacity = particle.MaxParticles; observedParticlePosition = particle.EmitPosition;
            observedParticleDuration = particle.GetPlayback().Duration; observedParticleRequested = (int)particle.SimulationMode;
            observedParticleActual = particle.IsGpuSimulated; observedParticleFallback = (int)particle.GpuFallbackReason;
            observedParticleNoise = particle.GetNoise().Strength;
            invalidMaterialRejected = !particle.TrySetMaterial(new MaterialAssetRef("missing-material-for-runtime-fixture"));
            break;
        }
        case 7: {
            ParticleSystem particle = owner.GetComponent<ParticleSystem>()!;
            try { particle.SimulationMode = (ParticleSimulationMode)99; } catch (ArgumentException) { invalidParticleRejected |= 1; }
            try { particle.SetCollision(ParticleCollisionMode.DEPTH, (ParticleCollisionResponse)9); } catch (ArgumentException) { invalidParticleRejected |= 2; }
            try { particle.EmitVelocity = new(float.NaN, 1, 2); } catch (ArgumentException) { invalidParticleRejected |= 4; }
            break;
        }
        case 8: {
            MotionWarp warp = owner.GetComponent<MotionWarp>()!;
            warp.SetTargetPoint(related, MotionWarpPointMode.CAPTURED);
            pointWarpStarted = warp.WarpToTarget(0.75f);
            observedWarpTime = warp.RemainingTime;
            IKSolver ik = owner.GetComponent<IKSolver>()!;
            ik.SetChainTarget("Hand", related); ik.SetChainPole("Hand", related);
            m_cachedPoint = related;
            break;
        }
        case 9: {
            MotionWarp warp = owner.GetComponent<MotionWarp>()!;
            try { warp.SetTargetPoint(related, MotionWarpPointMode.LIVE); pointWarpStarted = warp.WarpToTarget(1.25f); }
            catch (InvalidOperationException) { pointRejected = true; }
            pointModeLive = warp.TargetPointMode == MotionWarpPointMode.LIVE;
            break;
        }
        case 10:
            owner.GetComponent<MotionWarp>()!.WarpTo(new(2, 3, 4), 0.5f);
            break;
        case 11:
            try { owner.GetComponent<MotionWarp>()!.SetTargetPoint(m_cachedPoint); }
            catch (InvalidOperationException) { pointRejected = true; }
            break;
        case 12: {
            m_cachedPoint = related;
            GameObject? created = InstantiateAt(prefab, related!, spawnOffset, spawnRotationOffset);
            spawnSucceeded = created is not null;
            if (created is not null) { spawnedPosition = created.Transform.WorldPosition; spawnedRotation = created.Transform.WorldRotation; }
            break;
        }
        case 13:
            try { InstantiateAt(prefab, m_cachedPoint!, spawnOffset, spawnRotationOffset); }
            catch (InvalidOperationException) { spawnRejected = true; }
            break;
        case 14:
            try {
                GameObject? created = InstantiateAt(prefab, related!);
                spawnSucceeded = created is not null;
                if (created is not null) { spawnedPosition = created.Transform.WorldPosition; spawnedRotation = created.Transform.WorldRotation; }
            }
            catch (InvalidOperationException) { spawnRejected = true; }
            break;
        case 15:
            owner.GetComponent<IKSolver>()!.SetChainTarget("Hand", null);
            owner.GetComponent<IKSolver>()!.SetChainPole("Hand", null);
            poleCleared = true;
            break;
        }
    }
    protected override void FixedUpdate()
    {
        if (operation != 1) return;
        Ragdoll ragdoll = (target ?? GameObject).GetComponent<Ragdoll>()!;
        string[] roots = new string[130]; for (int index = 0; index < roots.Length; index++) roots[index] = "Bone" + index;
        ragdoll.SetRoots(roots, 3); ragdoll.SetExcludedBranches(["Accessory"]); ragdoll.SetProfile(RagdollProfile.HUMANOID);
        ragdoll.SetBlend(0.1f, 0.25f); ragdoll.Begin(2, 0.6f, 0); ragdoll.Push(new(1, 2, 3));
        observedRootCount = ragdoll.RootCount; fixedRequested = true;
    }
}
