/// @file    EngineNotificationComponent.cs
/// @brief   native Engine 通知の値・権限・所有購読・障害を実 CoreCLR で観測する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
using System;
using Fbzz;

namespace ManagedFixtures;

public sealed class EngineNotificationComponent : Script
{
    public int animationCount;
    public int motionCount;
    public int sequenceCount;
    public int finishedCount;
    public int arrivedCount;
    public int failedCount;
    public int spottedCount;
    public int lostCount;
    public int globalCount;
    public int connectCount;
    public int timeRejectedCount;
    public int physicsRejectedCount;
    public bool throwOnAnimation;
    public bool throwOnGlobal;
    public bool verifyScope;
    public bool publishManaged;
    public string eventName = "";
    public int eventInteger;
    public float eventScalar;
    public float eventTime;
    public string sequenceName = "";
    public Vector3 rootDelta;
    public Quaternion rootRotation = Quaternion.IDENTITY;
    public Vector3 worldDelta;
    public Vector3 worldVelocity;
    public float rootSeconds;
    public bool rootApplied;
    public bool growAgentsOnPathFailure;
    public int grownAgentCount;

    protected override void Connect()
    { connectCount++; Events.Subscribe<SequenceEvent>(ReceiveGlobal); }
    protected override void Update()
    {
        if (!publishManaged) return;
        publishManaged = false;
        Events.Publish(new SequenceEvent("Managed Cue", -7, 0.5f, 1.25f, "Managed Sequence"));
    }
    protected override void OnAnimationEvent(AnimationEvent info)
    {
        animationCount++; eventName = info.Name; eventInteger = info.IntParam; eventScalar = info.FloatParam; eventTime = info.ClipTime;
        CheckScope();
        if (throwOnAnimation) throw new InvalidOperationException("Intentional Engine animation callback failure.");
    }
    protected override void OnAnimatorMove(RootMotionInfo info)
    {
        motionCount++; rootDelta = info.DeltaPosition; rootRotation = info.DeltaRotation; worldDelta = info.WorldDeltaPosition;
        worldVelocity = info.WorldVelocity; rootSeconds = info.DeltaSeconds; rootApplied = info.AppliedByEngine; CheckScope();
    }
    protected override void OnSequenceEvent(SequenceEvent info) { sequenceCount++; Record(info); CheckScope(); }
    protected override void OnSequenceFinished(string name) { finishedCount++; sequenceName = name; CheckScope(); }
    protected override void OnNavMeshDestinationReached() { arrivedCount++; CheckScope(); }
    protected override void OnNavMeshPathFailed()
    {
        failedCount++; CheckScope();
        if (!growAgentsOnPathFailure) return;
        growAgentsOnPathFailure = false;
        SpawnPose[] poses = new SpawnPose[5001];
        GameObject[] objects = new GameObject[poses.Length];
        for (int index = 0; index < poses.Length; ++index)
            poses[index] = new SpawnPose(Vector3.ZERO, Quaternion.IDENTITY);
        BatchResult batch = Scene.CreateMany("AddedNavigationAgent", poses, objects);
        if (!batch.Succeeded) throw new InvalidOperationException("Navigation callback could not create its new agents.");
        foreach (GameObject item in objects) { item.AddComponent<NavMeshAgent>(); ++grownAgentCount; }
    }
    protected override void OnNavMeshTargetSpotted() { spottedCount++; CheckScope(); }
    protected override void OnNavMeshTargetLost() { lostCount++; CheckScope(); }
    void ReceiveGlobal(SequenceEvent info)
    {
        globalCount++; Record(info); CheckScope();
        if (throwOnGlobal) throw new InvalidOperationException("Intentional Engine global Sequence callback failure.");
    }
    void Record(SequenceEvent info)
    { eventName = info.Name; eventInteger = info.IntParam; eventScalar = info.FloatParam; eventTime = info.SequenceTime; sequenceName = info.SequenceName; }
    void CheckScope()
    {
        if (!verifyScope) return;
        if (!GameObject.IsAlive) throw new InvalidOperationException("Engine notification lost its owner.");
        Transform.TranslateLocal(Vector3.RIGHT);
        try { _ = Time.DeltaSeconds; }
        catch (InvalidOperationException) { timeRejectedCount++; }
        RigidBody body = GetComponent<RigidBody>();
        if (body != null) {
            try { body.Velocity = Vector3.RIGHT; }
            catch (InvalidOperationException) { physicsRejectedCount++; }
        }
    }
}
