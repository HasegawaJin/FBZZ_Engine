/// @file    CollisionComponent.cs
/// @brief   六つの private 物理 callback の値・順序・参照寿命・例外を観測する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using Fbzz;

namespace ManagedFixtures;

public sealed class CollisionComponent : Script
{
    public int collisionEnterCount;
    public int collisionStayCount;
    public int collisionExitCount;
    public int triggerEnterCount;
    public int triggerStayCount;
    public int triggerExitCount;
    public int destroyCount;
    public int[] eventOrder = Array.Empty<int>();
    public GameObject other;
    public Vector3 point;
    public Vector3 normal;
    public float depth;
    public Vector3 relativeVelocity;
    public float approachSpeed;
    public float impulse;
    public float deltaSeconds;
    public bool otherAlive;
    public bool cachedOtherAlive;
    public bool selfAlive;
    public bool throwOnEnter;
    public bool touchTransform;
    public bool attemptCachedMutation;
    public bool readTime;
    GameObject m_cachedOther;

    protected override void OnCollisionEnter(Collision collision)
    {
        collisionEnterCount++;
        RecordCollision(1, collision);
        if (throwOnEnter) throw new InvalidOperationException("Intentional collision callback failure.");
    }
    protected override void OnCollisionStay(Collision collision) { collisionStayCount++; RecordCollision(2, collision); }
    protected override void OnCollisionExit(Collision collision) { collisionExitCount++; RecordCollision(3, collision); }
    protected override void OnTriggerEnter(GameObject target) { triggerEnterCount++; Record(4, target); }
    protected override void OnTriggerStay(GameObject target) { triggerStayCount++; Record(5, target); }
    protected override void OnTriggerExit(GameObject target) { triggerExitCount++; Record(6, target); }
    void RecordCollision(int kind, Collision collision)
    {
        point = collision.Point;
        normal = collision.Normal;
        depth = collision.Depth;
        relativeVelocity = collision.RelativeVelocity;
        approachSpeed = collision.ApproachSpeed;
        impulse = collision.Impulse;
        Record(kind, collision.Other);
    }
    void Record(int kind, GameObject target)
    {
        int index = eventOrder.Length;
        Array.Resize(ref eventOrder, index + 1);
        eventOrder[index] = kind;
        other = target;
        m_cachedOther = target;
        otherAlive = target != null && target.IsAlive;
        selfAlive = GameObject.IsAlive;
        if (readTime) deltaSeconds = Time.DeltaSeconds;
        if (touchTransform) Transform.TranslateLocal(Vector3.RIGHT);
    }
    protected override void Update()
    {
        cachedOtherAlive = m_cachedOther != null && m_cachedOther.IsAlive;
        if (attemptCachedMutation && m_cachedOther != null) m_cachedOther.Transform.TranslateLocal(Vector3.RIGHT);
    }
    protected override void OnDestroy() { destroyCount++; }
}
